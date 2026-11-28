// Copyright 2026 GCSA
#include "components/aegis_access/access_inbound_preferences.h"

#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <utility>

int main() {
  using namespace aegis_access;
  using Protocol = AccessInboundProtocolPreference;
  using Mode = AccessInboundPortMode;
  using Error = AccessInboundPreferenceError;
  assert(ValidateAccessInboundPreferences({}) == Error::kNone);
  for (auto protocol : {Protocol::kAuto, Protocol::kHttp, Protocol::kSocks5}) {
    assert(ValidateAccessInboundPreferences({protocol, Mode::kSystemAllocated, 0}) == Error::kNone);
    for (uint32_t stale_port : {1u, 1080u, 10806u, 65535u}) {
      assert(ValidateAccessInboundPreferences({protocol, Mode::kSystemAllocated, stale_port}) ==
             Error::kUnexpectedRequestedPort);
    }
    assert(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, 10806}) == Error::kNone);
    for (uint32_t invalid : {0u, 65536u, std::numeric_limits<uint32_t>::max()}) {
      assert(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, invalid}) ==
             Error::kPortOutOfRange);
    }
  }
  for (auto protocol : {Protocol::kAuto, Protocol::kSocks5}) {
    for (uint32_t forbidden_fixed : {1u, 1080u, 8080u, 65535u}) {
      assert(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, forbidden_fixed}) ==
             Error::kFixedSocksPortNotAllowed);
    }
  }
  for (uint32_t http_port : {1u, 1080u, 8080u, 10806u, 65535u}) {
    assert(ValidateAccessInboundPreferences({Protocol::kHttp, Mode::kInternalFixed, http_port}) ==
           Error::kNone);
  }
  assert(ValidateAccessInboundPreferences({Protocol::kInvalid}) == Error::kInvalidProtocol);
  assert(ValidateAccessInboundPreferences({static_cast<Protocol>(255)}) == Error::kInvalidProtocol);
  assert(ValidateAccessInboundPreferences({Protocol::kHttp, Mode::kInvalid}) == Error::kInvalidPortMode);
  assert(ValidateAccessInboundPreferences({Protocol::kHttp, static_cast<Mode>(255)}) == Error::kInvalidPortMode);
  assert(ParseAccessInboundProtocolPreference("auto") == Protocol::kAuto);
  assert(ParseAccessInboundProtocolPreference("http") == Protocol::kHttp);
  assert(ParseAccessInboundProtocolPreference("socks5") == Protocol::kSocks5);
  for (auto invalid : {"", "HTTP", "socks", "direct", "http ", "https"}) {
    assert(ParseAccessInboundProtocolPreference(invalid) == Protocol::kInvalid);
  }
  assert(ParseAccessInboundPortMode("random") == Mode::kSystemAllocated);
  assert(ParseAccessInboundPortMode("fixed") == Mode::kInternalFixed);
  for (auto invalid : {"", "auto", "RANDOM", "fixed "}) {
    assert(ParseAccessInboundPortMode(invalid) == Mode::kInvalid);
  }
  for (const auto& [text, expected] : std::array<std::pair<std::string_view, uint16_t>, 7>{{
           {"1", 1}, {"80", 80}, {"1080", 1080}, {"8080", 8080},
           {"10806", 10806}, {"65535", 65535}, {"00001", 1}}}) {
    assert(ParseAccessInboundFixedPort(text) == expected);
  }
  for (auto invalid : {"", "0", "00000", "65536", "99999", "1e3", "1.0",
                       "-1", "+80", " 80", "80 ", "80\n", "80\r", "000080", "٨٠"}) {
    assert(!ParseAccessInboundFixedPort(invalid));
  }
  constexpr char embedded_nul[] = {'8', '0', '\0'};
  assert(!ParseAccessInboundFixedPort(std::string_view(embedded_nul, sizeof(embedded_nul))));
  std::cout << "PASS: local inbound preference and decimal port contracts\n";
}
