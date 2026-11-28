// Copyright 2026 GCSA
#include "services/network/aegis/access_route_registry.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"

#include <limits>
#include <vector>
#include "net/http/http_auth_cache.h"

namespace network {
namespace {
using aegis_access::GenerationTuple;
std::optional<net::AuthCredentials> LocalCredentials(
    const scoped_refptr<net::AccessProxyRoute>& route) {
  const auto& proxy = route->proxy_chain().GetProxyServer(0);
  auto* entry = route->proxy_auth_cache()->Lookup(
      url::SchemeHostPort(url::kHttpScheme, proxy.host_port_pair().host(),
                          proxy.host_port_pair().port()),
      net::HttpAuth::AUTH_PROXY, "proxy", net::HttpAuth::AUTH_SCHEME_BASIC,
      net::NetworkAnonymizationKey());
  return entry ? std::make_optional(entry->credentials()) : std::nullopt;
}
bool SameCredentials(const std::optional<net::AuthCredentials>& a,
                     const std::optional<net::AuthCredentials>& b) {
  return a.has_value() == b.has_value() && (!a || a->Equals(*b));
}
std::optional<aegis_access::AccessPreparedListenerBinding> ListenerBinding(
    const AccessRouteRegistry::Snapshot& snapshot, const std::string& group) {
  const auto found = snapshot.listener_bindings.find(group);
  return found == snapshot.listener_bindings.end() ? std::nullopt
                                                  : std::make_optional(found->second);
}
bool SameCommon(const GenerationTuple& a, const GenerationTuple& b) {
  return a.policy_generation == b.policy_generation &&
         a.identity_generation == b.identity_generation &&
         a.network_epoch == b.network_epoch &&
         a.base_proxy_config_generation == b.base_proxy_config_generation;
}
GenerationTuple Generations(const net::AccessRouteIsolationKey& key) {
  return {key.policy_generation, key.identity_generation,
          key.selection_generation, key.network_epoch,
          key.base_proxy_config_generation};
}
bool IsLiveCommon(const GenerationTuple& common) {
  return common.policy_generation && common.identity_generation &&
         common.network_epoch && common.base_proxy_config_generation &&
         common.selection_generation == 0;
}
bool IsNondecreasing(const GenerationTuple& next, const GenerationTuple& old) {
  return next.policy_generation >= old.policy_generation &&
         next.identity_generation >= old.identity_generation &&
         next.network_epoch >= old.network_epoch &&
         next.base_proxy_config_generation >= old.base_proxy_config_generation;
}
bool SameListenerInstance(const net::AccessRouteIsolationKey& a,
                          const net::AccessRouteIsolationKey& b) {
  auto previous = a;
  previous.policy_generation = b.policy_generation;
  return previous == b;
}
}  // namespace

AccessRouteRegistry::AccessRouteRegistry(aegis_access::OwnershipKey owner,
                                         std::string incarnation)
    : owner_(std::move(owner)), incarnation_(std::move(incarnation)) {
  CHECK(aegis_access::IsCompleteOwner(owner_));
  CHECK(!incarnation_.empty());
}
AccessRouteRegistry::~AccessRouteRegistry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

bool AccessRouteRegistry::Install(uint64_t sequence, Snapshot snapshot) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto& common = snapshot.common_generations;
  if (restoration_epoch_exhausted_ || sequence <= installed_sequence_ || snapshot.policy.owner != owner_ ||
      !IsLiveCommon(common) ||
      (last_policy_ &&
       snapshot.policy.policy_generation == last_policy_->policy_generation &&
       snapshot.policy != *last_policy_) ||
      snapshot.policy.policy_generation != common.policy_generation ||
      (high_water_generations_ &&
       !IsNondecreasing(common, *high_water_generations_)) ||
      aegis_access::ValidateAccessPolicySnapshot(snapshot.policy) !=
          aegis_access::PolicyMatchReason::kNone) {
    return false;
  }
  for (const auto& [group, route] : snapshot.routes) {
    if (!route || group.empty() || !route->proxy_chain().is_single_proxy()) {
      return false;
    }
    const auto& key = route->isolation_key();
    const auto binding = ListenerBinding(snapshot, group);
    const auto credentials = LocalCredentials(route);
    const bool no_auth = binding &&
        aegis_access::IsValidAccessPreparedListenerBinding(*binding) && !credentials;
    const bool legacy_http = !binding && route->proxy_chain().GetProxyServer(0).is_http() &&
        credentials && !credentials->username().empty() && !credentials->password().empty();
    if (!key.IsComplete() || failed_registrations_.contains(key.registration) || key.group != group ||
        key.channel != static_cast<uint8_t>(owner_.channel) ||
        key.profile != owner_.profile_token ||
        key.partition != owner_.storage_partition_token ||
        key.incarnation != incarnation_ ||
        !SameCommon(Generations(key), common) ||
        !route->proxy_chain().is_single_proxy() ||
        (!route->proxy_chain().GetProxyServer(0).is_http() &&
         route->proxy_chain().GetProxyServer(0).scheme() != net::ProxyServer::SCHEME_SOCKS5) ||
        (!no_auth && !legacy_http)) {
      return false;
    }
  }
  for (const auto& [group, binding] : snapshot.listener_bindings)
    if (!snapshot.routes.contains(group)) return false;
  for (const auto& [group, selection] : snapshot.unavailable_groups) {
    const auto high_water = selection_high_water_.find(group);
    if (group.empty() || snapshot.routes.contains(group) ||
        (high_water != selection_high_water_.end() &&
         selection < high_water->second)) {
      return false;
    }
  }
  std::map<std::string, std::string> candidate_registrations;
  for (const auto& [group, route] : snapshot.routes) {
    const auto& key = route->isolation_key();
    auto high_water = selection_high_water_.find(group);
    if (high_water != selection_high_water_.end() &&
        key.selection_generation < high_water->second) {
      return false;
    }
    if (!candidate_registrations.emplace(key.registration, group).second) {
      return false;
    }
    auto old = registrations_.find(key.registration);
    if (old != registrations_.end() &&
        (old->second.group != group ||
         old->second.proxy_chain != route->proxy_chain() ||
         old->second.listener_binding != ListenerBinding(snapshot, group) ||
         !SameCredentials(old->second.local_credentials, LocalCredentials(route)))) {
      return false;
    }
  }
  for (const auto& rule : snapshot.policy.rules) {
    if (rule.mode == aegis_access::AccessMode::kProxy &&
        !snapshot.routes.contains(rule.proxy_group_id) &&
        !snapshot.unavailable_groups.contains(rule.proxy_group_id)) {
      return false;
    }
  }
  // All validation completes before any live state or high-water mark changes.
  std::vector<net::AccessRouteIsolationKey> retired;
  for (const auto& [key, observers] : revocation_observers_) {
    const auto next = snapshot.routes.find(key.group);
    if (observers->empty() || next == snapshot.routes.end() ||
        !SameListenerInstance(key, next->second->isolation_key())) retired.push_back(key);
  }
  for (const auto& [group, selection] : snapshot.unavailable_groups) {
    selection_high_water_[group] = selection;
  }
  for (const auto& [group, route] : snapshot.routes) {
    const auto& key = route->isolation_key();
    selection_high_water_[group] = key.selection_generation;
    registrations_.insert_or_assign(
        key.registration, RegistrationBinding{group, route->proxy_chain(),
            LocalCredentials(route), ListenerBinding(snapshot, group)});
  }
  last_policy_ = snapshot.policy;
  installed_sequence_ = sequence;
  high_water_generations_ = common;
  snapshot_ = std::move(snapshot);
  for (const auto& key : retired) NotifyRegistrationRevoked(key);
  return true;
}

