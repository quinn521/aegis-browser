// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_vault_mac.h"

#include <map>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "base/uuid.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
using Status = AccessTestNodeVaultStatus;
struct Rows {
  std::map<std::pair<std::string, std::string>, std::string> values;
  bool deny_add = false;
  bool deny_delete = false;
  bool deny_list = false;
  std::optional<std::vector<std::string>> forced_inventory;
  std::function<void()> on_delete;
};
class MemoryBackend final : public AccessTestNodeVaultMac::Backend {
 public:
  explicit MemoryBackend(std::shared_ptr<Rows> rows) : rows_(std::move(rows)) {}
  Status Add(std::string_view service, std::string_view account,
             std::string_view secret) override {
    if (rows_->deny_add) return Status::kUnavailable;
    return rows_->values.emplace(std::make_pair(service, account), secret).second
               ? Status::kOk : Status::kUnavailable;
  }
  std::optional<std::string> Read(std::string_view service,
                                  std::string_view account) override {
    auto it = rows_->values.find({std::string(service), std::string(account)});
    return it == rows_->values.end() ? std::nullopt
                                   : std::make_optional(it->second);
  }
  Status Delete(std::string_view service, std::string_view account) override {
    if (rows_->deny_delete) return Status::kUnavailable;
    if (rows_->on_delete) rows_->on_delete();
    rows_->values.erase({std::string(service), std::string(account)});
    return Status::kOk;
  }
  std::optional<std::vector<std::string>> ListAccounts(std::string_view service) override {
    if (rows_->deny_list) return std::nullopt;
    if (rows_->forced_inventory) return rows_->forced_inventory;
    std::vector<std::string> accounts;
    for (const auto& [key, value] : rows_->values)
      if (key.first == service) accounts.push_back(key.second);
    return accounts;
  }
 private:
  std::shared_ptr<Rows> rows_;
};
std::string Id() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }
aegis_access::AccessTestNode Node() {
  return *aegis_access::ParseAccessTestNodeShare(
              aegis_access::test::kSyntheticTestNodeShare).node;
}
std::unique_ptr<MemoryBackend> Backend(const std::shared_ptr<Rows>& rows) {
  return std::make_unique<MemoryBackend>(rows);
}

TEST(AccessTestNodeVaultMacTest, WebSocketDraftIsStoredOnlyInDevScope) {
  const auto parsed = aegis_access::ParseAccessTestNodeShare(
      aegis_access::test::kSyntheticWebSocketTlsShare,
      aegis_access::AccessTestNodeImportMode::kAllowWebSocketTls);
  ASSERT_TRUE(parsed.node);
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac dev("debug", std::string(64, 'a'), Backend(rows));
  AccessTestNodeVaultMac alpha("alpha", std::string(64, 'a'), Backend(rows));
  const auto editor = Id();
  EXPECT_EQ(alpha.Stage(editor, *parsed.node).status, Status::kNotAdmitted);
  const auto staged = dev.Stage(editor, *parsed.node);
  ASSERT_TRUE(staged.reference);
  EXPECT_EQ(dev.Read(*staged.reference), parsed.node);
  EXPECT_FALSE(alpha.Read(*staged.reference));
  EXPECT_EQ(dev.Discard(editor, *staged.reference), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}

TEST(AccessTestNodeVaultMacTest, EditorCannotDiscardAnotherWindowsReference) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  const auto first = Id(), second = Id();
  const auto a = vault.Stage(first, Node()), b = vault.Stage(second, Node());
  ASSERT_TRUE(a.reference);
  ASSERT_TRUE(b.reference);
  EXPECT_NE(a.reference, b.reference);
  EXPECT_EQ(vault.Read(*a.reference), Node());
  EXPECT_EQ(vault.Discard(second, *a.reference), Status::kInvalidReference);
  vault.CloseEditor(first);
  EXPECT_FALSE(vault.Read(*a.reference));
  EXPECT_EQ(vault.Read(*b.reference), Node());
  EXPECT_EQ(vault.Discard(second, *b.reference), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}

TEST(AccessTestNodeVaultMacTest, ScopeAndChannelArePartOfKeychainAuthority) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac owner("debug", std::string(64, 'a'), Backend(rows));
  AccessTestNodeVaultMac other("debug", std::string(64, 'b'), Backend(rows));
  AccessTestNodeVaultMac alpha("alpha", std::string(64, 'a'), Backend(rows));
  AccessTestNodeVaultMac beta("beta", std::string(64, 'a'), Backend(rows));
  AccessTestNodeVaultMac invalid("debug", "untrusted-scope", Backend(rows));
  const auto staged = owner.Stage(Id(), Node());
  ASSERT_TRUE(staged.reference);
  EXPECT_FALSE(other.Read(*staged.reference));
  EXPECT_FALSE(alpha.Read(*staged.reference));
  EXPECT_EQ(beta.Stage(Id(), Node()).status, Status::kNotAdmitted);
  EXPECT_EQ(invalid.Stage(Id(), Node()).status, Status::kNotAdmitted);
  EXPECT_EQ(rows->values.size(), 1u);
}

