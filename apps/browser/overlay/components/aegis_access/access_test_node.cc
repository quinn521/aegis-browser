// Copyright 2026 GCSA
#include "components/aegis_access/access_test_node.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <memory>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace aegis_access {
namespace {

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool IsAlphanumeric(char c) {
  return IsDigit(c) || (LowerAscii(c) >= 'a' && LowerAscii(c) <= 'z');
}

int HexValue(char c) {
  if (IsDigit(c)) {
    return c - '0';
  }
  c = LowerAscii(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool IsUuid(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return false;
      }
    } else if (HexValue(text[i]) < 0) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> Unescape(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c == '%') {
      if (i + 2 >= value.size()) {
        return std::nullopt;
      }
      const int high = HexValue(value[i + 1]);
      const int low = HexValue(value[i + 2]);
      if (high < 0 || low < 0) {
        return std::nullopt;
      }
      c = static_cast<char>(high * 16 + low);
      i += 2;
    }
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      return std::nullopt;
    }
    decoded.push_back(c);
  }
  return decoded;
}

bool IsDnsName(std::string_view host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }
  if (host.back() == '.') {
    host.remove_suffix(1);
  }
  while (!host.empty()) {
    const auto separator = host.find('.');
    const auto label = host.substr(0, separator);
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-' || !std::ranges::all_of(label, [](char c) {
          return IsAlphanumeric(c) || c == '-';
        })) {
      return false;
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    host.remove_prefix(separator + 1);
  }
  return false;
}

bool IsIpAddress(std::string_view text, int family) {
  const std::string terminated(text);
  std::array<unsigned char, 16> address;
  return inet_pton(family, terminated.c_str(), address.data()) == 1;
}

bool IsAddress(std::string_view text) {
  if (text.starts_with('[') && text.ends_with(']')) {
    return IsIpAddress(text.substr(1, text.size() - 2), AF_INET6);
  }
  if (std::ranges::all_of(text,
                          [](char c) { return IsDigit(c) || c == '.'; })) {
    return IsIpAddress(text, AF_INET);
  }
  return IsDnsName(text);
}

std::optional<std::string> CanonicalizeDialHost(
    std::string_view address,
    AccessTestNodeIdnaCanonicalizer canonicalize_idna) {
  auto decoded = Unescape(address);
  if (!decoded || !std::ranges::all_of(*decoded, [](unsigned char c) {
        return c >= 0x80 || IsAlphanumeric(c) || c == '-' || c == '.';
      })) {
    return std::nullopt;
  }
  if (std::ranges::none_of(*decoded,
                           [](unsigned char c) { return c >= 0x80; })) {
    return decoded;
  }
  if (!canonicalize_idna) {
    return std::nullopt;
  }
  auto canonical = canonicalize_idna(*decoded);
  return canonical && IsDnsName(*canonical) ? std::move(canonical)
                                            : std::nullopt;
}

bool IsExtendedServerName(std::string_view name,
                          bool allows_root_label = false) {
  if (allows_root_label && name.ends_with('.')) {
    name.remove_suffix(1);
  }
  const auto dot = name.rfind('.');
  return IsDnsName(name) && dot != std::string_view::npos &&
         std::ranges::any_of(name.substr(dot + 1), [](char c) {
           return LowerAscii(c) >= 'a' && LowerAscii(c) <= 'z';
         });
}

bool IsExtendedAddress(std::string_view address) {
  if (address.starts_with('[') && address.ends_with(']')) {
    return IsIpAddress(address.substr(1, address.size() - 2), AF_INET6);
  }
  return IsExtendedServerName(address, /*allows_root_label=*/true) ||
         IsIpAddress(address, AF_INET);
}

bool IsHttpAuthority(std::string_view authority) {
  auto host_end = authority.find(':');
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    host_end = close + 1;
  }
  if (!IsExtendedAddress(authority.substr(0, host_end))) {
    return false;
  }
  if (host_end == std::string_view::npos || host_end == authority.size()) {
    return true;
  }
  if (authority[host_end] != ':') {
    return false;
  }
  const auto digits = authority.substr(host_end + 1);
  uint16_t port = 0;
  const auto* end = std::to_address(digits.end());
  const auto parsed = std::from_chars(digits.data(), end, port);
  return parsed.ec == std::errc() && parsed.ptr == end && port != 0;
}

