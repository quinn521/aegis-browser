// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_service_coordinator.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/time/time.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/aegis/access/access_proxy_selection_generation_source.h"
#include "chrome/browser/aegis/access/access_request_dispatch_state.h"
#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"

namespace aegis::access {

namespace {
const void* const kAccessServiceCoordinatorKey = &kAccessServiceCoordinatorKey;

bool SelectorMatchesMutation(
    const aegis_access::RequestCancellationSelector& selector,
    const SiteGroupMutationRequest& request) {
  if (!aegis_access::IsValidRequestCancellationSelector(selector) ||
      selector.owner != request.candidate_group.owner ||
      selector.exact_host != request.candidate_group.canonical_host) {
    return false;
  }
  return std::ranges::any_of(
      request.candidate_members, [&](const SiteProxyRuleMember& member) {
        return member.owner == selector.owner &&
               member.exact_host == selector.exact_host &&
               member.top_level_site == selector.top_level_site &&
               std::ranges::find(member.schemes, selector.scheme) !=
                   member.schemes.end();
      });
}

std::optional<AccessMode> OrdinaryMutationMode(
    const SiteGroupMutationRequest& request) {
  if (request.candidate_members.empty()) {
    return std::nullopt;
  }
  const AccessMode mode = request.candidate_members.front().mode;
  if (mode != AccessMode::kDirect && mode != AccessMode::kProxy) {
    return std::nullopt;
  }
  for (const auto& member : request.candidate_members) {
    if (member.mode != mode) {
      return std::nullopt;
    }
  }
  return mode;
}

AccessMutationTransactionResult Result(
    AccessMutationTransactionStatus status,
    StoreStatus store_status = StoreStatus::kValid,
    uint64_t generation = 0) {
  return {status, store_status, generation};
}
}

AccessServiceCoordinator::AccessServiceCoordinator(Profile* profile)
    : profile_(profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AccessServiceCoordinator::~AccessServiceCoordinator() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AccessServiceCoordinator* AccessServiceCoordinator::Get(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile) {
    return nullptr;
  }
  return static_cast<AccessServiceCoordinator*>(
      profile->GetUserData(kAccessServiceCoordinatorKey));
}

AccessServiceCoordinator* AccessServiceCoordinator::GetOrCreate(
    Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile || !aegis::IsAegisProfileSupported(profile)) {
    return nullptr;
  }

  auto* existing = Get(profile);
  if (existing) {
    return existing;
  }

  auto coordinator = std::unique_ptr<AccessServiceCoordinator>(
      new AccessServiceCoordinator(profile));
  auto* result = coordinator.get();
  profile->SetUserData(kAccessServiceCoordinatorKey, std::move(coordinator));
  return result;
}

