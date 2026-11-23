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
std::optional<Query> ParseQuery(std::string_view query) {
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
    if (!key || !value ||
        !result.emplace(CanonicalQueryKey(*key), std::move(*value)).second) {
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

}  // namespace

AccessTestNodeParseResult ParseAccessTestNodeShare(
    std::string_view share,
    AccessTestNodeImportMode mode) {
  constexpr AccessTestNodeError kInvalid = AccessTestNodeError::kInvalidShare;
  constexpr AccessTestNodeError kUnsupported =
      AccessTestNodeError::kUnsupportedOutbound;
  constexpr std::string_view kWhitespace = " \t\r\n";
  constexpr std::string_view kScheme = "vless://";
  if (share.size() > kMaxAccessTestNodeShareBytes ||
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
  share = share.substr(0, share.find('#'));
  const auto query_start = share.find('?');
  if (query_start == std::string_view::npos) {
    return {kInvalid, std::nullopt};
  }

  const auto authority = share.substr(0, query_start);
  const auto at = authority.find('@');
  const auto colon = authority.rfind(':');
  if (at == std::string_view::npos || colon == std::string_view::npos ||
      at >= colon) {
    return {kInvalid, std::nullopt};
  }
  const auto user_id = authority.substr(0, at);
  const auto address = authority.substr(at + 1, colon - at - 1);
  const auto port_text = authority.substr(colon + 1);
  uint16_t port = 0;
  const auto* port_end = std::to_address(port_text.end());
  const auto parsed_port = std::from_chars(port_text.data(), port_end, port);
  if (!IsUuid(user_id) || !IsAddress(address) || port == 0 ||
      parsed_port.ec != std::errc() || parsed_port.ptr != port_end) {
    return {kInvalid, std::nullopt};
  }

  const auto query = ParseQuery(share.substr(query_start + 1));
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
  if (mode == AccessTestNodeImportMode::kAllowWebSocketTls &&
      ValueOr(*query, "type") == "ws" && ValueOr(*query, "security") == "tls") {
    constexpr std::array<std::string_view, 8> kWebSocketKeys{
        "alpn", "encryption", "fp", "host", "path", "security", "sni", "type"};
    if (!std::ranges::all_of(*query, [&](const auto& field) {
          return std::ranges::find(kWebSocketKeys, field.first) !=
                 kWebSocketKeys.end();
        }) || !ValueOr(*query, "alpn").empty()) {
      return {kUnsupported, std::nullopt};
    }
    auto sni = ValueOr(*query, "sni");
    auto request_host = ValueOr(*query, "host");
    auto path = ValueOr(*query, "path", "/");
    auto fingerprint = ValueOr(*query, "fp", "chrome");
    if (!IsDnsName(sni) || !IsDnsName(request_host) || !path.starts_with('/') ||
        path.size() > 1024 ||
        !std::ranges::all_of(path, [](char c) {
          return IsAlphanumeric(c) ||
                 std::string_view("/-._~!$'()*+,;=:@").find(c) !=
                     std::string_view::npos;
        }) ||
        fingerprint.empty() || fingerprint.size() > 32 ||
        !std::ranges::all_of(fingerprint, [](char c) {
          return IsAlphanumeric(c) || c == '-' || c == '_';
        })) {
      return {kInvalid, std::nullopt};
    }
    return {AccessTestNodeError::kNone,
            AccessTestNode{std::move(host), port, std::move(canonical_user_id),
                           std::move(sni), {}, {}, std::move(fingerprint), {},
                           false, AccessTestNodeWebSocketTls{
                                      std::move(request_host), std::move(path)}}};
  }
  if (!std::ranges::all_of(
          *query,
          [](const auto& entry) { return IsRealityQueryKey(entry.first); }) ||
      ValueOr(*query, "type", "tcp") != "tcp" ||
      ValueOr(*query, "security") != "reality" ||
      ValueOr(*query, "flow", "xtls-rprx-vision") != "xtls-rprx-vision" ||
      ValueOr(*query, "headerType", "none") != "none" ||
      // Accept the empty exporter placeholder seen in authorized test input.
      // A populated value may carry unsupported verification requirements;
      // never silently discard it.
      !ValueOr(*query, "pcks").empty() ||
      // 3X-UI emits this advertised capability for REALITY shares. Preserve
      // its explicit true value as metadata; native JSON keeps the original
      // fingerprint and REALITY fields. Other values are outside this format.
      (query->contains("support-x25519mlkem768") &&
       ValueOr(*query, "support-x25519mlkem768") != "true")) {
    return {kUnsupported, std::nullopt};
  }

  auto sni = ValueOr(*query, "sni");
  auto public_key = ValueOr(*query, "pbk");
  auto short_id = ValueOr(*query, "sid");
  auto fingerprint = ValueOr(*query, "fp", "chrome");
  auto spider_x = ValueOr(*query, "spx");
  if (!IsDnsName(sni) || public_key.empty() || public_key.size() > 128 ||
      !std::ranges::all_of(
          public_key,
          [](char c) { return IsAlphanumeric(c) || c == '-' || c == '_'; }) ||
      !query->contains("sid") || short_id.size() > 16 ||
      !std::ranges::all_of(short_id, [](char c) { return HexValue(c) >= 0; }) ||
      fingerprint.empty() || fingerprint.size() > 32 ||
      !std::ranges::all_of(
          fingerprint,
          [](char c) { return IsAlphanumeric(c) || c == '-' || c == '_'; }) ||
      (!spider_x.empty() && !spider_x.starts_with('/'))) {
    return {kInvalid, std::nullopt};
  }

  std::ranges::transform(short_id, short_id.begin(), LowerAscii);
  return {
      AccessTestNodeError::kNone,
      AccessTestNode{std::move(host), port, std::move(canonical_user_id),
                     std::move(sni), std::move(public_key), std::move(short_id),
                     std::move(fingerprint), std::move(spider_x),
                     query->contains("support-x25519mlkem768")}};
}

}  // namespace aegis_access
