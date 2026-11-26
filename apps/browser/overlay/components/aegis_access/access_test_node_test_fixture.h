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

// Matches the iOS vless.raw.reality fixture with a core-valid key size.
inline constexpr std::string_view kSyntheticPinnedRealityVisionShare =
    "vless://00000000-0000-4000-8000-000000000001@192.0.2.10:443?type=tcp&s"
    "ecurity=reality&flow=xtls-rprx-vision&sni=cert.example.com&pbk=AAECAwQ"
    "FBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&sid=01020304&spx=%2Ftest#syntheti"
    "c-reality";

// Matches the iOS vless.ws.tls fixture, including opaque escaped bytes.
inline constexpr std::string_view kSyntheticWebSocketTlsQueryShare =
    "vless://00000000-0000-4000-8000-000000000002@192.0.2.20:8443"
    "?type=ws&security=tls&host=ws.example.com&fp=chrome&sni=cert.example.com"
    "&path=%2Fws%3Ftoken%3D%2574est-value%26opaque%3D%25E9"
    "&encryption=none&alpn=http%2F1.1#synthetic-ws";

inline constexpr std::string_view
    kSyntheticWebSocketTlsUnknownFingerprintShare =
        "vless://00000000-0000-4000-8000-000000000002@192.0.2.20:8443"
        "?type=ws&security=tls&host=ws.example.com&fp=synthetic_unknown"
        "&sni=cert.example.com&path=%2Fws&encryption=none";

inline constexpr std::string_view kSyntheticTcpTlsVisionShare =
    "vless://550e8400-e29b-41d4-a716-446655440000@192.0.2.30:443"
    "?encryption=none&flow=xtls-rprx-vision&fp=chrome&headerType=none"
    "&host=metadata.example&security=tls&sni=tls.example&type=tcp#synthetic-"
    "tcp";

// The public-key field encodes 32 repetitions of 0x42, as in the iOS test.
inline constexpr std::string_view kSyntheticGrpcRealityShare =
    "vless://6ba7b810-9dad-41d1-80b4-00c04fd430c8@[2001:db8::30]:8443"
    "?encryption=none&fp=chrome&path=%2Fsample%2FTun"
    "&pbk=QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI"
    "&security=reality&serviceName=%2Fsample%2FTun&sid=a1b2c3d4"
    "&sni=grpc.example&type=grpc#synthetic-grpc";

}  // namespace aegis_access::test

#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_TEST_FIXTURE_H_
