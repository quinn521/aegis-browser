// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_local_core_manager.h"

#include <atomic>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
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
AccessOwnedChildRetirementMac RetirementFault(const AccessOwnedChildMac& child, uint32_t wait) {
  retirement_pid.store(child.pid());
  if (const auto inspected = InspectAccessOwnedListenerMac(child); inspected.proof)
    retirement_port.store(inspected.proof->port);
  return block_retirement.load() ? AccessOwnedChildRetirementMac::kDeadlineExceeded
                                : RetireAccessOwnedChildMac(child, wait);
}
std::string Id() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }

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
}  // namespace
}  // namespace aegis::access