TEST(AccessTestNodeVaultMacTest, ClosingEditorCannotDeleteUnknownCommitOutcome) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeSecretReference reference;
  const auto editor = Id(), operation = Id();
  {
    AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
    auto staged = vault.Stage(editor, Node());
    ASSERT_TRUE(staged.reference);
    reference = *staged.reference;
    ASSERT_TRUE(vault.HoldForCommit(editor, reference, operation));
    vault.CloseEditor(editor);
    EXPECT_EQ(vault.Discard(editor, reference), Status::kInvalidReference);
  }
  // The durable journal must resolve this outcome; destruction cannot assume
  // failure and delete a reference that may already have been committed.
  EXPECT_EQ(rows->values.size(), 1u);
  AccessTestNodeVaultMac recovered("debug", std::string(64, 'a'), Backend(rows));
  EXPECT_EQ(recovered.Read(reference), Node());
}

TEST(AccessTestNodeVaultMacTest, CommitRetainsReferenceAndFailedCommitCleansClosedEditor) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeSecretReference committed;
  {
    AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
    const auto a = Id(), b = Id(), operation_a = Id(), operation_b = Id();
    const auto first = vault.Stage(a, Node()), second = vault.Stage(b, Node());
    ASSERT_TRUE(first.reference);
    ASSERT_TRUE(second.reference);
    ASSERT_TRUE(vault.HoldForCommit(a, *first.reference, operation_a));
    EXPECT_FALSE(vault.HoldForCommit(b, *second.reference, operation_a));
    ASSERT_TRUE(vault.HoldForCommit(b, *second.reference, operation_b));
    vault.CloseEditor(a);
    vault.CloseEditor(b);
    vault.ResolveCommit(operation_a, true);
    vault.ResolveCommit(operation_b, false);
    committed = *first.reference;
    EXPECT_FALSE(vault.Read(*second.reference));
  }
  EXPECT_EQ(rows->values.size(), 1u);
  AccessTestNodeVaultMac recovered("debug", std::string(64, 'a'), Backend(rows));
  EXPECT_EQ(recovered.Read(committed), Node());
}

TEST(AccessTestNodeVaultMacTest, StorageFailureDoesNotReturnFalseSuccessOrLoseCleanupOwnership) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  rows->deny_add = true;
  auto rejected = vault.Stage(Id(), Node());
  EXPECT_EQ(rejected.status, Status::kUnavailable);
  EXPECT_FALSE(rejected.reference);
  rows->deny_add = false;
  const auto editor = Id();
  auto staged = vault.Stage(editor, Node());
  ASSERT_TRUE(staged.reference);
  rows->deny_delete = true;
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kUnavailable);
  EXPECT_EQ(vault.CloseEditor(editor), Status::kUnavailable);
  rows->deny_delete = false;
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}

