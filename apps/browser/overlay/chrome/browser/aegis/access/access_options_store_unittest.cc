// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_options_store.h"
#include "chrome/browser/aegis/access/access_test_profile_config.h"
#include <array>
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/location.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/bind.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "sql/statement.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
bool CreateVersionOneOptions(const base::FilePath& folder) {
  if (!base::CreateDirectory(folder)) return false;
  sql::Database legacy("AegisAccess");
  return legacy.Open(folder.AppendASCII("options.db")) &&
      legacy.Execute("CREATE TABLE identity(singleton INTEGER PRIMARY KEY,version INTEGER,channel INTEGER,profile_id TEXT)") &&
      legacy.Execute("INSERT INTO identity VALUES(1,1,1,'profile')") &&
      legacy.Execute("CREATE TABLE configuration(singleton INTEGER PRIMARY KEY,revision INTEGER,payload TEXT)") &&
      legacy.Execute("INSERT INTO configuration VALUES(1,7,'{}')");
}
aegis_access::AccessBuildIdentity InternalIdentity() {
  return aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
}
AccessTestProfileDraft SyntheticDraft() {
  return {"550e8400-e29b-41d4-a716-446655440000", "TEST",
          "550e8400-e29b-41d4-a716-446655440001", {}};
}
std::string NewOperation() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }
}  // namespace

TEST(AccessOptionsProfileStoreTest, DurableIdempotenceAndContentConflicts) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto draft = SyntheticDraft();
  const auto operation = NewOperation();
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    const auto saved = store.SaveTestProfile(0, operation, draft);
    ASSERT_EQ(saved.status, StoreStatus::kValid);
    EXPECT_EQ(saved.outcome, AccessOptionsCommitOutcome::kCommitted);
    ASSERT_TRUE(saved.snapshot->test_profile);
    EXPECT_EQ(saved.snapshot->test_profile->configuration, draft);
    EXPECT_EQ(saved.snapshot->revision, 1u);
    EXPECT_EQ(saved.snapshot->test_profile->version, 1u);
    const auto repeated = store.SaveTestProfile(0, operation, draft);
    ASSERT_EQ(repeated.status, StoreStatus::kValid);
    EXPECT_EQ(repeated.operation->payload_fingerprint, saved.operation->payload_fingerprint);
    EXPECT_EQ(store.Read().value->revision, 1u);
    auto different = draft;
    different.inbound.protocol = aegis_access::AccessInboundProtocolPreference::kHttp;
    EXPECT_EQ(store.SaveTestProfile(0, operation, different).status, StoreStatus::kConflict);
    EXPECT_EQ(store.SaveTestProfile(1, operation, draft).status, StoreStatus::kConflict);
    EXPECT_EQ(store.SaveTestProfile(0, NewOperation(), draft).status, StoreStatus::kConflict);
    different = draft;
    different.id = NewOperation();
    EXPECT_EQ(store.SaveTestProfile(1, NewOperation(), different).status, StoreStatus::kConflict);
  }
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
  const auto queried = reopened.GetTestProfileOperation(operation);
  ASSERT_EQ(queried.status, StoreStatus::kValid);
  EXPECT_EQ(queried.outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(queried.operation->committed_revision, 1u);
  EXPECT_EQ(queried.snapshot->test_profile->configuration, draft);
}

TEST(AccessOptionsProfileStoreTest, ConfigurationAndJournalRollBackTogether) {
  using Failure = AccessOptionsStore::SaveFailurePointForTesting;
  for (const auto point : {Failure::kAfterConfigurationUpdate, Failure::kAfterJournalInsert}) {
    base::ScopedTempDir dir;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    const auto operation = NewOperation();
    {
      AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
      ASSERT_EQ(store.Open(), StoreStatus::kValid);
      store.SetSaveFailureForTesting(point);
      const auto failed = store.SaveTestProfile(0, operation, SyntheticDraft());
      EXPECT_EQ(failed.status, StoreStatus::kIoError);
      EXPECT_EQ(failed.outcome, AccessOptionsCommitOutcome::kNotCommitted);
      EXPECT_EQ(store.Read().value->revision, 0u);
      EXPECT_FALSE(store.Read().value->test_profile);
      const auto absent = store.GetTestProfileOperation(operation);
      EXPECT_EQ(absent.status, StoreStatus::kMissing);
      EXPECT_EQ(absent.outcome, AccessOptionsCommitOutcome::kUnknown);
    }
    AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
    EXPECT_EQ(reopened.Read().value->revision, 0u);
    EXPECT_EQ(reopened.SaveTestProfile(0, operation, SyntheticDraft()).status, StoreStatus::kValid);
  }
}

