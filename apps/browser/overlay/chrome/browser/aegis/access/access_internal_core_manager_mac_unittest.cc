// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_local_core_manager.h"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/synchronization/lock.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/execution_fence.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/timer/elapsed_timer.h"
#include "base/uuid.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
std::atomic_bool block_retirement{false};
std::atomic<pid_t> retirement_pid{0};
std::atomic<uint16_t> retirement_port{0};
struct RetiringExecutable {
  base::Lock lock;
  base::FilePath path GUARDED_BY(lock);
};

RetiringExecutable& GetRetiringExecutable() {
  static base::NoDestructor<RetiringExecutable> executable;
  return *executable;
}

AccessOwnedChildRetirementMac RetirementFault(const AccessOwnedChildMac& child, uint32_t wait) {
  retirement_pid.store(child.pid());
  {
    auto& executable = GetRetiringExecutable();
    base::AutoLock lock(executable.lock);
    executable.path = base::GetProcessExecutablePath(child.pid());
  }
  if (const auto inspected = InspectAccessOwnedListenerMac(child); inspected.proof)
    retirement_port.store(inspected.proof->port);
  return block_retirement.load() ? AccessOwnedChildRetirementMac::kDeadlineExceeded
                                : RetireAccessOwnedChildMac(child, wait);
}
std::string Id() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }
// True while some socket still listens on 127.0.0.1:port. SO_REUSEADDR keeps
// a released address in TIME_WAIT from reading as held.
bool LoopbackPortHeld(uint16_t port) {
  base::ScopedFD probe(socket(AF_INET, SOCK_STREAM, 0));
  if (!probe.is_valid()) return true;
  const int enable = 1;
  setsockopt(probe.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  sockaddr_in address = {};
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return bind(probe.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0;
}

class AccessInternalCoreManagerMacTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto executable = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
        "aegis-owned-core-test-binary");
    if (executable.empty()) GTEST_SKIP() << "Explicit isolated core binary required for integration";
    if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate))
      GTEST_SKIP() << "This build rejects internal test activation";
    const auto bytes = base::ReadFileToBytes(executable);
    ASSERT_TRUE(bytes);
    ASSERT_TRUE(fixture_.CreateUniqueTempDir());
    auto manifest = base::DictValue().Set("schemaVersion", 2)
        .Set("binaryPath", executable.AsUTF8Unsafe())
        .Set("binarySha256", base::HexEncodeLower(crypto::hash::Sha256(*bytes)))
        .Set("exclusiveLoopbackTcp", true).Set("systemAllocatedTcpPort", true)
        .Set("parentHeldListenerLease", true).Set("parentExitWatch", true)
        .Set("localHttpNoAuth", true).Set("localSocks5NoAuth", true);
    std::string encoded;
    ASSERT_TRUE(base::JSONWriter::Write(manifest, &encoded));
    input_.manifest = fixture_.GetPath().AppendASCII("manifest.json");
    ASSERT_TRUE(base::WriteFile(input_.manifest, encoded));
    auto scope = Id() + Id();
    std::erase(scope, '-');
    vault_ = std::make_unique<AccessTestNodeVaultMac>("debug", scope);
    auto parsed = aegis_access::ParseAccessTestNodeShare(aegis_access::test::kSyntheticTestNodeShare);
    ASSERT_TRUE(parsed.node);
    // No external node or origin is contacted. The canary's unsafe port fails
    // before dispatch, after the manager has observed its actual core listener.
    parsed.node->address = "127.0.0.1";
    parsed.node->port = 9;
    parsed.node->public_key = std::string(43, 'A');
    parsed.node->short_id.clear();
    const auto staged = vault_->Stage(Id(), *parsed.node);
    ASSERT_TRUE(staged.reference);
    AccessInternalTestCoreBindingMac binding{Id(), 1, 1, "debug", scope,
        *staged.reference, {}, aegis_access::RegisteredProxyTransport::kSocks5,
        GURL("https://127.0.0.1:1/controlled"), "0123456789abcdef0123456789abcdef"};
    binding.preferences.protocol = aegis_access::AccessInboundProtocolPreference::kSocks5;
    input_.source = AccessLocalCoreSource::kInternalTestProfile;
    input_.internal_test_profile = std::move(binding);
    input_.retire_owned_child_for_testing = &RetirementFault;
    retirement_pid.store(0);
    retirement_port.store(0);
    block_retirement.store(true);
  }
  void TearDown() override {
    block_retirement.store(false);
    manager_.reset();
    environment_.RunUntilIdle();
    vault_.reset();
  }
  void PrepareWithBlockedRetirement() {
    manager_ = std::make_unique<AccessLocalCoreManager>(input_);
    base::test::TestFuture<AccessLocalCoreReady> prepared;
    manager_->Prepare(prepared.GetCallback());
    ASSERT_FALSE(prepared.Get().ready);
    ASSERT_TRUE(prepared.Get().retirement_pending);
    ASSERT_GT(retirement_pid.load(), 0);
    ASSERT_GT(retirement_port.load(), 0);
  }
  base::test::TaskEnvironment environment_{base::test::TaskEnvironment::MainThreadType::IO};
  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(40)};
  base::ScopedTempDir fixture_;
  AccessLocalCoreInput input_;
  std::unique_ptr<AccessTestNodeVaultMac> vault_;
  std::unique_ptr<AccessLocalCoreManager> manager_;
};

