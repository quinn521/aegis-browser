// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_options_store.h"

#include <limits>
#include <array>
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/files/important_file_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "crypto/hash.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace aegis::access {
namespace {
constexpr int kVersion = kAccessOptionsSchemaVersion;
constexpr size_t kMaximumConfigurationBytes = 64 * 1024;
constexpr int kMaximumOperationRows = 128;
constexpr base::TimeDelta kOperationRetention = base::Days(7);
constexpr char kTestProfileKey[] = "internalTestProfile";
constexpr char kCreateOperations[] =
    "CREATE TABLE options_operations("
    "operation_id TEXT PRIMARY KEY NOT NULL,"
    "expected_revision INTEGER NOT NULL CHECK(expected_revision>=0),"
    "committed_revision INTEGER NOT NULL CHECK(committed_revision>0),"
    "profile_version INTEGER NOT NULL CHECK(profile_version>0),"
    "payload_fingerprint TEXT NOT NULL,payload TEXT NOT NULL,"
    "created_at_micros INTEGER NOT NULL CHECK(created_at_micros>0))";

std::optional<AccessOptionsSnapshot> DecodeSnapshot(
    uint64_t revision, const std::string& configuration) {
  if (configuration.size() > kMaximumConfigurationBytes) return std::nullopt;
  auto value = base::JSONReader::Read(configuration, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) return std::nullopt;
  AccessOptionsSnapshot result{revision, configuration, kVersion, std::nullopt};
  if (const auto* profile = value->GetDict().Find(kTestProfileKey)) {
    if (!profile->is_dict()) return std::nullopt;
    result.test_profile = DecodeAccessStoredTestProfile(profile->GetDict());
    if (!result.test_profile || result.test_profile->committed_revision > revision)
      return std::nullopt;
  }
  return result;
}
bool ValidConfiguration(const std::string& configuration) {
  if (configuration.size() > kMaximumConfigurationBytes)
    return false;
  auto value = base::JSONReader::Read(configuration, base::JSON_PARSE_RFC);
  return value && value->is_dict();
}

std::string ProfileFingerprint(uint64_t revision,
                               const AccessTestProfileDraft& profile) {
  auto encoded = EncodeAccessTestProfileDraft(profile);
  std::string canonical;
  if (!encoded || !base::JSONWriter::Write(*encoded, &canonical)) return {};
  const auto input = "access-test-profile-v1\n" + base::NumberToString(revision) +
      "\n" + canonical;
  return base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(input)));
}

struct StoredOperation {
  AccessOptionsOperationReceipt receipt;
  AccessStoredTestProfile profile;
};
StoreResult<StoredOperation> ReadOperation(sql::Database& database,
                                          const std::string& operation_id) {
  sql::Statement row(database.GetUniqueStatement(
      "SELECT expected_revision,committed_revision,profile_version,"
      "payload_fingerprint,payload,created_at_micros FROM options_operations "
      "WHERE operation_id=?"));
  row.BindString(0, operation_id);
  if (!row.Step())
    return {row.Succeeded() ? StoreStatus::kMissing : StoreStatus::kIoError,
            std::nullopt, "operation_read_failed"};
  for (int index = 0; index < 6; ++index) {
    if (row.GetColumnType(index) != (index == 3 || index == 4
            ? sql::ColumnType::kText : sql::ColumnType::kInteger))
      return {StoreStatus::kCorrupt, std::nullopt, "invalid_operation_column_type"};
  }
  const int64_t expected = row.ColumnInt64(0);
  const int64_t committed = row.ColumnInt64(1);
  const int64_t version = row.ColumnInt64(2);
  const auto payload = row.ColumnString(4);
  const auto value = payload.size() <= kMaximumConfigurationBytes
      ? base::JSONReader::Read(payload, base::JSON_PARSE_RFC) : std::nullopt;
  auto profile = value && value->is_dict()
      ? DecodeAccessStoredTestProfile(value->GetDict()) : std::nullopt;
  if (expected < 0 || expected == std::numeric_limits<int64_t>::max() ||
      committed != expected + 1 || version <= 0 || row.ColumnInt64(5) <= 0 ||
      !profile || profile->operation_id != operation_id ||
      profile->committed_revision != static_cast<uint64_t>(committed) ||
      profile->version != static_cast<uint64_t>(version) ||
      row.ColumnString(3) != ProfileFingerprint(expected, profile->configuration))
    return {StoreStatus::kCorrupt, std::nullopt, "invalid_operation_receipt"};
  return {StoreStatus::kValid,
      StoredOperation{AccessOptionsOperationReceipt{operation_id,
          static_cast<uint64_t>(expected), static_cast<uint64_t>(committed),
          static_cast<uint64_t>(version), row.ColumnString(3)}, std::move(*profile)}, {}};
}