TEST(AccessOptionsProfileStoreTest, LostCommitReplyQueriesSameOperationAfterReopen) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto operation = NewOperation();
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    store.SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting::kAfterCommit);
    const auto lost = store.SaveTestProfile(0, operation, SyntheticDraft());
    EXPECT_EQ(lost.status, StoreStatus::kIoError);
    EXPECT_EQ(lost.outcome, AccessOptionsCommitOutcome::kUnknown);
    EXPECT_FALSE(lost.operation);
  }
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
  const auto queried = reopened.GetTestProfileOperation(operation);
  ASSERT_EQ(queried.status, StoreStatus::kValid);
  EXPECT_EQ(queried.outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_TRUE(queried.operation);
  EXPECT_EQ(queried.operation->committed_revision, 1u);
  EXPECT_EQ(queried.operation->secret_reference,
            SyntheticDraft().secret_reference);
  EXPECT_EQ(reopened.SaveTestProfile(0, operation, SyntheticDraft()).status, StoreStatus::kValid);
  EXPECT_EQ(reopened.Read().value->revision, 1u);
  auto replacement = SyntheticDraft();
  replacement.secret_reference = NewOperation();
  ASSERT_EQ(reopened.SaveTestProfile(1, NewOperation(), replacement).status,
            StoreStatus::kValid);
  const auto historical = reopened.GetTestProfileOperation(operation);
  ASSERT_EQ(historical.outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_TRUE(historical.operation);
  ASSERT_TRUE(historical.snapshot && historical.snapshot->test_profile);
  EXPECT_EQ(historical.operation->secret_reference,
            SyntheticDraft().secret_reference);
  EXPECT_EQ(historical.snapshot->test_profile->configuration.secret_reference,
            replacement.secret_reference);
}

TEST(AccessOptionsProfileStoreTest, GenericWritesCannotForgeReplaceOrDeleteTypedProfile) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(store.Open(), StoreStatus::kValid);
  EXPECT_EQ(store.Write(0, "{\"internalTestProfile\":{}}"), StoreStatus::kInvalidArgument);
  ASSERT_EQ(store.SaveTestProfile(0, NewOperation(), SyntheticDraft()).status, StoreStatus::kValid);
  EXPECT_EQ(store.Write(1, "{}"), StoreStatus::kInvalidArgument);
  const auto current = store.Read();
  auto root = base::JSONReader::Read(current.value->configuration, base::JSON_PARSE_RFC);
  root->GetDict().Set("theme", "dark");
  std::string same_profile;
  ASSERT_TRUE(base::JSONWriter::Write(*root, &same_profile));
  EXPECT_EQ(store.Write(1, same_profile), StoreStatus::kValid);
  EXPECT_EQ(store.Read().value->test_profile, current.value->test_profile);
  root->GetDict().FindDict("internalTestProfile")->Set("secretReference", NewOperation());
  std::string forged;
  ASSERT_TRUE(base::JSONWriter::Write(*root, &forged));
  EXPECT_EQ(store.Write(2, forged), StoreStatus::kInvalidArgument);
  EXPECT_EQ(store.Read().value->revision, 2u);
}

TEST(AccessOptionsProfileStoreTest, PrivateAndRestrictedChannelsRejectTypedPersistence) {
  AccessOptionsStore private_store(InternalIdentity(), {}, "private", true);
  ASSERT_EQ(private_store.Open(), StoreStatus::kValid);
  EXPECT_EQ(private_store.SaveTestProfile(0, NewOperation(), SyntheticDraft()).status, StoreStatus::kConflict);
  EXPECT_EQ(private_store.Read().value->revision, 0u);
  EXPECT_EQ(private_store.GetTestProfileOperation(NewOperation()).status, StoreStatus::kConflict);
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  for (const auto product : {aegis_access::AccessProductChannel::kBeta,
                            aegis_access::AccessProductChannel::kStable}) {
    const auto identity = aegis_access::ResolveAccessBuildIdentity(product,
        aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kAlpha, true);
    AccessOptionsStore denied(identity, dir.GetPath(), "profile", false);
    EXPECT_EQ(denied.Open(), StoreStatus::kConflict);
    EXPECT_EQ(denied.SaveTestProfile(0, NewOperation(), SyntheticDraft()).status, StoreStatus::kConflict);
  }
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
}

TEST(AccessOptionsProfileStoreTest, CapacityRetentionAndExpiredCurrentOperationFallback) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(store.Open(), StoreStatus::kValid);
  const auto now = base::Time::Now();
  store.SetNowForTesting(now);
  std::string first;
  std::string last;
  for (size_t revision = 0; revision < 128; ++revision) {
    last = NewOperation();
    if (revision == 0) first = last;
    ASSERT_EQ(store.SaveTestProfile(revision, last, SyntheticDraft()).status, StoreStatus::kValid);
  }
  EXPECT_EQ(store.SaveTestProfile(128, NewOperation(), SyntheticDraft()).status, StoreStatus::kCapacity);
  EXPECT_EQ(store.Read().value->revision, 128u);
  store.SetNowForTesting(now + base::Days(8));
  // Expiry cleanup and the new operation are atomic, even on an injected fault.
  store.SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting::kAfterJournalInsert);
  EXPECT_EQ(store.SaveTestProfile(128, NewOperation(), SyntheticDraft()).status, StoreStatus::kIoError);
  EXPECT_EQ(store.GetTestProfileOperation(first).status, StoreStatus::kValid);
  store.SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting::kNone);
  ASSERT_EQ(store.SaveTestProfile(128, NewOperation(), SyntheticDraft()).status, StoreStatus::kValid);
  const auto expired = store.GetTestProfileOperation(first);
  EXPECT_EQ(expired.status, StoreStatus::kMissing);
  EXPECT_EQ(expired.outcome, AccessOptionsCommitOutcome::kUnknown);
  EXPECT_EQ(expired.snapshot->revision, 129u);
  EXPECT_EQ(store.SaveTestProfile(0, first, SyntheticDraft()).status, StoreStatus::kConflict);
  // Remove a current operation using a separate connection after the store is
  // closed below in a separate case, so production exclusive locks are kept.
}

