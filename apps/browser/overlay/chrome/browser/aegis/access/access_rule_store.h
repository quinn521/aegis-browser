// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_RULE_STORE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_RULE_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "components/aegis_access/site_proxy_rule_group.h"
#include "sql/database.h"
#include "sql/meta_table.h"

namespace aegis::access {

// A site value produced by a browser-owned navigation adapter. This class does
// not derive a registrable domain and deliberately cannot be constructed from
// the legacy SiteKeyForHost display helper.
struct BrowserConfirmedSite {
  std::string storage_partition_token;
  std::string canonical_host;
  std::string http_top_level_site;
  std::string https_top_level_site;

  bool IsComplete() const;

  friend bool operator==(const BrowserConfirmedSite&,
                         const BrowserConfirmedSite&) = default;
};

struct ExecutionContextBinding {
  std::string context_id;
  uint64_t incarnation = 0;

  friend bool operator==(const ExecutionContextBinding&,
                         const ExecutionContextBinding&) = default;
};

enum class SiteSelectionReadState {
  kMissing,
  kValid,
  kRestoring,
  kCorrupt,
  kStorageError,
};

struct SiteSelectionRecord {
  SiteSelectionReadState state = SiteSelectionReadState::kStorageError;
  aegis_access::GroupSelection selection =
      aegis_access::GroupSelection::kUnknown;
  aegis_access::GroupValidationError validation_error =
      aegis_access::GroupValidationError::kInvalidGroup;
  uint64_t revision = 0;
  uint64_t last_operation_sequence = 0;
  uint64_t policy_generation = 0;
  std::string proxy_group_id;
  bool has_independent_rule = false;
};

struct IndependentAccessRule {
  std::string rule_id;
  std::string storage_partition_token;
  std::string top_level_site;
  std::string exact_host;
  std::vector<aegis_access::RequestScheme> schemes;
  aegis_access::PortScope ports = aegis_access::PortScope::kInvalid;
  bool include_subdomains = false;
  aegis_access::AccessMode mode = aegis_access::AccessMode::kInvalid;
  std::string proxy_group_id;
  aegis_access::ProtectionOverride protection_override =
      aegis_access::ProtectionOverride::kInvalid;
  uint64_t row_revision = 0;
  uint64_t last_operation_sequence = 0;
};

struct SiteToggleRequest {
  std::string operation_id;
  std::string request_fingerprint;
  std::string site_toggle_id;
  BrowserConfirmedSite site;
  bool enabled = false;
  std::string proxy_group_id;
  uint64_t expected_revision = 0;
  uint64_t service_incarnation = 0;
  std::vector<ExecutionContextBinding> required_contexts;
};

struct PendingSiteOperation {
  SiteToggleRequest request;
  aegis_access::OwnershipKey owner;
  aegis_access::SiteProxyRuleGroup after_group;
  std::vector<aegis_access::SiteProxyRuleMember> after_members;
  uint64_t operation_sequence = 0;
  uint64_t policy_generation = 0;
  bool route_ready = false;
  bool publish_started = false;
  std::vector<ExecutionContextBinding> required_contexts;
  std::vector<std::string> acknowledged_context_ids;
};

enum class PrepareOperationStatus {
  kPrepared,
  kDuplicate,
  kDuplicateConflict,
  kRevisionConflict,
  kInvalidRequest,
  kCorruptState,
  kStorageError,
};

struct PrepareOperationResult {
  PrepareOperationStatus status = PrepareOperationStatus::kStorageError;
  std::optional<PendingSiteOperation> operation;
};

enum class CommitOperationStatus {
  kCommitted,
  kNotReady,
  kSuperseded,
  kStorageError,
};

class AccessRuleStore {
 public:
  // sql::Database is sequence-affine. Construct, initialize, and use this
  // store on one browser-owned sequence.
  enum class FailurePointForTesting {
    kNone,
    kPrepareOperation,
    kMarkRouteReady,
    kMarkPublishStarted,
    kMarkAcknowledged,
    kCommitOperation,
    kRebindRecovery,
  };

  AccessRuleStore(base::FilePath database_path,
                  aegis_access::OwnershipKey owner,
                  bool in_memory = false);
  AccessRuleStore(const AccessRuleStore&) = delete;
  AccessRuleStore& operator=(const AccessRuleStore&) = delete;
  ~AccessRuleStore();

  bool Initialize();
  bool is_initialized() const { return initialized_; }
  const aegis_access::OwnershipKey& owner() const { return owner_; }

  SiteSelectionRecord ReadSiteSelection(const BrowserConfirmedSite& site);
  PrepareOperationResult PrepareSiteToggleOperation(
      const SiteToggleRequest& request);
  std::optional<PendingSiteOperation> LoadPendingOperation(
      const std::string& operation_id);
  std::optional<PendingSiteOperation> LoadPendingOperationForSite(
      const BrowserConfirmedSite& site);
  std::vector<PendingSiteOperation> LoadPendingOperations();
  bool MarkRouteReady(const PendingSiteOperation& operation);
  bool MarkPublishStarted(const PendingSiteOperation& operation);
  bool MarkOperationFailed(const PendingSiteOperation& operation);
  bool MarkContextAcknowledged(const PendingSiteOperation& operation,
                               const ExecutionContextBinding& context);
  std::optional<PendingSiteOperation> RebindOperationForRecovery(
      const PendingSiteOperation& operation,
      uint64_t service_incarnation,
      const std::vector<ExecutionContextBinding>& required_contexts);
  CommitOperationStatus CommitOperation(const PendingSiteOperation& operation);

  bool last_pending_load_had_error() const {
    return last_pending_load_had_error_;
  }

  bool SaveIndependentRule(const IndependentAccessRule& rule);
  std::optional<IndependentAccessRule> LoadIndependentRule(
      const std::string& storage_partition_token,
      const std::string& rule_id);

  void SetFailurePointForTesting(FailurePointForTesting point) {
    failure_point_for_testing_ = point;
  }
  bool ExecuteSqlForTesting(const char* sql) { return database_.Execute(sql); }
  void CloseDatabaseForTesting() { database_.Close(); }

 private:
  enum class DurableOperationState {
    kActive = 0,
    kSuperseded = 1,
    kCommitted = 2,
    kFailed = 3,
  };

  SiteSelectionRecord ReadDurableSiteSelection(
      const BrowserConfirmedSite& site);
  std::optional<PendingSiteOperation> LoadOperation(
      const std::string& operation_id,
      bool active_only);
  bool IsBoundOwnerValid() const;
  bool ShouldFail(FailurePointForTesting point) const;

  const base::FilePath database_path_;
  const aegis_access::OwnershipKey owner_;
  const bool in_memory_;
  sql::Database database_;
  sql::MetaTable meta_table_;
  bool initialized_ = false;
  bool last_pending_load_had_error_ = false;
  FailurePointForTesting failure_point_for_testing_ =
      FailurePointForTesting::kNone;
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_RULE_STORE_H_
