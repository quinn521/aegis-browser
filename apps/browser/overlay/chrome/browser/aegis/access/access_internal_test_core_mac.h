// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_INTERNAL_TEST_CORE_MAC_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_INTERNAL_TEST_CORE_MAC_H_

#include <optional>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/process.h"
#include "base/time/time.h"
#include "chrome/browser/aegis/access/access_owned_listener_mac.h"
#include "chrome/browser/aegis/access/access_test_node_vault_mac.h"
#include "components/aegis_access/access_inbound_preferences.h"
#include "components/aegis_access/access_options_capability.h"
#include "components/aegis_access/access_proxy_route_adapter.h"
#include "url/gurl.h"

namespace aegis::access {
// Native-only binding derived from an actual Profile, its staged/committed
// graph and authorization generation. No renderer may supply these fields.
struct AccessInternalTestCoreBindingMac {
  std::string profile_id;
  uint64_t profile_version = 0;
  uint64_t authorization_generation = 0;
  std::string channel;
  std::string vault_scope_hash;
  AccessTestNodeSecretReference reference;
  aegis_access::AccessInboundPreferences preferences;
  aegis_access::RegisteredProxyTransport selected_transport =
      aegis_access::RegisteredProxyTransport::kInvalid;
  // Trusted native canary for this preparation, with a fresh non-secret marker.
  // Core launch itself does not consume these or claim readiness.
  GURL canary_url;
  std::string canary_marker;
};
enum class AccessInternalTestCoreLaunchStatusMac {
  kLaunched,
  kNotAdmitted,
  kInvalidBinding,
  kInvalidManifest,
  kUnsupportedProtocol,
  kInvalidCorePin,
  kSecretUnavailable,
  kConfigurationInvalid,
  kStageFailed,
  kLaunchFailed,
  kWriteFailed,
  kDeadlineExceeded,
  kChildIdentityMismatch,
};

// Blocking helper for AccessLocalCoreManager's existing worker/lifecycle.
// It never publishes or claims protocol/upstream readiness. The caller owns
// process cleanup on EVERY result, including a failure after child creation;
// only confirmed retirement permits releasing its binding or reusing a port.
// Directory and process must be empty on entry. Secrets cross an anonymous
// stdin pipe, while only the exact pinned executable is staged on disk.
// Reserve the metadata-only reference lease before queuing preparation. The
// helper validates that exact scope/reference and never creates or renews a
// runtime authorization. Keep the lease until confirmed retirement.
AccessInternalTestCoreLaunchStatusMac LaunchAccessInternalTestCoreMac(
    const aegis_access::AccessBuildIdentity& identity,
    const base::FilePath& manifest_path,
    const AccessInternalTestCoreBindingMac& binding,
    base::TimeTicks deadline,
    base::ScopedTempDir& directory,
    base::Process& process,
    std::optional<AccessOwnedChildMac>& child_identity,
    const std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease>& reference_lease,
    base::ScopedFD& listener_lease_channel);
}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_INTERNAL_TEST_CORE_MAC_H_
