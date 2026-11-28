// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_owned_listener_mac.h"

#include <arpa/inet.h>
#include <libproc.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <vector>

#include "base/posix/unix_domain_socket.h"

namespace aegis::access {
namespace {
using Status = AccessOwnedListenerStatusMac;
bool OwnedProcess(pid_t pid, proc_bsdinfo& info) {
  return pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) ==
      static_cast<int>(sizeof(info)) && info.pbi_pid == static_cast<uint32_t>(pid) &&
      info.pbi_ppid == static_cast<uint32_t>(getpid()) && info.pbi_uid == geteuid() &&
      info.pbi_start_tvsec != 0 && (info.pbi_flags & PROC_FLAG_INEXIT) == 0;
}
bool SameProcess(const AccessOwnedChildMac& child, const proc_bsdinfo& info) {
  return info.pbi_start_tvsec == child.start_seconds() &&
         info.pbi_start_tvusec == child.start_microseconds();
}
// proc_pidinfo reports ESRCH for a child that has exited but is not yet reaped,
// while its PID stays allocated. sysctl still exposes the zombie's birth, parent
// and state, so only the exact captured child is reaped here. Returns true once
// the captured child no longer holds the PID, false while that exact child has
// not reached the zombie state yet, and nullopt when its identity is unclear.
std::optional<bool> ReapExactExitedChild(const AccessOwnedChildMac& child) {
  kinfo_proc process{};
  size_t length = sizeof(process);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, child.pid()};
  if (sysctl(mib, 4, &process, &length, nullptr, 0) != 0) return std::nullopt;
  if (length == 0) return true;  // The PID is fully released.
  if (length != sizeof(process) || process.kp_proc.p_pid != child.pid())
    return std::nullopt;
  if (static_cast<uint64_t>(process.kp_proc.p_starttime.tv_sec) !=
          static_cast<uint64_t>(child.start_seconds()) ||
      static_cast<uint64_t>(process.kp_proc.p_starttime.tv_usec) !=
          static_cast<uint64_t>(child.start_microseconds()))
    return true;  // A different process now owns the PID.
  if (process.kp_eproc.e_ppid != getpid()) return std::nullopt;
  if (process.kp_proc.p_stat != SZOMB) return false;
  return ReapAccessExitedChildMac(child.pid()) ? std::make_optional(true) : std::nullopt;
}
bool IsListener(const socket_fdinfo& info) {
  return info.psi.soi_kind == SOCKINFO_TCP &&
         info.psi.soi_proto.pri_tcp.tcpsi_state == TSI_S_LISTEN;
}
bool ExclusiveLoopback(const socket_fdinfo& info) {
  const auto& tcp = info.psi.soi_proto.pri_tcp;
  return IsListener(info) && info.psi.soi_family == AF_INET &&
      info.psi.soi_type == SOCK_STREAM && info.psi.soi_protocol == IPPROTO_TCP &&
      (info.psi.soi_options & SO_REUSEPORT) == 0 &&
      tcp.tcpsi_ini.insi_laddr.ina_46.i46a_addr4.s_addr == htonl(INADDR_LOOPBACK) &&
      tcp.tcpsi_ini.insi_gencnt != 0 &&
      ntohs(static_cast<uint16_t>(tcp.tcpsi_ini.insi_lport)) != 0;
}
}  // namespace

AccessOwnedChildMac::AccessOwnedChildMac(pid_t pid, uint64_t seconds, uint64_t microseconds)
    : pid_(pid), start_seconds_(seconds), start_microseconds_(microseconds) {}

std::optional<AccessOwnedChildMac> CaptureAccessOwnedChildMac(pid_t pid) {
  proc_bsdinfo info{};
  if (!OwnedProcess(pid, info)) return std::nullopt;
  return AccessOwnedChildMac(pid, info.pbi_start_tvsec, info.pbi_start_tvusec);
}

bool AccessOwnedChildStillMatchesMac(const AccessOwnedChildMac& child) {
  proc_bsdinfo info{};
  return OwnedProcess(child.pid(), info) && SameProcess(child, info);
}

