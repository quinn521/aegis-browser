// Copyright 2026 GCSA
#include "components/aegis_access/access_prepared_listener.h"

#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace aegis_access {
namespace {
AccessPreparedListenerBinding Binding() {
  return {kAccessNoAuthListenerWireVersion, "11111111-2222-4333-8444-555555555555",
          9007199254740993ULL, 1, 1};
}
TEST(AccessPreparedListenerTest, RequiresExplicitKnownVersionAndCompleteNativeIdentity) {
  EXPECT_TRUE(IsValidAccessPreparedListenerBinding(Binding()));
  EXPECT_FALSE(IsValidAccessPreparedListenerBinding({}));
  for (const uint32_t version : {0u, 2u, std::numeric_limits<uint32_t>::max()}) {
    auto binding = Binding();
    binding.wire_version = version;
    EXPECT_FALSE(IsValidAccessPreparedListenerBinding(binding));
  }
  for (const auto* profile : {"", "not-an-id", "AAAAAAAA-2222-4333-8444-555555555555"}) {
    auto binding = Binding();
    binding.connection_profile_id = profile;
    EXPECT_FALSE(IsValidAccessPreparedListenerBinding(binding));
  }
  for (const auto member : {&AccessPreparedListenerBinding::profile_version,
                           &AccessPreparedListenerBinding::upstream_authorization_generation,
                           &AccessPreparedListenerBinding::listener_generation}) {
    auto binding = Binding();
    binding.*member = 0;
    EXPECT_FALSE(IsValidAccessPreparedListenerBinding(binding));
  }
  auto oversized = Binding();
  oversized.profile_version = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
  EXPECT_FALSE(IsValidAccessPreparedListenerBinding(oversized));
}
}  // namespace
}  // namespace aegis_access
