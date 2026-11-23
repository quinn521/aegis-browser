// Copyright 2026 GCSA
#include "components/aegis_access/access_test_node.h"

#include <cassert>
#include <iostream>
#include <string>

#include "components/aegis_access/access_test_node_test_fixture.h"

namespace {

using aegis_access::AccessTestNodeError;
using aegis_access::AccessTestNodeImportMode;
using aegis_access::ParseAccessTestNodeShare;
using aegis_access::test::kSyntheticTestNodeShare;
using aegis_access::test::kSyntheticWebSocketTlsShare;

std::string Replace(std::string_view old_value, std::string_view new_value) {
  std::string share(kSyntheticTestNodeShare);
  const auto position = share.find(old_value);
  assert(position != std::string::npos);
  share.replace(position, old_value.size(), new_value);
  return share;
}

void ExpectRejection(std::string_view share, AccessTestNodeError error) {
  const auto result = ParseAccessTestNodeShare(share);
  assert(result.error == error);
  assert(!result.node);
}

}  // namespace

int main() {
  const auto result = ParseAccessTestNodeShare(kSyntheticTestNodeShare);
  assert(result.error == AccessTestNodeError::kNone);
  assert(result.node);
  assert(result.node->address == "198.51.100.10");
  assert(result.node->port == 443);
  assert(result.node->user_id == "550e8400-e29b-41d4-a716-446655440000");
  assert(result.node->server_name == "www.example.com");
  assert(result.node->public_key == "abcdefghijklmnopqrstuvwxyzABCDEFG");
  assert(result.node->short_id == "abcd1234");
  assert(result.node->fingerprint == "chrome");
  assert(result.node->spider_x.empty());

  // The 1080 restriction belongs to our local SOCKS listener only. Never
  // rewrite or reject a valid upstream node port because of that policy.
  const auto upstream_1080 = ParseAccessTestNodeShare(
      Replace("198.51.100.10:443", "198.51.100.10:1080"));
  assert(upstream_1080.node && upstream_1080.node->port == 1080);

  const auto aliases = ParseAccessTestNodeShare(
      Replace("&sni=www.example.com", "&serverName=www%2eexample.com"));
  assert(aliases.node == result.node);
  const auto wrapped = ParseAccessTestNodeShare(
      " \t" + std::string(kSyntheticTestNodeShare) + "\r\n");
  assert(wrapped.node == result.node);
  const auto fragment =
      ParseAccessTestNodeShare(Replace("#demo-reality-vision", "#anything"));
  assert(fragment.node == result.node);
  const auto empty_exporter_placeholder = ParseAccessTestNodeShare(
      Replace("security=reality", "security=reality&pcks="));
  assert(empty_exporter_placeholder.node == result.node);
  const auto encoded_exporter_placeholder = ParseAccessTestNodeShare(
      Replace("security=reality", "security=reality&%70cks="));
  assert(encoded_exporter_placeholder.node == result.node);
  const auto mlkem_hint = ParseAccessTestNodeShare(
      Replace("security=reality", "security=reality&support-x25519mlkem768=true"));
  auto mlkem_expected = *result.node;
  mlkem_expected.advertises_x25519_mlkem768 = true;
  assert(mlkem_hint.node == mlkem_expected);
  const auto firefox_hint = ParseAccessTestNodeShare(
      Replace("fp=chrome", "fp=firefox&support-x25519mlkem768=true"));
  assert(firefox_hint.node && firefox_hint.node->fingerprint == "firefox" &&
         firefox_hint.node->advertises_x25519_mlkem768);
  const auto mlkem_empty_sid = ParseAccessTestNodeShare(
      Replace("&sid=abcd1234", "&sid=&spx=%2F&support-x25519mlkem768=true"));
  assert(mlkem_empty_sid.node && mlkem_empty_sid.node->short_id.empty() &&
         mlkem_empty_sid.node->spider_x == "/" &&
         mlkem_empty_sid.node->advertises_x25519_mlkem768);
  const auto ipv6 = ParseAccessTestNodeShare(
      Replace("198.51.100.10:443", "[2001:DB8::1]:65535"));
  assert(ipv6.node && ipv6.node->address == "2001:db8::1" &&
         ipv6.node->port == 65535);
  const auto empty_short_id = ParseAccessTestNodeShare(
      Replace("&sid=abcd1234", "&sid=&spx=%2Fpath%3Fa%3Db"));
  assert(empty_short_id.node && empty_short_id.node->short_id.empty() &&
         empty_short_id.node->spider_x == "/path?a=b");

  constexpr auto kInvalid = AccessTestNodeError::kInvalidShare;
  ExpectRejection("", kInvalid);
  ExpectRejection(
      std::string(aegis_access::kMaxAccessTestNodeShareBytes + 1, 'x'),
      kInvalid);
  ExpectRejection(std::string(kSyntheticTestNodeShare) + "\n" +
                      std::string(kSyntheticTestNodeShare),
                  kInvalid);
  ExpectRejection(Replace("550e8400-e29b-41d4-a716-446655440000", "not-a-uuid"),
                  kInvalid);
  ExpectRejection(Replace("@198.51.100.10:443", "@198.51.100.10:0"), kInvalid);
  ExpectRejection(Replace("@198.51.100.10:443", "@198.51.100.10:65536"),
                  kInvalid);
  ExpectRejection(Replace("@198.51.100.10:443", "@198.51.100.10:443/"),
                  kInvalid);
  ExpectRejection(Replace("@198.51.100.10", "@[2001::db8::1]"), kInvalid);
  ExpectRejection(Replace("@198.51.100.10", "@-example.com"), kInvalid);
  ExpectRejection(Replace("pbk=", "pbk=%00"), kInvalid);
  ExpectRejection(Replace("pbk=", "pbk=%"), kInvalid);
  ExpectRejection(Replace("sid=abcd1234", "sid=not-hex"), kInvalid);
  ExpectRejection(Replace("&sid=abcd1234", ""), kInvalid);
  ExpectRejection(Replace("type=tcp", "type=tcp&%74ype=ws"), kInvalid);
  ExpectRejection(Replace("sid=abcd1234", "sid=abcd1234&shortId=abc0"),
                  kInvalid);
  ExpectRejection(Replace("type=tcp", "type=tcp&pcks=&pcks="), kInvalid);
  ExpectRejection(Replace("type=tcp",
      "type=tcp&support-x25519mlkem768=true&%73upport-x25519mlkem768=true"),
      kInvalid);

  constexpr auto kUnsupported = AccessTestNodeError::kUnsupportedOutbound;
  for (const auto* value : {"", "false", "1", "TRUE", "unexpected"}) {
    ExpectRejection(Replace("type=tcp", std::string("type=tcp&support-x25519mlkem768=") + value),
                    kUnsupported);
  }
  ExpectRejection("https://example.test/subscription", kUnsupported);
  ExpectRejection(Replace("type=tcp", "type=ws"), kUnsupported);
  ExpectRejection(Replace("security=reality", "security=tls"), kUnsupported);
  ExpectRejection(Replace("flow=xtls-rprx-vision", "flow=xtls"), kUnsupported);
  ExpectRejection(Replace("type=tcp", "type=tcp&allowInsecure"), kInvalid);
  ExpectRejection(Replace("type=tcp", "type=tcp&%61llowInsecure=false"),
                  kUnsupported);
  ExpectRejection(Replace("type=tcp", "type=tcp&allowInsecure="),
                  kUnsupported);
  ExpectRejection(Replace("type=tcp", "type=tcp&pcks=required-value"),
                  kUnsupported);
  ExpectRejection(Replace("type=tcp", "type=tcp&unknown="),
                  kUnsupported);
  ExpectRejection(Replace("headerType=none", "headerType=http"), kUnsupported);
  ExpectRejection(kSyntheticWebSocketTlsShare, kUnsupported);
  constexpr auto kDev = AccessTestNodeImportMode::kAllowWebSocketTls;
  const auto ws = ParseAccessTestNodeShare(kSyntheticWebSocketTlsShare, kDev);
  assert(ws.node && ws.node->web_socket_tls && ws.node->port == 8443);
  assert(ws.node->web_socket_tls->host == "edge.example.test");
  assert(ws.node->web_socket_tls->path == "/test");
  assert(ws.node->server_name == "tls.example.test");
  assert(ws.node->public_key.empty() && ws.node->short_id.empty());
  assert(ParseAccessTestNodeShare(kSyntheticTestNodeShare, kDev).node ==
         result.node);
  const auto ws_query = kSyntheticWebSocketTlsShare.substr(
      0, kSyntheticWebSocketTlsShare.find('#'));
  for (const auto* parameter : {"&allowInsecure=true", "&flow=xtls-rprx-vision",
                                "&pbk=mixed"}) {
    const auto rejected =
        ParseAccessTestNodeShare(std::string(ws_query) + parameter, kDev);
    assert(rejected.error == kUnsupported && !rejected.node);
  }
  std::string injected_path(ws_query);
  injected_path.replace(injected_path.find("%2Ftest"), 7, "%2Ftest%0d%0a");
  assert(ParseAccessTestNodeShare(injected_path, kDev).error == kInvalid);
  std::cout << "PASS: Access test node share and rejection cases\n";
}
