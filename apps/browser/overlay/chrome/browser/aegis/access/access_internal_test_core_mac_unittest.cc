// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_internal_test_core_mac.h"

#include <signal.h>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/platform_thread.h"
#include "base/uuid.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
using Status = AccessInternalTestCoreLaunchStatusMac;
std::string Id() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }
aegis_access::AccessBuildIdentity Identity(aegis_access::AccessProductChannel channel) {
  return aegis_access::ResolveAccessBuildIdentity(channel,
      aegis_access::BrowserReleaseChannel::kUnknown, aegis_access::ChannelNamespace::kInvalid, true);
}
struct OwnedLaunch {
  base::ScopedTempDir directory;
  base::Process process;
  base::ScopedFD listener_channel;
  base::ScopedFD held_listener;
  std::optional<AccessOwnedChildMac> child;
  std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> lease;
  ~OwnedLaunch() { Stop(); }
  void Stop() {
    if (!process.IsValid()) { held_listener.reset(); listener_channel.reset(); lease.reset(); return; }
    const bool retired = child
        ? RetireAccessOwnedChildMac(*child, 2000) == AccessOwnedChildRetirementMac::kRetired
        : ReapAccessExitedChildMac(process.Pid());
    EXPECT_TRUE(retired);
    if (retired) {
      process.Close();
      held_listener.reset();
      listener_channel.reset();
      lease.reset();
    }
  }
};
AccessInternalTestCoreBindingMac Binding() {
  auto scope = Id() + Id();
  std::erase(scope, '-');
  return {Id(), 1, 1, "debug", scope, {Id()}, {},
          aegis_access::RegisteredProxyTransport::kSocks5};
}

TEST(AccessInternalTestCoreMacTest, RestrictedIdentityCannotReadManifestOrLaunch) {
  for (const auto product : {aegis_access::AccessProductChannel::kBeta,
                            aegis_access::AccessProductChannel::kStable,
                            aegis_access::AccessProductChannel::kUnknown}) {
    OwnedLaunch state;
    EXPECT_EQ(LaunchAccessInternalTestCoreMac(Identity(product), {}, Binding(),
        base::TimeTicks::Now() + base::Seconds(5), state.directory, state.process, state.child, state.lease, state.listener_channel),
        Status::kNotAdmitted);
    EXPECT_FALSE(state.directory.IsValid());
    EXPECT_FALSE(state.process.IsValid());
    EXPECT_FALSE(state.child);
  }
}

TEST(AccessInternalTestCoreMacTest, InvalidBindingAndMissingManifestCreateNoChild) {
  OwnedLaunch state;
  auto binding = Binding();
  binding.authorization_generation = 0;
  EXPECT_EQ(LaunchAccessInternalTestCoreMac(Identity(aegis_access::AccessProductChannel::kDebug),
      {}, binding, base::TimeTicks::Now() + base::Seconds(5),
      state.directory, state.process, state.child, state.lease, state.listener_channel), Status::kInvalidBinding);
  binding.authorization_generation = 1;
  state.lease = AccessTestNodeVaultMac::ReserveRuntimeReference(binding.channel,
      binding.vault_scope_hash, binding.reference);
  EXPECT_EQ(LaunchAccessInternalTestCoreMac(Identity(aegis_access::AccessProductChannel::kDebug),
      {}, binding, base::TimeTicks::Now() + base::Seconds(5),
      state.directory, state.process, state.child, state.lease, state.listener_channel), Status::kInvalidManifest);
  EXPECT_FALSE(state.directory.IsValid());
  EXPECT_FALSE(state.process.IsValid());
}
TEST(AccessInternalTestCoreMacTest, MissingOrForeignReferenceReservationCannotReadManifest) {
  OwnedLaunch state;
  const auto binding = Binding();
  const auto identity = Identity(aegis_access::AccessProductChannel::kDebug);
  EXPECT_EQ(LaunchAccessInternalTestCoreMac(identity, {}, binding,
      base::TimeTicks::Now() + base::Seconds(5), state.directory, state.process,
      state.child, state.lease, state.listener_channel), Status::kSecretUnavailable);
  state.lease = AccessTestNodeVaultMac::ReserveRuntimeReference(
      "alpha", binding.vault_scope_hash, binding.reference);
  ASSERT_TRUE(state.lease);
  EXPECT_EQ(LaunchAccessInternalTestCoreMac(identity, {}, binding,
      base::TimeTicks::Now() + base::Seconds(5), state.directory, state.process,
      state.child, state.lease, state.listener_channel), Status::kSecretUnavailable);
  EXPECT_FALSE(state.directory.IsValid());
  EXPECT_FALSE(state.process.IsValid());
}

