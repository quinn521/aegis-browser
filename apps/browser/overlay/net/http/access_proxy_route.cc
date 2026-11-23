// Copyright 2026 GCSA

#include "net/http/access_proxy_route.h"

#include <utility>

#include "net/base/ip_address.h"
#include "net/http/http_auth_controller.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"

namespace net {

scoped_refptr<AccessProxyRoute> AccessProxyRoute::Create(
    AccessRouteIsolationKey isolation_key,
    ProxyChain proxy_chain) {
  if (!isolation_key.IsComplete() || !proxy_chain.is_single_proxy()) {
    return nullptr;
  }
  const auto& proxy = proxy_chain.GetProxyServer(0);
  if (!proxy.is_valid() ||
      (proxy.scheme() != ProxyServer::SCHEME_HTTP &&
       proxy.scheme() != ProxyServer::SCHEME_SOCKS5) ||
      proxy.host_port_pair().port() == 0 ||
      (proxy.scheme() == ProxyServer::SCHEME_SOCKS5 &&
       proxy.host_port_pair().port() == 1080)) {
    return nullptr;
  }
  IPAddress address;
  if (!address.AssignFromIPLiteral(proxy.host_port_pair().host()) ||
      !address.IsLoopback()) {
    return nullptr;
  }
  return base::WrapRefCounted(new AccessProxyRoute(std::move(isolation_key),
                                                 std::move(proxy_chain)));
}

AccessProxyRoute::AccessProxyRoute(AccessRouteIsolationKey isolation_key,
                                 ProxyChain proxy_chain)
    : isolation_key_(std::move(isolation_key)),
      proxy_chain_(std::move(proxy_chain)),
      proxy_auth_cache_(/*key_server_entries_by_network_anonymization_key=*/true) {}

AccessProxyRoute::~AccessProxyRoute() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

HttpAuthCache* AccessProxyRoute::proxy_auth_cache() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return &proxy_auth_cache_;
}

bool AccessProxyRouteAllowsLegacyAuthentication(AccessProxyRoute& route) {
  const auto& proxy = route.proxy_chain().First();
  if (proxy.scheme() != ProxyServer::SCHEME_HTTP) return false;
  auto* entry = route.proxy_auth_cache()->Lookup(
      url::SchemeHostPort(url::kHttpScheme, proxy.host_port_pair().host(),
                          proxy.host_port_pair().port()),
      HttpAuth::AUTH_PROXY, "proxy", HttpAuth::AUTH_SCHEME_BASIC,
      NetworkAnonymizationKey());
  return entry && !entry->credentials().username().empty() &&
         !entry->credentials().password().empty();
}

void RestrictAccessProxyAuthentication(AccessProxyRoute& route,
                                       HttpAuthController& controller) {
  const bool legacy = AccessProxyRouteAllowsLegacyAuthentication(route);
  // Even the legacy adapter may use only its explicit local Basic identity.
  // NTLM/Negotiate, ambient credentials and embedded identities are unrelated
  // to upstream authorization and must never be offered to a local listener.
  controller.DisableEmbeddedIdentity();
  for (int value = HttpAuth::AUTH_SCHEME_BASIC;
       value < HttpAuth::AUTH_SCHEME_MAX; ++value) {
    if (!legacy || value != HttpAuth::AUTH_SCHEME_BASIC)
      controller.DisableAuthScheme(static_cast<HttpAuth::Scheme>(value));
  }
}

}  // namespace net
