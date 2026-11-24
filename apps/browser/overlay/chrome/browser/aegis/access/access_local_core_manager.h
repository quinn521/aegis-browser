// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_LOCAL_CORE_MANAGER_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_LOCAL_CORE_MANAGER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "base/files/scoped_file.h"
#include "chrome/browser/aegis/access/access_internal_test_core_mac.h"
#endif
#include "net/base/auth.h"
#include "components/aegis_access/access_proxy_route_adapter.h"
#include "chrome/browser/aegis/access/access_core_log_store.h"

namespace aegis::access {
enum class AccessLocalCoreSource {
  kDevFiles, kPrivateTestBundle,
#if BUILDFLAG(IS_MAC)
  kInternalTestProfile,
#endif
};
// Supplied by Browser's private Dev binding or its explicit Alpha build asset.
// Neither a page nor the retry IPC supplies a path, node, URL or credential.
struct AccessLocalCoreInput {
  base::FilePath manifest;
  base::FilePath node_config;
  AccessLocalCoreSource source = AccessLocalCoreSource::kDevFiles;
  scoped_refptr<AccessCoreLogStore> log_store = nullptr;
#if BUILDFLAG(IS_MAC)
  bool (*create_log_pipe_for_testing)(base::ScopedFD*, base::ScopedFD*, bool) = nullptr;
  std::optional<AccessInternalTestCoreBindingMac> internal_test_profile;
  // Optional original UI preparation deadline; store/read/launch do not reset it.
  base::TimeTicks internal_preparation_deadline;
  AccessOwnedChildRetirementMac (*retire_owned_child_for_testing)(
      const AccessOwnedChildMac&, uint32_t) = nullptr;
  // Tests only: after the owned listener and its parent-held lease are
  // verified, accept readiness without the controlled HTTPS canary.
  bool skip_internal_canary_for_testing = false;
#endif
};
// Resolves paths only. File parsing, pins, process launch and readiness remain
// on the core IO thread. Alpha never takes node paths from command-line flags.
std::optional<AccessLocalCoreInput> ResolveAccessLocalCoreInput();
#if BUILDFLAG(IS_MAC)
// Internal development only: derives the probe and manifest from native launch
// options. The caller derives binding fields from the actual committed Profile.
// Auto/default admission and an Alpha packaged-core contract remain separate.
std::optional<AccessLocalCoreInput> ResolveAccessInternalTestCoreInput(
    AccessInternalTestCoreBindingMac binding);
// Same build and launch-option admission as ResolveAccessInternalTestCoreInput,
// without generating a canary marker or reading any file.
bool CanResolveAccessInternalTestCoreInput();
#endif
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
// Display-only anonymous label. Call on a blocking sequence.
std::string ReadConfiguredAccessNodeId(const AccessLocalCoreInput& input);
#endif

struct AccessLocalCoreReady {
  bool ready = false;
  uint16_t port = 0;
  std::string registration_id;
  std::optional<net::AuthCredentials> local_credentials;
  std::string error;
  aegis_access::RegisteredProxyTransport transport = aegis_access::RegisteredProxyTransport::kInvalid;
  bool no_local_auth = false;
  std::string profile_id;
  uint64_t profile_version = 0;
  uint64_t authorization_generation = 0;
  uint64_t listener_socket_generation = 0;
  bool parent_listener_lease_held = false;
  bool retirement_pending = false;
};

// One child per owning Browser service. Blocking work and the TLS canary run
// on a dedicated IO thread. Legacy HTTP proves process-local authentication;
// internal Profile mode proves owned exclusive listener + validated HTTPS
// marker, with no local credentials. Network publication must still be ACKed.
class AccessLocalCoreManager {
 public:
  explicit AccessLocalCoreManager(AccessLocalCoreInput input);
  ~AccessLocalCoreManager();
  AccessLocalCoreManager(const AccessLocalCoreManager&) = delete;
  AccessLocalCoreManager& operator=(const AccessLocalCoreManager&) = delete;

  void Prepare(base::OnceCallback<void(AccessLocalCoreReady)> completion);
  void Verify(base::OnceCallback<void(AccessLocalCoreReady)> completion);
  void Stop(base::OnceClosure completion);
#if BUILDFLAG(IS_MAC)
  // Stop the child as soon as revocation begins. Keep a fenced listener
  // address reserved until the Network consumer confirms its retirement.
  void StopForwarding();
  // Native owner calls this ONLY after confirming removal of every published
  // consumer (or before any Ready result was exposed). It then retires the
  // child and releases the held listener/reference. A false result retains the
  // binding and forbids re-preparation or fallback. Never expose this to WebUI.
  // It cannot release a previous owner's quarantined published listener.
  void StopConfirmed(base::OnceCallback<void(bool)> completion);
#endif
  // Called on the owning Browser sequence before the session-ending rundown
  // wait. Completion runs after child cleanup on core threads, without needing
  // the blocked Browser sequence to pump replies. Pending worker destruction
  // remains covered after its owning service has gone away. A published
  // internal listener counts as cleaned up once forwarding stops and its child
  // is reaped; its address stays held until the Network fence or process exit.
  static void StopAllForSessionEnding(base::OnceClosure completion);
  // Reports an unexpected exit of this manager's prepared child. The private
  // registration ID lets the Browser ignore a late notification after rebind.
  void SetStoppedCallback(base::RepeatingCallback<void(std::string)> callback);

 private:
  class Worker;
  class ShutdownHandle;
  std::unique_ptr<base::Thread> thread_;
  std::shared_ptr<ShutdownHandle> shutdown_;
  base::SequenceBound<Worker> worker_;
};
}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_LOCAL_CORE_MANAGER_H_
