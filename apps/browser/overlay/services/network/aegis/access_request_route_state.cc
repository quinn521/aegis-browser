// Copyright 2026 GCSA
#include "services/network/aegis/access_request_route_state.h"
#include <limits>
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/http/http_util.h"
#include "url/origin.h"

namespace network {
scoped_refptr<AccessRequestRouteState> AccessRequestRouteState::Create(
    base::WeakPtr<AccessRouteRegistry> registry,
    aegis_access::BrowserOwnedRequestMetadata metadata, GURL target,
    std::string method, AccessRouteRegistry::Expectation expected,
    int* initial_error) {
  if (initial_error) *initial_error = net::ERR_BLOCKED_BY_CLIENT;
  if (!registry || registry->restoration_epoch() == 0 || !net::HttpUtil::IsToken(method)) {
    return nullptr;
  }
  auto result = base::WrapRefCounted(new AccessRequestRouteState(
      std::move(registry), std::move(metadata), std::move(target),
      std::move(method), std::move(expected)));
  const auto resolved = result->registry_->Resolve(
      result->metadata_, result->target_, result->expected_);
  if (initial_error) *initial_error = resolved.net_error;
  if (resolved.net_error != net::OK) {
    return nullptr;
  }
  if (resolved.route) result->current_route_key_ = resolved.route->isolation_key();
  return result;
}
AccessRequestRouteState::AccessRequestRouteState(
    base::WeakPtr<AccessRouteRegistry> registry,
    aegis_access::BrowserOwnedRequestMetadata metadata, GURL target,
    std::string method, AccessRouteRegistry::Expectation expected)
    : registry_(std::move(registry)), restoration_epoch_(registry_->restoration_epoch()),
      metadata_(std::move(metadata)),
      target_(std::move(target)), method_(std::move(method)),
      expected_(std::move(expected)) {}
AccessRequestRouteState::~AccessRequestRouteState() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}
bool AccessRequestRouteState::IsCurrentRestorationEpoch() const {
  return registry_ && restoration_epoch_ != 0 &&
      registry_->restoration_epoch() == restoration_epoch_;
}
base::CallbackListSubscription AccessRequestRouteState::ObserveRegistrationRevocation(
    base::RepeatingClosure callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!revocation_observers_) revocation_observers_ = std::make_shared<base::RepeatingClosureList>();
  auto subscription = revocation_observers_->Add(std::move(callback));
  UpdateRegistryRevocationSubscription();
  return subscription;
}
void AccessRequestRouteState::UpdateRegistryRevocationSubscription() {
  if (!revocation_observers_ || revocation_observers_->empty() || registry_revocation_subscription_)
    return;
  if (!listener_revoked_ && IsCurrentRestorationEpoch() && current_route_key_) {
    registry_revocation_subscription_ = registry_->ObserveRegistrationRevocation(*current_route_key_,
        base::BindRepeating(&AccessRequestRouteState::OnRegistrationRevoked, base::Unretained(this)));
    if (registry_revocation_subscription_) return;
  } else if (!listener_revoked_ && IsCurrentRestorationEpoch() && !current_route_key_) {
    return;  // Native traffic has no local listener to revoke.
  }
  listener_revoked_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](std::shared_ptr<base::RepeatingClosureList> observers) {
        observers->Notify();
      }, revocation_observers_));
}
void AccessRequestRouteState::OnRegistrationRevoked() {
  auto keep_alive = base::WrapRefCounted(this);
  listener_revoked_ = true;
  registry_revocation_subscription_.reset();
  auto observers = revocation_observers_;
  if (observers) observers->Notify();
}
bool AccessRequestRouteState::InvalidateCurrentListener() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return !listener_revoked_ && IsCurrentRestorationEpoch() && current_route_key_ &&
      registry_->InvalidateRegisteredListener(*current_route_key_);
}
// Preserve Browser metadata verbatim: the canonicalizer derives the hop site
// only for a main navigation (no top_frame_site). Subframe navigations carry
// the parent's explicit top_frame_site and must retain it across redirects.
AccessRouteRegistry::Result AccessRequestRouteState::BeginSend(
    const GURL& target, const std::string& method, bool is_internal_preflight) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (listener_revoked_ || !IsCurrentRestorationEpoch() || pending_redirect_ || target != target_ || sent_for_hop_ ||
      (is_internal_preflight ? method != "OPTIONS" : method != method_)) {
    return {};
  }
  auto result = registry_->Resolve(metadata_, target, expected_);
  if (result.net_error == net::OK && !is_internal_preflight) {
    sent_for_hop_ = true;
  }
  return result;
}
bool AccessRequestRouteState::ObserveRedirect(
    const GURL& target, const std::string& method) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (listener_revoked_ || !IsCurrentRestorationEpoch() || !sent_for_hop_ || pending_redirect_ || !target.is_valid() ||
      !net::HttpUtil::IsToken(method) ||
      hop_ == std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  pending_redirect_ = PendingRedirect{target, method};
  return true;
}
bool AccessRequestRouteState::ApproveRedirect(
    uint32_t hop, const GURL& target, const std::string& method,
    AccessRouteRegistry::Expectation expected) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (listener_revoked_ || !IsCurrentRestorationEpoch() || !pending_redirect_ || hop_ == std::numeric_limits<uint32_t>::max() ||
      (current_route_key_ && !registry_->IsRegistrationLive(*current_route_key_)) ||
      hop != hop_ + 1 || method != pending_redirect_->method ||
      !target.is_valid() ||
      !url::Origin::Create(target).IsSameOriginWith(
          url::Origin::Create(pending_redirect_->target)) ||
      registry_->Resolve(metadata_, target, expected).net_error !=
          net::OK) {
    return false;
  }
  hop_ = hop;
  target_ = target;
  method_ = method;
  expected_ = std::move(expected);
  const auto next = registry_->Resolve(metadata_, target_, expected_);
  current_route_key_ = next.route ? std::make_optional(next.route->isolation_key()) : std::nullopt;
  registry_revocation_subscription_.reset();
  UpdateRegistryRevocationSubscription();
  pending_redirect_.reset();
  sent_for_hop_ = false;
  return true;
}
}  // namespace network
