// Copyright 2026 GCSA
#include "components/aegis_access/access_test_node.h"

#include <cassert>
#include <iostream>
#include <string>
#include <utility>

#include "components/aegis_access/access_test_node_test_fixture.h"

namespace {

using aegis_access::AccessTestNodeError;
using aegis_access::AccessTestNodeImportMode;
using aegis_access::ParseAccessTestNodeShare;
using aegis_access::test::kSyntheticGrpcRealityFingerprintAliasShare;
using aegis_access::test::kSyntheticGrpcRealityInvalidDialAddressShare;
using aegis_access::test::kSyntheticGrpcRealityShare;
using aegis_access::test::kSyntheticGrpcRealitySlashShare;
using aegis_access::test::kSyntheticTcpRealityVisionConcatenatedShare;
using aegis_access::test::kSyntheticTcpRealityVisionInvalidFingerprintShare;
using aegis_access::test::kSyntheticTcpRealityVisionInvalidPublicKeyShare;
using aegis_access::test::kSyntheticTcpRealityVisionMissingFlowShare;
using aegis_access::test::kSyntheticTcpRealityVisionOddShortIdShare;
using aegis_access::test::kSyntheticTcpRealityVisionSlashAndAliasesShare;
using aegis_access::test::kSyntheticTcpRealityVisionUnsafeSpiderXShare;
using aegis_access::test::kSyntheticTcpTlsVisionFingerprintAliasShare;
using aegis_access::test::kSyntheticTcpTlsVisionInvalidDialAddressShare;
using aegis_access::test::kSyntheticTcpTlsVisionShare;
using aegis_access::test::kSyntheticTcpTlsVisionSlashShare;
using aegis_access::test::kSyntheticTestNodeShare;
using aegis_access::test::kSyntheticWebSocketTlsDoubleEncodedDialHostShare;
using aegis_access::test::kSyntheticWebSocketTlsEncodedAuthorityDelimiterShare;
using aegis_access::test::kSyntheticWebSocketTlsEscapedDialHostShare;
using aegis_access::test::kSyntheticWebSocketTlsFingerprintAliasShare;
using aegis_access::test::kSyntheticWebSocketTlsIdnaDialHostShare;
using aegis_access::test::kSyntheticWebSocketTlsInvalidUtf8DialHostShare;
using aegis_access::test::kSyntheticWebSocketTlsQueryShare;
using aegis_access::test::kSyntheticWebSocketTlsShare;
using aegis_access::test::kSyntheticWebSocketTlsSlashShare;
using aegis_access::test::kSyntheticWebSocketTlsUnknownFingerprintShare;

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
  assert(result.node->public_key ==
         "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
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
  constexpr auto kExtended = AccessTestNodeImportMode::kAllowExtendedVless;
  const auto ws = ParseAccessTestNodeShare(kSyntheticWebSocketTlsShare, kDev);
  assert(ws.node && ws.node->web_socket_tls && ws.node->port == 8443);
  assert(ws.node->web_socket_tls->host == "edge.example.test");
  assert(ws.node->web_socket_tls->path == "/test");
  assert(ws.node->server_name == "tls.example.test");
  assert(ws.node->public_key.empty() && ws.node->short_id.empty());
  const auto ws_query_alpn =
      ParseAccessTestNodeShare(kSyntheticWebSocketTlsQueryShare, kExtended);
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

  const auto unknown_ws_fingerprint = ParseAccessTestNodeShare(
      kSyntheticWebSocketTlsUnknownFingerprintShare, kExtended);
  assert(unknown_ws_fingerprint.error == kInvalid &&
         !unknown_ws_fingerprint.node);
  assert(ParseAccessTestNodeShare(kSyntheticWebSocketTlsUnknownFingerprintShare,
                                  kDev)
             .node);
  const auto legacy_path = ReplaceIn(ws_query, "%2Ftest", "%2F%2Fpath");
  const auto legacy_share = ReplaceIn(
      ReplaceIn(ReplaceIn(legacy_path, "host=edge.example.test", "host=Edge."),
                "sni=tls.example.test", "sni=Localhost"),
      "fp=chrome", "fp=Legacy_FP-1");
  const auto legacy = ParseAccessTestNodeShare(legacy_share, kDev);
  assert(legacy.node && legacy.node->web_socket_tls &&
         legacy.node->web_socket_tls->path == "//path");
  assert(legacy.node->server_name == "Localhost" &&
         legacy.node->web_socket_tls->host == "Edge." &&
         legacy.node->fingerprint == "Legacy_FP-1");
  assert(!ParseAccessTestNodeShare(legacy_path, kExtended).node);
  for (const auto* name :
       {"", "chrome", "firefox", "safari", "ios", "android", "edge"}) {
    const auto supported = ParseAccessTestNodeShare(
        ReplaceIn(ws_query, "fp=chrome", std::string("fp=") + name), kExtended);
    assert(supported.node && supported.node->fingerprint == name);
  }
  assert(!ParseAccessTestNodeShare(
              ReplaceIn(ws_query, "fp=chrome", "fp=Chrome"), kExtended)
              .node);
  for (const auto* host : {"edge", "123.123", "edge.example.test:0"}) {
    assert(
        !ParseAccessTestNodeShare(ReplaceIn(ws_query, "host=edge.example.test",
                                            std::string("host=") + host),
                                  kExtended)
             .node);
  }
  assert(
      !ParseAccessTestNodeShare(
           ReplaceIn(ws_query, "sni=tls.example.test", "sni=single"), kExtended)
           .node);
  assert(ParseAccessTestNodeShare(kSyntheticTestNodeShare, kExtended).node ==
         result.node);
  std::string at_share_limit =
      ReplaceIn(WithoutFragment(kSyntheticTestNodeShare), "&type=tcp", "");
  at_share_limit += "&spx=/strict";
  assert(at_share_limit.size() < aegis_access::kMaxAccessTestNodeShareBytes);
  at_share_limit.append(
      aegis_access::kMaxAccessTestNodeShareBytes - at_share_limit.size(), 'x');
  const auto at_limit = ParseAccessTestNodeShare(at_share_limit, kExtended);
  assert(at_limit.node);
  const std::string reconstructed =
      ReplaceIn(at_share_limit, "?flow=", "?type=tcp&flow=");
  assert(ParseAccessTestNodeShare(reconstructed, kExtended).error == kInvalid);
  assert(
      aegis_access::ReparseAccessTestNodeNativeShare(reconstructed, kExtended)
          .node == at_limit.node);
  const auto escaped_host = ParseAccessTestNodeShare(
      kSyntheticWebSocketTlsEscapedDialHostShare, kExtended);
  assert(escaped_host.node && escaped_host.node->address == "route.example.");
  assert(escaped_host.node->web_socket_tls &&
         escaped_host.node->web_socket_tls->path == "/upgrade?tag=%E9");
  assert(!ParseAccessTestNodeShare(kSyntheticWebSocketTlsEscapedDialHostShare,
                                   kDev)
              .node);
  assert(!ParseAccessTestNodeShare(kSyntheticWebSocketTlsIdnaDialHostShare,
                                   kExtended)
              .node);
  const auto idna = ParseAccessTestNodeShare(
      kSyntheticWebSocketTlsIdnaDialHostShare, kExtended,
      [](std::string_view decoded) -> std::optional<std::string> {
        assert(decoded ==
               "b\xC3\xBC"
               "cher.example");
        return "xn--bcher-kva.example";
      });
  assert(idna.node && idna.node->address == "xn--bcher-kva.example");
  assert(idna.node->web_socket_tls &&
         idna.node->web_socket_tls->host == "ws.example.com.:8443" &&
         idna.node->web_socket_tls->path ==
             "/upgrade?next=%68ttps://assets.example/");
  assert(!ParseAccessTestNodeShare(
              kSyntheticWebSocketTlsIdnaDialHostShare, kExtended,
              [](std::string_view) -> std::optional<std::string> {
                return "route.example:443";
              })
              .node);
  for (const auto share : {kSyntheticWebSocketTlsEncodedAuthorityDelimiterShare,
                           kSyntheticWebSocketTlsDoubleEncodedDialHostShare,
                           kSyntheticWebSocketTlsInvalidUtf8DialHostShare}) {
    const auto rejected = ParseAccessTestNodeShare(share, kExtended);
    assert(rejected.error == kInvalid && !rejected.node);
  }
  for (const auto* encoded :
       {"%3A", "%2F", "%5C", "%23", "%3F", "%5B", "%5D", "%00", "%20", "%ZZ"}) {
    assert(ParseAccessTestNodeShare(
               ReplaceIn(kSyntheticWebSocketTlsEncodedAuthorityDelimiterShare,
                         "%40", encoded),
               kExtended)
               .error == kInvalid);
  }
  const auto missing_vision = ParseAccessTestNodeShare(
      kSyntheticTcpRealityVisionMissingFlowShare, kExtended);
  assert(missing_vision.error == kUnsupported && !missing_vision.node);
  const auto concatenated = ParseAccessTestNodeShare(
      kSyntheticTcpRealityVisionConcatenatedShare, kExtended);
  assert(concatenated.error == kInvalid && !concatenated.node);
  assert(ParseAccessTestNodeShare(kSyntheticTcpRealityVisionMissingFlowShare)
             .node == result.node);
  assert(ParseAccessTestNodeShare(kSyntheticTcpRealityVisionConcatenatedShare)
             .node == result.node);
  const auto escaped_spider_uri = ParseAccessTestNodeShare(
      Replace(
          "fp=chrome",
          "fp=chrome&spx=%2Fmanual%3Fnext%3Dhttps%3A%2F%2Fcallback.example"),
      kExtended);
  assert(escaped_spider_uri.node &&
         escaped_spider_uri.node->spider_x ==
             "/manual?next=https://callback.example");
  assert(ParseAccessTestNodeShare(
             Replace("fp=chrome",
                     "fp=chrome&spx=/manual?next=https://callback.example"),
             kExtended)
             .error == kInvalid);
  for (const auto& [with_slash, plain] : {
           std::pair{kSyntheticTcpRealityVisionSlashAndAliasesShare,
                     kSyntheticTestNodeShare},
           std::pair{kSyntheticWebSocketTlsSlashShare,
                     kSyntheticWebSocketTlsQueryShare},
           std::pair{kSyntheticTcpTlsVisionSlashShare,
                     kSyntheticTcpTlsVisionShare},
           std::pair{kSyntheticGrpcRealitySlashShare,
                     kSyntheticGrpcRealityShare},
       }) {
    const auto parsed = ParseAccessTestNodeShare(with_slash, kExtended);
    assert(parsed.node &&
           parsed.node == ParseAccessTestNodeShare(plain, kExtended).node);
    for (const auto* path : {"//?", "/extra?", "%2F?"}) {
      assert(
          ParseAccessTestNodeShare(ReplaceIn(with_slash, "/?", path), kExtended)
              .error == kInvalid);
    }
  }
  assert(
      ParseAccessTestNodeShare(kSyntheticTcpRealityVisionSlashAndAliasesShare)
          .error == kInvalid);
  assert(ParseAccessTestNodeShare(kSyntheticWebSocketTlsFingerprintAliasShare,
                                  kDev)
             .node == ws.node);
  for (const auto alias : {
           kSyntheticWebSocketTlsFingerprintAliasShare,
           kSyntheticTcpTlsVisionFingerprintAliasShare,
           kSyntheticGrpcRealityFingerprintAliasShare,
       }) {
    const auto invalid = ParseAccessTestNodeShare(alias, kExtended);
    assert(invalid.error == kUnsupported && !invalid.node);
    assert(!ParseAccessTestNodeShare(
                ReplaceIn(alias, "&fingerprint=", "&%66ingerprint="), kExtended)
                .node);
  }
  assert(ParseAccessTestNodeShare(
             ReplaceIn(kSyntheticWebSocketTlsShare, "&sni=", "&server_name="),
             kExtended)
             .error == kUnsupported);
  assert(ParseAccessTestNodeShare(
             ReplaceIn(kSyntheticGrpcRealityShare, "&pbk=", "&publicKey="),
             kExtended)
             .error == kUnsupported);
  for (const auto share : {kSyntheticTcpRealityVisionInvalidFingerprintShare,
                           kSyntheticTcpRealityVisionInvalidPublicKeyShare,
                           kSyntheticTcpRealityVisionOddShortIdShare,
                           kSyntheticTcpRealityVisionUnsafeSpiderXShare}) {
    const auto dev = ParseAccessTestNodeShare(share, kExtended);
    assert(dev.error == kInvalid && !dev.node);
    assert(ParseAccessTestNodeShare(share).node);
  }
  const auto strict_exporter = ParseAccessTestNodeShare(
      Replace("fp=chrome",
              "fp=Chrome&pcks=&support-x25519mlkem768=true"
              "&spx=%2Fmanual%2520path%3Fq%3D%253B%26q%3D2"),
      kExtended);
  assert(strict_exporter.node &&
         strict_exporter.node->fingerprint == "Chrome" &&
         strict_exporter.node->advertises_x25519_mlkem768 &&
         strict_exporter.node->spider_x == "/manual%20path?q=%3B&q=2");
  for (const auto* path : {
           "relative",
           "%2F%2Fother.example",
           "%2Fbad%25ZZ",
           "%2Fbad%00",
           "%2Fpath%3Fx%3D1%3Bc%3D2",
           "%2Fpath%3Fx%3D%25ZZ",
           "%2Fpath%3Fx%3D1%26p",
           "%2Fpath%3Fc%3D",
           "%2Fpath%3F%2574%3D1",
           "%2Fpath%3Fi%3D1",
           "%2Fpath%3Fr%3D1",
       }) {
    assert(!ParseAccessTestNodeShare(
                Replace("fp=chrome", std::string("fp=chrome&spx=") + path),
                kExtended)
                .node);
  }
  for (const auto* name : {"unsafe", "HelloGolang"}) {
    assert(!ParseAccessTestNodeShare(
                Replace("fp=chrome", std::string("fp=") + name), kExtended)
                .node);
  }
  for (const auto share : {kSyntheticTcpTlsVisionInvalidDialAddressShare,
                           kSyntheticGrpcRealityInvalidDialAddressShare}) {
    const auto dev = ParseAccessTestNodeShare(share, kExtended);
    assert(dev.error == kInvalid && !dev.node);
  }
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