TEST_F(AccessInternalCoreManagerMacTest, UnconfirmedRetirementCannotVerifyOrReprepare) {
  PrepareWithBlockedRetirement();
  ASSERT_TRUE(manager_);
  const auto pid = retirement_pid.load();
  const auto child = CaptureAccessOwnedChildMac(pid);
  ASSERT_TRUE(child);
  const auto listener = InspectAccessOwnedListenerMac(*child);
  ASSERT_TRUE(listener.proof);
  base::test::TestFuture<bool> blocked;
  manager_->StopConfirmed(blocked.GetCallback());
  EXPECT_FALSE(blocked.Get());
  base::test::TestFuture<AccessLocalCoreReady> verified, prepared;
  manager_->Verify(verified.GetCallback());
  EXPECT_FALSE(verified.Get().ready);
  EXPECT_TRUE(verified.Get().retirement_pending);
  manager_->Prepare(prepared.GetCallback());
  EXPECT_FALSE(prepared.Get().ready);
  EXPECT_TRUE(prepared.Get().retirement_pending);
  EXPECT_EQ(retirement_pid.load(), pid);
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*child, *listener.proof));
  EXPECT_TRUE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
  block_retirement.store(false);
  base::test::TestFuture<bool> retired;
  manager_->StopConfirmed(retired.GetCallback());
  EXPECT_TRUE(retired.Get());
  EXPECT_FALSE(AccessOwnedChildStillMatchesMac(*child));
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*child, *listener.proof));
  EXPECT_FALSE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
  base::test::TestFuture<AccessLocalCoreReady> old_binding;
  manager_->Prepare(old_binding.GetCallback());
  EXPECT_FALSE(old_binding.Get().ready);
  EXPECT_FALSE(old_binding.Get().retirement_pending);
  EXPECT_FALSE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
}

TEST_F(AccessInternalCoreManagerMacTest, DestroyedOwnerRetainsBarrierUntilExactOldChildRetires) {
  PrepareWithBlockedRetirement();
  ASSERT_TRUE(manager_);
  const auto original = CaptureAccessOwnedChildMac(retirement_pid.load());
  ASSERT_TRUE(original);
  const auto listener = InspectAccessOwnedListenerMac(*original);
  ASSERT_TRUE(listener.proof);
  manager_.reset();
  environment_.RunUntilIdle();
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*original, *listener.proof));
  EXPECT_TRUE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
  // The new manager must retire the quarantined exact old child before launch.
  PrepareWithBlockedRetirement();
  EXPECT_FALSE(AccessOwnedChildStillMatchesMac(*original));
  EXPECT_NE(retirement_pid.load(), original->pid());
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*original, *listener.proof));
  block_retirement.store(false);
  base::test::TestFuture<bool> retired;
  manager_->StopConfirmed(retired.GetCallback());
  EXPECT_TRUE(retired.Get());
}