AccessTestProfileSaveResult SaveError(StoreStatus status, const char* detail,
    AccessOptionsCommitOutcome outcome = AccessOptionsCommitOutcome::kNotCommitted) {
  return {status, outcome, std::nullopt, std::nullopt, detail};
}
}  // namespace

bool AccessOptionsCommitFence::Cancel() {
  auto expected = State::kPending;
  return state_.compare_exchange_strong(expected, State::kCanceled) || expected == State::kCanceled;
}
bool AccessOptionsCommitFence::canceled() const { return state_.load() == State::kCanceled; }
bool AccessOptionsCommitFence::TryBeginCommit() {
  auto expected = State::kPending;
  return state_.compare_exchange_strong(expected, State::kCommitting);
}

AccessOptionsStore::AccessOptionsStore(aegis_access::AccessBuildIdentity identity,
                                     base::FilePath profile_path,
                                     std::string profile_id,
                                     bool is_private)
    : AccessOptionsStore(identity, std::move(profile_path), std::move(profile_id),
                         is_private, sql::DatabaseOptions()) {}

std::unique_ptr<AccessOptionsStore> AccessOptionsStore::CreateWithSharedLockingForTesting(
    aegis_access::AccessBuildIdentity identity, base::FilePath profile_path,
    std::string profile_id, bool is_private, bool enable_triggers_for_testing) {
  return std::unique_ptr<AccessOptionsStore>(new AccessOptionsStore(
      identity, std::move(profile_path), std::move(profile_id), is_private,
      sql::DatabaseOptions().set_exclusive_locking(false)
          .set_enable_triggers(enable_triggers_for_testing)));
}

