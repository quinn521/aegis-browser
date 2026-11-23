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
aegis_access::AccessTestNode Node() {
  return *aegis_access::ParseAccessTestNodeShare(aegis_access::test::kSyntheticTestNodeShare).node;
}

TEST(AccessTestNodeNativeConfigTest, SocksUsesSameSocketPortZeroNoAuthAndKeepsUpstreamIndependent) {
  auto node = Node();
  node.port = 1080;
  node.short_id.clear();
  node.fingerprint = "firefox";
  node.spider_x = "/synthetic?value=%22";
  node.advertises_x25519_mlkem768 = true;
  EXPECT_EQ(ValidateAccessTestNodeNative(node), node);
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
}  // namespace
}  // namespace aegis::access