TEST_F(AccessInternalCoreManagerMacTest, CancelDuringPreparationCannotReturnReadyAfterConfirmedStop) {
  block_retirement.store(false);
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> prepared;
  base::test::TestFuture<bool> retired;
  manager_->Prepare(prepared.GetCallback());
  manager_->StopConfirmed(retired.GetCallback());
  EXPECT_TRUE(retired.Get());
  EXPECT_FALSE(prepared.Get().ready);
  EXPECT_FALSE(prepared.Get().retirement_pending);
  base::test::TestFuture<AccessLocalCoreReady> verified;
  manager_->Verify(verified.GetCallback());
  EXPECT_FALSE(verified.Get().ready);
}
TEST_F(AccessInternalCoreManagerMacTest, NewOwnerCanRetireQuarantineWithoutPreparingAnotherCore) {
  PrepareWithBlockedRetirement();
  const auto original = CaptureAccessOwnedChildMac(retirement_pid.load());
  ASSERT_TRUE(original);
  manager_.reset();
  environment_.RunUntilIdle();
  EXPECT_TRUE(vault_->HasRuntimeReferences());
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  // Reservation precedes even the queued worker's construction/preparation.
  EXPECT_TRUE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
  base::test::TestFuture<bool> retired;
  manager_->StopConfirmed(retired.GetCallback());
  EXPECT_TRUE(retired.Get());
  EXPECT_FALSE(AccessOwnedChildStillMatchesMac(*original));
  EXPECT_EQ(retirement_pid.load(), original->pid());
  EXPECT_FALSE(vault_->HasRuntimeReferences());
}
TEST_F(AccessInternalCoreManagerMacTest, ExpiredOriginalDeadlineCannotLaunchOrRenewAuthorization) {
  input_.internal_preparation_deadline = base::TimeTicks::Now() - base::Milliseconds(1);
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> prepared;
  manager_->Prepare(prepared.GetCallback());
  EXPECT_FALSE(prepared.Get().ready);
  EXPECT_FALSE(prepared.Get().retirement_pending);
  EXPECT_EQ(prepared.Get().error, "The original preparation deadline has expired.");
  EXPECT_EQ(retirement_pid.load(), 0);
  EXPECT_FALSE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
}
TEST_F(AccessInternalCoreManagerMacTest, SessionEndingCompletesAfterReapWhilePublishedListenerStaysHeld) {
  block_retirement.store(false);
  input_.skip_internal_canary_for_testing = true;
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> prepared;
  manager_->Prepare(prepared.GetCallback());
  ASSERT_TRUE(prepared.Get().ready) << prepared.Get().error;
  ASSERT_TRUE(prepared.Get().parent_listener_lease_held);
  const uint16_t port = prepared.Get().port;
  ASSERT_GT(port, 0);
  ASSERT_TRUE(LoopbackPortHeld(port));
  ASSERT_EQ(retirement_pid.load(), 0);
  {
    // Without the fix the barrier never runs and EndSession waits its full 10 s.
    base::ElapsedTimer elapsed;
    auto stopped = std::make_shared<base::WaitableEvent>();
    AccessLocalCoreManager::StopAllForSessionEnding(base::BindOnce(
        [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); }, stopped));
    base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
    ASSERT_TRUE(stopped->TimedWait(base::Seconds(5)));
    EXPECT_LT(elapsed.Elapsed(), base::Seconds(5));
  }
  // Session ending retired and reaped the exact owned child.
  const pid_t pid = retirement_pid.load();
  ASSERT_GT(pid, 0);
  errno = 0;
  EXPECT_EQ(kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
  // The published address stays reserved until the Network fence confirms.
  EXPECT_TRUE(LoopbackPortHeld(port));
  EXPECT_TRUE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
  base::test::TestFuture<AccessLocalCoreReady> reprepared;
  manager_->Prepare(reprepared.GetCallback());
  EXPECT_FALSE(reprepared.Get().ready);
  base::test::TestFuture<bool> retired;
  manager_->StopConfirmed(retired.GetCallback());
  EXPECT_TRUE(retired.Get());
  EXPECT_FALSE(LoopbackPortHeld(port));
  EXPECT_FALSE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
}