bool IsFingerprint(std::string_view name) {
  return !name.empty() && name.size() <= 32 &&
         std::ranges::all_of(name, [](char c) {
           return IsAlphanumeric(c) || c == '-' || c == '_';
         });
}

bool IsWebSocketFingerprint(std::string_view name) {
  // AccessVLESSWebSocketTLSParameters in the pinned iOS source.
  constexpr std::string_view kNames[]{"",    "chrome",  "firefox", "safari",
                                      "ios", "android", "edge"};
  return std::ranges::find(kNames, name) != std::end(kNames);
}

bool IsExtendedFingerprint(std::string_view name) {
  // Match the fingerprint names from the pinned iOS Xray registry. The iOS
  // bridge also excludes unsafe and hellogolang for these profiles.
  constexpr std::string_view kNames[] = {
      "chrome",
      "firefox",
      "safari",
      "ios",
      "android",
      "edge",
      "360",
      "qq",
      "random",
      "randomized",
      "randomizednoalpn",
      "hellofirefox_120",
      "hellofirefox_148",
      "hellochrome_120",
      "hellochrome_131",
      "hellochrome_133",
      "hellochrome_155",
      "helloios_13",
      "helloios_14",
      "helloedge_106",
      "hellosafari_26_3",
      "hello360_11_0",
      "helloqq_11_1",
      "hellorandomized",
      "hellorandomizedalpn",
      "hellorandomizednoalpn",
      "hellofirefox_auto",
      "hellofirefox_55",
      "hellofirefox_56",
      "hellofirefox_63",
      "hellofirefox_65",
      "hellofirefox_99",
      "hellofirefox_102",
      "hellofirefox_105",
      "hellochrome_auto",
      "hellochrome_58",
      "hellochrome_62",
      "hellochrome_70",
      "hellochrome_72",
      "hellochrome_83",
      "hellochrome_87",
      "hellochrome_96",
      "hellochrome_100",
      "hellochrome_102",
      "hellochrome_106_shuffle",
      "helloios_auto",
      "helloios_11_1",
      "helloios_12_1",
      "helloandroid_11_okhttp",
      "helloedge_85",
      "helloedge_auto",
      "hellosafari_16_0",
      "hellosafari_auto",
      "hello360_auto",
      "hello360_7_5",
      "helloqq_auto",
      "hellochrome_100_psk",
      "hellochrome_112_psk_shuf",
      "hellochrome_114_padding_psk_shuf",
      "hellochrome_115_pq",
      "hellochrome_115_pq_psk",
      "hellochrome_120_pq",
  };
  return IsFingerprint(name) && std::ranges::any_of(kNames, [&](auto allowed) {
           return std::ranges::equal(name, allowed, {}, LowerAscii);
         });
}

bool IsServiceName(std::string_view name) {
  if (name.empty() || name.size() > 2048) {
    return false;
  }
  if (name.starts_with('/')) {
    name.remove_prefix(1);
  }
  for (;;) {
    const auto slash = name.find('/');
    const auto part = name.substr(0, slash);
    if (part.empty() || part == "." || part == ".." ||
        !std::ranges::all_of(part, [](char c) {
          return IsAlphanumeric(c) ||
                 std::string_view("-._~").find(c) != std::string_view::npos;
        })) {
      return false;
    }
    if (slash == std::string_view::npos) {
      return true;
    }
    name.remove_prefix(slash + 1);
  }
}

bool ContainsNonWebScheme(std::string_view text) {
  for (auto colon = text.find("://"); colon != std::string_view::npos;
       colon = text.find("://", colon + 3)) {
    size_t start = colon;
    while (start != 0 && (IsAlphanumeric(text[start - 1]) ||
                          std::string_view("+.-").find(text[start - 1]) !=
                              std::string_view::npos)) {
      --start;
    }
    const auto scheme = text.substr(start, colon - start);
    if (!std::ranges::any_of(scheme, [](char c) {
          return LowerAscii(c) >= 'a' && LowerAscii(c) <= 'z';
        })) {
      continue;
    }
    if (!std::ranges::equal(scheme, std::string_view("https"), {},
                            LowerAscii) &&
        !std::ranges::equal(scheme, std::string_view("http"), {}, LowerAscii)) {
      return true;
    }
  }
  return false;
}

