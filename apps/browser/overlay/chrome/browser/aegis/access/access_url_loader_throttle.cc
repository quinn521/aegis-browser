// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_url_loader_throttle.h"

#include <memory>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/aegis/access/access_browser_request_adapter.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/profiles/profile.h"
#include "components/aegis_access/access_policy_evaluator.h"
#include "components/aegis_access/published_request_runtime.h"
#include "components/aegis_access/request_policy_context.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"

namespace aegis::access {
namespace {

constexpr char kAccessCancelReason[] = "AegisAccessDispatch";

std::unique_ptr<AccessURLLoaderThrottle> Deny(const GURL& url) {
  return std::unique_ptr<AccessURLLoaderThrottle>(
      new AccessURLLoaderThrottle(
          url, AccessURLLoaderThrottle::StartDisposition::kDeny, nullptr, {}));
}

std::optional<aegis_access::RegisteredProxyEntry> ToRegisteredEntry(
    const aegis_access::RegisteredProxyEndpoint& endpoint) {
  if (endpoint.registration_id.empty() || endpoint.proxy_group_id.empty()) {
    return std::nullopt;
  }
  return aegis_access::RegisteredProxyEntry{
      endpoint.registration_id, endpoint.proxy_group_id, endpoint.owner,
      endpoint.generations};
}

}  // namespace

class AccessURLLoaderThrottleTerminationHandle final
    : public aegis_access::RequestTerminationHandle {
 public:
  AccessURLLoaderThrottleTerminationHandle(
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::WeakPtr<AccessURLLoaderThrottle> throttle)
      : runner_(std::move(runner)), throttle_(std::move(throttle)) {}

  void Terminate() override {
    runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&AccessURLLoaderThrottle::TerminateFromRegistry,
                       throttle_));
  }

 private:
  scoped_refptr<base::SequencedTaskRunner> runner_;
  base::WeakPtr<AccessURLLoaderThrottle> throttle_;
};

// static
std::unique_ptr<AccessURLLoaderThrottle> AccessURLLoaderThrottle::MaybeCreate(
    Profile* profile,
    const network::ResourceRequest& request,
    const base::RepeatingCallback<content::WebContents*()>& wc_getter,
    content::FrameTreeNodeId frame_tree_node_id,
    std::optional<int64_t> navigation_id) {
  if (!request.url.SchemeIsHTTPOrHTTPS()) {
    return nullptr;
  }

  AccessBrowserRequestMetadataResult metadata_result =
      BuildBrowserOwnedRequestMetadata(profile, wc_getter, frame_tree_node_id,
                                       navigation_id);
  if (!metadata_result.metadata.has_value()) {
    // Frame-less workers/prefetch/keepalive are outside this first slice. Do
    // not borrow another tab's identity; later request-surface slices provide
    // their own trusted ownership adapters.
    return nullptr;
  }

  aegis_access::RequestPolicyContextResult context_result =
      aegis_access::CanonicalizeBrowserOwnedRequest(*metadata_result.metadata,
                                                    request.url);
  if (!context_result.context.has_value()) {
    return Deny(request.url);
  }
  const aegis_access::RequestPolicyContext& context = *context_result.context;

  AccessPublishedRequestRuntime* published_runtime =
      AccessPublishedRequestRuntime::Get(profile);
  if (!published_runtime) {
    return nullptr;
  }
  const aegis_access::PublishedAccessPolicySnapshot* snapshot =
      published_runtime->GetPublishedPolicySnapshot(context.owner());
  if (!snapshot) {
    return nullptr;
  }

  const aegis_access::PolicyMatchResult match =
      aegis_access::EvaluateAccessPolicy(context, *snapshot);
  if (match.policy_state == aegis_access::PolicyState::kAbsent ||
      (match.policy_state == aegis_access::PolicyState::kValid &&
       match.effective_mode == aegis_access::AccessMode::kDirect)) {
    return nullptr;
  }
  if (match.policy_state != aegis_access::PolicyState::kValid ||
      match.effective_mode == aegis_access::AccessMode::kReject) {
    return Deny(request.url);
  }
  if (match.effective_mode != aegis_access::AccessMode::kProxy ||
      match.effective_proxy_group_id.empty()) {
    return Deny(request.url);
  }

  const aegis_access::RequestGenerationTupleBuildResult tuple_result =
      published_runtime->BuildProxyGenerationTuple(
          context.owner(), match.effective_proxy_group_id);
  if (!tuple_result.generations.has_value()) {
    return Deny(request.url);
  }

  AccessNetworkContextTransport* transport =
      AccessNetworkContextTransport::Get(profile);
  if (!transport) {
    return Deny(request.url);
  }
  const std::optional<aegis_access::RegisteredProxyEndpoint> endpoint =
      transport->ResolvePublishedProxyEndpoint(
          context.owner(), context.exact_host(),
          match.effective_proxy_group_id, *tuple_result.generations);
  if (!endpoint.has_value()) {
    return Deny(request.url);
  }
  const std::optional<aegis_access::RegisteredProxyEntry> registered_entry =
      ToRegisteredEntry(*endpoint);
  if (!registered_entry.has_value()) {
    return Deny(request.url);
  }

  scoped_refptr<AccessRequestDispatchState> dispatch_state =
      AccessRequestDispatchState::GetOrCreate(profile);
  if (!dispatch_state) {
    return Deny(request.url);
  }

  aegis_access::PublishedRequestRuntimeInput runtime_input;
  runtime_input.request =
      context.ToOwnershipRecord(*tuple_result.generations);
  runtime_input.policy_state = match.policy_state;
  runtime_input.effective_mode = match.effective_mode;
  runtime_input.policy_scope = match.policy_scope;
  runtime_input.effective_proxy_group_id = match.effective_proxy_group_id;
  runtime_input.matched_policy_generation = match.policy_generation;
  runtime_input.require_proxy_intent = true;
  runtime_input.snapshot_state = aegis_access::SnapshotState::kPublished;
  runtime_input.snapshot_owner = snapshot->owner;
  runtime_input.snapshot_generations = *tuple_result.generations;
  runtime_input.protection_restriction =
      aegis_access::ProtectionRestriction::kNone;
  runtime_input.managed_restriction =
      aegis_access::ManagedRestriction::kNone;
  runtime_input.runtime_state = aegis_access::ProxyRuntimeState::kReady;
  runtime_input.registered_proxy_entry = *registered_entry;

  const aegis_access::PublishedRequestRuntimeResult dispatch =
      dispatch_state->EvaluateAndRegister(runtime_input);
  if (dispatch.status !=
          aegis_access::PublishedRequestRuntimeStatus::kDispatchRegistered ||
      dispatch.route_plan.action !=
          aegis_access::RouteAction::kUseRegisteredProxy) {
    return Deny(request.url);
  }

  return std::unique_ptr<AccessURLLoaderThrottle>(
      new AccessURLLoaderThrottle(
          request.url, StartDisposition::kDispatchProxy,
          std::move(dispatch_state), std::move(runtime_input.request)));
}

