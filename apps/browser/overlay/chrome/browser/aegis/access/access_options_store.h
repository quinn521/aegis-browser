// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_STORE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_STORE_H_

#include <memory>
#include <set>
#include <atomic>

#include "base/time/time.h"
#include "base/functional/callback.h"
#include "chrome/browser/aegis/access/access_rule_store.h"
#include "chrome/browser/aegis/access/access_test_profile_config.h"
#include "components/aegis_access/access_options_capability.h"

namespace aegis::access {
inline constexpr int kAccessOptionsSchemaVersion = 3;
struct AccessOptionsSnapshot {
  uint64_t revision = 0;
  std::string configuration = "{}";
  int schema_version = kAccessOptionsSchemaVersion;
  std::optional<AccessStoredTestProfile> test_profile;
};

enum class AccessOptionsCommitOutcome { kNotCommitted, kCommitted, kUnknown };
// UI cancellation and the blocking actor race at one native commit boundary.
// Once COMMIT has begun, cancellation cannot label durable work as discarded.
class AccessOptionsCommitFence {
 public:
  bool Cancel();
  bool canceled() const;
 private:
  friend class AccessOptionsStore;
  bool TryBeginCommit();
  enum class State { kPending, kCanceled, kCommitting };
  std::atomic<State> state_{State::kPending};
};
struct AccessOptionsOperationReceipt {
  std::string operation_id;
  uint64_t expected_revision = 0;
  uint64_t committed_revision = 0;
  uint64_t profile_version = 0;
  std::string payload_fingerprint;
  bool recovered_from_snapshot = false;
  // Opaque native draft reference for this operation, even after a later save.
  std::string secret_reference;
};
struct AccessTestProfileSaveResult {
  StoreStatus status = StoreStatus::kIoError;
  AccessOptionsCommitOutcome outcome = AccessOptionsCommitOutcome::kUnknown;
  std::optional<AccessOptionsOperationReceipt> operation;
  std::optional<AccessOptionsSnapshot> snapshot;
  std::string detail;
};

// Sequence-bound store. Its owner must schedule disk work off the UI thread.
// Private instances own their own in-memory database and never open disk paths.
class AccessOptionsStore {
 public:
  enum class SaveFailurePointForTesting {
    kNone, kAfterConfigurationUpdate, kAfterJournalInsert, kAfterCommit,
  };
  AccessOptionsStore(aegis_access::AccessBuildIdentity identity,
                     base::FilePath profile_path,
                     std::string profile_id,
                     bool is_private);
  ~AccessOptionsStore();
  // Production retains Chromium's exclusive locking. Tests may opt into real
  // independent connections to exercise CAS and SQLite lock/commit failures.
  static std::unique_ptr<AccessOptionsStore> CreateWithSharedLockingForTesting(
      aegis_access::AccessBuildIdentity identity,
      base::FilePath profile_path,
      std::string profile_id,
      bool is_private,
      bool enable_triggers_for_testing = false);
  StoreStatus Open();
  void SetInterruptBackupForTesting(bool value) { interrupt_backup_ = value; }
  void SetInterruptMigrationForTesting(bool value) { interrupt_migration_ = value; }
  void SetNowForTesting(base::Time now) { now_for_testing_ = now; }
  void SetSaveFailureForTesting(SaveFailurePointForTesting point) {
    save_failure_for_testing_ = point;
  }
  void SetBeforeCommitForTesting(base::RepeatingClosure callback) {
    before_commit_for_testing_ = std::move(callback);
  }
  StoreResult<AccessOptionsSnapshot> Read();
  StoreResult<AccessOptionsSnapshot> OpenAndRead();
  StoreStatus Write(uint64_t expected_revision, const std::string& configuration);
  // Saves validated intent only. This never publishes a route or claims that a
  // core is active. The native owner must hold its scoped Keychain draft until
  // a committed/not-committed result is established, and fence activation.
  AccessTestProfileSaveResult SaveTestProfile(
      uint64_t expected_revision, const std::string& operation_id,
      const AccessTestProfileDraft& draft,
      std::shared_ptr<AccessOptionsCommitFence> commit_fence = nullptr);
  // A missing/expired record is uncertain; callers must read current versions,
  // never generate a fresh operation ID to replay an uncertain business action.
  AccessTestProfileSaveResult GetTestProfileOperation(
      const std::string& operation_id);
  // All retained durable references, including queryable historical commits.
  // An error supplies no cleanup authority; callers preserve the entire vault.
  // The native owner serializes reconciliation with its commit resolutions.
  StoreResult<std::set<std::string>> ReadTestProfileSecretReferences();
  const base::FilePath& database_path() const { return database_path_; }

 private:
  AccessOptionsStore(aegis_access::AccessBuildIdentity identity,
                     base::FilePath profile_path,
                     std::string profile_id,
                     bool is_private,
                     sql::DatabaseOptions options);
  StoreStatus ValidateIdentity();
  StoreStatus MigrateLegacySchema(int source_version);
  StoreStatus ValidateMigrationMarker();
  StoreStatus ValidateJournal();
  bool Admitted(aegis_access::AccessOptionsOperation operation) const;
  aegis_access::AccessBuildIdentity identity_;
  base::FilePath database_path_;
  std::string profile_id_;
  bool private_;
  bool open_ = false;
  bool interrupt_migration_ = false;
  bool interrupt_backup_ = false;
  std::optional<base::Time> now_for_testing_;
  SaveFailurePointForTesting save_failure_for_testing_ = SaveFailurePointForTesting::kNone;
  base::RepeatingClosure before_commit_for_testing_;
  sql::Database database_;
  SEQUENCE_CHECKER(sequence_checker_);
};
}  // namespace aegis::access
#endif