bool IsLegacyWebSocketPath(std::string_view path) {
  return path.starts_with('/') && path.size() <= 1024 &&
         std::ranges::all_of(path, [](char c) {
           return IsAlphanumeric(c) ||
                  std::string_view("/-._~!$'()*+,;=:@").find(c) !=
                      std::string_view::npos;
         });
}

bool IsWebSocketRequestTarget(std::string_view path) {
  if (!path.starts_with('/') || path.starts_with("//") || path.size() > 2048 ||
      !std::ranges::all_of(path, [](char c) {
        return IsAlphanumeric(c) ||
               std::string_view("/-._~!$&'()*+,;=:@%?").find(c) !=
                   std::string_view::npos;
      })) {
    return false;
  }
  // Validate the inner URL escapes without changing the originating request
  // target. Percent escapes can carry opaque bytes in query values.
  const auto decoded = Unescape(path);
  if (!decoded || ContainsNonWebScheme(*decoded)) {
    return false;
  }
  const auto question = path.find('?');
  if (question == std::string_view::npos) {
    return true;
  }
  auto fields = path.substr(question + 1);
  for (;;) {
    const auto ampersand = fields.find('&');
    const auto field = fields.substr(0, ampersand);
    if (Unescape(field.substr(0, field.find('='))) == "ed") {
      // Xray interprets this query key as WebSocket early-data configuration.
      return false;
    }
    if (ampersand == std::string_view::npos) {
      return true;
    }
    fields.remove_prefix(ampersand + 1);
  }
}

bool IsSafeRealitySpiderPath(std::string_view path) {
  if (path.empty()) {
    return true;
  }
  // Keep the input relative to the admitted server. The bridge checks that
  // the pinned Go URL parser accepts the path, fragment, and query escapes.
  if (!path.starts_with('/') || path.starts_with("//")) {
    return false;
  }
  for (auto percent = path.find('%'); percent != std::string_view::npos;
       percent = path.find('%', percent + 3)) {
    if (percent + 2 >= path.size() || HexValue(path[percent + 1]) < 0 ||
        HexValue(path[percent + 2]) < 0) {
      return false;
    }
  }
  const auto target = path.substr(0, path.find('#'));
  const auto question = target.find('?');
  if (question == std::string_view::npos) {
    return true;
  }
  auto fields = target.substr(question + 1);
  if (fields.find(';') != std::string_view::npos) {
    return false;  // Go's url.ParseQuery rejects raw query semicolons.
  }
  constexpr std::string_view kWorkKeys[]{"p", "c", "t", "i", "r"};
  while (!fields.empty()) {
    const auto ampersand = fields.find('&');
    const auto field = fields.substr(0, ampersand);
    const auto name = Unescape(field.substr(0, field.find('=')));
    if (name && std::ranges::find(kWorkKeys, *name) != std::end(kWorkKeys)) {
      return false;
    }
    if (ampersand == std::string_view::npos) {
      break;
    }
    fields.remove_prefix(ampersand + 1);
  }
  return true;
}

std::string_view CanonicalQueryKey(std::string_view key) {
  if (key == "serverName" || key == "server_name") {
    return "sni";
  }
  if (key == "publicKey" || key == "public_key") {
    return "pbk";
  }
  if (key == "shortId" || key == "short_id") {
    return "sid";
  }
  if (key == "fingerprint") {
    return "fp";
  }
  return key;
}

// A second alias for a field has the same authority as a duplicate key.
using Query = std::map<std::string, std::string, std::less<>>;

template <size_t N>
bool HasOnlyKeys(const Query& query, const std::string_view (&keys)[N]) {
  return std::ranges::all_of(query, [&](const auto& entry) {
    return std::ranges::find(keys, entry.first) != std::end(keys);
  });
}