TEST(AccessOptionsProfileStoreTest, CurrentMetadataResolvesPrunedJournalWithoutReplaying) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto operation = NewOperation();
  base::FilePath path;
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    ASSERT_EQ(store.SaveTestProfile(0, operation, SyntheticDraft()).status, StoreStatus::kValid);
    path = store.database_path();
  }
  {
    sql::Database fixture("AegisAccess");
    ASSERT_TRUE(fixture.Open(path));
    ASSERT_TRUE(fixture.Execute("DELETE FROM options_operations"));
  }
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
  const auto current = reopened.GetTestProfileOperation(operation);
  ASSERT_EQ(current.status, StoreStatus::kValid);
  EXPECT_TRUE(current.operation->recovered_from_snapshot);
  EXPECT_EQ(current.operation->secret_reference,
            SyntheticDraft().secret_reference);
  EXPECT_EQ(reopened.SaveTestProfile(0, operation, SyntheticDraft()).status, StoreStatus::kValid);
  EXPECT_EQ(reopened.Read().value->revision, 1u);
}

TEST(AccessOptionsProfileStoreTest, TypedUpdateAndJournalMustActuallyAffectOneRow) {
  for (const char* table : {"configuration", "options_operations"}) {
    base::ScopedTempDir dir;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    auto owned = AccessOptionsStore::CreateWithSharedLockingForTesting(
        InternalIdentity(), dir.GetPath(), "profile", false, true);
    ASSERT_EQ(owned->Open(), StoreStatus::kValid);
    {
      sql::Database injector("AegisAccess");
      ASSERT_TRUE(injector.Open(owned->database_path()));
      const std::string trigger = std::string("CREATE TRIGGER ignore_typed BEFORE ") +
          (std::string_view(table) == "configuration" ? "UPDATE" : "INSERT") +
          " ON " + table + " BEGIN SELECT RAISE(IGNORE); END";
      ASSERT_TRUE(injector.Execute(trigger));
    }
    const auto operation = NewOperation();
    const auto failed = owned->SaveTestProfile(0, operation, SyntheticDraft());
    EXPECT_NE(failed.status, StoreStatus::kValid);
    EXPECT_EQ(failed.outcome, AccessOptionsCommitOutcome::kNotCommitted);
    EXPECT_EQ(owned->Read().value->revision, 0u);
    EXPECT_EQ(owned->GetTestProfileOperation(operation).status, StoreStatus::kMissing);
  }
}

TEST(AccessOptionsProfileStoreTest, IndependentConnectionsCompeteOnOneTypedRevision) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto observer = AccessOptionsStore::CreateWithSharedLockingForTesting(
      InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(observer->Open(), StoreStatus::kValid);
  size_t total_committed = 0;
  for (size_t round = 0; round < 8; ++round) {
    const auto revision = observer->Read().value->revision;
    std::array<AccessTestProfileSaveResult, 2> results;
    std::array<AccessTestProfileDraft, 2> drafts{SyntheticDraft(), SyntheticDraft()};
    drafts[0].name = "Window A";
    drafts[1].name = "Window B";
    const std::array<std::string, 2> operations{NewOperation(), NewOperation()};
    base::WaitableEvent ready_a, ready_b, start;
    base::Thread a("TypedOptionsA"), b("TypedOptionsB");
    ASSERT_TRUE(a.Start());
    ASSERT_TRUE(b.Start());
    auto writer = [&](size_t index) {
      auto connection = AccessOptionsStore::CreateWithSharedLockingForTesting(
          InternalIdentity(), dir.GetPath(), "profile", false);
      const auto opened = connection->Open();
      (index == 0 ? ready_a : ready_b).Signal();
      if (start.TimedWait(base::Seconds(10)) && opened == StoreStatus::kValid)
        results[index] = connection->SaveTestProfile(revision, operations[index], drafts[index]);
    };
    a.task_runner()->PostTask(FROM_HERE, base::BindLambdaForTesting([&] { writer(0); }));
    b.task_runner()->PostTask(FROM_HERE, base::BindLambdaForTesting([&] { writer(1); }));
    const bool a_ready = ready_a.TimedWait(base::Seconds(10));
    const bool b_ready = ready_b.TimedWait(base::Seconds(10));
    start.Signal();
    a.Stop();
    b.Stop();
    ASSERT_TRUE(a_ready);
    ASSERT_TRUE(b_ready);
    const size_t committed = (results[0].status == StoreStatus::kValid ? 1 : 0) +
        (results[1].status == StoreStatus::kValid ? 1 : 0);
    ASSERT_LE(committed, 1u);
    total_committed += committed;
    const auto after = observer->Read();
    ASSERT_EQ(after.status, StoreStatus::kValid);
    EXPECT_EQ(after.value->revision, revision + committed);
    for (size_t index = 0; index < 2; ++index) {
      const auto operation = observer->GetTestProfileOperation(operations[index]);
      if (results[index].status == StoreStatus::kValid) {
        ASSERT_TRUE(after.value->test_profile);
        EXPECT_EQ(after.value->test_profile->configuration, drafts[index]);
        EXPECT_EQ(after.value->test_profile->operation_id, operations[index]);
        EXPECT_EQ(operation.status, StoreStatus::kValid);
        EXPECT_EQ(results[index].outcome, AccessOptionsCommitOutcome::kCommitted);
      } else {
        EXPECT_TRUE(results[index].status == StoreStatus::kConflict ||
                    results[index].status == StoreStatus::kIoError);
        EXPECT_EQ(operation.status, StoreStatus::kMissing);
      }
    }
  }
  EXPECT_GE(total_committed, 1u);
}

