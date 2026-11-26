// Copyright 2026 GCSA
#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aegis_access {

inline constexpr size_t kMaxAccessTestNodeShareBytes = 4096;

enum class AccessTestNodeError {
  kNone,
  kInvalidShare,
  kUnsupportedOutbound,
};

enum class AccessTestNodeImportMode {
  kRealityOnly,
  kAllowWebSocketTls,
  kAllowExtendedVless,
};

struct AccessTestNodeWebSocketTls {
  std::string host;
  std::string path;
  std::string alpn = {};

  bool operator==(const AccessTestNodeWebSocketTls&) const = default;
};

// Native-only credentials. Callers must keep these out of WebUI replies,
// preferences, exports, and diagnostics. The raw URI is never retained.
struct AccessTestNode {
  std::string address;
  uint16_t port;
  std::string user_id;
  std::string server_name;
  std::string public_key;
  std::string short_id;
  std::string fingerprint;
  std::string spider_x;
  // Preserve 3X-UI's advertised share-format capability. It is not a request
  // to override the fingerprint, force a TLS group, or claim negotiation.
  // Runtime admission is based on the exact core and official JSON semantics.
  bool advertises_x25519_mlkem768 = false;
  std::optional<AccessTestNodeWebSocketTls> web_socket_tls = std::nullopt;
  bool tcp_tls_vision = false;
  std::optional<std::string> grpc_reality_service_name = std::nullopt;

  bool operator==(const AccessTestNode&) const = default;
};

struct AccessTestNodeParseResult {
  AccessTestNodeError error;
  std::optional<AccessTestNode> node;
};

// Accepts one VLESS/TCP/REALITY/Vision share by default. Admitted macOS DEV
// import may also select WS/TLS, TCP/TLS/Vision, or gRPC/REALITY explicitly.
// The fragment is never retained.
AccessTestNodeParseResult ParseAccessTestNodeShare(
    std::string_view share,
    AccessTestNodeImportMode mode = AccessTestNodeImportMode::kRealityOnly);

}  // namespace aegis_access

#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_TEST_NODE_H_
