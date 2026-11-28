// Copyright 2026 GCSA
#ifndef SERVICES_NETWORK_AEGIS_ACCESS_ROUTE_REGISTRY_H_
#define SERVICES_NETWORK_AEGIS_ACCESS_ROUTE_REGISTRY_H_

#include <map>
#include <optional>
#include <string>
#include <set>
#include <memory>
#include "base/callback_list.h"
#include "base/sequence_checker.h"
#include "base/memory/weak_ptr.h"
#include "components/aegis_access/access_policy_evaluator.h"
#include "components/aegis_access/access_prepared_listener.h"
#include "net/base/net_errors.h"
#include "net/base/auth.h"
#include "net/http/access_proxy_route.h"

namespace network {
// Owned by one NetworkContext and called only on its network sequence. Inputs
// come from the Browser-only factory/publication channel, never ResourceRequest
// fields. This registry does not confer Browser provenance on arbitrary values:
// the private factory must also enforce its process/document binding.
class AccessRouteRegistry {
 public:
  struct Snapshot {
    aegis_access::PublishedAccessPolicySnapshot policy;
    // Selection is per group and must be zero here. Other counters are live.
    aegis_access::GenerationTuple common_generations;
    std::map<std::string, scoped_refptr<net::AccessProxyRoute>> routes;
    // Explicit failed/not-yet-ready groups remain PROXY policy, without a
    // fabricated endpoint. Zero only precedes the first group selection.
    std::map<std::string, uint64_t> unavailable_groups;
    // Keyed by proxy group. A no-auth route requires this exact native identity.
    std::map<std::string, aegis_access::AccessPreparedListenerBinding> listener_bindings;
  };
  struct Expectation {
    std::string incarnation;
    aegis_access::GenerationTuple generations;
    // Empty for native or an unavailable group; never interpreted as an endpoint.
    std::string registration_id;
  };
  struct Result {
    int net_error = net::ERR_BLOCKED_BY_CLIENT;
    scoped_refptr<net::AccessProxyRoute> route;
  };
  AccessRouteRegistry(aegis_access::OwnershipKey owner, std::string incarnation);
  ~AccessRouteRegistry();
  AccessRouteRegistry(const AccessRouteRegistry&) = delete;
  AccessRouteRegistry& operator=(const AccessRouteRegistry&) = delete;

  // Atomically installs a complete candidate. Failed installs leave the prior
  // snapshot intact; Browser publication barriers remain independently active.
  // Even an empty policy must be explicitly installed before native dispatch.
  bool Install(uint64_t sequence, Snapshot snapshot);
  void EnterRestoring();
  uint64_t restoration_epoch() const;
  // Native request state subscribes using a key actually returned by Resolve.
  // Notifications run after the current network stack unwinds. A publication
  // fence must wait behind these tasks before acknowledging consumer removal.
  std::optional<base::CallbackListSubscription> ObserveRegistrationRevocation(
      const net::AccessRouteIsolationKey& witness, base::RepeatingClosure callback);
  // An observed local-listener fault revokes only that installed registration,
  // including earlier policy snapshots of the same listener. It cannot be
  // revived under the same registration ID. Foreign/stale instances reject.
  bool InvalidateRegisteredListener(const net::AccessRouteIsolationKey& witness);
  bool IsRegistrationLive(const net::AccessRouteIsolationKey& witness) const;
  base::WeakPtr<AccessRouteRegistry> GetWeakPtr() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return weak_factory_.GetWeakPtr();
  }
  Result Resolve(const aegis_access::BrowserOwnedRequestMetadata& metadata,
                 const GURL& target, const Expectation& expected) const;

 private:
  void NotifyRegistrationRevoked(const net::AccessRouteIsolationKey& key);
  const aegis_access::OwnershipKey owner_;
  const std::string incarnation_;
  uint64_t installed_sequence_ = 0;
  std::optional<aegis_access::GenerationTuple> high_water_generations_;
  std::optional<aegis_access::PublishedAccessPolicySnapshot> last_policy_;
  std::map<std::string, uint64_t> selection_high_water_;
  struct RegistrationBinding {
    std::string group;
    net::ProxyChain proxy_chain;
    // Process-local inbound secret binding; never serialized or logged.
    std::optional<net::AuthCredentials> local_credentials;
    std::optional<aegis_access::AccessPreparedListenerBinding> listener_binding;
  };
  std::map<std::string, RegistrationBinding> registrations_;
  std::optional<Snapshot> snapshot_;
  std::set<std::string> failed_registrations_;
  std::map<net::AccessRouteIsolationKey, std::shared_ptr<base::RepeatingClosureList>>
      revocation_observers_;
  uint64_t restoration_epoch_ = 1;
  bool restoration_epoch_exhausted_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<AccessRouteRegistry> weak_factory_{this};
};
}  // namespace network
#endif  // SERVICES_NETWORK_AEGIS_ACCESS_ROUTE_REGISTRY_H_
