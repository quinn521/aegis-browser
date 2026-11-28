// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_native_config.h"

#include "base/json/json_reader.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
using Protocol = aegis_access::AccessInboundProtocolPreference;
using Mode = aegis_access::AccessInboundPortMode;
using Transport = aegis_access::RegisteredProxyTransport;
constexpr auto kExtended =
    aegis_access::AccessTestNodeImportMode::kAllowExtendedVless;
aegis_access::AccessTestNode Node() {
  return *aegis_access::ParseAccessTestNodeShare(
              aegis_access::test::kSyntheticPinnedRealityVisionShare)
              .node;
}

TEST(AccessTestNodeNativeConfigTest, SocksUsesSameSocketPortZeroNoAuthAndKeepsUpstreamIndependent) {
  auto node = Node();
  node.port = 1080;
  node.short_id.clear();
  node.fingerprint = "firefox";
  node.spider_x = "/synthetic?value=%22";
  node.advertises_x25519_mlkem768 = true;
  EXPECT_EQ(ValidateAccessTestNodeNative(node), node);
  EXPECT_EQ(ValidateAccessTestNodeNative(
                node, AccessTestNodeNativeValidation::kFreshDevImport),
            node);
  const auto encoded = BuildAccessTestNodeCoreConfiguration(node, {}, Transport::kSocks5);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->FindStringByDottedPath("log.loglevel"));
  EXPECT_EQ(*root->FindStringByDottedPath("log.loglevel"), "none");
  const auto* inbounds = root->FindList("inbounds");
  ASSERT_TRUE(inbounds);
  ASSERT_EQ(inbounds->size(), 1u);
  const auto& inbound = inbounds->front().GetDict();
  EXPECT_EQ(*inbound.FindString("listen"), "127.0.0.1");
  EXPECT_EQ(*inbound.FindString("protocol"), "socks");
  EXPECT_EQ(*inbound.FindString("port"), "0");
  EXPECT_EQ(*inbound.FindStringByDottedPath("settings.auth"), "noauth");
  EXPECT_EQ(inbound.FindBoolByDottedPath("settings.udp"), false);
  EXPECT_FALSE(inbound.FindByDottedPath("settings.accounts"));
  const auto* outbounds = root->FindList("outbounds");
  ASSERT_TRUE(outbounds);
  ASSERT_EQ(outbounds->size(), 1u);
  const auto& outbound = outbounds->front().GetDict();
  const auto& server = outbound.FindListByDottedPath("settings.vnext")->front().GetDict();
  EXPECT_EQ(server.FindInt("port"), 1080);
  EXPECT_EQ(*server.FindString("address"), node.address);
  const auto& user = server.FindList("users")->front().GetDict();
  EXPECT_EQ(*user.FindString("id"), node.user_id);
  EXPECT_EQ(*user.FindString("flow"), "xtls-rprx-vision");
  const auto* reality = outbound.FindDictByDottedPath("streamSettings.realitySettings");
  ASSERT_TRUE(reality);
  EXPECT_EQ(*reality->FindString("fingerprint"), "firefox");
  EXPECT_EQ(*reality->FindString("shortId"), "");
  EXPECT_EQ(*reality->FindString("publicKey"), node.public_key);
  EXPECT_EQ(*reality->FindString("spiderX"), node.spider_x);
  EXPECT_FALSE(reality->contains("support-x25519mlkem768"));
  EXPECT_FALSE(root->contains("dns"));
  EXPECT_FALSE(root->contains("routing"));
}

TEST(AccessTestNodeNativeConfigTest, HttpHasNoLocalAccountsAndFixedPortDoesNotChangeNodePort) {
  const auto node = Node();
  const auto encoded = BuildAccessTestNodeCoreConfiguration(node,
      {Protocol::kHttp, Mode::kInternalFixed, 1080}, Transport::kHttp);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  const auto& inbound = root->FindList("inbounds")->front().GetDict();
  EXPECT_EQ(inbound.FindInt("port"), 1080);
  EXPECT_EQ(*inbound.FindString("protocol"), "http");
  ASSERT_TRUE(inbound.FindDict("settings"));
  EXPECT_TRUE(inbound.FindDict("settings")->empty());
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  EXPECT_EQ(outbound.FindListByDottedPath("settings.vnext")->front().GetDict().FindInt("port"), node.port);
  const auto socks = BuildAccessTestNodeCoreConfiguration(node,
      {Protocol::kSocks5, Mode::kInternalFixed, 10806}, Transport::kSocks5);
  ASSERT_TRUE(socks);
  const auto socks_root = base::JSONReader::ReadDict(*socks, base::JSON_PARSE_RFC);
  ASSERT_TRUE(socks_root);
  EXPECT_EQ(socks_root->FindList("inbounds")->front().GetDict().FindInt("port"), 10806);
}