void AccessServiceCoordinator::CommitSiteGroupMutation(
    std::unique_ptr<AccessRuleStore> supplied_store,
    SiteGroupMutationRequest request,
    aegis_access::RequestCancellationSelector selector,
    base::OnceCallback<void(AccessMutationTransactionResult)> completion) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!completion) {
    return;
  }
  if (mutation_in_flight_) {
    std::move(completion).Run(Result(AccessMutationTransactionStatus::kBusy));
    return;
  }
  if (supplied_store && store_) {
    std::move(completion).Run(Result(AccessMutationTransactionStatus::kInvalidRequest));
    return;
  }
  if (supplied_store) {
    store_ = std::move(supplied_store);
  }
  AccessRuleStore* store = store_.get();
  mutation_in_flight_ = true;
  auto fail = [&](AccessMutationTransactionResult result) {
    Finish(std::move(completion), result);
  };

  if (!store || !store->is_open() || !SelectorMatchesMutation(selector, request) ||
      !OrdinaryMutationMode(request).has_value()) {
    fail(Result(AccessMutationTransactionStatus::kInvalidRequest));
    return;
  }

  std::optional<StoredPolicySnapshot> previous;
  StoreResult<StoredPolicySnapshot> prior = store->ReadCommittedSnapshot(
      request.candidate_group.owner.storage_partition_token);
  if (prior.status == StoreStatus::kValid && prior.value.has_value()) {
    previous = std::move(*prior.value);
  } else if (prior.status != StoreStatus::kMissing) {
    fail(Result(AccessMutationTransactionStatus::kStoreReadFailed,
                prior.status));
    return;
  }

  StoreResult<PendingMutationRecord> prepared =
      store->PrepareSiteGroupMutation(request);
  if (prepared.status != StoreStatus::kValid || !prepared.value.has_value()) {
    fail(Result(AccessMutationTransactionStatus::kPrepareFailed,
                prepared.status));
    return;
  }
  PendingMutationRecord pending = std::move(*prepared.value);

  auto supersede_and_fail = [&](AccessMutationTransactionResult result) {
    const auto cleanup = store->SupersedePreparedMutation(
        pending.operation_id, pending.request_fingerprint);
    if (cleanup != StoreStatus::kValid) {
      result.store_status = cleanup;
    }
    Finish(std::move(completion), result);
  };

  StoreResult<StoredPolicySnapshot> built =
      store->BuildPreparedCandidateSnapshot(pending);
  if (built.status != StoreStatus::kValid || !built.value.has_value()) {
    supersede_and_fail(Result(AccessMutationTransactionStatus::kCandidateBuildFailed,
                              built.status));
    return;
  }
  StoredPolicySnapshot candidate = std::move(*built.value);

  AccessPublishedRequestRuntime* runtime =
      AccessPublishedRequestRuntime::GetOrCreate(profile_);
  if (!runtime) {
    supersede_and_fail(Result(AccessMutationTransactionStatus::kMissingRuntime));
    return;
  }
  AccessRequestDispatchState* dispatch =
      AccessRequestDispatchState::GetOrCreate(profile_);
  if (!dispatch) {
    supersede_and_fail(
        Result(AccessMutationTransactionStatus::kMissingDispatchState));
    return;
  }
  AccessNetworkContextTransport* transport =
      AccessNetworkContextTransport::Get(profile_);
  if (!transport || !transport->OwnsConfiguredPartition(pending.owner)) {
    supersede_and_fail(Result(AccessMutationTransactionStatus::kMissingTransport));
    return;
  }

  const AccessMode mode = pending.candidate.members.front().policy.mode;
  uint64_t selection_generation = 0;
  if (mode == AccessMode::kProxy) {
    const std::string& proxy_group_id =
        pending.candidate.members.front().policy.proxy_group_id;
    const auto endpoint = transport->CaptureSelectedProxyEndpoint(
        pending.owner, proxy_group_id, selector.exact_host);
    if (!endpoint.has_value() ||
        endpoint->generations.selection_generation == 0 ||
        endpoint->generations.network_epoch != transport->network_epoch() ||
        !AccessProxySelectionGenerationSource::Get(profile_) ||
        AccessProxySelectionGenerationSource::Get(profile_)->selection_generation(
            proxy_group_id) != endpoint->generations.selection_generation) {
      supersede_and_fail(
          Result(AccessMutationTransactionStatus::kProxySelectionUnavailable));
      return;
    }
    selection_generation = endpoint->generations.selection_generation;
  }

  aegis_access::PolicyPublicationIdentity identity{
      pending.operation_id,
      pending.operation_sequence,
      pending.candidate.policy_generation,
      selection_generation,
      transport->network_epoch(),
      selector,
  };
  const auto begin = dispatch->BeginPolicyPublication({
      identity,
      {"network-context"},
      false,
  });
  if (begin.status != aegis_access::PolicyPublicationAckStatus::kPending) {
    supersede_and_fail(
        Result(AccessMutationTransactionStatus::kPublicationTrackerRejected));
    return;
  }

  const auto previous_endpoint = transport->CurrentEndpoint(pending.owner);
  if (previous_endpoint &&
      !transport->ReplaceEndpointPolicyGeneration(
          pending.owner, *previous_endpoint, pending.operation_sequence)) {
    dispatch->FailPolicyPublication(identity);
    supersede_and_fail(
        Result(AccessMutationTransactionStatus::kRuntimePublicationFailed));
    return;
  }

  const AccessPolicyPublicationResult publication =
      runtime->PublishPreparedPolicyCandidate(candidate);
  if (publication.status != AccessPolicyPublicationStatus::kPublished &&
      publication.status != AccessPolicyPublicationStatus::kUnchanged) {
    if (previous_endpoint) {
      auto rebound = *previous_endpoint;
      rebound.generations.policy_generation = pending.operation_sequence;
      transport->ReplaceEndpointPolicyGeneration(
          pending.owner, rebound, previous_endpoint->generations.policy_generation);
    }
    dispatch->FailPolicyPublication(identity);
    supersede_and_fail(
        Result(AccessMutationTransactionStatus::kRuntimePublicationFailed));
    return;
  }

  // Copy call arguments before moving transaction ownership into the callback.
  const auto publication_identity = identity;
  const auto publication_owner = pending.owner;
  auto settlement = std::make_shared<base::OnceCallback<void(bool)>>(base::BindOnce(
      &AccessServiceCoordinator::OnNetworkContextPublicationAck,
      weak_factory_.GetWeakPtr(), std::move(pending),
      std::move(candidate), std::move(previous), previous_endpoint, std::move(identity),
      std::move(completion)));
  auto settle = [](std::shared_ptr<base::OnceCallback<void(bool)>> callback,
                   bool accepted) {
    if (*callback) {
      std::move(*callback).Run(accepted);
    }
  };
  publication_timeout_.Start(
      FROM_HERE, base::Seconds(30), base::BindOnce(settle, settlement, false));
  dispatch->RequestNetworkContextPublicationAck(
      publication_identity, publication_owner,
      base::BindOnce(settle, settlement));
}

