// Copyright 2026 GCSA
#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_INBOUND_PREFERENCES_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_INBOUND_PREFERENCES_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace aegis_access {

enum class AccessInboundProtocolPreference {
  kInvalid,
  kAuto,
  kHttp,
  kSocks5,
};

enum class AccessInboundPortMode {
  kInvalid,
  kSystemAllocated,
  kInternalFixed,
};

inline constexpr uint16_t kAccessInternalFixedSocks5Port = 10806;

// Application-owned loopback preferences, independent of upstream node ports.
// Zero requests the OS allocation only; it is never a registered actual port.
// These defaults do not enable a platform/channel or grant runtime admission.
struct AccessInboundPreferences {
  AccessInboundProtocolPreference protocol = AccessInboundProtocolPreference::kAuto;
  AccessInboundPortMode port_mode = AccessInboundPortMode::kSystemAllocated;
  uint32_t requested_port = 0;
  bool operator==(const AccessInboundPreferences&) const = default;
};

enum class AccessInboundPreferenceError {
  kNone,
  kInvalidProtocol,
  kInvalidPortMode,
  kUnexpectedRequestedPort,
  kPortOutOfRange,
  kFixedSocksPortNotAllowed,
};

AccessInboundProtocolPreference ParseAccessInboundProtocolPreference(
    std::string_view value);
AccessInboundPortMode ParseAccessInboundPortMode(std::string_view value);
// One to five ASCII decimal digits, value 1..65535. No sign, whitespace,
// floating point, exponent or implicit truncation from a wider integer.
std::optional<uint16_t> ParseAccessInboundFixedPort(std::string_view value);

// Validates intent only. The native channel, authorization, owned listener,
// actual nonzero port, protocol readiness and exact publication ACK remain
// separate requirements. This never launches, binds, selects a fallback or
// changes an existing route; local authentication is not a preference field.
AccessInboundPreferenceError ValidateAccessInboundPreferences(
    const AccessInboundPreferences& preferences);

}  // namespace aegis_access
#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_INBOUND_PREFERENCES_H_