AccessOptionsStore::AccessOptionsStore(aegis_access::AccessBuildIdentity identity,
                                     base::FilePath profile_path,
                                     std::string profile_id,
                                     bool is_private,
                                     sql::DatabaseOptions options)
    : identity_(identity), profile_id_(std::move(profile_id)),
      private_(is_private), database_(std::move(options), "AegisAccess") {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  if (!private_ && !profile_path.empty()) {
    const char* channel = identity.product == aegis_access::AccessProductChannel::kDebug
        ? "debug" : "alpha";
    database_path_ = profile_path.AppendASCII("AccessOptions")
        .AppendASCII(channel).AppendASCII("options.db");
  }
}
AccessOptionsStore::~AccessOptionsStore() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}
bool AccessOptionsStore::Admitted(aegis_access::AccessOptionsOperation operation) const {
  return aegis_access::CanUseAccessOptions(identity_, operation);
}
StoreStatus AccessOptionsStore::ValidateIdentity() {
  sql::Statement statement(database_.GetUniqueStatement(
      "SELECT version,channel,profile_id FROM identity WHERE singleton=1"));
  if (!statement.Step())
    return StoreStatus::kCorrupt;
  if (statement.GetColumnType(0) != sql::ColumnType::kInteger ||
      statement.GetColumnType(1) != sql::ColumnType::kInteger ||
      statement.GetColumnType(2) != sql::ColumnType::kText)
    return StoreStatus::kCorrupt;
  if (statement.ColumnInt(1) != static_cast<int>(identity_.channel) ||
      statement.ColumnString(2) != profile_id_)
    return StoreStatus::kConflict;
  if (statement.ColumnInt(0) != kVersion)
    return StoreStatus::kIncompatible;
  return StoreStatus::kValid;
}
StoreStatus AccessOptionsStore::MigrateLegacySchema(int source_version) {
  if (source_version != 1 && source_version != 2)
    return StoreStatus::kIncompatible;
  const auto directory = database_path_.DirName();
  const auto backup = directory.AppendASCII(
      source_version == 1 ? "options.v1.db" : "options.v2.db");
  const auto backup_staging = directory.AppendASCII("options.backup-staging.db");
  const auto temporary = directory.AppendASCII("options.migrating.db");
  const auto marker = directory.AppendASCII("migration.pending");
  const std::string transition = base::NumberToString(source_version) + "->3";
  if (base::PathExists(marker)) {
    std::string state;
    if (!base::ReadFileToString(marker, &state))
      return StoreStatus::kRecoveryRequired;
    if (state == "1->2" && source_version == 2) {
      const auto completed = ValidateMigrationMarker();
      if (completed != StoreStatus::kValid) return completed;
    } else if (state != transition && !(state == "1->2" && source_version == 1)) {
      return StoreStatus::kRecoveryRequired;
    }
  }
  sql::Statement version(database_.GetUniqueStatement(
      "SELECT version FROM identity WHERE singleton=1"));
  if (!version.Step() || version.ColumnInt(0) != source_version)
    return StoreStatus::kIncompatible;
  version.Clear();
  sql::Statement contents(database_.GetUniqueStatement(
      "SELECT revision,payload FROM configuration WHERE singleton=1"));
  if (!contents.Step() || contents.ColumnInt64(0) < 0 ||
      !ValidConfiguration(contents.ColumnString(1)))
    return StoreStatus::kCorrupt;
  const auto revision = contents.ColumnInt64(0);
  const auto payload = contents.ColumnString(1);
  contents.Clear();
  // Old generic writers could store arbitrary dictionaries. A reserved field
  // in legacy data is not an authorized credential/profile binding.
  const auto old_value = base::JSONReader::Read(payload, base::JSON_PARSE_RFC);
  if (old_value->GetDict().contains(kTestProfileKey))
    return StoreStatus::kIncompatible;
  auto validate_backup = [&](const base::FilePath& path) {
    sql::Database original("AegisAccess");
    if (!original.Open(path) || !original.DoesTableExist("identity") ||
        !original.DoesTableExist("configuration"))
      return StoreStatus::kRecoveryRequired;
    sql::Statement check(original.GetUniqueStatement(
        "SELECT i.version,i.channel,i.profile_id,c.revision,c.payload "
        "FROM identity i,configuration c WHERE i.singleton=1 AND c.singleton=1"));
    if (!check.Step() || check.ColumnInt(0) != source_version ||
        check.ColumnInt(1) != static_cast<int>(identity_.channel) ||
        check.ColumnString(2) != profile_id_ || check.ColumnInt64(3) != revision ||
        check.ColumnString(4) != payload)
      return StoreStatus::kConflict;
    return StoreStatus::kValid;
  };
  if (base::PathExists(backup)) {
    const auto status = validate_backup(backup);
    if (status != StoreStatus::kValid)
      return status;
  } else {
    // A crashed VACUUM may leave an incomplete destination. Only the staged
    // name can be discarded; a published recovery source is never overwritten.
    if (base::PathExists(backup_staging) && !base::DeleteFile(backup_staging))
      return StoreStatus::kIoError;
    if (interrupt_backup_) {
      if (!base::WriteFile(backup_staging, "interrupted-vacuum"))
        return StoreStatus::kIoError;
      return StoreStatus::kRecoveryRequired;
    }
    {
      sql::Statement copy(database_.GetUniqueStatement("VACUUM INTO ?"));
      copy.BindString(0, backup_staging.AsUTF8Unsafe());
      if (!copy.Run())
        return StoreStatus::kIoError;
    }
    if (validate_backup(backup_staging) != StoreStatus::kValid ||
        !base::ReplaceFile(backup_staging, backup, nullptr))
      return StoreStatus::kRecoveryRequired;
  }
  // The original remains intact at both paths until the upgraded temporary
  // database has committed and the durable recovery marker has been written.
  if (base::PathExists(temporary) && !base::DeleteFile(temporary))
    return StoreStatus::kIoError;
  {
    sql::Statement copy(database_.GetUniqueStatement("VACUUM INTO ?"));
    copy.BindString(0, temporary.AsUTF8Unsafe());
    if (!copy.Run())
      return StoreStatus::kIoError;
  }
  {
    sql::Database upgraded("AegisAccess");
    if (!upgraded.Open(temporary))
      return StoreStatus::kIoError;
    sql::Transaction transaction(&upgraded);
    if (!transaction.Begin() || !upgraded.Execute(kCreateOperations))
      return StoreStatus::kIoError;
    sql::Statement update(upgraded.GetUniqueStatement(
        "UPDATE identity SET version=3 WHERE singleton=1 AND version=?"));
    update.BindInt(0, source_version);
    if (!update.Run() || upgraded.GetLastChangeCount() != 1 || !transaction.Commit())
      return StoreStatus::kIoError;
  }
  if (!base::ImportantFileWriter::WriteFileAtomically(marker, transition))
    return StoreStatus::kIoError;
  if (interrupt_migration_)
    return StoreStatus::kRecoveryRequired;
  database_.Close();
  if (!base::ReplaceFile(temporary, database_path_, nullptr) ||
      !database_.Open(database_path_))
    return StoreStatus::kRecoveryRequired;
  if (ValidateIdentity() != StoreStatus::kValid ||
      ValidateMigrationMarker() != StoreStatus::kValid)
    return StoreStatus::kRecoveryRequired;
  return StoreStatus::kValid;
}