void AccessServiceCoordinator::OnNetworkContextPublicationAck(
    PendingMutationRecord pending,
    StoredPolicySnapshot candidate,
    std::optional<StoredPolicySnapshot> previous,
    std::optional<aegis_access::RegisteredProxyEndpoint> previous_endpoint,
    aegis_access::PolicyPublicationIdentity identity,
    base::OnceCallback<void(AccessMutationTransactionResult)> completion,
    bool acknowledged) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  AccessRuleStore* store = store_.get();
  AccessPublishedRequestRuntime* runtime =
      AccessPublishedRequestRuntime::Get(profile_);
  AccessRequestDispatchState* dispatch = AccessRequestDispatchState::Get(profile_);

  auto fail_and_restore = [&](AccessMutationTransactionStatus status,
                              StoreStatus store_status = StoreStatus::kValid) {
    if (dispatch) {
      dispatch->FailPolicyPublication(identity);
    }
    if (runtime) {
      if (runtime->RollbackPreparedPolicyCandidate(candidate, previous) &&
          previous_endpoint) {
        auto* transport = AccessNetworkContextTransport::Get(profile_);
        auto rebound = *previous_endpoint;
        rebound.generations.policy_generation = pending.operation_sequence;
        if (transport) {
          transport->ReplaceEndpointPolicyGeneration(
              pending.owner, rebound,
              previous_endpoint->generations.policy_generation);
        }
      }
    }
    if (store) {
      const auto cleanup = store->SupersedePreparedMutation(
          pending.operation_id, pending.request_fingerprint);
      if (cleanup != StoreStatus::kValid) {
        store_status = cleanup;
      }
    }
    Finish(std::move(completion), Result(status, store_status));
  };

  if (!acknowledged || !dispatch || !runtime || !store) {
    fail_and_restore(AccessMutationTransactionStatus::kNetworkPublicationFailed);
    return;
  }

  // ACKs cross an asynchronous boundary. The exact candidate and proxy
  // selection must still be current at the durable commit point.
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto* published = runtime->GetPublishedPolicySnapshot(pending.owner);
  const auto adapted = AccessRuleStore::AdaptMatcherSnapshot(candidate);
  if (!transport || !transport->OwnsConfiguredPartition(pending.owner) ||
      transport->network_epoch() != identity.network_epoch || !published ||
      !adapted.value ||
      *published != aegis_access::PublishedAccessPolicySnapshot{
                        adapted.value->owner,
                        adapted.value->committed_policy_generation,
                        adapted.value->rules}) {
    fail_and_restore(AccessMutationTransactionStatus::kNetworkPublicationFailed);
    return;
  }
  if (previous_endpoint) {
    auto expected_endpoint = *previous_endpoint;
    expected_endpoint.generations.policy_generation = pending.operation_sequence;
    if (transport->CurrentEndpoint(pending.owner) != expected_endpoint) {
      fail_and_restore(AccessMutationTransactionStatus::kNetworkPublicationFailed);
      return;
    }
  } else if (transport->CurrentEndpoint(pending.owner)) {
    fail_and_restore(AccessMutationTransactionStatus::kNetworkPublicationFailed);
    return;
  }
  if (identity.selection_generation != 0) {
    const auto endpoint = transport->CaptureSelectedProxyEndpoint(
        pending.owner, pending.candidate.members.front().policy.proxy_group_id,
        identity.selector.exact_host);
    auto* selection = AccessProxySelectionGenerationSource::Get(profile_);
    if (!endpoint || !selection ||
        selection->selection_generation(endpoint->proxy_group_id) !=
            identity.selection_generation ||
        endpoint->generations.selection_generation != identity.selection_generation) {
      fail_and_restore(AccessMutationTransactionStatus::kNetworkPublicationFailed);
      return;
    }
  }

  StoreResult<PendingMutationRecord> committed =
      store->CommitPreparedMutation(pending);
  if (committed.status != StoreStatus::kValid || !committed.value.has_value()) {
    fail_and_restore(AccessMutationTransactionStatus::kCommitFailed,
                     committed.status);
    return;
  }

  const auto ready = dispatch->MarkPolicyPublicationDurablyCommitted(identity);
  if (ready.status != aegis_access::PolicyPublicationAckStatus::kReady) {
    Finish(std::move(completion),
           Result(AccessMutationTransactionStatus::kFinalizeFailed,
                  StoreStatus::kValid, pending.operation_sequence));
    return;
  }
  const auto finalized = dispatch->FinalizePolicyPublication(identity);
  if (finalized.status != aegis_access::PolicyPublicationAckStatus::kFinalized) {
    Finish(std::move(completion),
           Result(AccessMutationTransactionStatus::kFinalizeFailed,
                  StoreStatus::kValid, pending.operation_sequence));
    return;
  }

  ++state_generation_;
  Finish(std::move(completion),
         Result(AccessMutationTransactionStatus::kCommitted,
                StoreStatus::kValid, committed.value->committed_policy_generation));
}

void AccessServiceCoordinator::Finish(
    base::OnceCallback<void(AccessMutationTransactionResult)> completion,
    AccessMutationTransactionResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  publication_timeout_.Stop();
  mutation_in_flight_ = false;
  if (completion) {
    std::move(completion).Run(result);
  }
}

}  // namespace aegis::access
