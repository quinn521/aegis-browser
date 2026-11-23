// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OWNED_LISTENER_MAC_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OWNED_LISTENER_MAC_H_

#include <sys/types.h>

#include <cstdint>
#include <optional>

#include "base/files/scoped_file.h"

namespace aegis::access {
// A child captured by its actual parent, for this process/UID and birth time.
// A PID or a listening port supplied by a page cannot construct this identity.
class AccessOwnedChildMac {
 public:
  pid_t pid() const { return pid_; }
  uint64_t start_seconds() const { return start_seconds_; }
  uint64_t start_microseconds() const { return start_microseconds_; }
 private:
  friend std::optional<AccessOwnedChildMac> CaptureAccessOwnedChildMac(pid_t pid);
  AccessOwnedChildMac(pid_t pid, uint64_t seconds, uint64_t microseconds);
  pid_t pid_;
  uint64_t start_seconds_;
  uint64_t start_microseconds_;
};

struct AccessOwnedListenerProofMac {
  uint16_t port = 0;
  int descriptor = -1;
  uint64_t socket_generation = 0;
  bool operator==(const AccessOwnedListenerProofMac&) const = default;
};
enum class AccessOwnedListenerStatusMac {
  kReady,
  kWrongOwnerOrExited,
  kNotListening,
  kDescriptorQueryFailed,
  kDescriptorBudgetExceeded,
  kMultipleListeners,
  kListenerNotExclusiveLoopback,
  kListenerChanged,
};
struct AccessOwnedListenerInspectionMac {
  AccessOwnedListenerStatusMac status;
  std::optional<AccessOwnedListenerProofMac> proof;
};

std::optional<AccessOwnedChildMac> CaptureAccessOwnedChildMac(pid_t pid);
// A process health check that never reaps the child. Keeping the child waitable
// prevents its PID from being reused before the owning worker retires it.
bool AccessOwnedChildStillMatchesMac(const AccessOwnedChildMac& child);
enum class AccessOwnedChildRetirementMac {
  kRetired,
  kIdentityUnavailable,
  kSignalFailed,
  kDeadlineExceeded,
};
// Only the original parent calls this, without another reaper for this child.
// Signals require the captured birth/UID/parent to match. Returns kRetired only
// after exit/reaping or proof that this exact process no longer exists. Unknown
// outcomes keep the caller's Process, directory and binding behind a barrier.
// The total wait is bounded to 2 seconds, including a short SIGTERM grace.
AccessOwnedChildRetirementMac RetireAccessOwnedChildMac(
    const AccessOwnedChildMac& child, uint32_t maximum_wait_milliseconds);
// For a launch that exited before its birth identity could be captured. Never
// signals or treats a query error as exit; only reaps this actual exited child.
bool ReapAccessExitedChildMac(pid_t pid);
// Reads only this captured child's descriptors. No process-wide search or
// reserve/release/rebind: the actual OS port comes from the same live socket.
// This proves exclusive TCP listener ownership, not protocol or upstream health.
AccessOwnedListenerInspectionMac InspectAccessOwnedListenerMac(
    const AccessOwnedChildMac& child);
// Every later use must revalidate this exact process, descriptor and socket
// generation; a reused numeric port or PID does not revive an old proof.
bool AccessOwnedListenerStillMatchesMac(const AccessOwnedChildMac& child,
                                        const AccessOwnedListenerProofMac& proof);

enum class AccessListenerLeaseStatusMac { kPending, kReady, kInvalid };
struct AccessListenerLeaseResultMac {
  AccessListenerLeaseStatusMac status;
  base::ScopedFD descriptor;
};
// Receives exactly one listener FD over a native-created nonblocking socketpair
// inherited only by this child. Version 1 is one byte with value 1 and one FD.
// The received kernel socket must match the actual child's current proof.
// A duplicate keeps the port occupied after child death; its native owner must
// retain it until Network Service confirms the old consumer has been revoked.
AccessListenerLeaseResultMac ReceiveAccessListenerLeaseMac(
    int channel, const AccessOwnedChildMac& child,
    const AccessOwnedListenerProofMac& proof);
// Does not require the child to remain alive. Never accepts a numeric port as
// proof that an unrelated new listener is the original kernel socket.
bool AccessHeldListenerStillMatchesMac(int descriptor,
                                      const AccessOwnedListenerProofMac& proof);
}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OWNED_LISTENER_MAC_H_