bool ReapAccessExitedChildMac(pid_t pid) {
  if (pid <= 0) return false;
  int status = 0;
  pid_t result;
  do {
    errno = 0;
    result = waitpid(pid, &status, WNOHANG);
  } while (result < 0 && errno == EINTR);
  return result == pid && (WIFEXITED(status) || WIFSIGNALED(status));
}

AccessOwnedChildRetirementMac RetireAccessOwnedChildMac(
    const AccessOwnedChildMac& child, uint32_t maximum_wait_milliseconds) {
  using Result = AccessOwnedChildRetirementMac;
  using Clock = std::chrono::steady_clock;
  if (maximum_wait_milliseconds == 0 || maximum_wait_milliseconds > 2000)
    return Result::kDeadlineExceeded;
  const auto deadline = Clock::now() + std::chrono::milliseconds(maximum_wait_milliseconds);
  const auto grace = std::min(deadline, Clock::now() + std::chrono::milliseconds(250));
  const auto observe = [&]() -> std::optional<bool> {
    proc_bsdinfo info{};
    errno = 0;
    const auto size = proc_pidinfo(child.pid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (size != static_cast<int>(sizeof(info)))
      return errno == ESRCH ? ReapExactExitedChild(child) : std::nullopt;
    if (!SameProcess(child, info)) return true;
    if (info.pbi_pid != static_cast<uint32_t>(child.pid()) ||
        info.pbi_ppid != static_cast<uint32_t>(getpid()) || info.pbi_uid != geteuid())
      return std::nullopt;
    if (ReapAccessExitedChildMac(child.pid())) return true;
    return errno != 0 ? std::nullopt : std::make_optional(false);
  };
  const auto initial = observe();
  if (!initial) return Result::kIdentityUnavailable;
  if (*initial) return Result::kRetired;
  // The original, unreaped child still owns this PID at each signal boundary.
  if (kill(child.pid(), SIGTERM) != 0) {
    const auto exited = observe();
    return exited && *exited ? Result::kRetired : Result::kSignalFailed;
  }
  for (;;) {
    const auto exited = observe();
    if (!exited) return Result::kIdentityUnavailable;
    if (*exited) return Result::kRetired;
    if (Clock::now() >= grace) break;
    poll(nullptr, 0, 5);
  }
  if (Clock::now() >= deadline) return Result::kDeadlineExceeded;
  if (!AccessOwnedChildStillMatchesMac(child)) {
    const auto exited = observe();
    return exited && *exited ? Result::kRetired : Result::kIdentityUnavailable;
  }
  if (kill(child.pid(), SIGKILL) != 0) {
    const auto exited = observe();
    return exited && *exited ? Result::kRetired : Result::kSignalFailed;
  }
  for (;;) {
    const auto exited = observe();
    if (!exited) return Result::kIdentityUnavailable;
    if (*exited) return Result::kRetired;
    if (Clock::now() >= deadline) return Result::kDeadlineExceeded;
    poll(nullptr, 0, 5);
  }
}

AccessOwnedListenerInspectionMac InspectAccessOwnedListenerMac(const AccessOwnedChildMac& child) {
  proc_bsdinfo before{};
  if (!OwnedProcess(child.pid(), before) || !SameProcess(child, before))
    return {Status::kWrongOwnerOrExited, std::nullopt};
  std::vector<proc_fdinfo> descriptors(128);
  int bytes = 0;
  for (;;) {
    bytes = proc_pidinfo(child.pid(), PROC_PIDLISTFDS, 0, descriptors.data(),
                        static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
    if (bytes < 0 || bytes % sizeof(proc_fdinfo) != 0)
      return {Status::kDescriptorQueryFailed, std::nullopt};
    if (static_cast<size_t>(bytes) < descriptors.size() * sizeof(proc_fdinfo)) break;
    if (descriptors.size() >= 4096) return {Status::kDescriptorBudgetExceeded, std::nullopt};
    descriptors.resize(descriptors.size() * 2);
  }
  descriptors.resize(static_cast<size_t>(bytes) / sizeof(proc_fdinfo));
  std::optional<AccessOwnedListenerProofMac> found;
  for (const auto& descriptor : descriptors) {
    if (descriptor.proc_fdtype != PROX_FDTYPE_SOCKET) continue;
    socket_fdinfo socket{};
    errno = 0;
    if (proc_pidfdinfo(child.pid(), descriptor.proc_fd, PROC_PIDFDSOCKETINFO, &socket,
                       sizeof(socket)) != static_cast<int>(sizeof(socket))) {
      if (errno == EBADF || errno == ENOENT) continue;
      return {Status::kDescriptorQueryFailed, std::nullopt};
    }
    if (!IsListener(socket)) continue;
    if (found) return {Status::kMultipleListeners, std::nullopt};
    if (!ExclusiveLoopback(socket)) return {Status::kListenerNotExclusiveLoopback, std::nullopt};
    const auto& ip = socket.psi.soi_proto.pri_tcp.tcpsi_ini;
    found = AccessOwnedListenerProofMac{ntohs(static_cast<uint16_t>(ip.insi_lport)),
                                       descriptor.proc_fd, ip.insi_gencnt};
  }
  if (!found) return {Status::kNotListening, std::nullopt};
  socket_fdinfo final_socket{};
  proc_bsdinfo after{};
  if (proc_pidfdinfo(child.pid(), found->descriptor, PROC_PIDFDSOCKETINFO, &final_socket,
                     sizeof(final_socket)) != static_cast<int>(sizeof(final_socket)) ||
      !ExclusiveLoopback(final_socket) ||
      final_socket.psi.soi_proto.pri_tcp.tcpsi_ini.insi_gencnt != found->socket_generation ||
      ntohs(static_cast<uint16_t>(final_socket.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport)) != found->port ||
      !OwnedProcess(child.pid(), after) || !SameProcess(child, after))
    return {Status::kListenerChanged, std::nullopt};
  return {Status::kReady, found};
}

bool AccessOwnedListenerStillMatchesMac(const AccessOwnedChildMac& child,
                                        const AccessOwnedListenerProofMac& proof) {
  const auto current = InspectAccessOwnedListenerMac(child);
  return current.status == Status::kReady && current.proof == proof;
}

bool AccessHeldListenerStillMatchesMac(int descriptor,
                                      const AccessOwnedListenerProofMac& proof) {
  socket_fdinfo socket{};
  return descriptor >= 0 && proof.port != 0 && proof.socket_generation != 0 &&
      proc_pidfdinfo(getpid(), descriptor, PROC_PIDFDSOCKETINFO, &socket,
                    sizeof(socket)) == static_cast<int>(sizeof(socket)) &&
      ExclusiveLoopback(socket) &&
      socket.psi.soi_proto.pri_tcp.tcpsi_ini.insi_gencnt == proof.socket_generation &&
      ntohs(static_cast<uint16_t>(socket.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport)) == proof.port;
}

AccessListenerLeaseResultMac ReceiveAccessListenerLeaseMac(
    int channel, const AccessOwnedChildMac& child,
    const AccessOwnedListenerProofMac& proof) {
  using LeaseStatus = AccessListenerLeaseStatusMac;
  const int flags = fcntl(channel, F_GETFL);
  if (flags < 0 || !(flags & O_NONBLOCK) ||
      !AccessOwnedListenerStillMatchesMac(child, proof))
    return {LeaseStatus::kInvalid, {}};
  std::array<uint8_t, 2> message{};
  std::vector<base::ScopedFD> descriptors;
  const auto count = base::UnixDomainSocket::RecvMsg(channel, message, &descriptors);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return {LeaseStatus::kPending, {}};
  if (count != 1 || message[0] != 1 || descriptors.size() != 1 ||
      fcntl(descriptors.front().get(), F_SETFD, FD_CLOEXEC) < 0 ||
      !AccessHeldListenerStillMatchesMac(descriptors.front().get(), proof) ||
      !AccessOwnedListenerStillMatchesMac(child, proof))
    return {LeaseStatus::kInvalid, {}};
  return {LeaseStatus::kReady, std::move(descriptors.front())};
}
}  // namespace aegis::access