TEST(AccessOptionsProfileStoreTest, VersionTwoMigrationKeepsExactRecoverySource) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
  ASSERT_TRUE(CreateVersionOneOptions(folder));
  {
    sql::Database legacy("AegisAccess");
    ASSERT_TRUE(legacy.Open(folder.AppendASCII("options.db")));
    ASSERT_TRUE(legacy.Execute("UPDATE identity SET version=2"));
  }
  std::string original;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.db"), &original));
  {
    AccessOptionsStore interrupted(InternalIdentity(), dir.GetPath(), "profile", false);
    interrupted.SetInterruptMigrationForTesting(true);
    EXPECT_EQ(interrupted.Open(), StoreStatus::kRecoveryRequired);
  }
  std::string primary;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.db"), &primary));
  EXPECT_EQ(primary, original);
  std::string marker;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("migration.pending"), &marker));
  EXPECT_EQ(marker, "2->3");
  AccessOptionsStore resumed(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(resumed.Open(), StoreStatus::kValid);
  EXPECT_EQ(resumed.Read().value->schema_version, 3);
  EXPECT_EQ(resumed.Read().value->revision, 7u);
  EXPECT_EQ(resumed.SaveTestProfile(7, NewOperation(), SyntheticDraft()).status, StoreStatus::kValid);
  sql::Database backup("AegisAccess");
  ASSERT_TRUE(backup.Open(folder.AppendASCII("options.v2.db")));
  sql::Statement check(backup.GetUniqueStatement(
      "SELECT i.version,c.revision,c.payload FROM identity i,configuration c"));
  ASSERT_TRUE(check.Step());
  EXPECT_EQ(check.ColumnInt(0), 2);
  EXPECT_EQ(check.ColumnInt64(1), 7);
  EXPECT_EQ(check.ColumnString(2), "{}");
  EXPECT_FALSE(base::PathExists(folder.AppendASCII("migration.pending")));
}

TEST(AccessOptionsProfileStoreTest, CompletedOldMigrationRequiresMatchingBackupBeforeUpgrade) {
  for (const bool matching : {false, true}) {
    base::ScopedTempDir dir;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
    ASSERT_TRUE(CreateVersionOneOptions(folder));
    ASSERT_TRUE(base::CopyFile(folder.AppendASCII("options.db"), folder.AppendASCII("options.v1.db")));
    {
      sql::Database current("AegisAccess");
      ASSERT_TRUE(current.Open(folder.AppendASCII("options.db")));
      ASSERT_TRUE(current.Execute("UPDATE identity SET version=2"));
      if (!matching) ASSERT_TRUE(current.Execute("UPDATE configuration SET revision=8"));
    }
    ASSERT_TRUE(base::WriteFile(folder.AppendASCII("migration.pending"), "1->2"));
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    EXPECT_EQ(store.Open(), matching ? StoreStatus::kValid : StoreStatus::kRecoveryRequired);
    EXPECT_TRUE(base::PathExists(folder.AppendASCII("options.v1.db")));
    if (!matching) {
      EXPECT_TRUE(base::PathExists(folder.AppendASCII("migration.pending")));
      EXPECT_FALSE(base::PathExists(folder.AppendASCII("options.v2.db")));
    } else {
      EXPECT_TRUE(base::PathExists(folder.AppendASCII("options.v2.db")));
      EXPECT_EQ(store.Read().value->revision, 7u);
    }
  }
}

TEST(AccessOptionsProfileStoreTest, UnknownMigrationAndLegacyReservedDataFailClosed) {
  for (const bool reserved_data : {false, true}) {
    base::ScopedTempDir dir;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
    ASSERT_TRUE(CreateVersionOneOptions(folder));
    if (reserved_data) {
      sql::Database legacy("AegisAccess");
      ASSERT_TRUE(legacy.Open(folder.AppendASCII("options.db")));
      ASSERT_TRUE(legacy.Execute("UPDATE configuration SET payload='{\"internalTestProfile\":{}}'"));
    } else {
      ASSERT_TRUE(base::WriteFile(folder.AppendASCII("migration.pending"), "2->999"));
    }
    std::string before;
    ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.db"), &before));
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    EXPECT_EQ(store.Open(), reserved_data ? StoreStatus::kIncompatible : StoreStatus::kRecoveryRequired);
    std::string after;
    ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.db"), &after));
    EXPECT_EQ(before, after);
    EXPECT_FALSE(store.Read().value);
  }
}

TEST(AccessOptionsProfileStoreTest, SnapshotCannotDisagreeWithItsRetainedCommitReceipt) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto operation = NewOperation();
  base::FilePath path;
  std::string corrupted;
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    ASSERT_EQ(store.SaveTestProfile(0, operation, SyntheticDraft()).status, StoreStatus::kValid);
    auto root = base::JSONReader::Read(store.Read().value->configuration, base::JSON_PARSE_RFC);
    root->GetDict().FindDict("internalTestProfile")->Set("secretReference", NewOperation());
    ASSERT_TRUE(base::JSONWriter::Write(*root, &corrupted));
    path = store.database_path();
  }
  {
    sql::Database fault("AegisAccess");
    ASSERT_TRUE(fault.Open(path));
    sql::Statement update(fault.GetUniqueStatement("UPDATE configuration SET payload=?"));
    update.BindString(0, corrupted);
    ASSERT_TRUE(update.Run());
  }
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
  EXPECT_EQ(reopened.Read().status, StoreStatus::kCorrupt);
  EXPECT_EQ(reopened.GetTestProfileOperation(operation).status, StoreStatus::kCorrupt);
  EXPECT_EQ(reopened.SaveTestProfile(1, NewOperation(), SyntheticDraft()).status, StoreStatus::kCorrupt);
}

