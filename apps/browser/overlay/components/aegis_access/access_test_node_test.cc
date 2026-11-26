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
using aegis_access::test::kSyntheticGrpcRealityShare;
using aegis_access::test::kSyntheticTcpTlsVisionShare;
using aegis_access::test::kSyntheticTestNodeShare;
using aegis_access::test::kSyntheticWebSocketTlsQueryShare;
using aegis_access::test::kSyntheticWebSocketTlsShare;

std::string ReplaceIn(std::string_view input,
                      std::string_view old_value,
                      std::string_view new_value) {
  std::string share(input);
  const auto position = share.find(old_value);
  assert(position != std::string::npos);
  share.replace(position, old_value.size(), new_value);
  return share;
}

std::string Replace(std::string_view old_value, std::string_view new_value) {
  return ReplaceIn(kSyntheticTestNodeShare, old_value, new_value);
}

std::string_view WithoutFragment(std::string_view share) {
  return share.substr(0, share.find('#'));
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
  const auto ws_query_alpn =
      ParseAccessTestNodeShare(kSyntheticWebSocketTlsQueryShare, kDev);
  assert(ws_query_alpn.node && ws_query_alpn.node->web_socket_tls);
  assert(ws_query_alpn.node->web_socket_tls->path ==
         "/ws?token=%74est-value&opaque=%E9");
  assert(ws_query_alpn.node->web_socket_tls->alpn == "http/1.1");
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

  constexpr auto kExtended = AccessTestNodeImportMode::kAllowExtendedVless;
  assert(ParseAccessTestNodeShare(kSyntheticTestNodeShare, kExtended).node ==
         result.node);
  assert(ParseAccessTestNodeShare(kSyntheticWebSocketTlsQueryShare, kExtended)
             .node == ws_query_alpn.node);
  const auto ws_without_fragment =
      WithoutFragment(kSyntheticWebSocketTlsQueryShare);
  const auto default_ws = ParseAccessTestNodeShare(
      ReplaceIn(ReplaceIn(ws_without_fragment, "&host=ws.example.com", ""),
                "&fp=chrome", ""),
      kExtended);
  assert(default_ws.node && default_ws.node->web_socket_tls);
  assert(default_ws.node->web_socket_tls->host == "cert.example.com");
  assert(default_ws.node->fingerprint.empty());
  const auto nested_web_url = ParseAccessTestNodeShare(
      ReplaceIn(ws_query, "%2Ftest",
                "%2Fws%3Fx%3Dhttps%3A%2F%2Fcallback.example%2Fa%3Fq%3D1"),
      kExtended);
  assert(nested_web_url.node && nested_web_url.node->web_socket_tls);
  assert(nested_web_url.node->web_socket_tls->path ==
         "/ws?x=https://callback.example/a?q=1");
  for (const auto* bad_target : {
           "%2F%2Fother.example",
           "%2Ftest%23fragment",
           "%2Ftest%5Cother",
           "%2Ftest%250d%250a",
           "%2Ftest%25broken",
           "%2Ftest%3Fed%3D2048",
           "%2Ftest%3Fx%3D%25E9%26%2565d%3D32",
           "%2Ftest%3Fx%3Dvless%3A%2F%2Fother.example",
           "%2Ftest%3Fx%3Dvless%253A%252F%252Fother.example",
       }) {
    assert(!ParseAccessTestNodeShare(ReplaceIn(ws_query, "%2Ftest", bad_target),
                                     kExtended)
                .node);
  }
  for (const auto* alpn : {"h2", "h2,http%2F1.1", "HTTP%2F1.1"}) {
    assert(!ParseAccessTestNodeShare(
                ReplaceIn(ws_without_fragment, "http%2F1.1", alpn), kExtended)
                .node);
  }

  const auto tcp =
      ParseAccessTestNodeShare(kSyntheticTcpTlsVisionShare, kExtended);
  assert(tcp.node && tcp.node->tcp_tls_vision && !tcp.node->web_socket_tls &&
         !tcp.node->grpc_reality_service_name);
  assert(tcp.node->server_name == "tls.example" &&
         tcp.node->public_key.empty());
  const auto grpc =
      ParseAccessTestNodeShare(kSyntheticGrpcRealityShare, kExtended);
  assert(grpc.node && grpc.node->grpc_reality_service_name == "/sample/Tun" &&
         !grpc.node->tcp_tls_vision && !grpc.node->web_socket_tls);
  assert(grpc.node->address == "2001:db8::30" &&
         grpc.node->short_id == "a1b2c3d4");
  for (const auto share :
       {kSyntheticTcpTlsVisionShare, kSyntheticGrpcRealityShare}) {
    ExpectRejection(share, kUnsupported);
    assert(ParseAccessTestNodeShare(share, kDev).error == kUnsupported);
  }
  const auto tcp_query = WithoutFragment(kSyntheticTcpTlsVisionShare);
  for (const auto* hint :
       {"&allowInsecure=1", "&alpn=h2", "&%66low=xtls-rprx-vision"}) {
    assert(!ParseAccessTestNodeShare(std::string(tcp_query) + hint, kExtended)
                .node);
  }
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(tcp_query, "headerType=none", "headerType=http"),
              kExtended)
              .node);
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(tcp_query, "flow=xtls-rprx-vision", "flow="), kExtended)
              .node);
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(tcp_query, "fp=chrome", "fp=unsafe"), kExtended)
              .node);
  const auto grpc_query = WithoutFragment(kSyntheticGrpcRealityShare);
  for (const auto* hint : {"&flow=xtls-rprx-vision", "&allowInsecure=1",
                           "&mode=multi", "&%73erviceName=%2Fsample%2FTun"}) {
    assert(!ParseAccessTestNodeShare(std::string(grpc_query) + hint, kExtended)
                .node);
  }
  for (const auto& equivalent : {
           ReplaceIn(grpc_query, "&serviceName=%2Fsample%2FTun", ""),
           ReplaceIn(grpc_query, "&path=%2Fsample%2FTun", ""),
       }) {
    assert(ParseAccessTestNodeShare(equivalent, kExtended).node == grpc.node);
  }
  assert(
      !ParseAccessTestNodeShare(
           ReplaceIn(grpc_query, "path=%2Fsample", "path=%2Fother"), kExtended)
           .node);
  assert(!ParseAccessTestNodeShare(ReplaceIn(grpc_query, "&pbk=", "&pbk=x"),
                                   kExtended)
              .node);
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(grpc_query, "sid=a1b2c3d4", "sid=abc"), kExtended)
              .node);
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(grpc_query, "%2Fsample%2FTun", "%2Fsample%2F%2E%2E"),
              kExtended)
              .node);
  assert(!ParseAccessTestNodeShare(std::string(kSyntheticGrpcRealityShare) +
                                       std::string(kSyntheticTcpTlsVisionShare),
                                   kExtended)
              .node);
  assert(
      !ParseAccessTestNodeShare("https://example.test/subscription", kExtended)
           .node);
  std::cout << "PASS: Access test node share and rejection cases\n";
}