TEST(AccessTestNodeNativeConfigTest, RejectsProtocolSubstitutionForbiddenPortsAndInvalidNativeNode) {
  const auto node = Node();
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(node,
      {Protocol::kSocks5, Mode::kInternalFixed, 1080}, Transport::kSocks5));
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(node,
      {Protocol::kSocks5}, Transport::kHttp));
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(node,
      {Protocol::kHttp}, Transport::kSocks5));
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(node, {}, Transport::kInvalid));
  auto invalid = node;
  invalid.port = 0;
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(invalid, {}, Transport::kSocks5));
  invalid = node;
  invalid.spider_x = "/" + std::string(8192, 'x');
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(invalid, {}, Transport::kSocks5));
  invalid = node;
  invalid.address.push_back('\0');
  EXPECT_FALSE(ValidateAccessTestNodeNative(invalid));
}

TEST(AccessTestNodeNativeConfigTest, WebSocketUsesVerifiedTlsAndNoVisionFlow) {
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticWebSocketTlsShare,
      aegis_access::AccessTestNodeImportMode::kAllowWebSocketTls);
  ASSERT_TRUE(parsed.node);
  const auto encoded =
      BuildAccessTestNodeCoreConfiguration(*parsed.node, {}, Transport::kSocks5);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  EXPECT_EQ(*outbound.FindStringByDottedPath("streamSettings.network"), "ws");
  EXPECT_EQ(*outbound.FindStringByDottedPath("streamSettings.security"), "tls");
  EXPECT_EQ(outbound.FindBoolByDottedPath("streamSettings.tlsSettings.allowInsecure"), false);
  EXPECT_EQ(*outbound.FindStringByDottedPath("streamSettings.tlsSettings.serverName"), "tls.example.test");
  EXPECT_EQ(*outbound.FindStringByDottedPath("streamSettings.wsSettings.host"), "edge.example.test");
  EXPECT_EQ(*outbound.FindStringByDottedPath("streamSettings.wsSettings.path"), "/test");
  EXPECT_FALSE(outbound.FindByDottedPath("streamSettings.realitySettings"));
  const auto& server = outbound.FindListByDottedPath("settings.vnext")->front().GetDict();
  EXPECT_EQ(server.FindInt("port"), 8443);
  EXPECT_FALSE(server.FindList("users")->front().GetDict().contains("flow"));
  auto mixed = *parsed.node;
  mixed.public_key = "mixed";
  EXPECT_FALSE(ValidateAccessTestNodeNative(mixed));
}

TEST(AccessTestNodeNativeConfigTest, WebSocketKeepsExplicitAlpnAndWireQuery) {
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticWebSocketTlsQueryShare, kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(*parsed.node), parsed.node);
  const auto encoded =
      BuildAccessTestNodeCoreConfiguration(*parsed.node, {}, Transport::kHttp);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->FindList("outbounds"));
  ASSERT_EQ(root->FindList("outbounds")->size(), 1u);
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  const auto* tls = outbound.FindDictByDottedPath("streamSettings.tlsSettings");
  ASSERT_TRUE(tls);
  EXPECT_EQ(tls->FindBool("allowInsecure"), false);
  EXPECT_EQ(tls->FindBool("useSystemCA"), true);
  auto* alpn = tls->FindList("alpn");
  ASSERT_TRUE(alpn);
  ASSERT_EQ(alpn->size(), 1u);
  EXPECT_EQ(alpn->front().GetString(), "http/1.1");
  const auto* path =
      outbound.FindStringByDottedPath("streamSettings.wsSettings.path");
  ASSERT_TRUE(path);
  EXPECT_EQ(*path, "/ws?token=%74est-value&opaque=%E9");
  EXPECT_EQ(encoded->find("vless://"), std::string::npos);
}