TEST(AccessOptionsProfileStoreTest, ReferenceRecoveryRetainsAllValidatedDurableReferences) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto first = NewOperation();
  auto draft = SyntheticDraft();
  const auto first_reference = draft.secret_reference;
  base::FilePath path;
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    ASSERT_EQ(store.ReadTestProfileSecretReferences().status, StoreStatus::kValid);
    EXPECT_TRUE(store.ReadTestProfileSecretReferences().value->empty());
    ASSERT_EQ(store.SaveTestProfile(0, first, draft).status, StoreStatus::kValid);
    draft.secret_reference = NewOperation();
    ASSERT_EQ(store.SaveTestProfile(1, NewOperation(), draft).status, StoreStatus::kValid);
    const auto references = store.ReadTestProfileSecretReferences();
    ASSERT_EQ(references.status, StoreStatus::kValid);
    EXPECT_EQ(*references.value, (std::set<std::string>{first_reference, draft.secret_reference}));
    path = store.database_path();
  }
  {
    sql::Database fault("AegisAccess");
    ASSERT_TRUE(fault.Open(path));
    sql::Statement corrupt(fault.GetUniqueStatement(
        "UPDATE options_operations SET payload_fingerprint='corrupt' WHERE operation_id=?"));
    corrupt.BindString(0, first);
    ASSERT_TRUE(corrupt.Run());
  }
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(reopened.Open(), StoreStatus::kValid);
  EXPECT_EQ(reopened.Read().status, StoreStatus::kValid);
  const auto untrusted_inventory = reopened.ReadTestProfileSecretReferences();
  EXPECT_EQ(untrusted_inventory.status, StoreStatus::kCorrupt);
  EXPECT_FALSE(untrusted_inventory.value);
}

TEST(AccessOptionsProfileStoreTest, RealCommitContentionRemainsUncertainUntilQuery) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto store = AccessOptionsStore::CreateWithSharedLockingForTesting(
      InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(store->Open(), StoreStatus::kValid);
  sql::Database reader(sql::DatabaseOptions().set_exclusive_locking(false), "AegisAccess");
  ASSERT_TRUE(reader.Open(store->database_path()));
  ASSERT_TRUE(reader.Execute("BEGIN"));
  const auto operation = NewOperation();
  {
    sql::Statement locked(reader.GetUniqueStatement("SELECT revision FROM configuration"));
    ASSERT_TRUE(locked.Step());
    const auto failed = store->SaveTestProfile(0, operation, SyntheticDraft());
    EXPECT_EQ(failed.status, StoreStatus::kIoError);
    EXPECT_EQ(failed.outcome, AccessOptionsCommitOutcome::kUnknown);
  }
  ASSERT_TRUE(reader.Execute("ROLLBACK"));
  const auto queried = store->GetTestProfileOperation(operation);
  EXPECT_EQ(queried.status, StoreStatus::kMissing);
  EXPECT_EQ(queried.outcome, AccessOptionsCommitOutcome::kUnknown);
  ASSERT_TRUE(queried.snapshot);
  EXPECT_EQ(queried.snapshot->revision, 0u);
  EXPECT_FALSE(queried.snapshot->test_profile);
  EXPECT_TRUE(store->ReadTestProfileSecretReferences().value->empty());
}

TEST(AccessOptionsProfileStoreTest, CancellationBeforeCommitRollsBackBothRows) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
  ASSERT_EQ(store.Open(), StoreStatus::kValid);
  const auto operation = NewOperation();
  auto fence = std::make_shared<AccessOptionsCommitFence>();
  store.SetBeforeCommitForTesting(base::BindLambdaForTesting([&] {
    // Both UPDATE and journal INSERT have executed, but neither is durable.
    EXPECT_TRUE(fence->Cancel());
  }));
  const auto canceled = store.SaveTestProfile(0, operation, SyntheticDraft(), fence);
  EXPECT_EQ(canceled.status, StoreStatus::kConflict);
  EXPECT_EQ(canceled.outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_TRUE(fence->canceled());
  EXPECT_EQ(store.Read().value->revision, 0u);
  EXPECT_EQ(store.GetTestProfileOperation(operation).status, StoreStatus::kMissing);
  EXPECT_TRUE(store.ReadTestProfileSecretReferences().value->empty());
  store.SetBeforeCommitForTesting({});
  EXPECT_EQ(store.SaveTestProfile(0, operation, SyntheticDraft(), fence).outcome,
            AccessOptionsCommitOutcome::kNotCommitted);
  auto new_fence = std::make_shared<AccessOptionsCommitFence>();
  store.SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting::kAfterCommit);
  const auto lost = store.SaveTestProfile(0, operation, SyntheticDraft(), new_fence);
  EXPECT_EQ(lost.outcome, AccessOptionsCommitOutcome::kUnknown);
  EXPECT_FALSE(new_fence->Cancel());
  EXPECT_FALSE(new_fence->canceled());
  EXPECT_EQ(store.GetTestProfileOperation(operation).outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(store.Read().value->revision, 1u);
}

