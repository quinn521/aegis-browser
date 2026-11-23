// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_owned_listener_mac.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "base/command_line.h"
#include "base/files/scoped_file.h"
#include "base/process/process.h"
#include "base/posix/unix_domain_socket.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/multiprocess_test.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "testing/multiprocess_func_list.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
using Status = AccessOwnedListenerStatusMac;

MULTIPROCESS_TEST_MAIN(AccessOwnedListenerMacChild) {
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto mode = command->GetSwitchValueASCII("listener-test-mode");
  if (mode == "ignore-term") signal(SIGTERM, SIG_IGN);
  unsigned requested = 0;
  if (!base::StringToUint(command->GetSwitchValueASCII("listener-test-port"), &requested) ||
      requested > 65535) return 10;
  base::ScopedFD first, second;
  auto listen = [&](base::ScopedFD& descriptor) {
    descriptor.reset(socket(AF_INET, SOCK_STREAM, 0));
    if (!descriptor.is_valid()) return false;
    if (mode == "reuse") {
      const int enabled = 1;
      if (setsockopt(descriptor.get(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0)
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(mode == "wildcard" ? INADDR_ANY : INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(requested));
    return bind(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
           ::listen(descriptor.get(), 1) == 0;
  };
  if (mode != "none" && !listen(first)) return 11;
  if (mode == "multiple" && !listen(second)) return 12;
  if (mode.starts_with("lease")) {
    unsigned channel = 0;
    if (!base::StringToUint(command->GetSwitchValueASCII("listener-test-channel"), &channel) ||
        channel != 200) return 13;
    uint8_t version = mode == "lease-version" ? 2 : 1;
    std::vector<int> descriptors;
    if (mode == "lease-wrong") {
      second.reset(socket(AF_INET, SOCK_STREAM, 0));
      descriptors.push_back(second.get());
    } else if (mode != "lease-none") {
      descriptors.push_back(first.get());
      if (mode == "lease-multiple") descriptors.push_back(first.get());
    }
    if (!base::UnixDomainSocket::SendMsg(channel, base::span_from_ref(version), descriptors))
      return 14;
    second.reset();
    close(channel);
  }
  for (;;) pause();
}

class ScopedListenerChild {
 public:
  explicit ScopedListenerChild(const char* mode, uint16_t port = 0) {
    auto command = base::GetMultiProcessTestChildBaseCommandLine();
    command.AppendSwitchASCII("listener-test-mode", mode);
    command.AppendSwitchASCII("listener-test-port", base::NumberToString(port));
    base::LaunchOptions options;
    base::ScopedFD child_channel;
    if (std::string_view(mode).starts_with("lease")) {
      if (!base::CreateSocketPair(&channel_, &child_channel) ||
          fcntl(channel_.get(), F_SETFL, O_NONBLOCK) < 0 ||
          fcntl(channel_.get(), F_SETFD, FD_CLOEXEC) < 0) return;
      options.fds_to_remap.emplace_back(child_channel.get(), 200);
      command.AppendSwitchASCII("listener-test-channel", "200");
    }
    process_ = base::SpawnMultiProcessTestChild("AccessOwnedListenerMacChild", command, options);
  }
  ~ScopedListenerChild() { Stop(); }
  pid_t pid() const { return process_.Pid(); }
  int channel() const { return channel_.get(); }
  AccessOwnedChildRetirementMac Retire(const AccessOwnedChildMac& child, uint32_t wait) {
    const auto result = RetireAccessOwnedChildMac(child, wait);
    if (result == AccessOwnedChildRetirementMac::kRetired) process_.Close();
    return result;
  }
  void Stop() {
    if (process_.IsValid()) {
      base::TerminateMultiProcessTestChild(process_, 0, true);
      process_.Close();
    }
  }
 private:
  base::Process process_;
  base::ScopedFD channel_;
};

AccessOwnedListenerInspectionMac WaitFor(const AccessOwnedChildMac& child, Status expected) {
  auto result = InspectAccessOwnedListenerMac(child);
  const auto deadline = base::TimeTicks::Now() + base::Seconds(5);
  while (result.status != expected && base::TimeTicks::Now() < deadline) {
    base::PlatformThread::Sleep(base::Milliseconds(10));
    result = InspectAccessOwnedListenerMac(child);
  }
  return result;
}

AccessListenerLeaseResultMac WaitForLease(int channel, const AccessOwnedChildMac& child,
                                         const AccessOwnedListenerProofMac& proof) {
  auto result = ReceiveAccessListenerLeaseMac(channel, child, proof);
  const auto deadline = base::TimeTicks::Now() + base::Seconds(5);
  while (result.status == AccessListenerLeaseStatusMac::kPending &&
         base::TimeTicks::Now() < deadline) {
    base::PlatformThread::Sleep(base::Milliseconds(10));
    result = ReceiveAccessListenerLeaseMac(channel, child, proof);
  }
  return result;
}

TEST(AccessOwnedListenerMacTest, SameOwnedSocketReportsDistinctSystemAssignedPortsAndRetiresIndependently) {
  ScopedListenerChild a("exclusive"), b("exclusive");
  const auto owned_a = CaptureAccessOwnedChildMac(a.pid());
  const auto owned_b = CaptureAccessOwnedChildMac(b.pid());
  ASSERT_TRUE(owned_a);
  ASSERT_TRUE(owned_b);
  const auto first = WaitFor(*owned_a, Status::kReady);
  const auto second = WaitFor(*owned_b, Status::kReady);
  ASSERT_TRUE(first.proof);
  ASSERT_TRUE(second.proof);
  EXPECT_NE(first.proof->port, 0u);
  EXPECT_NE(second.proof->port, 0u);
  EXPECT_NE(first.proof->port, second.proof->port);
  EXPECT_NE(first.proof->socket_generation, second.proof->socket_generation);
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*owned_a, *first.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*owned_b, *second.proof));
  a.Stop();
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*owned_a, *first.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*owned_b, *second.proof));
  EXPECT_FALSE(CaptureAccessOwnedChildMac(getpid()));
}