StoreStatus AccessOptionsStore::ValidateMigrationMarker() {
  const auto folder = database_path_.DirName();
  const auto marker = folder.AppendASCII("migration.pending");
  if (!base::PathExists(marker)) return StoreStatus::kValid;
  std::string state;
  if (!base::ReadFileToString(marker, &state)) return StoreStatus::kRecoveryRequired;
  int before = 0;
  int after = 0;
  if (state == "1->2") { before = 1; after = 2; }
  else if (state == "1->3") { before = 1; after = 3; }
  else if (state == "2->3") { before = 2; after = 3; }
  else return StoreStatus::kRecoveryRequired;
  sql::Database backup("AegisAccess");
  const auto source = folder.AppendASCII(before == 1 ? "options.v1.db" : "options.v2.db");
  if (!base::PathExists(source) || !backup.Open(source))
    return StoreStatus::kRecoveryRequired;
  constexpr char kContents[] =
      "SELECT i.version,i.channel,i.profile_id,c.revision,c.payload "
      "FROM identity i,configuration c WHERE i.singleton=1 AND c.singleton=1";
  sql::Statement original(backup.GetUniqueStatement(kContents));
  sql::Statement current(database_.GetUniqueStatement(kContents));
  if (!original.Step() || !current.Step() || original.ColumnInt(0) != before ||
      current.ColumnInt(0) != after ||
      original.ColumnInt(1) != static_cast<int>(identity_.channel) ||
      current.ColumnInt(1) != static_cast<int>(identity_.channel) ||
      original.ColumnString(2) != profile_id_ || current.ColumnString(2) != profile_id_ ||
      original.ColumnInt64(3) != current.ColumnInt64(3) ||
      original.ColumnString(4) != current.ColumnString(4))
    return StoreStatus::kRecoveryRequired;
  return base::DeleteFile(marker) ? StoreStatus::kValid : StoreStatus::kIoError;
}