TEST(AccessInternalTestCoreMacTest, PinnedCoreLoadsNativeKeychainConfigViaPipeAndOwnsDistinctPorts) {
  const auto executable = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      "aegis-owned-core-test-binary");
  if (executable.empty()) GTEST_SKIP() << "Explicit isolated core binary required for integration";
  const auto bytes = base::ReadFileToBytes(executable);
  ASSERT_TRUE(bytes);
  const auto pin = base::HexEncodeLower(crypto::hash::Sha256(*bytes));
  base::ScopedTempDir fixture;
  ASSERT_TRUE(fixture.CreateUniqueTempDir());
  const auto manifest_path = fixture.GetPath().AppendASCII("core-manifest.json");
  auto manifest = base::DictValue().Set("schemaVersion", 2)
      .Set("binaryPath", executable.AsUTF8Unsafe()).Set("binarySha256", pin)
      .Set("exclusiveLoopbackTcp", true).Set("systemAllocatedTcpPort", true)
      .Set("parentHeldListenerLease", true).Set("parentExitWatch", true).Set("localHttpNoAuth", true).Set("localSocks5NoAuth", true);
  std::string encoded;
  ASSERT_TRUE(base::JSONWriter::Write(manifest, &encoded));
  ASSERT_TRUE(base::WriteFile(manifest_path, encoded));
  auto first_binding = Binding(), second_binding = Binding();
  second_binding.selected_transport = aegis_access::RegisteredProxyTransport::kHttp;
  AccessTestNodeVaultMac first_vault("debug", first_binding.vault_scope_hash),
      second_vault("debug", second_binding.vault_scope_hash);
  auto node = *aegis_access::ParseAccessTestNodeShare(aegis_access::test::kSyntheticTestNodeShare).node;
  node.public_key = std::string(43, 'A');
  node.short_id.clear();
  node.advertises_x25519_mlkem768 = true;
  const auto first_node = first_vault.Stage(Id(), node), second_node = second_vault.Stage(Id(), node);
  ASSERT_TRUE(first_node.reference);
  ASSERT_TRUE(second_node.reference);
  first_binding.reference = *first_node.reference;
  second_binding.reference = *second_node.reference;
  OwnedLaunch first, second;
  for (const auto& [binding, state] : {
           std::pair{&first_binding, &first}, std::pair{&second_binding, &second}}) {
    state->lease = AccessTestNodeVaultMac::ReserveRuntimeReference(binding->channel,
        binding->vault_scope_hash, binding->reference);
    ASSERT_TRUE(state->lease);
    EXPECT_EQ(LaunchAccessInternalTestCoreMac(Identity(aegis_access::AccessProductChannel::kDebug),
        manifest_path, *binding, base::TimeTicks::Now() + base::Seconds(5),
        state->directory, state->process, state->child, state->lease, state->listener_channel), Status::kLaunched);
    ASSERT_TRUE(state->child);
    EXPECT_FALSE(base::PathExists(state->directory.GetPath().AppendASCII("config.json")));
    const auto staged = base::ReadFileToBytes(state->directory.GetPath().AppendASCII("xray"));
    ASSERT_TRUE(staged);
    EXPECT_EQ(base::HexEncodeLower(crypto::hash::Sha256(*staged)), pin);
  }
  const auto deadline = base::TimeTicks::Now() + base::Seconds(5);
  auto one = InspectAccessOwnedListenerMac(*first.child), two = InspectAccessOwnedListenerMac(*second.child);
  while ((!one.proof || !two.proof) && base::TimeTicks::Now() < deadline) {
    base::PlatformThread::Sleep(base::Milliseconds(10));
    one = InspectAccessOwnedListenerMac(*first.child);
    two = InspectAccessOwnedListenerMac(*second.child);
  }
  ASSERT_TRUE(one.proof);
  ASSERT_TRUE(two.proof);
  for (const auto& [state, proof] : {
           std::pair{&first, &*one.proof}, std::pair{&second, &*two.proof}}) {
    auto retained = ReceiveAccessListenerLeaseMac(state->listener_channel.get(), *state->child, *proof);
    while (retained.status == AccessListenerLeaseStatusMac::kPending &&
           base::TimeTicks::Now() < deadline) {
      base::PlatformThread::Sleep(base::Milliseconds(10));
      retained = ReceiveAccessListenerLeaseMac(state->listener_channel.get(), *state->child, *proof);
    }
    ASSERT_EQ(retained.status, AccessListenerLeaseStatusMac::kReady);
    state->held_listener = std::move(retained.descriptor);
    state->listener_channel.reset();
    EXPECT_TRUE(AccessHeldListenerStillMatchesMac(state->held_listener.get(), *proof));
  }
  EXPECT_NE(one.proof->port, two.proof->port);
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*first.child, *one.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*second.child, *two.proof));
  EXPECT_TRUE(first_vault.IsRuntimeReferenced(first_binding.reference));
  EXPECT_TRUE(second_vault.IsRuntimeReferenced(second_binding.reference));
  first.Stop();
  EXPECT_FALSE(first_vault.IsRuntimeReferenced(first_binding.reference));
  EXPECT_TRUE(second_vault.IsRuntimeReferenced(second_binding.reference));
  EXPECT_FALSE(AccessOwnedListenerStillMatchesMac(*first.child, *one.proof));
  EXPECT_TRUE(AccessOwnedListenerStillMatchesMac(*second.child, *two.proof));
}
}  // namespace
}  // namespace aegis::access