TEST(AccessTestNodeNativeConfigTest, RestoredV2WebSocketKeepsLegacyConfig) {
  auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticWebSocketTlsShare, kExtended);
  ASSERT_TRUE(parsed.node && parsed.node->web_socket_tls);
  auto& node = *parsed.node;
  node.server_name = "Localhost";
  node.fingerprint = "Legacy_FP-1";
  node.web_socket_tls->host = "Edge.";
  node.web_socket_tls->path = "//path";
  EXPECT_FALSE(
      BuildAccessTestNodeCoreConfiguration(node, {}, Transport::kHttp));
  node.web_socket_tls->restored_from_v2 = true;
  const auto encoded =
      BuildAccessTestNodeCoreConfiguration(node, {}, Transport::kHttp);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  const auto* outbounds = root->FindList("outbounds");
  ASSERT_TRUE(outbounds);
  ASSERT_EQ(outbounds->size(), 1u);
  const auto& outbound = outbounds->front().GetDict();
  const auto* tls = outbound.FindDictByDottedPath("streamSettings.tlsSettings");
  ASSERT_TRUE(tls && tls->FindString("serverName") &&
              tls->FindString("fingerprint"));
  EXPECT_EQ(*tls->FindString("serverName"), "Localhost");
  EXPECT_EQ(*tls->FindString("fingerprint"), "Legacy_FP-1");
  EXPECT_EQ(tls->FindBool("allowInsecure"), false);
  EXPECT_FALSE(tls->contains("alpn"));
  EXPECT_FALSE(tls->contains("useSystemCA"));
  const auto* ws = outbound.FindDictByDottedPath("streamSettings.wsSettings");
  ASSERT_TRUE(ws && ws->FindString("host") && ws->FindString("path"));
  EXPECT_EQ(*ws->FindString("host"), "Edge.");
  EXPECT_EQ(*ws->FindString("path"), "//path");
}

TEST(AccessTestNodeNativeConfigTest, StoredRealityRetainsV1Grammar) {
  for (const auto share : {
           aegis_access::test::
               kSyntheticTcpRealityVisionInvalidFingerprintShare,
           aegis_access::test::kSyntheticTcpRealityVisionInvalidPublicKeyShare,
           aegis_access::test::kSyntheticTcpRealityVisionOddShortIdShare,
           aegis_access::test::kSyntheticTcpRealityVisionUnsafeSpiderXShare,
       }) {
    const auto legacy = aegis_access::ParseAccessTestNodeShare(share);
    ASSERT_TRUE(legacy.node);
    EXPECT_FALSE(ValidateAccessTestNodeNative(
        *legacy.node, AccessTestNodeNativeValidation::kFreshDevImport));
    const auto encoded = BuildAccessTestNodeCoreConfiguration(*legacy.node, {},
                                                              Transport::kHttp);
    ASSERT_TRUE(encoded);
    const auto root =
        base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
    ASSERT_TRUE(root && root->FindList("outbounds"));
    ASSERT_EQ(root->FindList("outbounds")->size(), 1u);
    const auto* reality =
        root->FindList("outbounds")
            ->front()
            .GetDict()
            .FindDictByDottedPath("streamSettings.realitySettings");
    ASSERT_TRUE(reality && reality->FindString("fingerprint") &&
                reality->FindString("publicKey") &&
                reality->FindString("shortId") &&
                reality->FindString("spiderX"));
    EXPECT_EQ(*reality->FindString("fingerprint"), legacy.node->fingerprint);
    EXPECT_EQ(*reality->FindString("publicKey"), legacy.node->public_key);
    EXPECT_EQ(*reality->FindString("shortId"), legacy.node->short_id);
    EXPECT_EQ(*reality->FindString("spiderX"), legacy.node->spider_x);
  }
}

TEST(AccessTestNodeNativeConfigTest,
     DevRealityRevalidationEscapesNestedFieldUri) {
  auto node = Node();
  node.spider_x = "/manual?next=https://callback.example";
  EXPECT_EQ(ValidateAccessTestNodeNative(
                node, AccessTestNodeNativeValidation::kFreshDevImport),
            node);
}

TEST(AccessTestNodeNativeConfigTest, StoredV1SpiderXAtShareLimitRevalidates) {
  const auto node = Node();
  std::string share = "vless://" + node.user_id + "@" + node.address +
                      ":443?sni=" + node.server_name +
                      "&type=tcp&security=reality&flow=xtls-rprx-vision&pbk=" +
                      node.public_key + "&sid=" + node.short_id +
                      "&spx=/legacy?next=https://assets.example/";
  constexpr auto kMax = aegis_access::kMaxAccessTestNodeShareBytes;
  ASSERT_LT(share.size(), kMax);
  share.append(kMax - share.size(), 'x');
  const auto parsed = aegis_access::ParseAccessTestNodeShare(share);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(*parsed.node), parsed.node);
  EXPECT_TRUE(
      BuildAccessTestNodeCoreConfiguration(*parsed.node, {}, Transport::kHttp));
}

