// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_native_config.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"

namespace aegis::access {
std::optional<aegis_access::AccessTestNode> ValidateAccessTestNodeNative(
    const aegis_access::AccessTestNode& node) {
  const auto escaped = [](std::string_view text) {
    return base::EscapeQueryParamValue(text, /*use_plus=*/false);
  };
  const std::string address = node.address.find(':') == std::string::npos
      ? node.address : "[" + node.address + "]";
  auto share = "vless://" + node.user_id + "@" + address + ":" +
               base::NumberToString(node.port) + "?sni=" +
               escaped(node.server_name) + "&fp=" + escaped(node.fingerprint);
  if (const auto& ws = node.web_socket_tls) {
    share += "&type=ws&security=tls&host=" + escaped(ws->host) +
             "&path=" + escaped(ws->path);
  } else {
    share += "&type=tcp&security=reality&flow=xtls-rprx-vision&pbk=" +
             escaped(node.public_key) + "&sid=" + escaped(node.short_id) +
             "&spx=" + escaped(node.spider_x) +
             (node.advertises_x25519_mlkem768
                  ? "&support-x25519mlkem768=true"
                  : "");
  }
  auto parsed = aegis_access::ParseAccessTestNodeShare(
      share, aegis_access::AccessTestNodeImportMode::kAllowWebSocketTls);
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
  auto transport = base::DictValue().Set("serverName", node.server_name)
                       .Set("fingerprint", node.fingerprint);
  base::DictValue stream;
  if (const auto& ws = node.web_socket_tls) {
    transport.Set("allowInsecure", false);
    stream.Set("network", "ws");
    stream.Set("security", "tls");
    stream.Set("tlsSettings", std::move(transport));
    stream.Set("wsSettings", base::DictValue().Set("host", ws->host)
                                 .Set("path", ws->path));
  } else {
    user.Set("flow", "xtls-rprx-vision");
    // The advertised capability remains metadata. The core uses its pinned
    // fingerprint and the explicit REALITY fields for the handshake.
    transport.Set("publicKey", node.public_key);
    transport.Set("shortId", node.short_id);
    transport.Set("spiderX", node.spider_x);
    stream.Set("network", "tcp");
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