TEST_F(AccessInternalCoreManagerMacTest,
       SessionEndingCompletesAfterManagerDestroyedWithPublishedListener) {
  block_retirement.store(false);
  input_.skip_internal_canary_for_testing = true;
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> prepared;
  manager_->Prepare(prepared.GetCallback());
  ASSERT_TRUE(prepared.Get().ready) << prepared.Get().error;
  ASSERT_TRUE(prepared.Get().parent_listener_lease_held);
  const uint16_t port = prepared.Get().port;
  ASSERT_GT(port, 0);

  manager_.reset();
  // The manager posts the worker-thread join to the ThreadPool.
  base::ThreadPoolInstance::Get()->FlushForTesting();
  const pid_t pid = retirement_pid.load();
  ASSERT_GT(pid, 0);
  ASSERT_EQ(kill(pid, 0), -1);
  ASSERT_EQ(errno, ESRCH);

  auto stopped = std::make_shared<base::WaitableEvent>();
  AccessLocalCoreManager::StopAllForSessionEnding(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      stopped));
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  EXPECT_TRUE(stopped->TimedWait(base::Seconds(5)));
  base::FilePath staged_executable;
  {
    auto& executable = GetRetiringExecutable();
    base::AutoLock lock(executable.lock);
    staged_executable = executable.path;
  }
  ASSERT_EQ(staged_executable.BaseName().value(), "xray");
  EXPECT_FALSE(base::PathExists(staged_executable.DirName()));
  EXPECT_TRUE(LoopbackPortHeld(port));
  EXPECT_TRUE(vault_->IsRuntimeReferenced(input_.internal_test_profile->reference));
}

TEST_F(AccessInternalCoreManagerMacTest,
       QueuedSessionStopWaitsForQuarantinedChildReap) {
  input_.skip_internal_canary_for_testing = true;
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> prepared;
  manager_->Prepare(prepared.GetCallback());
  ASSERT_TRUE(prepared.Get().ready) << prepared.Get().error;
  ASSERT_TRUE(prepared.Get().parent_listener_lease_held);
  const uint16_t port = prepared.Get().port;
  ASSERT_GT(port, 0);

  auto stopped = std::make_shared<base::WaitableEvent>();
  {
    // Queue session stop behind worker destruction and ahead of the join.
    base::ScopedThreadPoolExecutionFence defer_thread_join;
    manager_.reset();
    AccessLocalCoreManager::StopAllForSessionEnding(base::BindOnce(
        [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
        stopped));
  }
  base::ThreadPoolInstance::Get()->FlushForTesting();
  const pid_t pid = retirement_pid.load();
  ASSERT_GT(pid, 0);
  EXPECT_EQ(kill(pid, 0), 0);
  EXPECT_FALSE(stopped->IsSignaled());
  EXPECT_TRUE(LoopbackPortHeld(port));

  // A subsequent attempt can reap the quarantined child. Its prior Network
  // binding still prevents this new manager from using the listener.
  block_retirement.store(false);
  manager_ = std::make_unique<AccessLocalCoreManager>(input_);
  base::test::TestFuture<AccessLocalCoreReady> retry;
  manager_->Prepare(retry.GetCallback());
  EXPECT_FALSE(retry.Get().ready);
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  EXPECT_TRUE(stopped->TimedWait(base::Seconds(5)));
  EXPECT_EQ(kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
  EXPECT_TRUE(LoopbackPortHeld(port));
}
}  // namespace
}  // namespace aegis::access