TEST(AccessTestNodeVaultMacTest, RejectsMalformedReferencesAndCorruptStoredNode) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  EXPECT_EQ(vault.Stage("renderer-input", Node()).status, Status::kInvalidReference);
  auto invalid = Node();
  invalid.port = 0;
  EXPECT_EQ(vault.Stage(Id(), invalid).status, Status::kInvalidNode);
  auto staged = vault.Stage(Id(), Node());
  ASSERT_TRUE(staged.reference);
  ASSERT_EQ(rows->values.size(), 1u);
  rows->values.begin()->second = "{\"schemaVersion\":999}";
  EXPECT_FALSE(vault.Read(*staged.reference));
  EXPECT_FALSE(vault.Read({"not-a-reference"}));
}

TEST(AccessTestNodeVaultMacTest, NativeKeychainRoundTripDeletesOnlySyntheticRecord) {
  // A fresh synthetic namespace and the vault's own random account avoid all
  // pre-existing user records. This exercises the real macOS backend.
  auto scope = Id() + Id();
  std::erase(scope, '-');
  ASSERT_EQ(scope.size(), 64u);
  AccessTestNodeVaultMac vault("debug", scope);
  const auto editor = Id();
  auto node = Node();
  node.advertises_x25519_mlkem768 = true;
  auto staged = vault.Stage(editor, node);
  ASSERT_EQ(staged.status, Status::kOk);
  ASSERT_TRUE(staged.reference);
  EXPECT_EQ(vault.Read(*staged.reference), node);
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kOk);
  EXPECT_FALSE(vault.Read(*staged.reference));
}

TEST(AccessTestNodeVaultMacTest, RecoveryPreservesDurableLiveUnknownAndForeignReferences) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeSecretReference durable, orphan, foreign;
  {
    AccessTestNodeVaultMac prior("debug", std::string(64, 'a'), Backend(rows));
    const auto editor = Id(), operation = Id();
    const auto saved = prior.Stage(editor, Node());
    ASSERT_TRUE(saved.reference);
    durable = *saved.reference;
    ASSERT_TRUE(prior.HoldForCommit(editor, durable, operation));
    prior.ResolveCommit(operation, true);
    const auto interrupted = prior.Stage(editor, Node());
    ASSERT_TRUE(interrupted.reference);
    orphan = *interrupted.reference;
    ASSERT_TRUE(prior.HoldForCommit(editor, orphan, Id()));
    prior.CloseEditor(editor);
  }
  {
    AccessTestNodeVaultMac other("alpha", std::string(64, 'b'), Backend(rows));
    const auto editor = Id(), operation = Id();
    const auto saved = other.Stage(editor, Node());
    ASSERT_TRUE(saved.reference);
    foreign = *saved.reference;
    ASSERT_TRUE(other.HoldForCommit(editor, foreign, operation));
    other.ResolveCommit(operation, true);
  }
  AccessTestNodeVaultMac recovered("debug", std::string(64, 'a'), Backend(rows));
  const auto editor = Id(), operation = Id();
  const auto live = recovered.Stage(editor, Node());
  ASSERT_TRUE(live.reference);
  ASSERT_TRUE(recovered.HoldForCommit(editor, *live.reference, operation));
  recovered.CloseEditor(editor);
  EXPECT_EQ(recovered.ReconcileReferences({durable.id}), Status::kOk);
  EXPECT_EQ(recovered.Read(durable), Node());
  EXPECT_FALSE(recovered.Read(orphan));
  EXPECT_EQ(recovered.Read(*live.reference), Node());
  EXPECT_FALSE(recovered.Read(foreign));
  EXPECT_EQ(rows->values.size(), 3u);
  recovered.ResolveCommit(operation, false);
  EXPECT_EQ(recovered.ReconcileReferences({}), Status::kOk);
  EXPECT_EQ(rows->values.size(), 1u);
  AccessTestNodeVaultMac other("alpha", std::string(64, 'b'), Backend(rows));
  EXPECT_EQ(other.Read(foreign), Node());
}