StoreStatus AccessOptionsStore::ValidateJournal() {
  if (!database_.DoesTableExist("options_operations")) return StoreStatus::kCorrupt;
  sql::Statement columns(database_.GetUniqueStatement("PRAGMA table_info(options_operations)"));
  constexpr std::array<std::string_view, 7> names = {"operation_id", "expected_revision",
      "committed_revision", "profile_version", "payload_fingerprint", "payload",
      "created_at_micros"};
  size_t index = 0;
  while (columns.Step()) {
    if (index >= std::size(names) || columns.ColumnString(1) != names[index] ||
        columns.ColumnString(2) != (index == 0 || index == 4 || index == 5 ? "TEXT" : "INTEGER") ||
        columns.ColumnInt(3) != 1 || columns.ColumnInt(5) != (index == 0 ? 1 : 0))
      return StoreStatus::kCorrupt;
    ++index;
  }
  return columns.Succeeded() && index == std::size(names)
      ? StoreStatus::kValid : StoreStatus::kCorrupt;
}
StoreStatus AccessOptionsStore::Open() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!Admitted(aegis_access::AccessOptionsOperation::kRead))
    return StoreStatus::kConflict;
  if (profile_id_.empty() || profile_id_.size() > 256 ||
      (!private_ && (!database_path_.IsAbsolute() || database_path_.ReferencesParent())))
    return StoreStatus::kInvalidArgument;
  if (!private_ && !base::CreateDirectory(database_path_.DirName()))
    return StoreStatus::kIoError;
  if (open_)
    return StoreStatus::kValid;
  if (database_.is_open())
    database_.Close();
  const bool existed = !private_ && base::PathExists(database_path_);
  const auto owner_marker = private_ ? base::FilePath() : database_path_.DirName().AppendASCII("options.owner");
  const std::string owner_input = base::NumberToString(static_cast<int>(identity_.channel)) + "\n" + profile_id_;
  const std::string owner_proof = "access-options-owner-v1:" +
      base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(owner_input)));
  const bool has_owner_marker = !private_ && base::PathExists(owner_marker);
  if (has_owner_marker) {
    std::string previous;
    if (!base::ReadFileToStringWithMaxSize(owner_marker, &previous, 128))
      return StoreStatus::kRecoveryRequired;
    if (previous != owner_proof) return StoreStatus::kConflict;
  }
  if (!private_ && !existed) {
    const auto directory = database_path_.DirName();
    if (has_owner_marker || base::PathExists(directory.AppendASCII("migration.pending")) ||
        base::PathExists(directory.AppendASCII("options.v1.db")) ||
        base::PathExists(directory.AppendASCII("options.v2.db")) ||
        base::PathExists(directory.AppendASCII("options.backup-staging.db")) ||
        base::PathExists(directory.AppendASCII("options.migrating.db")))
      return StoreStatus::kRecoveryRequired;
  }
  if (!(private_ ? database_.OpenInMemory() : database_.Open(database_path_)))
    return StoreStatus::kIoError;
  if (!database_.DoesTableExist("identity")) {
    if (existed) {
      database_.Close();
      return StoreStatus::kCorrupt;
    }
    sql::Transaction transaction(&database_);
    if (!transaction.Begin() ||
        !database_.Execute("CREATE TABLE identity(singleton INTEGER PRIMARY KEY CHECK(singleton=1),version INTEGER NOT NULL,channel INTEGER NOT NULL,profile_id TEXT NOT NULL)") ||
        !database_.Execute("CREATE TABLE configuration(singleton INTEGER PRIMARY KEY CHECK(singleton=1),revision INTEGER NOT NULL,payload TEXT NOT NULL)") ||
        !database_.Execute(kCreateOperations)) {
      return StoreStatus::kIoError;
    }
    sql::Statement insert(database_.GetUniqueStatement(
        "INSERT INTO identity VALUES(1,?,?,?)"));
    insert.BindInt(0, kVersion);
    insert.BindInt(1, static_cast<int>(identity_.channel));
    insert.BindString(2, profile_id_);
    if (!insert.Run() || !database_.Execute("INSERT INTO configuration VALUES(1,0,'{}')") ||
        !transaction.Commit()) {
      return StoreStatus::kIoError;
    }
  }
  auto status = ValidateIdentity();
  if (status == StoreStatus::kIncompatible && !private_) {
    sql::Statement version(database_.GetUniqueStatement(
        "SELECT version FROM identity WHERE singleton=1"));
    const int source_version = version.Step() ? version.ColumnInt(0) : 0;
    version.Clear();
    status = MigrateLegacySchema(source_version);
  }
  if (status == StoreStatus::kValid && !private_) status = ValidateMigrationMarker();
  if (status == StoreStatus::kValid) status = ValidateJournal();
  // Once a store has been admitted, loss of the primary cannot silently create
  // an empty configuration and authorize deletion of its Keychain references.
  if (status == StoreStatus::kValid && !private_ && !has_owner_marker &&
      !base::ImportantFileWriter::WriteFileAtomically(owner_marker, owner_proof))
    status = StoreStatus::kRecoveryRequired;
  open_ = status == StoreStatus::kValid;
  if (!open_)
    database_.Close();
  return status;
}
StoreResult<AccessOptionsSnapshot> AccessOptionsStore::OpenAndRead() {
  const auto status = Open();
  if (status != StoreStatus::kValid)
    return {status, std::nullopt, "options_open_failed"};
  return Read();
}
StoreResult<AccessOptionsSnapshot> AccessOptionsStore::Read() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ || !Admitted(aegis_access::AccessOptionsOperation::kRead))
    return {StoreStatus::kConflict, std::nullopt, "options_not_available"};
  sql::Statement configuration(database_.GetUniqueStatement(
      "SELECT revision,payload FROM configuration WHERE singleton=1"));
  if (!configuration.Step())
    return {configuration.Succeeded() ? StoreStatus::kCorrupt : StoreStatus::kIoError,
            std::nullopt, "configuration_read_failed"};
  if (configuration.GetColumnType(0) != sql::ColumnType::kInteger ||
      configuration.GetColumnType(1) != sql::ColumnType::kText ||
      configuration.ColumnInt64(0) < 0)
    return {StoreStatus::kCorrupt, std::nullopt, "invalid_configuration"};
  auto snapshot = DecodeSnapshot(configuration.ColumnInt64(0), configuration.ColumnString(1));
  if (snapshot && snapshot->test_profile) {
    const auto current_operation = ReadOperation(database_, snapshot->test_profile->operation_id);
    if (current_operation.status != StoreStatus::kMissing &&
        (current_operation.status != StoreStatus::kValid ||
         current_operation.value->profile != *snapshot->test_profile))
      return {current_operation.status == StoreStatus::kIoError ? StoreStatus::kIoError
                                                               : StoreStatus::kCorrupt,
              std::nullopt, "configuration_receipt_mismatch"};
  }
  return snapshot ? StoreResult<AccessOptionsSnapshot>{StoreStatus::kValid, std::move(snapshot), {}}
                  : StoreResult<AccessOptionsSnapshot>{StoreStatus::kCorrupt, std::nullopt, "invalid_configuration"};
}
StoreStatus AccessOptionsStore::Write(uint64_t expected_revision,
                                     const std::string& configuration) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ || !Admitted(aegis_access::AccessOptionsOperation::kWrite))
    return StoreStatus::kConflict;
  if (expected_revision >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      !ValidConfiguration(configuration))
    return StoreStatus::kInvalidArgument;
  sql::Transaction transaction(&database_);
  if (!transaction.Begin())
    return StoreStatus::kIoError;
  auto current = Read();
  if (current.status != StoreStatus::kValid || current.value->revision != expected_revision)
    return StoreStatus::kConflict;
  const auto previous = base::JSONReader::Read(current.value->configuration, base::JSON_PARSE_RFC);
  const auto incoming = base::JSONReader::Read(configuration, base::JSON_PARSE_RFC);
  const auto* old_profile = previous->GetDict().Find(kTestProfileKey);
  const auto* new_profile = incoming->GetDict().Find(kTestProfileKey);
  if ((old_profile == nullptr) != (new_profile == nullptr) ||
      (old_profile && *old_profile != *new_profile))
    return StoreStatus::kInvalidArgument;
  sql::Statement update(database_.GetUniqueStatement(
      "UPDATE configuration SET revision=?,payload=? WHERE singleton=1 AND revision=?"));
  update.BindInt64(0, expected_revision + 1);
  update.BindString(1, configuration);
  update.BindInt64(2, expected_revision);
  if (!update.Run())
    return StoreStatus::kIoError;
  if (database_.GetLastChangeCount() != 1)
    return StoreStatus::kConflict;
  if (!transaction.Commit())
    return StoreStatus::kIoError;
  return StoreStatus::kValid;
}