TEST(AccessOptionsProfileStoreTest, MissingAdmittedPrimaryCannotSupplyEmptyRecoveryAuthority) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  base::FilePath path;
  {
    AccessOptionsStore store(InternalIdentity(), dir.GetPath(), "profile", false);
    ASSERT_EQ(store.Open(), StoreStatus::kValid);
    ASSERT_EQ(store.SaveTestProfile(0, NewOperation(), SyntheticDraft()).status, StoreStatus::kValid);
    path = store.database_path();
  }
  const auto marker = path.DirName().AppendASCII("options.owner");
  std::string before;
  ASSERT_TRUE(base::ReadFileToString(marker, &before));
  ASSERT_TRUE(base::DeleteFile(path));
  AccessOptionsStore reopened(InternalIdentity(), dir.GetPath(), "profile", false);
  EXPECT_EQ(reopened.Open(), StoreStatus::kRecoveryRequired);
  EXPECT_FALSE(reopened.ReadTestProfileSecretReferences().value);
  EXPECT_FALSE(base::PathExists(path));
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(marker, &after));
  EXPECT_EQ(before, after);
}
TEST(AccessTestProfileCodecTest, PreservesExactVersionsAndOnlySafeReferenceFields) {
  const AccessTestProfileDraft draft{
      "550e8400-e29b-41d4-a716-446655440000", "测试 Profile",
      "550e8400-e29b-41d4-a716-446655440001", {}};
  const AccessStoredTestProfile stored{
      draft, 9007199254740993ULL, 9223372036854775807ULL,
      "550e8400-e29b-41d4-a716-446655440002"};
  const auto encoded = EncodeAccessStoredTestProfile(stored);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*encoded->FindString("version"), "9007199254740993");
  EXPECT_EQ(*encoded->FindString("committedRevision"), "9223372036854775807");
  EXPECT_EQ(DecodeAccessStoredTestProfile(*encoded), stored);
  EXPECT_FALSE(encoded->contains("address"));
  EXPECT_FALSE(encoded->contains("actualPort"));
  EXPECT_FALSE(encoded->contains("password"));
  EXPECT_EQ(ParseAccessOptionsRevision("0"), 0u);
  EXPECT_EQ(ParseAccessOptionsRevision("9223372036854775807"), 9223372036854775807ULL);
  for (auto invalid : {"", "00", "01", "-1", "+1", " 1", "1e3", "1.0",
                       "9223372036854775808", "18446744073709551615"}) {
    EXPECT_FALSE(ParseAccessOptionsRevision(invalid));
  }
}

TEST(AccessTestProfileCodecTest, RejectsUnknownFieldsTypesAndForbiddenPreferences) {
  AccessTestProfileDraft draft{
      "550e8400-e29b-41d4-a716-446655440000", "TEST01",
      "550e8400-e29b-41d4-a716-446655440001", {}};
  auto encoded = EncodeAccessTestProfileDraft(draft);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(DecodeAccessTestProfileDraft(*encoded), draft);
  encoded->Set("address", "198.51.100.10");
  EXPECT_FALSE(DecodeAccessTestProfileDraft(*encoded));
  draft.name = "vless://synthetic";
  EXPECT_FALSE(EncodeAccessTestProfileDraft(draft));
  draft.name = "TEST01";
  draft.inbound = {aegis_access::AccessInboundProtocolPreference::kSocks5,
                   aegis_access::AccessInboundPortMode::kInternalFixed, 1080};
  EXPECT_FALSE(EncodeAccessTestProfileDraft(draft));
  draft.inbound.protocol = aegis_access::AccessInboundProtocolPreference::kHttp;
  encoded = EncodeAccessTestProfileDraft(draft);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(DecodeAccessTestProfileDraft(*encoded), draft);
  encoded->FindDict("inbound")->Set("requestedPort", 1080.0);
  EXPECT_FALSE(DecodeAccessTestProfileDraft(*encoded));
}

TEST(AccessOptionsStoreTest, IgnoredConditionalUpdateCannotReportSuccess) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  auto owned = AccessOptionsStore::CreateWithSharedLockingForTesting(
      identity, dir.GetPath(), "profile", false, /*enable_triggers_for_testing=*/true);
  auto& store = *owned;
  ASSERT_EQ(store.Open(), StoreStatus::kValid);
  {
    sql::Database injector("AegisAccess");
    ASSERT_TRUE(injector.Open(store.database_path()));
    // A genuine SQLite statement can complete successfully with zero changes.
    // This is fault injection, independent of the concurrent-writer test.
    ASSERT_TRUE(injector.Execute(
        "CREATE TRIGGER ignore_write BEFORE UPDATE ON configuration "
        "BEGIN SELECT RAISE(IGNORE); END"));
  }
  EXPECT_EQ(store.Write(0, "{\"test\":\"new\"}"), StoreStatus::kConflict);
  const auto read = store.Read();
  ASSERT_EQ(read.status, StoreStatus::kValid);
  ASSERT_TRUE(read.value);
  EXPECT_EQ(read.value->revision, 0u);
  EXPECT_EQ(read.value->configuration, "{}");
}