TEST(AccessTestNodeVaultMacTest, RecoveryFailureAndInvalidInventoryGrantNoCleanup) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  const auto editor = Id(), operation = Id();
  const auto staged = vault.Stage(editor, Node());
  ASSERT_TRUE(staged.reference);
  ASSERT_TRUE(vault.HoldForCommit(editor, *staged.reference, operation));
  vault.ResolveCommit(operation, true);
  rows->deny_list = true;
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kUnavailable);
  EXPECT_EQ(rows->values.size(), 1u);
  rows->deny_list = false;
  rows->forced_inventory = std::vector<std::string>{staged.reference->id, "foreign-non-uuid"};
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kInvalidReference);
  EXPECT_EQ(rows->values.size(), 1u);
  rows->forced_inventory = std::vector<std::string>{staged.reference->id, staged.reference->id};
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kInvalidReference);
  EXPECT_EQ(rows->values.size(), 1u);
  rows->forced_inventory.reset();
  EXPECT_EQ(vault.ReconcileReferences({"not-a-reference"}), Status::kInvalidReference);
  EXPECT_EQ(rows->values.size(), 1u);
  rows->deny_delete = true;
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kUnavailable);
  EXPECT_EQ(vault.Read(*staged.reference), Node());
  rows->deny_delete = false;
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}

