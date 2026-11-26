// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_NATIVE_CONFIG_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_NATIVE_CONFIG_H_

#include <optional>
#include <string>

#include "components/aegis_access/access_inbound_preferences.h"
#include "components/aegis_access/access_proxy_route_adapter.h"
#include "components/aegis_access/access_test_node.h"

namespace aegis::access {
enum class AccessTestNodeNativeValidation {
  kStored,
  kFreshDevImport,
};

// Fresh DEV staging uses current import rules. Stored v1 records keep their
// original TCP/REALITY validation, including when generating core config.
std::optional<aegis_access::AccessTestNode> ValidateAccessTestNodeNative(
    const aegis_access::AccessTestNode& node,
    AccessTestNodeNativeValidation validation =
        AccessTestNodeNativeValidation::kStored);

// Ephemeral secret bytes for an anonymous pipe to the exact admitted core.
// Callers must not persist, log or export this result. This serializer neither
// grants channel/runtime admission nor selects a fallback, binds or publishes.
std::optional<std::string> BuildAccessTestNodeCoreConfiguration(
    const aegis_access::AccessTestNode& node,
    const aegis_access::AccessInboundPreferences& preferences,
    aegis_access::RegisteredProxyTransport selected_transport);
}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_NATIVE_CONFIG_H_