std::optional<Query> ParseQuery(std::string_view query, bool& has_alias) {
  Query result;
  for (;;) {
    const auto next = query.find('&');
    const auto item = query.substr(0, next);
    const auto equal = item.find('=');
    if (equal == std::string_view::npos || equal == 0) {
      return std::nullopt;
    }
    const auto key = Unescape(item.substr(0, equal));
    auto value = Unescape(item.substr(equal + 1));
    if (!key || !value) {
      return std::nullopt;
    }
    const auto canonical = CanonicalQueryKey(*key);
    has_alias |= canonical != *key;
    if (!result.emplace(canonical, std::move(*value)).second) {
      return std::nullopt;
    }
    if (next == std::string_view::npos) {
      return result;
    }
    query.remove_prefix(next + 1);
  }
}

bool IsRealityQueryKey(std::string_view key) {
  constexpr std::array<std::string_view, 12> kKeys{
      "encryption", "flow", "fp",  "headerType", "pbk", "pcks",
      "security",   "sid",  "sni", "spx",        "type",
      "support-x25519mlkem768",
  };
  return std::ranges::find(kKeys, key) != kKeys.end();
}

std::string ValueOr(const Query& query,
                    std::string_view key,
                    std::string_view fallback = {}) {
  const auto it = query.find(key);
  return it == query.end() ? std::string(fallback) : it->second;
}