AccessTestProfileSaveResult AccessOptionsStore::GetTestProfileOperation(
    const std::string& operation_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ || private_ || !Admitted(aegis_access::AccessOptionsOperation::kImport))
    return SaveError(StoreStatus::kConflict, "test_profile_not_admitted",
                     AccessOptionsCommitOutcome::kUnknown);
  if (!base::Uuid::ParseLowercase(operation_id).is_valid())
    return SaveError(StoreStatus::kInvalidArgument, "invalid_operation_id");
  const auto operation = ReadOperation(database_, operation_id);
  std::optional<AccessOptionsOperationReceipt> receipt;
  if (operation.status == StoreStatus::kValid) {
    receipt = operation.value->receipt;
  } else if (operation.status != StoreStatus::kMissing) {
    return SaveError(operation.status, "operation_read_failed",
                     AccessOptionsCommitOutcome::kUnknown);
  }
  auto current = Read();
  if (current.status != StoreStatus::kValid)
    return SaveError(current.status, "operation_snapshot_unavailable",
                     AccessOptionsCommitOutcome::kUnknown);
  // The current row is an independent, exact fallback after journal expiry.
  // An absent historical operation cannot be classified as not committed.
  if (!receipt && current.value->test_profile &&
      current.value->test_profile->operation_id == operation_id) {
    const auto& profile = *current.value->test_profile;
    receipt = AccessOptionsOperationReceipt{operation_id, profile.committed_revision - 1,
        profile.committed_revision, profile.version,
        ProfileFingerprint(profile.committed_revision - 1, profile.configuration), true};
  }
  if (!receipt)
    return {StoreStatus::kMissing, AccessOptionsCommitOutcome::kUnknown, std::nullopt,
            std::move(current.value), "operation_not_retained"};
  if (receipt->committed_revision > current.value->revision ||
      !current.value->test_profile ||
      current.value->test_profile->version < receipt->profile_version ||
      (operation.value && operation.value->profile.configuration.id !=
                              current.value->test_profile->configuration.id) ||
      (current.value->test_profile->version == receipt->profile_version &&
       current.value->test_profile->operation_id != operation_id))
    return SaveError(StoreStatus::kCorrupt, "operation_snapshot_mismatch",
                     AccessOptionsCommitOutcome::kUnknown);
  return {StoreStatus::kValid, AccessOptionsCommitOutcome::kCommitted,
          std::move(receipt), std::move(current.value), {}};
}