TEST(AccessTestNodeVaultMacTest, NativeKeychainReconciliationUsesOnlyFreshSyntheticNamespace) {
  auto scope = Id() + Id();
  std::erase(scope, '-');
  AccessTestNodeVaultMac vault("debug", scope);
  const auto editor = Id();
  const auto retained = vault.Stage(editor, Node()), orphan = vault.Stage(editor, Node());
  ASSERT_TRUE(retained.reference);
  ASSERT_TRUE(orphan.reference);
  for (const auto& reference : {*retained.reference, *orphan.reference}) {
    const auto operation = Id();
    ASSERT_TRUE(vault.HoldForCommit(editor, reference, operation));
    vault.ResolveCommit(operation, true);
  }
  EXPECT_EQ(vault.ReconcileReferences({retained.reference->id}), Status::kOk);
  EXPECT_EQ(vault.Read(*retained.reference), Node());
  EXPECT_FALSE(vault.Read(*orphan.reference));
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kOk);
  EXPECT_FALSE(vault.Read(*retained.reference));
}
TEST(AccessTestNodeVaultMacTest, SharedRuntimeReservationsProtectPendingReferenceUntilLastRelease) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  const auto editor = Id();
  const auto staged = vault.Stage(editor, Node());
  ASSERT_TRUE(staged.reference);
  auto first = AccessTestNodeVaultMac::ReserveRuntimeReference("debug", std::string(64, 'a'), *staged.reference);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->Matches("debug", std::string(64, 'a'), *staged.reference));
  EXPECT_FALSE(first->Matches("alpha", std::string(64, 'a'), *staged.reference));
  auto shared = first;
  auto second = AccessTestNodeVaultMac::ReserveRuntimeReference("debug", std::string(64, 'a'), *staged.reference);
  ASSERT_TRUE(second);
  EXPECT_TRUE(vault.IsRuntimeReferenced(*staged.reference));
  EXPECT_TRUE(vault.HasRuntimeReferences());
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kInUse);
  EXPECT_EQ(vault.CloseEditor(editor), Status::kInUse);
  first.reset();
  shared.reset();
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kInUse);
  second.reset();
  EXPECT_FALSE(vault.IsRuntimeReferenced(*staged.reference));
  EXPECT_FALSE(vault.HasRuntimeReferences());
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}
TEST(AccessTestNodeVaultMacTest, OwnerDestructionAndRecoveryRetainRuntimeReference) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeSecretReference reference;
  std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> lease;
  {
    AccessTestNodeVaultMac owner("debug", std::string(64, 'a'), Backend(rows));
    const auto staged = owner.Stage(Id(), Node());
    ASSERT_TRUE(staged.reference);
    reference = *staged.reference;
    lease = AccessTestNodeVaultMac::ReserveRuntimeReference("debug", std::string(64, 'a'), reference);
    ASSERT_TRUE(lease);
  }
  AccessTestNodeVaultMac recovered("debug", std::string(64, 'a'), Backend(rows));
  EXPECT_EQ(recovered.ReconcileReferences({}), Status::kOk);
  EXPECT_EQ(recovered.Read(reference), Node());
  lease.reset();
  EXPECT_EQ(recovered.ReconcileReferences({}), Status::kOk);
  EXPECT_TRUE(rows->values.empty());
}
TEST(AccessTestNodeVaultMacTest, DeletionWindowRejectsReservationAndFailureReleasesDeletionState) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  const auto editor = Id();
  const auto staged = vault.Stage(editor, Node());
  ASSERT_TRUE(staged.reference);
  const auto operation = Id();
  ASSERT_TRUE(vault.HoldForCommit(editor, *staged.reference, operation));
  vault.ResolveCommit(operation, true);
  auto observed = std::make_shared<bool>(false);
  rows->on_delete = [reference = *staged.reference, observed] {
    *observed = true;
    EXPECT_FALSE(AccessTestNodeVaultMac::ReserveRuntimeReference("debug", std::string(64, 'a'), reference));
  };
  EXPECT_EQ(vault.ReconcileReferences({}), Status::kOk);
  EXPECT_TRUE(*observed);
  rows->on_delete = {};
  const auto next = vault.Stage(editor, Node());
  ASSERT_TRUE(next.reference);
  rows->deny_delete = true;
  EXPECT_EQ(vault.Discard(editor, *next.reference), Status::kUnavailable);
  auto lease = AccessTestNodeVaultMac::ReserveRuntimeReference("debug", std::string(64, 'a'), *next.reference);
  ASSERT_TRUE(lease);
  rows->deny_delete = false;
  EXPECT_EQ(vault.Discard(editor, *next.reference), Status::kInUse);
  lease.reset();
  EXPECT_EQ(vault.Discard(editor, *next.reference), Status::kOk);
}
TEST(AccessTestNodeVaultMacTest, MetadataReservationDoesNotAdmitRestrictedOrForeignScope) {
  auto rows = std::make_shared<Rows>();
  AccessTestNodeVaultMac vault("debug", std::string(64, 'a'), Backend(rows));
  const auto editor = Id();
  const auto staged = vault.Stage(editor, Node());
  ASSERT_TRUE(staged.reference);
  for (const auto channel : {"beta", "stable", "unknown", ""})
    EXPECT_FALSE(AccessTestNodeVaultMac::ReserveRuntimeReference(channel, std::string(64, 'a'), *staged.reference));
  EXPECT_FALSE(AccessTestNodeVaultMac::ReserveRuntimeReference("debug", "invalid", *staged.reference));
  auto foreign = AccessTestNodeVaultMac::ReserveRuntimeReference("alpha", std::string(64, 'a'), *staged.reference);
  ASSERT_TRUE(foreign);
  EXPECT_FALSE(vault.IsRuntimeReferenced(*staged.reference));
  AccessTestNodeVaultMac other("alpha", std::string(64, 'a'), Backend(rows));
  EXPECT_FALSE(vault.HasRuntimeReferences());
  EXPECT_TRUE(other.HasRuntimeReferences());
  EXPECT_FALSE(other.Read(*staged.reference));
  EXPECT_EQ(vault.Discard(editor, *staged.reference), Status::kOk);
}
TEST(AccessTestNodeVaultMacTest, NativeKeychainRuntimePinSurvivesOwnerThenReleasesForReconciliation) {
  auto scope = Id() + Id();
  std::erase(scope, '-');
  AccessTestNodeSecretReference reference;
  std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> lease;
  {
    AccessTestNodeVaultMac owner("debug", scope);
    const auto staged = owner.Stage(Id(), Node());
    ASSERT_TRUE(staged.reference);
    reference = *staged.reference;
    lease = AccessTestNodeVaultMac::ReserveRuntimeReference("debug", scope, reference);
    ASSERT_TRUE(lease);
  }
  AccessTestNodeVaultMac recovered("debug", scope);
  EXPECT_EQ(recovered.ReconcileReferences({}), Status::kOk);
  EXPECT_EQ(recovered.Read(reference), Node());
  lease.reset();
  EXPECT_EQ(recovered.ReconcileReferences({}), Status::kOk);
  EXPECT_FALSE(recovered.Read(reference));
}
}  // namespace
}  // namespace aegis::access
