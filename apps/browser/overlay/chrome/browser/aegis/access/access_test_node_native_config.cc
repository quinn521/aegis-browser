// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_native_config.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"

namespace aegis::access {
namespace {

std::string EscapeNativeQueryValue(std::string_view value) {
  // The share decoder splits on '&' and '#' and decodes '%' exactly once.
  // Preserve every other printable character to avoid expanding the request
  // target or gRPC service name beyond the parser's share budget.
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (unsigned char byte : value) {
    if (byte <= 0x20 || byte == 0x7f || byte == '&' || byte == '#' ||
        byte == '%') {
      result.push_back('%');
      result.push_back(kHex[byte >> 4]);
      result.push_back(kHex[byte & 15]);
    } else {
      result.push_back(static_cast<char>(byte));
    }
  }
  return result;
}

}  // namespace

std::optional<aegis_access::AccessTestNode> ValidateAccessTestNodeNative(
    const aegis_access::AccessTestNode& node) {
  const std::string address = node.address.find(':') == std::string::npos
      ? node.address : "[" + node.address + "]";
  auto share = "vless://" + node.user_id + "@" + address + ":" +
               base::NumberToString(node.port) +
               "?sni=" + EscapeNativeQueryValue(node.server_name);
  const std::string_view default_fingerprint =
      node.web_socket_tls ? "" : "chrome";
  if (node.fingerprint != default_fingerprint) {
    share += "&fp=" + EscapeNativeQueryValue(node.fingerprint);
  }
  if (const auto& ws = node.web_socket_tls) {
    share += "&type=ws&security=tls";
    if (ws->host != node.server_name) {
      share += "&host=" + EscapeNativeQueryValue(ws->host);
    }
    if (ws->path != "/") {
      share += "&path=" + EscapeNativeQueryValue(ws->path);
    }
    if (!ws->alpn.empty()) {
      share += "&alpn=" + EscapeNativeQueryValue(ws->alpn);
    }
  } else if (node.tcp_tls_vision) {
    share += "&type=tcp&security=tls&flow=xtls-rprx-vision";
  } else if (const auto& service_name = node.grpc_reality_service_name) {
    share += "&type=grpc&security=reality&pbk=" +
             EscapeNativeQueryValue(node.public_key) +
             "&sid=" + EscapeNativeQueryValue(node.short_id) +
             "&path=" + EscapeNativeQueryValue(*service_name);
  } else {
    share +=
        "&type=tcp&security=reality&flow=xtls-rprx-vision&pbk=" +
        EscapeNativeQueryValue(node.public_key) +
        "&sid=" + EscapeNativeQueryValue(node.short_id) +
        "&spx=" + EscapeNativeQueryValue(node.spider_x) +
        (node.advertises_x25519_mlkem768 ? "&support-x25519mlkem768=true" : "");
  }
  auto parsed = aegis_access::ParseAccessTestNodeShare(
      share, aegis_access::AccessTestNodeImportMode::kAllowExtendedVless);
  return parsed.node == node ? std::move(parsed.node) : std::nullopt;
}

std::optional<std::string> BuildAccessTestNodeCoreConfiguration(
    const aegis_access::AccessTestNode& node,
    const aegis_access::AccessInboundPreferences& preferences,
    aegis_access::RegisteredProxyTransport selected_transport) {
  using Protocol = aegis_access::AccessInboundProtocolPreference;
  using Transport = aegis_access::RegisteredProxyTransport;
  if (aegis_access::ValidateAccessInboundPreferences(preferences) !=
          aegis_access::AccessInboundPreferenceError::kNone ||
      (selected_transport != Transport::kHttp && selected_transport != Transport::kSocks5) ||
      (preferences.protocol == Protocol::kHttp && selected_transport != Transport::kHttp) ||
      (preferences.protocol == Protocol::kSocks5 && selected_transport != Transport::kSocks5) ||
      !ValidateAccessTestNodeNative(node)) return std::nullopt;

  const bool socks = selected_transport == Transport::kSocks5;
  auto inbound = base::DictValue().Set("listen", "127.0.0.1")
      .Set("protocol", socks ? "socks" : "http");
  if (preferences.port_mode == aegis_access::AccessInboundPortMode::kSystemAllocated) {
    // The admitted macOS core parses the string range "0" as one port. Numeric
    // zero means an empty range in its JSON decoder and is not equivalent.
    inbound.Set("port", "0");
  } else {
    inbound.Set("port", static_cast<int>(preferences.requested_port));
  }
  inbound.Set("settings", socks ? base::DictValue().Set("auth", "noauth").Set("udp", false)
                                : base::DictValue());
  auto user = base::DictValue().Set("id", node.user_id).Set("encryption", "none");
  auto transport = base::DictValue().Set("serverName", node.server_name);
  if (!node.fingerprint.empty()) {
    transport.Set("fingerprint", node.fingerprint);
  }
  base::DictValue stream;
  if (node.web_socket_tls || node.tcp_tls_vision) {
    transport.Set("allowInsecure", false);
    transport.Set("useSystemCA", true);
    stream.Set("security", "tls");
    if (const auto& ws = node.web_socket_tls) {
      if (!ws->alpn.empty()) {
        transport.Set("alpn", base::ListValue().Append(ws->alpn));
      }
      stream.Set("network", "ws");
      stream.Set("wsSettings",
                 base::DictValue().Set("host", ws->host).Set("path", ws->path));
    } else {
      user.Set("flow", "xtls-rprx-vision");
      stream.Set("network", "tcp");
    }
    stream.Set("tlsSettings", std::move(transport));
  } else {
    // The advertised capability remains metadata. The core uses its pinned
    // fingerprint and the explicit REALITY fields for the handshake.
    transport.Set("publicKey", node.public_key);
    transport.Set("shortId", node.short_id);
    if (const auto& service_name = node.grpc_reality_service_name) {
      stream.Set("network", "grpc");
      stream.Set("grpcSettings",
                 base::DictValue().Set("serviceName", *service_name));
    } else {
      transport.Set("spiderX", node.spider_x);
      user.Set("flow", "xtls-rprx-vision");
      stream.Set("network", "tcp");
    }
    stream.Set("security", "reality");
    stream.Set("realitySettings", std::move(transport));
  }
  auto server = base::DictValue().Set("address", node.address).Set("port", node.port)
      .Set("users", base::ListValue().Append(std::move(user)));
  auto settings = base::DictValue().Set("vnext", base::ListValue().Append(std::move(server)));
  auto outbound = base::DictValue().Set("protocol", "vless").Set("settings", std::move(settings))
      .Set("streamSettings", std::move(stream));
  auto configuration = base::DictValue().Set("log", base::DictValue().Set("loglevel", "none"))
      .Set("inbounds", base::ListValue().Append(std::move(inbound)))
      .Set("outbounds", base::ListValue().Append(std::move(outbound)));
  std::string serialized;
  if (!base::JSONWriter::Write(configuration, &serialized) || serialized.size() > 8192)
    return std::nullopt;
  return serialized;
}
}  // namespace aegis::access