AccessTestProfileSaveResult AccessOptionsStore::SaveTestProfile(
    uint64_t expected_revision, const std::string& operation_id,
    const AccessTestProfileDraft& draft,
    std::shared_ptr<AccessOptionsCommitFence> commit_fence) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ || private_ || !Admitted(aegis_access::AccessOptionsOperation::kImport) ||
      !Admitted(aegis_access::AccessOptionsOperation::kWrite))
    return SaveError(StoreStatus::kConflict, "test_profile_not_admitted");
  if (expected_revision >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      !base::Uuid::ParseLowercase(operation_id).is_valid() || !IsValidAccessTestProfileDraft(draft))
    return SaveError(StoreStatus::kInvalidArgument, "invalid_test_profile_operation");
  const auto fingerprint = ProfileFingerprint(expected_revision, draft);
  if (fingerprint.empty()) return SaveError(StoreStatus::kInvalidArgument, "profile_encoding_failed");
  if (commit_fence && commit_fence->canceled())
    return SaveError(StoreStatus::kConflict, "operation_canceled_before_commit");
  sql::Transaction transaction(&database_);
  if (!transaction.Begin()) return SaveError(StoreStatus::kIoError, "transaction_unavailable");
  auto previous = GetTestProfileOperation(operation_id);
  if (previous.status == StoreStatus::kValid) {
    if (previous.operation->expected_revision != expected_revision ||
        previous.operation->payload_fingerprint != fingerprint)
      return SaveError(StoreStatus::kConflict, "operation_content_conflict");
    return previous;
  }
  if (previous.status != StoreStatus::kMissing) return previous;
  auto current = std::move(previous.snapshot);
  if (!current || current->revision != expected_revision)
    return SaveError(StoreStatus::kConflict, "configuration_revision_conflict");
  const uint64_t version = current->test_profile ? current->test_profile->version + 1 : 1;
  if (current->test_profile && current->test_profile->configuration.id != draft.id)
    return SaveError(StoreStatus::kConflict, "profile_identity_conflict");
  AccessStoredTestProfile stored{draft, version, expected_revision + 1, operation_id};
  auto encoded = EncodeAccessStoredTestProfile(stored);
  auto value = base::JSONReader::Read(current->configuration, base::JSON_PARSE_RFC);
  std::string operation_payload;
  std::string configuration;
  if (!encoded || !base::JSONWriter::Write(*encoded, &operation_payload))
    return SaveError(StoreStatus::kInvalidArgument, "profile_encoding_failed");
  value->GetDict().Set(kTestProfileKey, std::move(*encoded));
  if (!base::JSONWriter::Write(*value, &configuration) ||
      configuration.size() > kMaximumConfigurationBytes)
    return SaveError(StoreStatus::kCapacity, "configuration_capacity");
  const auto now = now_for_testing_.value_or(base::Time::Now());
  if (now.is_null() || now.ToDeltaSinceWindowsEpoch().InMicroseconds() <= 0)
    return SaveError(StoreStatus::kIoError, "clock_unavailable");
  sql::Statement prune(database_.GetUniqueStatement(
      "DELETE FROM options_operations WHERE created_at_micros<?"));
  prune.BindInt64(0, (now - kOperationRetention).ToDeltaSinceWindowsEpoch().InMicroseconds());
  if (!prune.Run()) return SaveError(StoreStatus::kIoError, "operation_retention_failed");
  sql::Statement count(database_.GetUniqueStatement("SELECT COUNT(*) FROM options_operations"));
  if (!count.Step()) return SaveError(StoreStatus::kIoError, "operation_count_failed");
  if (count.ColumnInt64(0) >= kMaximumOperationRows)
    return SaveError(StoreStatus::kCapacity, "operation_capacity");
  count.Clear();
  sql::Statement update(database_.GetUniqueStatement(
      "UPDATE configuration SET revision=?,payload=? WHERE singleton=1 AND revision=?"));
  update.BindInt64(0, expected_revision + 1);
  update.BindString(1, configuration);
  update.BindInt64(2, expected_revision);
  if (!update.Run()) return SaveError(StoreStatus::kIoError, "configuration_write_failed");
  if (database_.GetLastChangeCount() != 1)
    return SaveError(StoreStatus::kConflict, "configuration_revision_conflict");
  if (save_failure_for_testing_ == SaveFailurePointForTesting::kAfterConfigurationUpdate)
    return SaveError(StoreStatus::kIoError, "injected_before_journal");
  sql::Statement insert(database_.GetUniqueStatement(
      "INSERT INTO options_operations VALUES(?,?,?,?,?,?,?)"));
  insert.BindString(0, operation_id);
  insert.BindInt64(1, expected_revision);
  insert.BindInt64(2, expected_revision + 1);
  insert.BindInt64(3, version);
  insert.BindString(4, fingerprint);
  insert.BindString(5, operation_payload);
  insert.BindInt64(6, now.ToDeltaSinceWindowsEpoch().InMicroseconds());
  if (!insert.Run() || database_.GetLastChangeCount() != 1)
    return SaveError(StoreStatus::kIoError, "operation_write_failed");
  if (save_failure_for_testing_ == SaveFailurePointForTesting::kAfterJournalInsert)
    return SaveError(StoreStatus::kIoError, "injected_before_commit");
  if (before_commit_for_testing_) before_commit_for_testing_.Run();
  if (commit_fence && !commit_fence->TryBeginCommit())
    return SaveError(StoreStatus::kConflict, "operation_canceled_before_commit");
  if (!transaction.Commit())
    return SaveError(StoreStatus::kIoError, "commit_result_unknown", AccessOptionsCommitOutcome::kUnknown);
  if (save_failure_for_testing_ == SaveFailurePointForTesting::kAfterCommit)
    return SaveError(StoreStatus::kIoError, "injected_reply_lost", AccessOptionsCommitOutcome::kUnknown);
  return {StoreStatus::kValid, AccessOptionsCommitOutcome::kCommitted,
      AccessOptionsOperationReceipt{operation_id, expected_revision, expected_revision + 1,
                                    version, fingerprint},
      AccessOptionsSnapshot{expected_revision + 1, std::move(configuration), kVersion,
                            std::move(stored)}, {}};
}