void AccessRouteRegistry::EnterRestoring() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  snapshot_.reset();
  if (restoration_epoch_ == std::numeric_limits<uint64_t>::max())
    restoration_epoch_exhausted_ = true;
  else
    ++restoration_epoch_;
  while (!revocation_observers_.empty())
    NotifyRegistrationRevoked(revocation_observers_.begin()->first);
}

uint64_t AccessRouteRegistry::restoration_epoch() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return restoration_epoch_exhausted_ ? 0 : restoration_epoch_;
}

void AccessRouteRegistry::NotifyRegistrationRevoked(const net::AccessRouteIsolationKey& key) {
  const auto found = revocation_observers_.find(key);
  if (found == revocation_observers_.end()) return;
  auto observers = std::move(found->second);
  revocation_observers_.erase(found);
  if (observers->empty()) return;
  // Do not destroy an HTTP transaction from inside its own auth callback.
  // Capturing this list, rather than looking it up later, also prevents a
  // reinstalled key from subscribing new requests to an old cancellation.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](std::shared_ptr<base::RepeatingClosureList> pending) {
        pending->Notify();
      }, std::move(observers)));
}

std::optional<base::CallbackListSubscription> AccessRouteRegistry::ObserveRegistrationRevocation(
    const net::AccessRouteIsolationKey& witness, base::RepeatingClosure callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!snapshot_ || !callback || !witness.IsComplete() ||
      failed_registrations_.contains(witness.registration)) return std::nullopt;
  const auto current = snapshot_->routes.find(witness.group);
  if (current == snapshot_->routes.end() ||
      !SameListenerInstance(witness, current->second->isolation_key())) return std::nullopt;
  auto found = revocation_observers_.find(witness);
  if (witness != current->second->isolation_key() && found == revocation_observers_.end())
    return std::nullopt;
  if (found == revocation_observers_.end())
    found = revocation_observers_.emplace(witness, std::make_shared<base::RepeatingClosureList>()).first;
  return found->second->Add(std::move(callback));
}