TEST(AccessOwnedListenerMacTest, RejectsReusePortWildcardAndMultipleListeners) {
  for (const auto mode : {"reuse", "wildcard", "multiple"}) {
    SCOPED_TRACE(mode);
    ScopedListenerChild child(mode);
    const auto owned = CaptureAccessOwnedChildMac(child.pid());
    ASSERT_TRUE(owned);
    const auto expected = std::string_view(mode) == "multiple"
        ? Status::kMultipleListeners : Status::kListenerNotExclusiveLoopback;
    const auto inspected = WaitFor(*owned, expected);
    EXPECT_EQ(inspected.status, expected);
    EXPECT_FALSE(inspected.proof);
  }
}

TEST(AccessOwnedListenerMacTest, ReboundPortDoesNotReviveOldProcessOrSocketProof) {
  ScopedListenerChild first("exclusive");
  const auto original = CaptureAccessOwnedChildMac(first.pid());
  ASSERT_TRUE(original);
  const auto proof = WaitFor(*original, Status::kReady);
  ASSERT_TRUE(proof.proof);
  first.Stop();
  ScopedListenerChild next("exclusive", proof.proof->port);
  const auto replacement = CaptureAccessOwnedChildMac(next.pid());
  ASSERT_TRUE(replacement);
  const auto new_proof = WaitFor(*replacement, Status::kReady);
  ASSERT_TRUE(new_proof.proof);
  EXPECT_EQ(proof.proof->port, new_proof.proof->port);
  EXPECT_NE(proof.proof->socket_generation, new_proof.proof->socket_generation);
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*original, *proof.proof));
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*replacement, *proof.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*replacement, *new_proof.proof));
}