TEST(AccessTestNodeNativeConfigTest, StrictWebSocketAtShareLimitRevalidates) {
  std::string share =
      "vless://00000000-0000-4000-8000-000000000002@192.0.2.20:8443"
      "?sni=cert.example.com&type=ws&security=tls"
      "&path=/ws?next=https://assets.example/";
  constexpr auto kMax = aegis_access::kMaxAccessTestNodeShareBytes;
  while (share.size() + 3 <= kMax) {
    share += "%26";
  }
  share.append(kMax - share.size(), 'q');
  const auto parsed = aegis_access::ParseAccessTestNodeShare(share, kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(
                *parsed.node, AccessTestNodeNativeValidation::kFreshDevImport),
            parsed.node);
  EXPECT_TRUE(
      BuildAccessTestNodeCoreConfiguration(*parsed.node, {}, Transport::kHttp));
}

TEST(AccessTestNodeNativeConfigTest, DevRealityCanonicalShareCanGrowPastLimit) {
  const auto node = Node();
  std::string share =
      "vless://" + node.user_id + "@" + node.address +
      ":443?sni=" + node.server_name +
      "&security=reality&flow=xtls-rprx-vision&pbk=" + node.public_key +
      "&sid=" + node.short_id + "&spx=/strict?next=https%3A//assets.example/";
  constexpr auto kMax = aegis_access::kMaxExtendedAccessTestNodeShareBytes;
  ASSERT_LT(share.size(), kMax);
  share.append(kMax - share.size(), 'x');
  const auto parsed = aegis_access::ParseAccessTestNodeShare(share, kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(
                *parsed.node, AccessTestNodeNativeValidation::kFreshDevImport),
            parsed.node);
  share.push_back('x');
  EXPECT_FALSE(aegis_access::ParseAccessTestNodeShare(share, kExtended).node);
}

TEST(AccessTestNodeNativeConfigTest,
     RejectsExtendedModelsWithInvalidDialAddress) {
  for (const auto share : {aegis_access::test::kSyntheticTcpTlsVisionShare,
                           aegis_access::test::kSyntheticGrpcRealityShare}) {
    auto parsed = aegis_access::ParseAccessTestNodeShare(share, kExtended);
    ASSERT_TRUE(parsed.node);
    parsed.node->address = "single-label";
    EXPECT_FALSE(ValidateAccessTestNodeNative(*parsed.node));
    EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(*parsed.node, {},
                                                      Transport::kHttp));
  }
}

TEST(AccessTestNodeNativeConfigTest, WebSocketRevalidatesLongRequestTarget) {
  const std::string path = "/ws" + std::string(2045, '/');
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      "vless://00000000-0000-4000-8000-000000000002@192.0.2.20:8443"
      "?type=ws&security=tls&sni=cert.example.com&path=" +
          path,
      kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(*parsed.node), parsed.node);
  const auto encoded =
      BuildAccessTestNodeCoreConfiguration(*parsed.node, {}, Transport::kHttp);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->FindList("outbounds"));
  ASSERT_EQ(root->FindList("outbounds")->size(), 1u);
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  const auto* wire_path =
      outbound.FindStringByDottedPath("streamSettings.wsSettings.path");
  ASSERT_TRUE(wire_path);
  EXPECT_EQ(*wire_path, path);
  EXPECT_FALSE(
      outbound.FindByDottedPath("streamSettings.tlsSettings.fingerprint"));
  EXPECT_FALSE(outbound.FindByDottedPath("streamSettings.tlsSettings.alpn"));
}

TEST(AccessTestNodeNativeConfigTest, TcpTlsUsesVisionAndTheExplicitServerName) {
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticTcpTlsVisionShare, kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(*parsed.node), parsed.node);
  const auto encoded = BuildAccessTestNodeCoreConfiguration(*parsed.node, {},
                                                            Transport::kSocks5);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->FindList("outbounds"));
  ASSERT_EQ(root->FindList("outbounds")->size(), 1u);
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  const auto* network =
      outbound.FindStringByDottedPath("streamSettings.network");
  const auto* security =
      outbound.FindStringByDottedPath("streamSettings.security");
  ASSERT_TRUE(network && security);
  EXPECT_EQ(*network, "tcp");
  EXPECT_EQ(*security, "tls");
  const auto* tls = outbound.FindDictByDottedPath("streamSettings.tlsSettings");
  ASSERT_TRUE(tls && tls->FindString("serverName"));
  EXPECT_EQ(*tls->FindString("serverName"), "tls.example");
  EXPECT_EQ(tls->FindBool("allowInsecure"), false);
  EXPECT_EQ(tls->FindBool("useSystemCA"), true);
  EXPECT_FALSE(tls->contains("alpn"));
  EXPECT_FALSE(outbound.FindByDottedPath("streamSettings.wsSettings"));
  EXPECT_FALSE(outbound.FindByDottedPath("streamSettings.realitySettings"));
  const auto* servers = outbound.FindListByDottedPath("settings.vnext");
  ASSERT_TRUE(servers);
  ASSERT_EQ(servers->size(), 1u);
  const auto* users = servers->front().GetDict().FindList("users");
  ASSERT_TRUE(users);
  ASSERT_EQ(users->size(), 1u);
  const auto* flow = users->front().GetDict().FindString("flow");
  ASSERT_TRUE(flow);
  EXPECT_EQ(*flow, "xtls-rprx-vision");
  EXPECT_EQ(encoded->find("metadata.example"), std::string::npos);
}