StoreResult<std::set<std::string>> AccessOptionsStore::ReadTestProfileSecretReferences() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ || private_ || !Admitted(aegis_access::AccessOptionsOperation::kImport))
    return {StoreStatus::kConflict, std::nullopt, "test_profile_not_admitted"};
  sql::Transaction transaction(&database_);
  if (!transaction.Begin()) return {StoreStatus::kIoError, std::nullopt, "reference_read_failed"};
  const auto snapshot = Read();
  if (snapshot.status != StoreStatus::kValid)
    return {snapshot.status, std::nullopt, "reference_snapshot_unavailable"};
  std::set<std::string> references;
  if (snapshot.value->test_profile)
    references.insert(snapshot.value->test_profile->configuration.secret_reference);
  sql::Statement rows(database_.GetUniqueStatement("SELECT operation_id FROM options_operations"));
  size_t count = 0;
  while (rows.Step()) {
    if (++count > kMaximumOperationRows)
      return {StoreStatus::kCapacity, std::nullopt, "operation_capacity"};
    const auto operation_id = rows.ColumnString(0);
    if (rows.GetColumnType(0) != sql::ColumnType::kText ||
        !base::Uuid::ParseLowercase(operation_id).is_valid())
      return {StoreStatus::kCorrupt, std::nullopt, "invalid_operation_id"};
    const auto operation = ReadOperation(database_, operation_id);
    if (operation.status != StoreStatus::kValid ||
        operation.value->profile.committed_revision > snapshot.value->revision ||
        !snapshot.value->test_profile ||
        operation.value->profile.configuration.id != snapshot.value->test_profile->configuration.id ||
        operation.value->profile.version > snapshot.value->test_profile->version)
      return {operation.status == StoreStatus::kIoError ? StoreStatus::kIoError : StoreStatus::kCorrupt,
              std::nullopt, "invalid_retained_operation"};
    references.insert(operation.value->profile.configuration.secret_reference);
  }
  if (!rows.Succeeded()) return {StoreStatus::kIoError, std::nullopt, "reference_read_failed"};
  rows.Clear();
  if (!transaction.Commit()) return {StoreStatus::kIoError, std::nullopt, "reference_read_failed"};
  return {StoreStatus::kValid, std::move(references), {}};
}
}  // namespace aegis::access