AccessTestNodeParseResult ParseShare(
    std::string_view share,
    AccessTestNodeImportMode mode,
    size_t max_share_bytes,
    AccessTestNodeIdnaCanonicalizer canonicalize_idna) {
  constexpr AccessTestNodeError kInvalid = AccessTestNodeError::kInvalidShare;
  constexpr AccessTestNodeError kUnsupported =
      AccessTestNodeError::kUnsupportedOutbound;
  constexpr std::string_view kWhitespace = " \t\r\n";
  constexpr std::string_view kScheme = "vless://";
  if (share.size() > max_share_bytes ||
      share.find_first_not_of(kWhitespace) == std::string_view::npos) {
    return {kInvalid, std::nullopt};
  }
  share.remove_prefix(share.find_first_not_of(kWhitespace));
  share = share.substr(0, share.find_last_not_of(kWhitespace) + 1);
  if (std::ranges::any_of(
          share, [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
    return {kInvalid, std::nullopt};
  }
  if (share.size() < kScheme.size() ||
      !std::ranges::equal(share.substr(0, kScheme.size()), kScheme, {},
                          LowerAscii)) {
    return {kUnsupported, std::nullopt};
  }
  share.remove_prefix(kScheme.size());
  const bool extended = mode == AccessTestNodeImportMode::kAllowExtendedVless;
  const bool has_nested_uri =
      extended && share.find("://") != std::string_view::npos;
  const auto fragment_start = share.find('#');
  const auto fragment = share.substr(std::min(fragment_start, share.size()));
  share = share.substr(0, fragment_start);
  const auto query_start = share.find('?');
  if (query_start == std::string_view::npos) {
    return {kInvalid, std::nullopt};
  }

  auto authority = share.substr(0, query_start);
  if (extended && authority.ends_with('/')) {
    authority.remove_suffix(1);
  }
  const auto at = authority.find('@');
  const auto colon = authority.rfind(':');
  if (at == std::string_view::npos || colon == std::string_view::npos ||
      at >= colon) {
    return {kInvalid, std::nullopt};
  }
  const auto user_id = authority.substr(0, at);
  auto address = authority.substr(at + 1, colon - at - 1);
  std::optional<std::string> decoded_host;
  if (extended && !address.starts_with('[')) {
    decoded_host = CanonicalizeDialHost(address, canonicalize_idna);
    if (!decoded_host) {
      return {kInvalid, std::nullopt};
    }
    address = *decoded_host;
  }
  const auto port_text = authority.substr(colon + 1);
  uint16_t port = 0;
  const auto* port_end = std::to_address(port_text.end());
  const auto parsed_port = std::from_chars(port_text.data(), port_end, port);
  if (!IsUuid(user_id) || !IsAddress(address) || port == 0 ||
      parsed_port.ec != std::errc() || parsed_port.ptr != port_end) {
    return {kInvalid, std::nullopt};
  }

  bool has_alias = false;
  const auto query = ParseQuery(share.substr(query_start + 1), has_alias);
  if (!query) {
    return {kInvalid, std::nullopt};
  }
  std::string host(address.starts_with('[')
                       ? address.substr(1, address.size() - 2)
                       : address);
  std::string canonical_user_id(user_id);
  std::ranges::transform(host, host.begin(), LowerAscii);
  std::ranges::transform(canonical_user_id, canonical_user_id.begin(), LowerAscii);
  const auto encryption = ValueOr(*query, "encryption");
  if (!encryption.empty() && encryption != "none") {
    return {kUnsupported, std::nullopt};
  }
  const auto type = ValueOr(*query, "type", "tcp");
  const auto security = ValueOr(*query, "security");
  const bool can_use_ws =
      extended || mode == AccessTestNodeImportMode::kAllowWebSocketTls;
  const bool ws = can_use_ws && type == "ws" && security == "tls";
  const bool tcp_tls =
      extended && type == "tcp" && security == "tls" && query->contains("type");
  const bool grpc = extended && type == "grpc" && security == "reality";
  if (extended && (ws || tcp_tls || grpc)) {
    if (!IsExtendedAddress(address) ||
        fragment.find("://") != std::string_view::npos) {
      return {kInvalid, std::nullopt};
    }
    // Only the original RAW/REALITY import accepts the exporter aliases.
    if (has_alias) {
      return {kUnsupported, std::nullopt};
    }
  }
  if (has_nested_uri && type == "tcp" && security == "reality") {
    return {kInvalid, std::nullopt};
  }
  AccessTestNode node{std::move(host),
                      port,
                      std::move(canonical_user_id),
                      ValueOr(*query, "sni"),
                      {},
                      {},
                      ValueOr(*query, "fp", ws && extended ? "" : "chrome"),
                      {}};
  if (ws) {
    constexpr std::string_view kWebSocketKeys[]{
        "alpn", "encryption", "fp", "host", "path", "security", "sni", "type"};
    auto alpn = ValueOr(*query, "alpn");
    if (!HasOnlyKeys(*query, kWebSocketKeys) ||
        (!alpn.empty() && (!extended || alpn != "http/1.1"))) {
      return {kUnsupported, std::nullopt};
    }
    const std::string_view default_host =
        extended ? std::string_view(node.server_name) : std::string_view();
    auto request_host = ValueOr(*query, "host", default_host);
    auto path = ValueOr(*query, "path", "/");
    const bool valid = extended ? IsExtendedServerName(node.server_name) &&
                                      IsHttpAuthority(request_host) &&
                                      IsWebSocketRequestTarget(path) &&
                                      IsWebSocketFingerprint(node.fingerprint)
                                : IsDnsName(node.server_name) &&
                                      IsDnsName(request_host) &&
                                      IsLegacyWebSocketPath(path) &&
                                      IsFingerprint(node.fingerprint);
    if (!valid) {
      return {kInvalid, std::nullopt};
    }
    node.web_socket_tls = AccessTestNodeWebSocketTls{
        std::move(request_host), std::move(path), std::move(alpn)};
    return {AccessTestNodeError::kNone, std::move(node)};
  }
  if (tcp_tls) {
    constexpr std::string_view kTcpTlsKeys[]{"encryption", "flow", "fp",
                                             "headerType", "host", "security",
                                             "sni",        "type"};
    if (!HasOnlyKeys(*query, kTcpTlsKeys) ||
        ValueOr(*query, "flow") != "xtls-rprx-vision" ||
        ValueOr(*query, "headerType", "none") != "none") {
      return {kUnsupported, std::nullopt};
    }
    const auto host_hint = ValueOr(*query, "host");
    if (!IsExtendedServerName(node.server_name) ||
        !IsExtendedFingerprint(node.fingerprint) ||
        (!host_hint.empty() && !IsHttpAuthority(host_hint))) {
      return {kInvalid, std::nullopt};
    }
    // A RAW connection uses explicit SNI. The exporter Host is metadata.
    node.tcp_tls_vision = true;
    return {AccessTestNodeError::kNone, std::move(node)};
  }

  if (grpc) {
    constexpr std::string_view kGrpcKeys[]{
        "encryption",  "fp",  "path", "pbk", "security",
        "serviceName", "sid", "sni",  "type"};
    if (!HasOnlyKeys(*query, kGrpcKeys)) {
      return {kUnsupported, std::nullopt};
    }
    const auto path = query->find("path");
    auto service_name = ValueOr(*query, "serviceName");
    if (!query->contains("serviceName") && path != query->end()) {
      service_name = path->second;
    }
    if (!IsServiceName(service_name) ||
        (path != query->end() && path->second != service_name)) {
      return {kInvalid, std::nullopt};
    }
    node.grpc_reality_service_name = std::move(service_name);
  }
  // Preserve the legacy RAW/REALITY query grammar, including empty pcks and
  // the optional affirmative exporter capability.
  if (!grpc && (!std::ranges::all_of(*query,
                                     [](const auto& entry) {
                                       return IsRealityQueryKey(entry.first);
                                     }) ||
                type != "tcp" || security != "reality" ||
                ValueOr(*query, "flow", extended ? "" : "xtls-rprx-vision") !=
                    "xtls-rprx-vision" ||
                ValueOr(*query, "headerType", "none") != "none" ||
                !ValueOr(*query, "pcks").empty() ||
                (query->contains("support-x25519mlkem768") &&
                 ValueOr(*query, "support-x25519mlkem768") != "true"))) {
    return {kUnsupported, std::nullopt};
  }

  node.public_key = ValueOr(*query, "pbk");
  node.short_id = ValueOr(*query, "sid");
  node.spider_x = ValueOr(*query, "spx");
  if (!IsDnsName(node.server_name) || node.public_key.empty() ||
      node.public_key.size() > 128 ||
      !std::ranges::all_of(
          node.public_key,
          [](char c) { return IsAlphanumeric(c) || c == '-' || c == '_'; }) ||
      !query->contains("sid") || node.short_id.size() > 16 ||
      !std::ranges::all_of(node.short_id,
                           [](char c) { return HexValue(c) >= 0; }) ||
      !IsFingerprint(node.fingerprint) ||
      (!node.spider_x.empty() && !node.spider_x.starts_with('/')) ||
      (grpc && !IsExtendedServerName(node.server_name)) ||
      // The accepted base64url alphabet and 43-byte length decode to exactly
      // 32 bytes under the pinned core's unpadded decoder.
      (extended &&
       (!IsExtendedFingerprint(node.fingerprint) ||
        node.public_key.size() != 43 || node.short_id.size() % 2 != 0 ||
        !IsSafeRealitySpiderPath(node.spider_x)))) {
    return {kInvalid, std::nullopt};
  }

  std::ranges::transform(node.short_id, node.short_id.begin(), LowerAscii);
  node.advertises_x25519_mlkem768 = query->contains("support-x25519mlkem768");
  return {AccessTestNodeError::kNone, std::move(node)};
}

}  // namespace

AccessTestNodeParseResult ParseAccessTestNodeShare(
    std::string_view share,
    AccessTestNodeImportMode mode,
    AccessTestNodeIdnaCanonicalizer canonicalize_idna) {
  const size_t max_share_bytes =
      mode == AccessTestNodeImportMode::kAllowExtendedVless
          ? kMaxExtendedAccessTestNodeShareBytes
          : kMaxAccessTestNodeShareBytes;
  return ParseShare(share, mode, max_share_bytes, canonicalize_idna);
}

AccessTestNodeParseResult ReparseAccessTestNodeNativeShare(
    std::string_view share,
    AccessTestNodeImportMode mode) {
  // Percent encoding uses at most three bytes per input byte. Allow room for
  // canonical defaults and a dial hostname normalized to an ASCII A-label.
  constexpr size_t kMaxNativeShareBytes =
      3 * kMaxExtendedAccessTestNodeShareBytes + 512;
  return ParseShare(share, mode, kMaxNativeShareBytes, nullptr);
}

}  // namespace aegis_access