TEST(AccessOptionsStoreTest, IndependentConnectionsNeverReportAnUncommittedWrite) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  auto owned = AccessOptionsStore::CreateWithSharedLockingForTesting(
      identity, dir.GetPath(), "profile", false);
  auto& observer = *owned;
  ASSERT_EQ(observer.Open(), StoreStatus::kValid);
  for (size_t round = 0; round < 8; ++round) {
    const auto before = observer.Read();
    ASSERT_EQ(before.status, StoreStatus::kValid);
    ASSERT_TRUE(before.value);
    const auto revision = before.value->revision;
    std::array<StoreStatus, 2> opened, results;
    const std::array<std::string, 2> payloads = {
        "{\"test\":\"writer-a-" + std::to_string(round) + "\"}",
        "{\"test\":\"writer-b-" + std::to_string(round) + "\"}"};
    base::WaitableEvent ready_a, ready_b, start;
    base::Thread a("OptionsWriterA"), b("OptionsWriterB");
    ASSERT_TRUE(a.Start());
    ASSERT_TRUE(b.Start());
    auto contender = [&](size_t i) {
      // Separate connections and sequences; no shared in-process store actor.
      auto owned_connection = AccessOptionsStore::CreateWithSharedLockingForTesting(
          identity, dir.GetPath(), "profile", false);
      auto& connection = *owned_connection;
      opened[i] = connection.Open();
      (i == 0 ? ready_a : ready_b).Signal();
      const bool released = start.TimedWait(base::Seconds(10));
      results[i] = !released ? StoreStatus::kIoError
          : opened[i] == StoreStatus::kValid
              ? connection.Write(revision, payloads[i]) : opened[i];
    };
    a.task_runner()->PostTask(FROM_HERE, base::BindLambdaForTesting([&] { contender(0); }));
    b.task_runner()->PostTask(FROM_HERE, base::BindLambdaForTesting([&] { contender(1); }));
    const bool a_ready = ready_a.TimedWait(base::Seconds(10));
    const bool b_ready = ready_b.TimedWait(base::Seconds(10));
    start.Signal();
    a.Stop();
    b.Stop();
    ASSERT_TRUE(a_ready);
    ASSERT_TRUE(b_ready);
    ASSERT_EQ(opened[0], StoreStatus::kValid);
    ASSERT_EQ(opened[1], StoreStatus::kValid);
    size_t accepted = 0;
    for (const auto result : results) {
      EXPECT_TRUE(result == StoreStatus::kValid || result == StoreStatus::kConflict ||
                  result == StoreStatus::kIoError);
      accepted += result == StoreStatus::kValid;
    }
    ASSERT_LE(accepted, 1u);
    const auto after = observer.Read();
    ASSERT_EQ(after.status, StoreStatus::kValid);
    ASSERT_TRUE(after.value);
    EXPECT_EQ(after.value->revision, revision + accepted);
    if (accepted == 0) {
      EXPECT_EQ(after.value->configuration, before.value->configuration);
    } else {
      EXPECT_EQ(after.value->configuration,
                payloads[results[0] == StoreStatus::kValid ? 0 : 1]);
    }
  }
}

TEST(AccessOptionsStoreTest, WriteLockAndFailedCommitCannotReportSuccess) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  auto owned = AccessOptionsStore::CreateWithSharedLockingForTesting(
      identity, dir.GetPath(), "profile", false);
  auto& store = *owned;
  ASSERT_EQ(store.Open(), StoreStatus::kValid);
  sql::Database other(sql::DatabaseOptions().set_exclusive_locking(false), "AegisAccess");
  ASSERT_TRUE(other.Open(store.database_path()));
  ASSERT_TRUE(other.Execute("BEGIN IMMEDIATE"));
  EXPECT_EQ(store.Write(0, "{\"test\":\"locked\"}"), StoreStatus::kIoError);
  ASSERT_TRUE(other.Execute("ROLLBACK"));
  ASSERT_TRUE(other.Execute("BEGIN"));
  {
    sql::Statement reader(other.GetUniqueStatement("SELECT revision FROM configuration"));
    ASSERT_TRUE(reader.Step());
    // This reader permits the writer's UPDATE, but prevents its COMMIT.
    EXPECT_EQ(store.Write(0, "{\"test\":\"uncommitted\"}"), StoreStatus::kIoError);
  }
  ASSERT_TRUE(other.Execute("ROLLBACK"));
  const auto read = store.Read();
  ASSERT_EQ(read.status, StoreStatus::kValid);
  ASSERT_TRUE(read.value);
  EXPECT_EQ(read.value->revision, 0u);
  EXPECT_EQ(read.value->configuration, "{}");
}

