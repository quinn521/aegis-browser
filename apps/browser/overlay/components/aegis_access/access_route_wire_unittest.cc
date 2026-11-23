// Copyright 2026 GCSA
#include "services/network/aegis/access_route_wire.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_auth_controller.h"
#include "mojo/public/cpp/test_support/test_utils.h"
#include "services/network/public/mojom/aegis_access_route_types.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
mojom::AegisAccessSnapshotPtr WireSnapshot() {
  auto s = mojom::AegisAccessSnapshot::New();
  s->owner = mojom::AegisAccessOwner::New(mojom::AegisAccessChannel::kDev,
                                        "profile", "partition");
  s->incarnation = "incarnation";
  s->sequence = 1;
  s->common_generations = mojom::AegisAccessGenerations::New(1,1,0,1,1);
  auto rule = mojom::AegisAccessRule::New();
  rule->rule_id = "rule";
  rule->scope = mojom::AegisAccessRuleScope::kProfile;
  rule->destination_host = "destination.test";
  rule->schemes = {mojom::AegisAccessScheme::kHttp};
  rule->port_scope = mojom::AegisAccessPortScope::kAllBrowserPermitted;
  rule->mode = mojom::AegisAccessMode::kProxy;
  rule->proxy_group_id = "group";
  rule->protection_override = mojom::AegisAccessProtection::kNone;
  rule->row_revision = 1;
  rule->last_operation_sequence = 1;
  s->rules.push_back(std::move(rule));
  auto endpoint = mojom::AegisAccessProxyEndpoint::New();
  endpoint->proxy_group_id = "group";
  endpoint->registration_id = "registration";
  endpoint->selection_generation = 1;
  endpoint->numeric_loopback_host = "127.0.0.1";
  endpoint->port = 12345;
  endpoint->credentials = net::AuthCredentials(u"local-user", u"local-secret");
  s->endpoints.push_back(std::move(endpoint));
  return s;
}
mojom::AegisAccessSnapshotPtr NoAuthSnapshot(mojom::AegisAccessProxyTransport transport) {
  auto wire = WireSnapshot();
  auto& endpoint = *wire->endpoints[0];
  endpoint.transport = transport;
  endpoint.credentials.reset();
  endpoint.listener_binding = mojom::AegisAccessPreparedListenerBinding::New(
      aegis_access::kAccessNoAuthListenerWireVersion,
      "11111111-2222-4333-8444-555555555555", 9007199254740993ULL, 3, 4);
  return wire;
}
TEST(AccessRouteWireTest, ExplicitNoAuthHttpAndSocksKeepTheirActualSingleChain) {
  for (const auto transport : {mojom::AegisAccessProxyTransport::kHttp,
                               mojom::AegisAccessProxyTransport::kSocks5}) {
    auto wire = NoAuthSnapshot(transport);
    auto decoded = DecodeAccessRouteSnapshot(*wire);
    ASSERT_TRUE(decoded);
    const auto route = decoded->snapshot.routes.at("group");
    ASSERT_TRUE(route->proxy_chain().is_single_proxy());
    EXPECT_EQ(route->proxy_chain().GetProxyServer(0).scheme(),
              transport == mojom::AegisAccessProxyTransport::kSocks5
                  ? net::ProxyServer::SCHEME_SOCKS5 : net::ProxyServer::SCHEME_HTTP);
    EXPECT_EQ(decoded->snapshot.listener_bindings.at("group").profile_version,
              9007199254740993ULL);
    const url::SchemeHostPort proxy(GURL("http://127.0.0.1:12345"));
    EXPECT_FALSE(route->proxy_auth_cache()->Lookup(proxy, net::HttpAuth::AUTH_PROXY,
        "proxy", net::HttpAuth::AUTH_SCHEME_BASIC, net::NetworkAnonymizationKey()));
    wire->endpoints[0]->port = 1080;
    EXPECT_EQ(DecodeAccessRouteSnapshot(*wire).has_value(),
              transport == mojom::AegisAccessProxyTransport::kHttp);
  }
}
TEST(AccessRouteWireTest, NoAuthNeverAcceptsLegacyMissingVersionCredentialsOrUnknownProtocol) {
  auto legacy = WireSnapshot();
  legacy->endpoints[0]->credentials.reset();
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*legacy));
  auto wire = NoAuthSnapshot(mojom::AegisAccessProxyTransport::kSocks5);
  wire->endpoints[0]->listener_binding.reset();
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  for (const uint32_t version : {0u, 2u}) {
    wire = NoAuthSnapshot(mojom::AegisAccessProxyTransport::kHttp);
    wire->endpoints[0]->listener_binding->wire_version = version;
    EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  }
  wire = NoAuthSnapshot(mojom::AegisAccessProxyTransport::kHttp);
  wire->endpoints[0]->credentials = net::AuthCredentials(u"unexpected", u"credential");
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  wire = NoAuthSnapshot(mojom::AegisAccessProxyTransport::kHttp);
  wire->endpoints[0]->transport = static_cast<mojom::AegisAccessProxyTransport>(99);
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
}
TEST(AccessRouteWireTest, NoAuthIdentityAndExactVersionsSurviveRealMojoSerialization) {
  auto original = NoAuthSnapshot(mojom::AegisAccessProxyTransport::kSocks5);
  mojom::AegisAccessSnapshotPtr decoded;
  ASSERT_TRUE(mojo::test::SerializeAndDeserialize<mojom::AegisAccessSnapshot>(original, decoded));
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded->Equals(*original));
  ASSERT_TRUE(decoded->endpoints[0]->listener_binding);
  EXPECT_EQ(decoded->endpoints[0]->listener_binding->profile_version, 9007199254740993ULL);
  EXPECT_EQ(decoded->endpoints[0]->transport, mojom::AegisAccessProxyTransport::kSocks5);
  EXPECT_TRUE(DecodeAccessRouteSnapshot(*decoded));
}
TEST(AccessRouteWireTest, NoAuthCannotOfferAnyProxyAuthenticationScheme) {
  for (const auto transport : {mojom::AegisAccessProxyTransport::kHttp,
                               mojom::AegisAccessProxyTransport::kSocks5}) {
    auto decoded = DecodeAccessRouteSnapshot(*NoAuthSnapshot(transport));
    ASSERT_TRUE(decoded);
    const auto route = decoded->snapshot.routes.at("group");
    EXPECT_FALSE(net::AccessProxyRouteAllowsLegacyAuthentication(*route));
    auto controller = base::MakeRefCounted<net::HttpAuthController>(
        net::HttpAuth::AUTH_PROXY, GURL("http://127.0.0.1:12345/"),
        net::NetworkAnonymizationKey(), route->proxy_auth_cache(), nullptr, nullptr);
    net::RestrictAccessProxyAuthentication(*route, *controller);
    for (int value = net::HttpAuth::AUTH_SCHEME_BASIC;
         value < net::HttpAuth::AUTH_SCHEME_MAX; ++value)
      EXPECT_TRUE(controller->IsAuthSchemeDisabled(static_cast<net::HttpAuth::Scheme>(value)));
    EXPECT_FALSE(controller->HaveAuth());
  }
}
TEST(AccessRouteWireTest, LegacyLocalBasicCannotEnableSsoOrAnotherRealm) {
  auto decoded = DecodeAccessRouteSnapshot(*WireSnapshot());
  ASSERT_TRUE(decoded);
  const auto route = decoded->snapshot.routes.at("group");
  ASSERT_TRUE(net::AccessProxyRouteAllowsLegacyAuthentication(*route));
  auto controller = base::MakeRefCounted<net::HttpAuthController>(
      net::HttpAuth::AUTH_PROXY, GURL("http://127.0.0.1:12345/"),
      net::NetworkAnonymizationKey(), route->proxy_auth_cache(), nullptr, nullptr);
  net::RestrictAccessProxyAuthentication(*route, *controller);
  EXPECT_FALSE(controller->IsAuthSchemeDisabled(net::HttpAuth::AUTH_SCHEME_BASIC));
  EXPECT_TRUE(controller->IsAuthSchemeDisabled(net::HttpAuth::AUTH_SCHEME_NTLM));
  EXPECT_TRUE(controller->IsAuthSchemeDisabled(net::HttpAuth::AUTH_SCHEME_NEGOTIATE));
  auto no_auth = DecodeAccessRouteSnapshot(*NoAuthSnapshot(mojom::AegisAccessProxyTransport::kHttp));
  ASSERT_TRUE(no_auth);
  const auto other = no_auth->snapshot.routes.at("group");
  const url::SchemeHostPort proxy(GURL("http://127.0.0.1:12345/"));
  other->proxy_auth_cache()->Add(proxy, net::HttpAuth::AUTH_PROXY, "unrelated",
      net::HttpAuth::AUTH_SCHEME_BASIC, net::NetworkAnonymizationKey(),
      "Basic realm=\"unrelated\"", net::AuthCredentials(u"test-user", u"test-secret"), "/");
  EXPECT_FALSE(net::AccessProxyRouteAllowsLegacyAuthentication(*other));
  other->proxy_auth_cache()->Add(proxy, net::HttpAuth::AUTH_PROXY, "proxy",
      net::HttpAuth::AUTH_SCHEME_BASIC, net::NetworkAnonymizationKey(),
      "Basic realm=\"proxy\"", net::AuthCredentials(u"test-user", std::u16string()), "/");
  EXPECT_FALSE(net::AccessProxyRouteAllowsLegacyAuthentication(*other));
}
TEST(AccessRouteWireTest, CompleteSnapshotInstallsWithPrivateLocalCredentials) {
  auto decoded = DecodeAccessRouteSnapshot(*WireSnapshot());
  ASSERT_TRUE(decoded);
  auto route = decoded->snapshot.routes.at("group");
  const url::SchemeHostPort proxy(GURL("http://127.0.0.1:12345"));
  auto* entry = route->proxy_auth_cache()->Lookup(proxy, net::HttpAuth::AUTH_PROXY,
      "proxy", net::HttpAuth::AUTH_SCHEME_BASIC, net::NetworkAnonymizationKey());
  ASSERT_TRUE(entry);
  EXPECT_TRUE(entry->credentials().Equals(
      net::AuthCredentials(u"local-user", u"local-secret")));
  AccessRouteRegistry registry(decoded->owner, decoded->incarnation);
  ASSERT_TRUE(registry.Install(decoded->sequence, std::move(decoded->snapshot)));
  aegis_access::BrowserOwnedRequestMetadata metadata{"request", decoded->owner,
      aegis_access::RequestAttributionKind::kProfileOnly, {}, {}, std::nullopt};
  EXPECT_EQ(registry.Resolve(metadata, GURL("http://destination.test/"),
      {"incarnation", {1,1,1,1,1}, "registration"}).net_error, net::OK);
}
TEST(AccessRouteWireTest, RejectsNonLoopbackAndDuplicateGroups) {
  auto s = WireSnapshot();
  for (const auto* host : {"example.test", "127.1", "8.8.8.8"}) {
    s->endpoints[0]->numeric_loopback_host = host;
    EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
  }
  s = WireSnapshot();
  s->endpoints.push_back(s->endpoints[0].Clone());
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
}
TEST(AccessRouteWireTest, RejectsMalformedPolicyAndIncompleteGenerations) {
  auto s = WireSnapshot();
  s->rules[0]->destination_host = "DESTINATION.test";
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
  s = WireSnapshot();
  s->common_generations->selection_generation = 1;
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
  s = WireSnapshot();
  s->owner->profile_token.clear();
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
}
TEST(AccessRouteWireTest, RegistrationCannotReplaceLocalCredentials) {
  auto wire = WireSnapshot();
  auto first = DecodeAccessRouteSnapshot(*wire);
  ASSERT_TRUE(first);
  AccessRouteRegistry registry(first->owner, first->incarnation);
  ASSERT_TRUE(registry.Install(first->sequence, std::move(first->snapshot)));
  wire->sequence = 2;
  wire->endpoints[0]->credentials = net::AuthCredentials(u"other", u"changed");
  auto changed = DecodeAccessRouteSnapshot(*wire);
  ASSERT_TRUE(changed);
  EXPECT_FALSE(registry.Install(changed->sequence, std::move(changed->snapshot)));
  wire->endpoints[0]->registration_id = "new-registration";
  wire->endpoints[0]->selection_generation = 2;
  auto replacement = DecodeAccessRouteSnapshot(*wire);
  ASSERT_TRUE(replacement);
  EXPECT_TRUE(registry.Install(replacement->sequence, std::move(replacement->snapshot)));
}
TEST(AccessRouteWireTest, EmptyPolicyRequiresExplicitValidSnapshot) {
  auto s = WireSnapshot();
  s->rules.clear();
  s->endpoints.clear();
  auto decoded = DecodeAccessRouteSnapshot(*s);
  ASSERT_TRUE(decoded);
  AccessRouteRegistry registry(decoded->owner, decoded->incarnation);
  ASSERT_TRUE(registry.Install(decoded->sequence, std::move(decoded->snapshot)));
  s->sequence = 0;
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*s));
}
TEST(AccessRouteWireTest, UnavailableGroupPreservesProxyPolicyWithoutEndpoint) {
  auto wire = WireSnapshot();
  wire->endpoints.clear();
  wire->unavailable_groups.push_back(
      mojom::AegisAccessUnavailableGroup::New("group", 0));
  auto decoded = DecodeAccessRouteSnapshot(*wire);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded->snapshot.routes.empty());
  EXPECT_EQ(decoded->snapshot.unavailable_groups.at("group"), 0u);
  AccessRouteRegistry registry(decoded->owner, decoded->incarnation);
  ASSERT_TRUE(registry.Install(decoded->sequence, std::move(decoded->snapshot)));
  aegis_access::BrowserOwnedRequestMetadata metadata{"request", decoded->owner,
      aegis_access::RequestAttributionKind::kProfileOnly, {}, {}, std::nullopt};
  EXPECT_EQ(registry.Resolve(metadata, GURL("http://destination.test/"),
      {"incarnation", {1,1,0,1,1}, {}}).net_error,
      net::ERR_PROXY_CONNECTION_FAILED);
  EXPECT_EQ(registry.Resolve(metadata, GURL("http://unmatched.test/"),
      {"incarnation", {1,1,0,1,1}, {}}).net_error, net::OK);
}
TEST(AccessRouteWireTest, UnavailableGroupMustBeExplicitUniqueAndNotReady) {
  auto wire = WireSnapshot();
  wire->endpoints.clear();
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  wire->unavailable_groups.push_back(
      mojom::AegisAccessUnavailableGroup::New("group", 0));
  wire->unavailable_groups.push_back(wire->unavailable_groups.front().Clone());
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  wire = WireSnapshot();
  wire->unavailable_groups.push_back(
      mojom::AegisAccessUnavailableGroup::New("group", 1));
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
  wire = WireSnapshot();
  wire->unavailable_groups.push_back(
      mojom::AegisAccessUnavailableGroup::New("", 0));
  EXPECT_FALSE(DecodeAccessRouteSnapshot(*wire));
}
TEST(AccessRouteWireTest, WebsiteCdnIsIndependentlyMatchedAgainstBoundTopSite) {
  auto wire = WireSnapshot();
  wire->rules[0]->scope = mojom::AegisAccessRuleScope::kWebsite;
  wire->rules[0]->destination_host = "pinterest.com";
  wire->rules[0]->top_level_site = "https://pinterest.com";
  auto decoded = DecodeAccessRouteSnapshot(*wire);
  ASSERT_TRUE(decoded);
  const auto owner = decoded->owner;
  AccessRouteRegistry registry(owner, decoded->incarnation);
  ASSERT_TRUE(registry.Install(decoded->sequence, std::move(decoded->snapshot)));
  aegis_access::BrowserOwnedRequestMetadata metadata{"resource", owner,
      aegis_access::RequestAttributionKind::kDocument, "document", {},
      net::SchemefulSite(GURL("https://www.pinterest.com/"))};
  const GURL resource("http://s.pinimg.com/app.js");
  auto result = registry.Resolve(metadata, resource,
      {"incarnation", {1,1,1,1,1}, "registration"});
  EXPECT_EQ(result.net_error, net::OK);
  EXPECT_TRUE(result.route);
  metadata.top_frame_site = net::SchemefulSite(GURL("https://unrelated.test/"));
  result = registry.Resolve(metadata, resource,
      {"incarnation", {1,1,0,1,1}, {}});
  EXPECT_EQ(result.net_error, net::OK);
  EXPECT_FALSE(result.route);
  EXPECT_NE(registry.Resolve(metadata, resource,
      {"incarnation", {1,1,1,1,1}, "registration"}).net_error, net::OK);
}

}  // namespace
}  // namespace network