bool AccessRouteRegistry::InvalidateRegisteredListener(const net::AccessRouteIsolationKey& witness) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!snapshot_ || !witness.IsComplete() || failed_registrations_.contains(witness.registration) ||
      !revocation_observers_.contains(witness) || revocation_observers_.at(witness)->empty()) return false;
  const auto current = snapshot_->routes.find(witness.group);
  if (current == snapshot_->routes.end() ||
      !SameListenerInstance(witness, current->second->isolation_key())) return false;
  failed_registrations_.insert(witness.registration);
  std::vector<net::AccessRouteIsolationKey> retired;
  for (const auto& entry : revocation_observers_)
    if (SameListenerInstance(entry.first, witness)) retired.push_back(entry.first);
  for (const auto& key : retired) NotifyRegistrationRevoked(key);
  return true;
}

bool AccessRouteRegistry::IsRegistrationLive(
    const net::AccessRouteIsolationKey& witness) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!snapshot_ || !witness.IsComplete() ||
      failed_registrations_.contains(witness.registration)) {
    return false;
  }
  const auto current = snapshot_->routes.find(witness.group);
  return current != snapshot_->routes.end() &&
         SameListenerInstance(witness, current->second->isolation_key());
}

AccessRouteRegistry::Result AccessRouteRegistry::Resolve(
    const aegis_access::BrowserOwnedRequestMetadata& metadata,
    const GURL& target, const Expectation& expected) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!snapshot_ || metadata.owner != owner_ ||
      expected.incarnation != incarnation_ ||
      !SameCommon(expected.generations, snapshot_->common_generations)) {
    return {};
  }
  auto canonical = aegis_access::CanonicalizeBrowserOwnedRequest(metadata, target);
  if (!canonical.context) {
    return {};
  }
  const auto match = aegis_access::EvaluateAccessPolicy(*canonical.context,
                                                       snapshot_->policy);
  if (match.policy_state != aegis_access::PolicyState::kValid &&
      match.policy_state != aegis_access::PolicyState::kAbsent) {
    return {};
  }
  switch (match.effective_mode) {
    case aegis_access::AccessMode::kNone:
    case aegis_access::AccessMode::kDirect:
      if (expected.registration_id.empty() &&
          expected.generations.selection_generation == 0) {
        return {net::OK, nullptr};
      }
      return {};
    case aegis_access::AccessMode::kProxy: {
      if (snapshot_->unavailable_groups.contains(match.effective_proxy_group_id)) {
        return {net::ERR_PROXY_CONNECTION_FAILED, nullptr};
      }
      auto found = snapshot_->routes.find(match.effective_proxy_group_id);
      if (found == snapshot_->routes.end()) {
        return {};
      }
      const auto& key = found->second->isolation_key();
      if (failed_registrations_.contains(key.registration))
        return {net::ERR_PROXY_CONNECTION_FAILED, nullptr};
      if (expected.generations != Generations(key) ||
          expected.registration_id != key.registration) {
        return {};
      }
      return {net::OK, found->second};
    }
    case aegis_access::AccessMode::kReject:
    case aegis_access::AccessMode::kInvalid:
      return {};
  }
  return {};
}
}  // namespace network