TEST(AccessOptionsStoreTest, BetaCannotCreateConfiguration) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto beta = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kBeta,
      aegis_access::BrowserReleaseChannel::kUnknown,
      ChannelNamespace::kAlpha, true);
  AccessOptionsStore store(beta, dir.GetPath(), "profile", false);
  EXPECT_EQ(store.Open(), StoreStatus::kConflict);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
}
TEST(AccessOptionsStoreTest, InternalChannelsAndPrivateProfilesAreIsolated) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  for (auto product : {aegis_access::AccessProductChannel::kDebug,
                       aegis_access::AccessProductChannel::kAlpha}) {
    const auto identity = aegis_access::ResolveAccessBuildIdentity(product,
        aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
    AccessOptionsStore ordinary(identity, dir.GetPath(), "profile", false);
    ASSERT_EQ(ordinary.Open(), StoreStatus::kValid);
    ASSERT_EQ(ordinary.Write(0, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
    EXPECT_EQ(ordinary.Read().value->revision, 1u);
    AccessOptionsStore private_store(identity, dir.GetPath(), "private", true);
    ASSERT_EQ(private_store.Open(), StoreStatus::kValid);
    EXPECT_EQ(private_store.Read().value->revision, 0u);
    EXPECT_TRUE(private_store.database_path().empty());
    EXPECT_EQ(ordinary.Write(0, "{}"), StoreStatus::kConflict);
  }
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("access.db")));
}
TEST(AccessOptionsStoreTest, InterruptedMigrationPreservesOriginalAndRecovers) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
  ASSERT_TRUE(base::CreateDirectory(folder));
  {
    sql::Database legacy("AegisAccess");
    ASSERT_TRUE(legacy.Open(folder.AppendASCII("options.db")));
    ASSERT_TRUE(legacy.Execute("CREATE TABLE identity(singleton INTEGER PRIMARY KEY,version INTEGER,channel INTEGER,profile_id TEXT)"));
    ASSERT_TRUE(legacy.Execute("INSERT INTO identity VALUES(1,1,1,'profile')"));
    ASSERT_TRUE(legacy.Execute("CREATE TABLE configuration(singleton INTEGER PRIMARY KEY,revision INTEGER,payload TEXT)"));
    ASSERT_TRUE(legacy.Execute("INSERT INTO configuration VALUES(1,7,'{}')"));
  }
  {
    AccessOptionsStore interrupted(identity, dir.GetPath(), "profile", false);
    interrupted.SetInterruptMigrationForTesting(true);
    EXPECT_EQ(interrupted.Open(), StoreStatus::kRecoveryRequired);
    EXPECT_FALSE(interrupted.Read().value);
  }
  EXPECT_TRUE(base::PathExists(folder.AppendASCII("options.v1.db")));
  AccessOptionsStore resumed(identity, dir.GetPath(), "profile", false);
  ASSERT_EQ(resumed.Open(), StoreStatus::kValid);
  ASSERT_EQ(resumed.Read().value->revision, 7u);
  ASSERT_EQ(resumed.Write(7, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
  sql::Database source("AegisAccess");
  ASSERT_TRUE(source.Open(folder.AppendASCII("options.v1.db")));
  sql::Statement query(source.GetUniqueStatement("SELECT revision FROM configuration"));
  ASSERT_TRUE(query.Step());
  EXPECT_EQ(query.ColumnInt64(0), 7);
}
TEST(AccessOptionsStoreTest, WrongOwnerAndUnsupportedSchemaPreserveConfiguration) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  {
    AccessOptionsStore first(identity, dir.GetPath(), "profile", false);
    ASSERT_EQ(first.Open(), StoreStatus::kValid);
    EXPECT_EQ(first.Write(0, "[]"), StoreStatus::kInvalidArgument);
    EXPECT_EQ(first.Write(0, std::string(65537, 'a')), StoreStatus::kInvalidArgument);
    ASSERT_EQ(first.Write(0, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
  }
  const auto path = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha").AppendASCII("options.db");
  {
    AccessOptionsStore other(identity, dir.GetPath(), "other-profile", false);
    EXPECT_EQ(other.Open(), StoreStatus::kConflict);
  }
  {
    sql::Database future("AegisAccess");
    ASSERT_TRUE(future.Open(path));
    ASSERT_TRUE(future.Execute("UPDATE identity SET version=999"));
  }
  std::string before;
  ASSERT_TRUE(base::ReadFileToString(path, &before));
  AccessOptionsStore future(identity, dir.GetPath(), "profile", false);
  EXPECT_EQ(future.Open(), StoreStatus::kIncompatible);
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(path, &after));
  EXPECT_EQ(before, after);
}
TEST(AccessOptionsStoreTest, NewPrivateLifetimeHasNoPreviousConfiguration) {
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  {
    AccessOptionsStore previous(identity, {}, "private-session", true);
    ASSERT_EQ(previous.Open(), StoreStatus::kValid);
    ASSERT_EQ(previous.Write(0, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
    EXPECT_EQ(previous.Read().value->revision, 1u);
  }
  AccessOptionsStore next(identity, {}, "private-session", true);
  ASSERT_EQ(next.Open(), StoreStatus::kValid);
  EXPECT_EQ(next.Read().value->revision, 0u);
  EXPECT_EQ(next.Read().value->configuration, "{}");
}
TEST(AccessOptionsStoreTest, FailedDirectoryCreationReportsIoAndPreservesFile) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto path = dir.GetPath().AppendASCII("file");
  ASSERT_TRUE(base::WriteFile(path, "original"));
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  AccessOptionsStore store(identity, path, "profile", false);
  EXPECT_EQ(store.Open(), StoreStatus::kIoError);
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  EXPECT_EQ(bytes, "original");
}
TEST(AccessOptionsStoreTest, InterruptedBackupNeverPublishesPartialRecoverySource) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
  ASSERT_TRUE(CreateVersionOneOptions(folder));
  {
    AccessOptionsStore interrupted(identity, dir.GetPath(), "profile", false);
    interrupted.SetInterruptBackupForTesting(true);
    EXPECT_EQ(interrupted.Open(), StoreStatus::kRecoveryRequired);
  }
  ASSERT_FALSE(base::PathExists(folder.AppendASCII("options.v1.db")));
  EXPECT_TRUE(base::PathExists(folder.AppendASCII("options.backup-staging.db")));
  AccessOptionsStore resumed(identity, dir.GetPath(), "profile", false);
  ASSERT_EQ(resumed.Open(), StoreStatus::kValid);
  ASSERT_EQ(resumed.Read().value->revision, 7u);
  EXPECT_FALSE(base::PathExists(folder.AppendASCII("options.backup-staging.db")));
  sql::Database original("AegisAccess");
  ASSERT_TRUE(original.Open(folder.AppendASCII("options.v1.db")));
  sql::Statement source(original.GetUniqueStatement("SELECT revision FROM configuration"));
  ASSERT_TRUE(source.Step());
  EXPECT_EQ(source.ColumnInt64(0), 7);
}
TEST(AccessOptionsStoreTest, MissingPrimaryDuringMigrationCannotCreateEmptyConfiguration) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
  ASSERT_TRUE(CreateVersionOneOptions(folder));
  {
    AccessOptionsStore interrupted(identity, dir.GetPath(), "profile", false);
    interrupted.SetInterruptMigrationForTesting(true);
    ASSERT_EQ(interrupted.Open(), StoreStatus::kRecoveryRequired);
  }
  ASSERT_TRUE(base::DeleteFile(folder.AppendASCII("options.db")));
  std::string backup;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.v1.db"), &backup));
  AccessOptionsStore resumed(identity, dir.GetPath(), "profile", false);
  EXPECT_EQ(resumed.Open(), StoreStatus::kRecoveryRequired);
  EXPECT_FALSE(base::PathExists(folder.AppendASCII("options.db")));
  EXPECT_TRUE(base::PathExists(folder.AppendASCII("migration.pending")));
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("options.v1.db"), &after));
  EXPECT_EQ(backup, after);
}
}  // namespace aegis::access