TEST(AccessOwnedListenerMacTest, RetirementTimeoutPreservesIdentityUntilConfirmedForcedExit) {
  using Retirement = AccessOwnedChildRetirementMac;
  ScopedListenerChild blocked("ignore-term"), other("exclusive");
  const auto owned = CaptureAccessOwnedChildMac(blocked.pid());
  const auto separate = CaptureAccessOwnedChildMac(other.pid());
  ASSERT_TRUE(owned);
  ASSERT_TRUE(separate);
  const auto listener = WaitFor(*owned, Status::kReady);
  const auto other_listener = WaitFor(*separate, Status::kReady);
  ASSERT_TRUE(listener.proof);
  ASSERT_TRUE(other_listener.proof);
  EXPECT_EQ(blocked.Retire(*owned, 0), Retirement::kDeadlineExceeded);
  EXPECT_EQ(blocked.Retire(*owned, 50), Retirement::kDeadlineExceeded);
  EXPECT_TRUE(AccessOwnedChildStillMatchesMac(*owned));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*owned, *listener.proof));
  const auto started = base::TimeTicks::Now();
  EXPECT_EQ(blocked.Retire(*owned, 2000), Retirement::kRetired);
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(2));
  EXPECT_FALSE(AccessOwnedChildStillMatchesMac(*owned));
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*owned, *listener.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*separate, *other_listener.proof));
  EXPECT_EQ(RetireAccessOwnedChildMac(*owned, 2000), Retirement::kRetired);
  EXPECT_FALSE(ReapAccessExitedChildMac(getpid()));
}

TEST(AccessOwnedListenerMacTest, NormalRetirementConfirmsExitWithoutReapingDuringHealthChecks) {
  ScopedListenerChild child("exclusive");
  const auto owned = CaptureAccessOwnedChildMac(child.pid());
  ASSERT_TRUE(owned);
  ASSERT_TRUE(WaitFor(*owned, Status::kReady).proof);
  EXPECT_TRUE(AccessOwnedChildStillMatchesMac(*owned));
  EXPECT_TRUE(AccessOwnedChildStillMatchesMac(*owned));
  EXPECT_FALSE(ReapAccessExitedChildMac(child.pid()));
  EXPECT_EQ(child.Retire(*owned, 2000), AccessOwnedChildRetirementMac::kRetired);
  EXPECT_FALSE(AccessOwnedChildStillMatchesMac(*owned));
}

TEST(AccessOwnedListenerMacTest, DuplicatedKernelListenerReservesAddressAfterChildDeathUntilNativeRelease) {
  ScopedListenerChild child("lease");
  const auto owned = CaptureAccessOwnedChildMac(child.pid());
  ASSERT_TRUE(owned);
  const auto listener = WaitFor(*owned, Status::kReady);
  ASSERT_TRUE(listener.proof);
  auto lease = WaitForLease(child.channel(), *owned, *listener.proof);
  ASSERT_EQ(lease.status, AccessListenerLeaseStatusMac::kReady);
  ASSERT_TRUE(lease.descriptor.is_valid());
  EXPECT_TRUE(AccessHeldListenerStillMatchesMac(lease.descriptor.get(), *listener.proof));
  EXPECT_EQ(child.Retire(*owned, 2000), AccessOwnedChildRetirementMac::kRetired);
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*owned, *listener.proof));
  EXPECT_TRUE(AccessHeldListenerStillMatchesMac(lease.descriptor.get(), *listener.proof));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(listener.proof->port);
  base::ScopedFD next(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(next.is_valid());
  EXPECT_EQ(bind(next.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), -1);
  EXPECT_EQ(errno, EADDRINUSE);
  lease.descriptor.reset();
  EXPECT_EQ(bind(next.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
}

TEST(AccessOwnedListenerMacTest, ListenerLeaseRejectsForeignSocketExtraDescriptorsVersionAndEmptyRights) {
  for (const auto mode : {"lease-wrong", "lease-multiple", "lease-version", "lease-none"}) {
    SCOPED_TRACE(mode);
    ScopedListenerChild child(mode);
    const auto owned = CaptureAccessOwnedChildMac(child.pid());
    ASSERT_TRUE(owned);
    const auto listener = WaitFor(*owned, Status::kReady);
    ASSERT_TRUE(listener.proof);
    auto lease = WaitForLease(child.channel(), *owned, *listener.proof);
    EXPECT_EQ(lease.status, AccessListenerLeaseStatusMac::kInvalid);
    EXPECT_FALSE(lease.descriptor.is_valid());
    EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*owned, *listener.proof));
  }
}
}  // namespace
}  // namespace aegis::access
