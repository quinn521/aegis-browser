// Copyright 2026 GCSA
#include "components/aegis_access/access_inbound_preferences.h"

#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace aegis_access {
namespace {
using Protocol = AccessInboundProtocolPreference;
using Mode = AccessInboundPortMode;
using Error = AccessInboundPreferenceError;

TEST(AccessInboundPreferencesTest, RandomHasNoSavedActualPortAndFixedSocksIs10806Only) {
  EXPECT_EQ(ValidateAccessInboundPreferences({}), Error::kNone);
  for (auto protocol : {Protocol::kAuto, Protocol::kHttp, Protocol::kSocks5}) {
    EXPECT_EQ(ValidateAccessInboundPreferences({protocol, Mode::kSystemAllocated, 0}), Error::kNone);
    EXPECT_EQ(ValidateAccessInboundPreferences({protocol, Mode::kSystemAllocated, 10806}),
              Error::kUnexpectedRequestedPort);
    EXPECT_EQ(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, 10806}), Error::kNone);
    for (uint32_t invalid : {0u, 65536u, std::numeric_limits<uint32_t>::max()})
      EXPECT_EQ(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, invalid}), Error::kPortOutOfRange);
  }
  for (auto protocol : {Protocol::kAuto, Protocol::kSocks5}) {
    for (uint32_t forbidden : {1u, 1080u, 8080u, 65535u})
      EXPECT_EQ(ValidateAccessInboundPreferences({protocol, Mode::kInternalFixed, forbidden}),
                Error::kFixedSocksPortNotAllowed);
  }
  for (uint32_t port : {1u, 1080u, 8080u, 10806u, 65535u})
    EXPECT_EQ(ValidateAccessInboundPreferences({Protocol::kHttp, Mode::kInternalFixed, port}), Error::kNone);
}

TEST(AccessInboundPreferencesTest, RejectsUnknownEnumsAndNonDecimalOrOutOfRangePorts) {
  EXPECT_EQ(ValidateAccessInboundPreferences({static_cast<Protocol>(255)}), Error::kInvalidProtocol);
  EXPECT_EQ(ValidateAccessInboundPreferences({Protocol::kHttp, static_cast<Mode>(255)}), Error::kInvalidPortMode);
  EXPECT_EQ(ParseAccessInboundFixedPort("1"), 1u);
  EXPECT_EQ(ParseAccessInboundFixedPort("00001"), 1u);
  EXPECT_EQ(ParseAccessInboundFixedPort("10806"), 10806u);
  EXPECT_EQ(ParseAccessInboundFixedPort("65535"), 65535u);
  for (const auto invalid : {"", "0", "65536", "99999", "1e3", "1.0", "-1",
                            "+80", " 80", "80 ", "80\n", "000080", "٨٠"})
    EXPECT_FALSE(ParseAccessInboundFixedPort(invalid));
  constexpr char embedded_nul[] = {'8', '0', '\0'};
  EXPECT_FALSE(ParseAccessInboundFixedPort(std::string_view(embedded_nul, sizeof(embedded_nul))));
}
}  // namespace
}  // namespace aegis_access
