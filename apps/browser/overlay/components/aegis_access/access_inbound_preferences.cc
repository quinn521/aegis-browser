// Copyright 2026 GCSA
#include "components/aegis_access/access_inbound_preferences.h"

namespace aegis_access {

AccessInboundProtocolPreference ParseAccessInboundProtocolPreference(
    std::string_view value) {
  if (value == "auto") return AccessInboundProtocolPreference::kAuto;
  if (value == "http") return AccessInboundProtocolPreference::kHttp;
  if (value == "socks5") return AccessInboundProtocolPreference::kSocks5;
  return AccessInboundProtocolPreference::kInvalid;
}

AccessInboundPortMode ParseAccessInboundPortMode(std::string_view value) {
  if (value == "random") return AccessInboundPortMode::kSystemAllocated;
  if (value == "fixed") return AccessInboundPortMode::kInternalFixed;
  return AccessInboundPortMode::kInvalid;
}

std::optional<uint16_t> ParseAccessInboundFixedPort(std::string_view value) {
  if (value.empty() || value.size() > 5) return std::nullopt;
  uint32_t port = 0;
  for (const char digit : value) {
    if (digit < '0' || digit > '9') return std::nullopt;
    port = port * 10 + static_cast<uint32_t>(digit - '0');
  }
  if (port == 0 || port > 65535) return std::nullopt;
  return static_cast<uint16_t>(port);
}

AccessInboundPreferenceError ValidateAccessInboundPreferences(
    const AccessInboundPreferences& preferences) {
  using Error = AccessInboundPreferenceError;
  using Protocol = AccessInboundProtocolPreference;
  using Mode = AccessInboundPortMode;
  if (preferences.protocol != Protocol::kAuto &&
      preferences.protocol != Protocol::kHttp &&
      preferences.protocol != Protocol::kSocks5) return Error::kInvalidProtocol;
  if (preferences.port_mode == Mode::kSystemAllocated) {
    return preferences.requested_port == 0 ? Error::kNone
                                          : Error::kUnexpectedRequestedPort;
  }
  if (preferences.port_mode != Mode::kInternalFixed) return Error::kInvalidPortMode;
  if (preferences.requested_port == 0 || preferences.requested_port > 65535)
    return Error::kPortOutOfRange;
  if (preferences.protocol != Protocol::kHttp &&
      preferences.requested_port != kAccessInternalFixedSocks5Port)
    return Error::kFixedSocksPortNotAllowed;
  return Error::kNone;
}

}  // namespace aegis_access
