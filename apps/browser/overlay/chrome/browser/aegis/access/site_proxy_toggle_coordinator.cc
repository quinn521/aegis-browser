// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/site_proxy_toggle_coordinator.h"

#include <algorithm>

namespace aegis::access {
namespace {

bool IsProductToggleChannel(aegis_access::ChannelNamespace channel) {
  return channel == aegis_access::ChannelNamespace::kBeta ||
         channel == aegis_access::ChannelNamespace::kRelease;
}

ToggleStartStatus MapPrepareFailure(PrepareOperationStatus status) {
  switch (status) {
    case PrepareOperationStatus::kDuplicate:
      return ToggleStartStatus::kDuplicate;
    case PrepareOperationStatus::kDuplicateConflict:
    case PrepareOperationStatus::kInvalidRequest:
      return ToggleStartStatus::kInvalidRequest;
    case PrepareOperationStatus::kRevisionConflict:
      return ToggleStartStatus::kRevisionConflict;
    case PrepareOperationStatus::kCorruptState:
      return ToggleStartStatus::kCorruptState;
    case PrepareOperationStatus::kStorageError:
      return ToggleStartStatus::kSaveFailed;
    case PrepareOperationStatus::kPrepared:
      break;
  }
  return ToggleStartStatus::kInvalidRequest;
}

}  // namespace

SiteProxyToggleCoordinator::SiteProxyToggleCoordinator(
    AccessRuleStore* store,
    RoutePreparer* route_preparer,
    PolicyPublisher* policy_publisher)
    : store_(store),
      route_preparer_(route_preparer),
      policy_publisher_(policy_publisher) {}

SiteProxyToggleCoordinator::~SiteProxyToggleCoordinator() = default;

ToggleStartResult SiteProxyToggleCoordinator::SetSiteProxy(
    const SiteToggleRequest& request) {
  if (!store_ || !route_preparer_ || !policy_publisher_ ||
      !IsProductToggleChannel(store_->owner().channel)) {
    return {ToggleStartStatus::kRejectedChannel, std::nullopt};
  }
  std::optional<PendingSiteOperation> previous =
      store_->LoadPendingOperationForSite(request.site);
  if (store_->last_pending_load_had_error()) {
    policy_publisher_->EnterStoreFailClosed(store_->owner(),
                                            "pending_journal_corrupt");
    return {ToggleStartStatus::kCorruptState, std::nullopt};
  }
  PrepareOperationResult prepared = store_->PrepareSiteToggleOperation(request);
  if (prepared.status != PrepareOperationStatus::kPrepared) {
    return {MapPrepareFailure(prepared.status), std::move(prepared.operation)};
  }

  if (previous && previous->publish_started) {
    policy_publisher_->EnterFailClosed(
        previous->request.site, previous->policy_generation,
        "published_operation_superseded");
  }

  if (prepared.operation->request.enabled) {
    route_preparer_->PrepareRoute(*prepared.operation);
    return {ToggleStartStatus::kPendingRoute, std::move(prepared.operation)};
  }
  if (TryPublishNextReady() == ToggleEventResult::kSaveFailed) {
    return {ToggleStartStatus::kSaveFailed, std::move(prepared.operation)};
  }
  return {ToggleStartStatus::kPendingPublish, std::move(prepared.operation)};
}

bool SiteProxyToggleCoordinator::EventMatches(
    const PendingSiteOperation& operation,
    const OperationEventIdentity& event) {
  return event.operation_id == operation.request.operation_id &&
         event.request_fingerprint == operation.request.request_fingerprint &&
         event.owner == operation.owner &&
         event.expected_revision == operation.request.expected_revision &&
         event.policy_generation == operation.policy_generation &&
         event.operation_sequence == operation.operation_sequence &&
         event.service_incarnation == operation.request.service_incarnation;
}

ToggleEventResult SiteProxyToggleCoordinator::OnRouteReady(
    const RouteReadyEvent& event) {
  if (!store_ || !policy_publisher_) {
    return ToggleEventResult::kIgnored;
  }
  std::optional<PendingSiteOperation> operation =
      store_->LoadPendingOperation(event.operation_id);
  if (!operation || !operation->request.enabled ||
      !EventMatches(*operation, event)) {
    return ToggleEventResult::kIgnored;
  }
  if (!store_->MarkRouteReady(*operation)) {
    // The after-image has not been published, so the old committed selection
    // remains authoritative and no fail-closed transition is necessary.
    return ToggleEventResult::kSaveFailed;
  }
  return TryPublishNextReady();
}

ToggleEventResult SiteProxyToggleCoordinator::OnRouteFailed(
    const RouteReadyEvent& event) {
  if (!store_ || !policy_publisher_) {
    return ToggleEventResult::kIgnored;
  }
  std::optional<PendingSiteOperation> operation =
      store_->LoadPendingOperation(event.operation_id);
  if (!operation || !operation->request.enabled ||
      operation->publish_started || !EventMatches(*operation, event)) {
    return ToggleEventResult::kIgnored;
  }
  if (!store_->MarkOperationFailed(*operation)) {
    return ToggleEventResult::kSaveFailed;
  }
  TryPublishNextReady();
  return ToggleEventResult::kFailed;
}

ToggleEventResult SiteProxyToggleCoordinator::TryPublishNextReady() {
  std::vector<PendingSiteOperation> pending = store_->LoadPendingOperations();
  if (store_->last_pending_load_had_error()) {
    policy_publisher_->EnterStoreFailClosed(store_->owner(),
                                            "pending_journal_corrupt");
    return ToggleEventResult::kSaveFailed;
  }
  if (pending.empty() || pending.front().publish_started ||
      (pending.front().request.enabled && !pending.front().route_ready)) {
    return ToggleEventResult::kWaiting;
  }
  PendingSiteOperation operation = std::move(pending.front());
  if (!store_->MarkPublishStarted(operation)) {
    return ToggleEventResult::kSaveFailed;
  }
  operation.publish_started = true;
  policy_publisher_->PublishPolicy(operation);
  return ToggleEventResult::kPublished;
}

ToggleEventResult SiteProxyToggleCoordinator::OnPolicyAcknowledged(
    const PolicyAckEvent& event) {
  if (!store_ || !policy_publisher_) {
    return ToggleEventResult::kIgnored;
  }
  std::optional<PendingSiteOperation> operation =
      store_->LoadPendingOperation(event.operation_id);
  if (!operation || !operation->publish_started ||
      !EventMatches(*operation, event)) {
    return ToggleEventResult::kIgnored;
  }
  const auto context = std::ranges::find_if(
      operation->required_contexts, [&](const ExecutionContextBinding& item) {
        return item.context_id == event.context_id &&
               item.incarnation == event.context_incarnation;
      });
  if (context == operation->required_contexts.end()) {
    return ToggleEventResult::kIgnored;
  }
  if (std::ranges::find(operation->acknowledged_context_ids,
                        context->context_id) !=
      operation->acknowledged_context_ids.end()) {
    return ToggleEventResult::kWaiting;
  }
  if (!store_->MarkContextAcknowledged(*operation, *context)) {
    policy_publisher_->EnterFailClosed(operation->request.site,
                                       operation->policy_generation,
                                       "ack_save_failed");
    return ToggleEventResult::kSaveFailed;
  }

  std::optional<PendingSiteOperation> refreshed =
      store_->LoadPendingOperation(event.operation_id);
  if (!refreshed) {
    return ToggleEventResult::kIgnored;
  }
  if (refreshed->acknowledged_context_ids.size() !=
      refreshed->required_contexts.size()) {
    return ToggleEventResult::kWaiting;
  }

  switch (store_->CommitOperation(*refreshed)) {
    case CommitOperationStatus::kCommitted:
      policy_publisher_->CommitAndRelease(*refreshed);
      TryPublishNextReady();
      return ToggleEventResult::kCommitted;
    case CommitOperationStatus::kNotReady:
      return ToggleEventResult::kWaiting;
    case CommitOperationStatus::kSuperseded:
      return ToggleEventResult::kIgnored;
    case CommitOperationStatus::kStorageError:
      policy_publisher_->EnterFailClosed(refreshed->request.site,
                                         refreshed->policy_generation,
                                         "commit_save_failed");
      return ToggleEventResult::kSaveFailed;
  }
  return ToggleEventResult::kIgnored;
}

void SiteProxyToggleCoordinator::RecoverPendingOperations(
    uint64_t service_incarnation,
    const std::vector<ExecutionContextBinding>& required_contexts) {
  if (!store_ || !route_preparer_ || !policy_publisher_) {
    return;
  }
  std::vector<PendingSiteOperation> pending = store_->LoadPendingOperations();
  if (store_->last_pending_load_had_error()) {
    policy_publisher_->EnterStoreFailClosed(store_->owner(),
                                            "pending_journal_corrupt");
    return;
  }
  for (const PendingSiteOperation& operation : pending) {
    policy_publisher_->EnterFailClosed(operation.request.site,
                                       operation.policy_generation,
                                       "pending_operation_recovery");
    std::optional<PendingSiteOperation> rebound =
        store_->RebindOperationForRecovery(
            operation, service_incarnation, required_contexts);
    if (!rebound) {
      policy_publisher_->EnterStoreFailClosed(store_->owner(),
                                              "pending_rebind_failed");
      return;
    }
    if (rebound->request.enabled) {
      route_preparer_->PrepareRoute(*rebound);
    }
  }
  TryPublishNextReady();
}

std::string SiteProxyToggleCoordinator::RuntimeKey(
    const BrowserConfirmedSite& site) {
  return site.storage_partition_token + "\n" + site.canonical_host;
}

SiteToggleState SiteProxyToggleCoordinator::GetSiteToggleState(
    const BrowserConfirmedSite& site) {
  SiteToggleState state;
  state.committed =
      store_ ? store_->ReadSiteSelection(site) : SiteSelectionRecord{};
  const auto found = runtime_states_.find(RuntimeKey(site));
  if (found != runtime_states_.end()) {
    state.runtime_state = found->second;
  }
  return state;
}

void SiteProxyToggleCoordinator::SetRuntimeState(
    const BrowserConfirmedSite& site,
    aegis_access::ProxyRuntimeState state) {
  if (site.IsComplete()) {
    runtime_states_[RuntimeKey(site)] = state;
  }
}

}  // namespace aegis::access
