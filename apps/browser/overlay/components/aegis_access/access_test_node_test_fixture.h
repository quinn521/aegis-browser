// Copyright 2026 GCSA
#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_TEST_FIXTURE_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_TEST_FIXTURE_H_

#include <string_view>

namespace aegis_access::test {

// Matches the iOS AccessLocalV2RayConfigTests TEST-NET node.
inline constexpr std::string_view kSyntheticTestNodeShare =
    "vless://550e8400-e29b-41d4-a716-446655440000@198.51.100.10:443"
    "?flow=xtls-rprx-vision&security=reality&encryption=none&type=tcp"
    "&sni=www.example.com&fp=chrome"
    "&pbk=abcdefghijklmnopqrstuvwxyzABCDEFG&sid=abcd1234"
    "&headerType=none#demo-reality-vision";

inline constexpr std::string_view kSyntheticWebSocketTlsShare =
    "vless://550e8400-e29b-41d4-a716-446655440000@192.0.2.8:8443"
    "?security=tls&type=ws&host=edge.example.test&fp=chrome"
    "&sni=tls.example.test&path=%2Ftest&encryption=none&alpn=#ignored";

}  // namespace aegis_access::test

#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_TEST_FIXTURE_H_