AccessURLLoaderThrottle::AccessURLLoaderThrottle(
    GURL expected_url,
    StartDisposition disposition,
    scoped_refptr<AccessRequestDispatchState> dispatch_state,
    aegis_access::RequestOwnershipRecord request_record)
    : expected_url_(std::move(expected_url)),
      disposition_(disposition),
      dispatch_state_(std::move(dispatch_state)),
      request_record_(std::move(request_record)) {}

AccessURLLoaderThrottle::~AccessURLLoaderThrottle() {
  CompleteRegistered();
}

void AccessURLLoaderThrottle::DetachFromCurrentSequence() {
  // No WeakPtr is created until WillStartRequest on the destination sequence.
}

void AccessURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
  if (defer) {
    *defer = false;
  }
  if (disposition_ == StartDisposition::kDeny || !request ||
      request->url != expected_url_ || !dispatch_state_) {
    CancelWithoutRegistry(kAccessCancelReason);
    return;
  }

  const aegis_access::RequestOwnershipLookupResult lookup =
      dispatch_state_->Lookup(request_record_.request_id, request_record_.owner,
                              request_record_.generations);
  if (lookup.status != aegis_access::RequestOwnershipStatus::kOk ||
      !lookup.snapshot.has_value() ||
      lookup.snapshot->lifecycle !=
          aegis_access::RequestOwnershipLifecycle::kNew) {
    CancelWithoutRegistry(kAccessCancelReason);
    return;
  }

  auto handle = std::make_unique<AccessURLLoaderThrottleTerminationHandle>(
      base::SequencedTaskRunner::GetCurrentDefault(),
      weak_factory_.GetWeakPtr());
  const aegis_access::RequestOwnershipStatus status =
      dispatch_state_->MarkDispatched(
          request_record_.request_id, request_record_.owner,
          request_record_.generations, std::move(handle));
  if (status != aegis_access::RequestOwnershipStatus::kOk) {
    dispatch_state_->Complete(request_record_.request_id, request_record_.owner,
                              request_record_.generations);
    CancelWithoutRegistry(kAccessCancelReason);
    return;
  }
  registered_ = true;
}

void AccessURLLoaderThrottle::WillRedirectRequest(
    net::RedirectInfo*,
    const network::mojom::URLResponseHead&,
    bool* defer,
    network::HttpRequestHeadersUpdateParams*) {
  if (defer) {
    *defer = false;
  }
  // This first slice has not yet rebuilt ownership/policy for redirect targets.
  // Stop before following the redirect instead of allowing a stale host route.
  CancelRegistered("AegisAccessRedirectRequiresReevaluation");
}

void AccessURLLoaderThrottle::WillProcessResponse(
    const GURL&,
    network::mojom::URLResponseHead*,
    bool* defer) {
  if (defer) {
    *defer = false;
  }
  if (!registered_ || !dispatch_state_) {
    return;
  }
  if (dispatch_state_->MarkStreaming(
          request_record_.request_id, request_record_.owner,
          request_record_.generations) !=
      aegis_access::RequestOwnershipStatus::kOk) {
    CancelRegistered(kAccessCancelReason);
  }
}

void AccessURLLoaderThrottle::WillOnCompleteWithError(
    const network::URLLoaderCompletionStatus&) {
  CompleteRegistered();
}

void AccessURLLoaderThrottle::CancelWithoutRegistry(std::string_view reason) {
  if (delegate_) {
    delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT, reason);
  }
}

void AccessURLLoaderThrottle::CancelRegistered(std::string_view reason) {
  if (registered_ && dispatch_state_) {
    dispatch_state_->Complete(request_record_.request_id, request_record_.owner,
                              request_record_.generations);
    registered_ = false;
  }
  CancelWithoutRegistry(reason);
}

void AccessURLLoaderThrottle::CompleteRegistered() {
  if (!registered_ || !dispatch_state_) {
    return;
  }
  dispatch_state_->Complete(request_record_.request_id, request_record_.owner,
                            request_record_.generations);
  registered_ = false;
}

void AccessURLLoaderThrottle::TerminateFromRegistry() {
  registered_ = false;
  CancelWithoutRegistry("AegisAccessPolicyTermination");
}

}  // namespace aegis::access
