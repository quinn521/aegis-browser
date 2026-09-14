// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_SITE_PROXY_TOGGLE_COORDINATOR_H_
#define CHROME_BROWSER_AEGIS_ACCESS_SITE_PROXY_TOGGLE_COORDINATOR_H_

#include <cstdint>
#include <map>
#include <string>

#include "chrome/browser/aegis/access/access_rule_store.h"
#include "components/aegis_access/access_route_types.h"

namespace aegis::access {

struct OperationEventIdentity {
  std::string operation_id;
  std::string request_fingerprint;
  aegis_access::OwnershipKey owner;
  uint64_t expected_revision = 0;
  uint64_t policy_generation = 0;
  uint64_t operation_sequence = 0;
  uint64_t service_incarnation = 0;
};

struct RouteReadyEvent : OperationEventIdentity {};

struct PolicyAckEvent : OperationEventIdentity {
  std::string context_id;
  uint64_t context_incarnation = 0;
};

enum class ToggleStartStatus {
  kPendingRoute,
  kPendingPublish,
  kDuplicate,
  kRejectedChannel,
  kInvalidRequest,
  kRevisionConflict,
  kCorruptState,
  kSaveFailed,
};

struct ToggleStartResult {
  ToggleStartStatus status = ToggleStartStatus::kInvalidRequest;
  std::optional<PendingSiteOperation> operation;
};

enum class ToggleEventResult {
  kIgnored,
  kWaiting,
  kPublished,
  kCommitted,
  kFailed,
  kSaveFailed,
};

class RoutePreparer {
 public:
  virtual ~RoutePreparer() = default;
  virtual void PrepareRoute(const PendingSiteOperation& operation) = 0;
};

class PolicyPublisher {
 public:
  virtual ~PolicyPublisher() = default;
  virtual void PublishPolicy(const PendingSiteOperation& operation) = 0;
  virtual void CommitAndRelease(const PendingSiteOperation& operation) = 0;
  virtual void EnterFailClosed(const BrowserConfirmedSite& site,
                               uint64_t policy_generation,
                               const std::string& reason) = 0;
  virtual void EnterStoreFailClosed(
      const aegis_access::OwnershipKey& owner,
      const std::string& reason) = 0;
};

struct SiteToggleState {
  SiteSelectionRecord committed;
  aegis_access::ProxyRuntimeState runtime_state =
      aegis_access::ProxyRuntimeState::kStopped;
};

// Coordinates only the trusted Beta/Release website toggle. It intentionally
// exposes no general debug rule-mutation entry point and performs no real proxy
// or Network Service integration.
class SiteProxyToggleCoordinator {
 public:
  SiteProxyToggleCoordinator(AccessRuleStore* store,
                             RoutePreparer* route_preparer,
                             PolicyPublisher* policy_publisher);
  SiteProxyToggleCoordinator(const SiteProxyToggleCoordinator&) = delete;
  SiteProxyToggleCoordinator& operator=(const SiteProxyToggleCoordinator&) =
      delete;
  ~SiteProxyToggleCoordinator();

  ToggleStartResult SetSiteProxy(const SiteToggleRequest& request);
  ToggleEventResult OnRouteReady(const RouteReadyEvent& event);
  ToggleEventResult OnRouteFailed(const RouteReadyEvent& event);
  ToggleEventResult OnPolicyAcknowledged(const PolicyAckEvent& event);
  void RecoverPendingOperations(
      uint64_t service_incarnation,
      const std::vector<ExecutionContextBinding>& required_contexts);

  SiteToggleState GetSiteToggleState(const BrowserConfirmedSite& site);
  void SetRuntimeState(const BrowserConfirmedSite& site,
                       aegis_access::ProxyRuntimeState state);

 private:
  static std::string RuntimeKey(const BrowserConfirmedSite& site);
  static bool EventMatches(const PendingSiteOperation& operation,
                           const OperationEventIdentity& event);
  ToggleEventResult TryPublishNextReady();

  AccessRuleStore* const store_;
  RoutePreparer* const route_preparer_;
  PolicyPublisher* const policy_publisher_;
  std::map<std::string, aegis_access::ProxyRuntimeState> runtime_states_;
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_SITE_PROXY_TOGGLE_COORDINATOR_H_