TEST(AccessTestNodeNativeConfigTest, GrpcRealityKeepsServiceNameWithoutVision) {
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticGrpcRealityShare, kExtended);
  ASSERT_TRUE(parsed.node);
  EXPECT_EQ(ValidateAccessTestNodeNative(*parsed.node), parsed.node);
  const auto encoded = BuildAccessTestNodeCoreConfiguration(*parsed.node, {},
                                                            Transport::kSocks5);
  ASSERT_TRUE(encoded);
  const auto root = base::JSONReader::ReadDict(*encoded, base::JSON_PARSE_RFC);
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->FindList("outbounds"));
  ASSERT_EQ(root->FindList("outbounds")->size(), 1u);
  const auto& outbound = root->FindList("outbounds")->front().GetDict();
  const auto* network =
      outbound.FindStringByDottedPath("streamSettings.network");
  const auto* security =
      outbound.FindStringByDottedPath("streamSettings.security");
  const auto* service = outbound.FindStringByDottedPath(
      "streamSettings.grpcSettings.serviceName");
  ASSERT_TRUE(network && security && service);
  EXPECT_EQ(*network, "grpc");
  EXPECT_EQ(*security, "reality");
  EXPECT_EQ(*service, "/sample/Tun");
  const auto* reality =
      outbound.FindDictByDottedPath("streamSettings.realitySettings");
  ASSERT_TRUE(reality && reality->FindString("publicKey") &&
              reality->FindString("shortId"));
  EXPECT_EQ(*reality->FindString("publicKey"), parsed.node->public_key);
  EXPECT_EQ(*reality->FindString("shortId"), "a1b2c3d4");
  EXPECT_FALSE(reality->contains("spiderX"));
  EXPECT_FALSE(outbound.FindByDottedPath("streamSettings.tlsSettings"));
  const auto* servers = outbound.FindListByDottedPath("settings.vnext");
  ASSERT_TRUE(servers);
  ASSERT_EQ(servers->size(), 1u);
  const auto* users = servers->front().GetDict().FindList("users");
  ASSERT_TRUE(users);
  ASSERT_EQ(users->size(), 1u);
  EXPECT_FALSE(users->front().GetDict().contains("flow"));
}

TEST(AccessTestNodeNativeConfigTest,
     RejectsFabricatedMixedAndUnsafeNativeNodes) {
  auto ws = aegis_access::ParseAccessTestNodeShare(
                aegis_access::test::kSyntheticWebSocketTlsQueryShare, kExtended)
                .node;
  auto tcp = aegis_access::ParseAccessTestNodeShare(
                 aegis_access::test::kSyntheticTcpTlsVisionShare, kExtended)
                 .node;
  auto grpc = aegis_access::ParseAccessTestNodeShare(
                  aegis_access::test::kSyntheticGrpcRealityShare, kExtended)
                  .node;
  ASSERT_TRUE(ws && tcp && grpc);
  ws->web_socket_tls->alpn = "h2";
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(*ws, {}, Transport::kHttp));
  ws->web_socket_tls->alpn = "http/1.1";
  ws->web_socket_tls->path = "/test?%65d=512";
  EXPECT_FALSE(BuildAccessTestNodeCoreConfiguration(*ws, {}, Transport::kHttp));
  tcp->short_id = "abcd";
  EXPECT_FALSE(
      BuildAccessTestNodeCoreConfiguration(*tcp, {}, Transport::kHttp));
  grpc->tcp_tls_vision = true;
  EXPECT_FALSE(
      BuildAccessTestNodeCoreConfiguration(*grpc, {}, Transport::kHttp));
  grpc->tcp_tls_vision = false;
  grpc->spider_x = "/test";
  EXPECT_FALSE(
      BuildAccessTestNodeCoreConfiguration(*grpc, {}, Transport::kHttp));
}
}  // namespace
}  // namespace aegis::access
