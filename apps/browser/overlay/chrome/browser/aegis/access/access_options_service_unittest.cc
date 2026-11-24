// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_options_service.h"
#include <atomic>
#include <map>
#include <memory>
#include "chrome/browser/aegis/access/access_service_coordinator.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "chrome/test/base/testing_profile.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
#if BUILDFLAG(IS_MAC)
namespace {
constexpr char kSyntheticShare[] =
    "vless://550e8400-e29b-41d4-a716-446655440000@198.51.100.10:443"
    "?security=reality&type=tcp&flow=xtls-rprx-vision&sni=www.example.com"
    "&pbk=abcdefghijklmnopqrstuvwxyzABCDEFG&sid=abcd1234#synthetic";
struct SyntheticVaultState {
  std::map<std::pair<std::string, std::string>, std::string> rows;
  std::atomic<size_t> stored{0};
};
class SyntheticVaultBackend final : public AccessTestNodeVaultMac::Backend {
 public:
  explicit SyntheticVaultBackend(std::shared_ptr<SyntheticVaultState> state) : state_(std::move(state)) {}
  AccessTestNodeVaultStatus Add(std::string_view service, std::string_view account,
                               std::string_view secret) override {
    const bool inserted = state_->rows.emplace(std::make_pair(service, account), secret).second;
    state_->stored = state_->rows.size();
    return inserted ? AccessTestNodeVaultStatus::kOk : AccessTestNodeVaultStatus::kUnavailable;
  }
  std::optional<std::string> Read(std::string_view service, std::string_view account) override {
    const auto found = state_->rows.find({std::string(service), std::string(account)});
    return found == state_->rows.end() ? std::nullopt : std::make_optional(found->second);
  }
  AccessTestNodeVaultStatus Delete(std::string_view service, std::string_view account) override {
    state_->rows.erase({std::string(service), std::string(account)});
    state_->stored = state_->rows.size();
    return AccessTestNodeVaultStatus::kOk;
  }
  std::optional<std::vector<std::string>> ListAccounts(std::string_view service) override {
    std::vector<std::string> accounts;
    for (const auto& [key, value] : state_->rows)
      if (key.first == service) accounts.push_back(key.second);
    return accounts;
  }
 private:
  std::shared_ptr<SyntheticVaultState> state_;
};
}  // namespace

TEST(AccessOptionsServiceTest, NativeEditorsCompeteWithoutDeletingCommittedReference) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kImport))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto a = options->BeginTestNodeEditor(), b = options->BeginTestNodeEditor();
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  base::test::TestFuture<TestNodePreviewResult> first, second;
  options->PreviewTestNodeText(*a, kSyntheticShare, first.GetCallback());
  options->PreviewTestNodeText(*b, kSyntheticShare, second.GetCallback());
  ASSERT_EQ(first.Get().status, TestNodePreviewStatus::kPreview);
  ASSERT_EQ(second.Get().status, TestNodePreviewStatus::kPreview);
  const auto operation = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::test::TestFuture<AccessTestProfileSaveResult> save_a;
  options->SaveTestNodeProfile(*a, 0, operation, "TEST01", {}, save_a.GetCallback());
  ASSERT_EQ(save_a.Get().status, StoreStatus::kValid);
  EXPECT_EQ(save_a.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(save_a.Get().snapshot->test_profile->configuration.secret_reference, first.Get().reference->id);
  base::test::TestFuture<AccessTestProfileSaveResult> stale_b;
  options->SaveTestNodeProfile(*b, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST02", {}, stale_b.GetCallback());
  EXPECT_EQ(stale_b.Get().status, StoreStatus::kConflict);
  EXPECT_EQ(stale_b.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_EQ(stale_b.Get().snapshot->revision, 1u);
  EXPECT_EQ(state->stored, 2u);
  base::test::TestFuture<TestNodePreviewStatus> discard;
  options->DiscardTestNodeDraft(*b, discard.GetCallback());
  EXPECT_EQ(discard.Get(), TestNodePreviewStatus::kCanceled);
  EXPECT_EQ(state->stored, 1u);
  options->EndTestNodeEditor(*a);
  base::test::TestFuture<AccessTestProfileSaveResult> queried;
  options->QueryTestNodeProfileOperation(operation, queried.GetCallback());
  EXPECT_EQ(queried.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(queried.Get().snapshot->revision, 1u);
  EXPECT_EQ(state->stored, 1u);
  auto* transport = AccessNetworkContextTransport::Get(profile.get());
  const auto owner = transport->OwnerForPartition(AccessChannelForCurrentBuild(), {});
  ASSERT_TRUE(owner);
  EXPECT_TRUE(transport->PrivateEndpoints(*owner).empty());
}

TEST(AccessOptionsServiceTest, LostSaveReplyAndClosedEditorResolveOnlyBySameOperationQuery) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kImport))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(*editor, kSyntheticShare, preview.GetCallback());
  ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
  options->SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting::kAfterCommit);
  const auto operation = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::test::TestFuture<AccessTestProfileSaveResult> lost;
  options->SaveTestNodeProfile(*editor, 0, operation, "TEST01", {}, lost.GetCallback());
  EXPECT_EQ(lost.Get().status, StoreStatus::kIoError);
  EXPECT_EQ(lost.Get().outcome, AccessOptionsCommitOutcome::kUnknown);
  base::test::TestFuture<TestNodePreviewStatus> discard;
  options->DiscardTestNodeDraft(*editor, discard.GetCallback());
  EXPECT_EQ(discard.Get(), TestNodePreviewStatus::kBusy);
  EXPECT_EQ(state->stored, 1u);
  options->EndTestNodeEditor(*editor);
  base::test::TestFuture<AccessTestProfileSaveResult> queried;
  options->QueryTestNodeProfileOperation(operation, queried.GetCallback());
  EXPECT_EQ(queried.Get().status, StoreStatus::kValid);
  EXPECT_EQ(queried.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(queried.Get().snapshot->revision, 1u);
  EXPECT_EQ(state->stored, 1u);
}
TEST(AccessOptionsServiceTest, RuntimeReservationBlocksProfileUpdateUntilConfirmedRelease) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kImport))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto first_editor = options->BeginTestNodeEditor(), next_editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(first_editor);
  ASSERT_TRUE(next_editor);
  base::test::TestFuture<TestNodePreviewResult> first;
  options->PreviewTestNodeText(*first_editor, kSyntheticShare, first.GetCallback());
  ASSERT_EQ(first.Get().status, TestNodePreviewStatus::kPreview);
  base::test::TestFuture<AccessTestProfileSaveResult> saved;
  options->SaveTestNodeProfile(*first_editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST01", {}, saved.GetCallback());
  ASSERT_EQ(saved.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_EQ(state->rows.size(), 1u);
  const auto service = state->rows.begin()->first.first;
  const std::string prefix = "app.gcsa.aegis.access.test-node.v1.";
  ASSERT_TRUE(service.starts_with(prefix));
  const auto scoped = service.substr(prefix.size());
  const auto separator = scoped.find('.');
  ASSERT_NE(separator, std::string::npos);
  auto lease = AccessTestNodeVaultMac::ReserveRuntimeReference(scoped.substr(0, separator),
      scoped.substr(separator + 1), *first.Get().reference);
  ASSERT_TRUE(lease);
  base::test::TestFuture<TestNodePreviewResult> next;
  options->PreviewTestNodeText(*next_editor, kSyntheticShare, next.GetCallback());
  ASSERT_EQ(next.Get().status, TestNodePreviewStatus::kPreview);
  const auto operation = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::test::TestFuture<AccessTestProfileSaveResult> blocked;
  options->SaveTestNodeProfile(*next_editor, 1, operation, "TEST02", {}, blocked.GetCallback());
  EXPECT_EQ(blocked.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_EQ(blocked.Get().detail, "profile_runtime_in_use");
  ASSERT_TRUE(blocked.Get().snapshot);
  EXPECT_EQ(blocked.Get().snapshot->revision, 1u);
  EXPECT_EQ(blocked.Get().snapshot->test_profile->configuration.name, "TEST01");
  lease.reset();
  base::test::TestFuture<AccessTestProfileSaveResult> retried;
  options->SaveTestNodeProfile(*next_editor, 1, operation, "TEST02", {}, retried.GetCallback());
  EXPECT_EQ(retried.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_TRUE(retried.Get().snapshot);
  EXPECT_EQ(retried.Get().snapshot->revision, 2u);
  EXPECT_EQ(retried.Get().snapshot->test_profile->configuration.name, "TEST02");
}

TEST(AccessOptionsServiceTest, PreferencesOnlySaveReusesCommittedReferenceBehindFences) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kImport))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  const aegis_access::AccessInboundPreferences http{
      aegis_access::AccessInboundProtocolPreference::kHttp,
      aegis_access::AccessInboundPortMode::kSystemAllocated, 0};
  base::test::TestFuture<AccessTestProfileSaveResult> nothing_saved;
  options->SaveTestNodePreferences(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      http, nothing_saved.GetCallback());
  EXPECT_EQ(nothing_saved.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_EQ(nothing_saved.Get().detail, "no_saved_test_profile");
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(*editor, kSyntheticShare, preview.GetCallback());
  ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
  base::test::TestFuture<AccessTestProfileSaveResult> pending_import;
  options->SaveTestNodePreferences(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      http, pending_import.GetCallback());
  EXPECT_EQ(pending_import.Get().detail, "invalid_test_profile_draft");
  base::test::TestFuture<AccessTestProfileSaveResult> saved;
  options->SaveTestNodeProfile(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST01", {}, saved.GetCallback());
  ASSERT_EQ(saved.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_EQ(state->rows.size(), 1u);
  const auto committed = saved.Get().snapshot->test_profile->configuration;
  base::test::TestFuture<AccessTestProfileSaveResult> stale;
  options->SaveTestNodePreferences(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      http, stale.GetCallback());
  EXPECT_EQ(stale.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_EQ(stale.Get().detail, "configuration_revision_conflict");
  const auto service = state->rows.begin()->first.first;
  const std::string prefix = "app.gcsa.aegis.access.test-node.v1.";
  ASSERT_TRUE(service.starts_with(prefix));
  const auto scoped = service.substr(prefix.size());
  const auto separator = scoped.find('.');
  ASSERT_NE(separator, std::string::npos);
  auto lease = AccessTestNodeVaultMac::ReserveRuntimeReference(scoped.substr(0, separator),
      scoped.substr(separator + 1), {committed.secret_reference});
  ASSERT_TRUE(lease);
  base::test::TestFuture<AccessTestProfileSaveResult> in_use;
  options->SaveTestNodePreferences(*editor, 1, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      http, in_use.GetCallback());
  EXPECT_EQ(in_use.Get().detail, "profile_runtime_in_use");
  lease.reset();
  base::test::TestFuture<AccessTestProfileSaveResult> updated;
  options->SaveTestNodePreferences(*editor, 1, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      http, updated.GetCallback());
  ASSERT_EQ(updated.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_TRUE(updated.Get().snapshot && updated.Get().snapshot->test_profile);
  const auto& stored = *updated.Get().snapshot->test_profile;
  EXPECT_EQ(updated.Get().snapshot->revision, 2u);
  EXPECT_EQ(stored.version, 2u);
  EXPECT_EQ(stored.configuration.id, committed.id);
  EXPECT_EQ(stored.configuration.name, "TEST01");
  EXPECT_EQ(stored.configuration.secret_reference, committed.secret_reference);
  EXPECT_EQ(stored.configuration.inbound, http);
  EXPECT_EQ(state->rows.size(), 1u);
}

TEST(AccessOptionsServiceTest, CloseBeforeDeferredSaveCannotCommitStaleDraft) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kImport))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(*editor, kSyntheticShare, preview.GetCallback());
  ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
  base::test::TestFuture<AccessTestProfileSaveResult> save;
  options->SaveTestNodeProfile(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST01", {}, save.GetCallback());
  // Do not pump the UI callback which schedules the blocking commit.
  options->EndTestNodeEditor(*editor);
  EXPECT_EQ(save.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> latest;
  options->Read(latest.GetCallback());
  ASSERT_EQ(latest.Get().status, StoreStatus::kValid);
  EXPECT_EQ(latest.Get().value->revision, 0u);
  EXPECT_FALSE(latest.Get().value->test_profile);
  EXPECT_EQ(state->stored, 0u);
}

TEST(AccessOptionsServiceTest, PreparingFencesSecondWindowSaveBeforeAsynchronousRead) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate))
    GTEST_SKIP() << "Requires an admitted internal build";
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto first = options->BeginTestNodeEditor(), second = options->BeginTestNodeEditor();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  base::test::TestFuture<TestNodePreviewResult> preview_first, preview_second;
  options->PreviewTestNodeText(*first, kSyntheticShare, preview_first.GetCallback());
  options->PreviewTestNodeText(*second, kSyntheticShare, preview_second.GetCallback());
  ASSERT_EQ(preview_first.Get().status, TestNodePreviewStatus::kPreview);
  ASSERT_EQ(preview_second.Get().status, TestNodePreviewStatus::kPreview);
  base::test::TestFuture<AccessTestProfileSaveResult> saved;
  aegis_access::AccessInboundPreferences inbound;
  inbound.protocol = aegis_access::AccessInboundProtocolPreference::kSocks5;
  options->SaveTestNodeProfile(*first, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST01", inbound, saved.GetCallback());
  ASSERT_EQ(saved.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  base::test::TestFuture<AccessLocalCoreReady> prepare;
  options->PrepareTestNodeProfile(1, prepare.GetCallback());
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kPreparing);
  base::test::TestFuture<AccessTestProfileSaveResult> blocked;
  options->SaveTestNodeProfile(*second, 1, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST02", inbound, blocked.GetCallback());
  EXPECT_EQ(blocked.Get().outcome, AccessOptionsCommitOutcome::kNotCommitted);
  EXPECT_EQ(blocked.Get().detail, "profile_runtime_in_use");
  // Cancel before the native read reply can create a core. This cannot open
  // an endpoint or authorize a late preparation callback from the first call.
  base::test::TestFuture<bool> revoked;
  options->RevokeTestNodeProfile(revoked.GetCallback());
  EXPECT_TRUE(revoked.Get());
  EXPECT_FALSE(prepare.Get().ready);
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kIdle);
  EXPECT_FALSE(options->test_profile_runtime().ready);
  base::test::TestFuture<AccessTestProfileSaveResult> after_cancel;
  options->SaveTestNodeProfile(*second, 1, base::Uuid::GenerateRandomV4().AsLowercaseString(),
      "TEST02", inbound, after_cancel.GetCallback());
  EXPECT_EQ(after_cancel.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  EXPECT_EQ(after_cancel.Get().snapshot->revision, 2u);
}

TEST(AccessOptionsServiceTest, SavedPreparationRejectsMismatchedProfileIdOrVersion) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto state = std::make_shared<SyntheticVaultState>();
  auto open_service = [&] {
    return AccessOptionsService::CreateForTesting(
        profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  };
  auto options = open_service();
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(*editor, kSyntheticShare, preview.GetCallback());
  ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
  base::test::TestFuture<AccessTestProfileSaveResult> saved;
  aegis_access::AccessInboundPreferences inbound;
  inbound.protocol = aegis_access::AccessInboundProtocolPreference::kSocks5;
  options->SaveTestNodeProfile(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
                               "Synthetic", inbound, saved.GetCallback());
  ASSERT_EQ(saved.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
  ASSERT_TRUE(saved.Get().snapshot && saved.Get().snapshot->test_profile);
  const auto stored = *saved.Get().snapshot->test_profile;
  options->EndTestNodeEditor(*editor);

  for (const auto& [id, version] : {
           std::pair(base::Uuid::GenerateRandomV4().AsLowercaseString(), stored.version),
           std::pair(stored.configuration.id, stored.version + 1)}) {
    options.reset();
    environment.RunUntilIdle();
    options = open_service();
    base::test::TestFuture<AccessLocalCoreReady> mismatch;
    options->PrepareSavedTestNodeProfile(id, version, mismatch.GetCallback());
    EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kPreparing);
    EXPECT_FALSE(mismatch.Get().ready);
    EXPECT_EQ(mismatch.Get().error, "The committed test Profile does not match the requested binding.");
    EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kIdle);
    EXPECT_FALSE(options->PreparedTestNode());
    EXPECT_FALSE(options->test_profile_runtime().ready);

    base::test::TestFuture<AccessLocalCoreReady> retry;
    options->PrepareSavedTestNodeProfile(stored.configuration.id, stored.version,
                                         retry.GetCallback());
    ASSERT_EQ(options->test_profile_runtime_state(),
              AccessTestProfileRuntimeState::kPreparing);
    ASSERT_FALSE(retry.IsReady());
    // Cancel before the store reply can launch a core.
    base::test::TestFuture<bool> revoked;
    options->RevokeTestNodeProfile(revoked.GetCallback());
    EXPECT_TRUE(revoked.Get());
    EXPECT_FALSE(retry.Get().ready);
    EXPECT_EQ(retry.Get().error, "Test node preparation was canceled.");
    EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kIdle);

    base::test::TestFuture<AccessLocalCoreReady> after_revoke;
    options->PrepareSavedTestNodeProfile(stored.configuration.id, stored.version,
                                         after_revoke.GetCallback());
    ASSERT_TRUE(after_revoke.IsReady());
    EXPECT_FALSE(after_revoke.Get().ready);
  }
  EXPECT_EQ(state->stored, 1u);
}

TEST(AccessOptionsServiceTest, PreviewReturnsReferenceWithoutWritingOptionsDatabase) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  auto* options = AccessOptionsService::GetOrCreate(profile.get());
  ASSERT_TRUE(options);
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(*editor, kSyntheticShare, preview.GetCallback());
  ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
  ASSERT_TRUE(preview.Get().reference);
  EXPECT_FALSE(preview.Get().reference->id.empty());
  base::test::TestFuture<TestNodePreviewResult> rejected;
  options->PreviewTestNodeText(*editor, "https://example.test", rejected.GetCallback());
  EXPECT_EQ(rejected.Get().status, TestNodePreviewStatus::kUnsupportedOutbound);
  EXPECT_FALSE(rejected.Get().reference);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
}

TEST(AccessOptionsServiceTest, PrivateProfileCannotObtainParsedNode) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  auto profile = TestingProfile::Builder().Build();
  auto* options = AccessOptionsService::GetOrCreate(
      profile->GetPrimaryOTRProfile(/*create_if_needed=*/true));
  ASSERT_TRUE(options);
  EXPECT_FALSE(options->BeginTestNodeEditor());
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText("forged-editor", kSyntheticShare,
                              preview.GetCallback());
  EXPECT_EQ(preview.Get().status, TestNodePreviewStatus::kNotAdmitted);
  EXPECT_FALSE(preview.Get().reference);
}

TEST(AccessOptionsServiceTest, WebSocketTextPreviewUsesNativeDevAdmission) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  const bool debug = AccessBuildIdentityForCurrentBuild().product ==
                     aegis_access::AccessProductChannel::kDebug;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  auto state = std::make_shared<SyntheticVaultState>();
  auto options = AccessOptionsService::CreateForTesting(
      profile.get(), std::make_unique<SyntheticVaultBackend>(state));
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeText(
      *editor, aegis_access::test::kSyntheticWebSocketTlsShare,
      preview.GetCallback());
  EXPECT_EQ(preview.Get().status, debug ? TestNodePreviewStatus::kPreview
                                      : TestNodePreviewStatus::kUnsupportedOutbound);
  EXPECT_EQ(preview.Get().reference.has_value(), debug);
  EXPECT_EQ(preview.Get().uses_web_socket_tls, debug);
  EXPECT_EQ(state->stored, debug ? 1u : 0u);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
}

TEST(AccessOptionsServiceTest, InvalidImageCannotProduceNativeNode) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  auto profile = TestingProfile::Builder().Build();
  auto* options = AccessOptionsService::GetOrCreate(profile.get());
  ASSERT_TRUE(options);
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> preview;
  options->PreviewTestNodeQr(*editor, "not base64", preview.GetCallback());
  EXPECT_EQ(preview.Get().status, TestNodePreviewStatus::kInvalidImage);
  EXPECT_FALSE(preview.Get().reference);
}

TEST(AccessOptionsServiceTest, CanceledAndClosedEditorsRejectLateStagedReference) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  auto* options = AccessOptionsService::GetOrCreate(profile.get());
  ASSERT_TRUE(options);
  const auto a = options->BeginTestNodeEditor(), b = options->BeginTestNodeEditor();
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NE(a, b);
  base::test::TestFuture<TestNodePreviewResult> canceled, closed;
  options->PreviewTestNodeText(*a, kSyntheticShare, canceled.GetCallback());
  options->PreviewTestNodeText(*b, kSyntheticShare, closed.GetCallback());
  options->CancelTestNodePreview(*a);
  options->EndTestNodeEditor(*b);
  EXPECT_EQ(canceled.Get().status, TestNodePreviewStatus::kCanceled);
  EXPECT_FALSE(canceled.Get().reference);
  EXPECT_EQ(closed.Get().status, TestNodePreviewStatus::kCanceled);
  EXPECT_FALSE(closed.Get().reference);
  base::test::TestFuture<TestNodePreviewResult> corrected;
  options->PreviewTestNodeText(*a, kSyntheticShare, corrected.GetCallback());
  EXPECT_EQ(corrected.Get().status, TestNodePreviewStatus::kPreview);
  EXPECT_TRUE(corrected.Get().reference);
}

TEST(AccessOptionsServiceTest, KeychainFailureCannotReturnReadyPreview) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  class DeniedBackend final : public AccessTestNodeVaultMac::Backend {
   public:
    AccessTestNodeVaultStatus Add(std::string_view, std::string_view,
                                  std::string_view) override {
      return AccessTestNodeVaultStatus::kUnavailable;
    }
    std::optional<std::string> Read(std::string_view,
                                    std::string_view) override {
      return std::nullopt;
    }
    AccessTestNodeVaultStatus Delete(std::string_view,
                                     std::string_view) override {
      return AccessTestNodeVaultStatus::kOk;
    }
  };
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  auto options = AccessOptionsService::CreateForTesting(
      profile.get(), std::make_unique<DeniedBackend>());
  const auto editor = options->BeginTestNodeEditor();
  ASSERT_TRUE(editor);
  base::test::TestFuture<TestNodePreviewResult> result;
  options->PreviewTestNodeText(*editor, kSyntheticShare, result.GetCallback());
  EXPECT_EQ(result.Get().status, TestNodePreviewStatus::kSecureStorageUnavailable);
  EXPECT_FALSE(result.Get().reference);
}

TEST(AccessOptionsServiceTest, FailedDiscardAndReplacementKeepOtherEditorsDraft) {
  content::BrowserTaskEnvironment environment;
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  struct State {
    std::atomic<bool> deny_delete{false};
    std::atomic<size_t> stored{0};
  };
  class Backend final : public AccessTestNodeVaultMac::Backend {
   public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    AccessTestNodeVaultStatus Add(std::string_view service, std::string_view account,
                                  std::string_view secret) override {
      const auto inserted = rows_.emplace(std::make_pair(service, account), secret).second;
      state_->stored = rows_.size();
      return inserted ? AccessTestNodeVaultStatus::kOk
                      : AccessTestNodeVaultStatus::kUnavailable;
    }
    std::optional<std::string> Read(std::string_view service,
                                    std::string_view account) override {
      const auto row = rows_.find({std::string(service), std::string(account)});
      return row == rows_.end() ? std::nullopt : std::make_optional(row->second);
    }
    AccessTestNodeVaultStatus Delete(std::string_view service,
                                     std::string_view account) override {
      if (state_->deny_delete) return AccessTestNodeVaultStatus::kUnavailable;
      rows_.erase({std::string(service), std::string(account)});
      state_->stored = rows_.size();
      return AccessTestNodeVaultStatus::kOk;
    }
   private:
    std::shared_ptr<State> state_;
    std::map<std::pair<std::string, std::string>, std::string> rows_;
  };
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  auto state = std::make_shared<State>();
  auto options = AccessOptionsService::CreateForTesting(
      profile.get(), std::make_unique<Backend>(state));
  const auto a = options->BeginTestNodeEditor(), b = options->BeginTestNodeEditor();
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  base::test::TestFuture<TestNodePreviewResult> first, second;
  options->PreviewTestNodeText(*a, kSyntheticShare, first.GetCallback());
  options->PreviewTestNodeText(*b, kSyntheticShare, second.GetCallback());
  ASSERT_EQ(first.Get().status, TestNodePreviewStatus::kPreview);
  ASSERT_EQ(second.Get().status, TestNodePreviewStatus::kPreview);
  ASSERT_EQ(state->stored, 2u);
  state->deny_delete = true;
  base::test::TestFuture<TestNodePreviewResult> replacement;
  options->PreviewTestNodeText(*a, kSyntheticShare, replacement.GetCallback());
  EXPECT_EQ(replacement.Get().status, TestNodePreviewStatus::kSecureStorageUnavailable);
  EXPECT_FALSE(replacement.Get().reference);
  base::test::TestFuture<TestNodePreviewStatus> failed;
  options->DiscardTestNodeDraft(*a, failed.GetCallback());
  EXPECT_EQ(failed.Get(), TestNodePreviewStatus::kSecureStorageUnavailable);
  EXPECT_EQ(state->stored, 2u);
  state->deny_delete = false;
  base::test::TestFuture<TestNodePreviewStatus> retry, other;
  options->DiscardTestNodeDraft(*a, retry.GetCallback());
  EXPECT_EQ(retry.Get(), TestNodePreviewStatus::kCanceled);
  EXPECT_EQ(state->stored, 1u);
  options->DiscardTestNodeDraft(*b, other.GetCallback());
  EXPECT_EQ(other.Get(), TestNodePreviewStatus::kCanceled);
  EXPECT_EQ(state->stored, 0u);
}
#endif  // BUILDFLAG(IS_MAC)

TEST(AccessOptionsServiceTest, ProductChannelConflictPreservesExistingDatabase) {
  content::BrowserTaskEnvironment environment;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto profile = TestingProfile::Builder().SetPath(dir.GetPath()).Build();
  const auto other_channel = AccessChannelForCurrentBuild() == ChannelNamespace::kBeta
      ? ChannelNamespace::kDev : ChannelNamespace::kBeta;
  {
    auto binding = TrustedAccessStoreBindingFactory::CreatePersistent(
        other_channel, "aegis-access-profile:" + dir.GetPath().AsUTF8Unsafe(),
        "previous-runtime", dir.GetPath());
    ASSERT_TRUE(binding);
    AccessRuleStore previous(*binding);
    ASSERT_EQ(previous.Open(), StoreStatus::kValid);
  }
  std::string original;
  ASSERT_TRUE(base::ReadFileToString(dir.GetPath().AppendASCII("access.db"), &original));
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile.get());
  ASSERT_TRUE(coordinator);
  base::test::TestFuture<StoreStatus> checked;
  coordinator->CheckOptionsStoreBinding(checked.GetCallback());
  EXPECT_EQ(checked.Get(), StoreStatus::kConflict);
  if (auto* options = AccessOptionsService::GetOrCreate(profile.get())) {
    base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> read;
    options->Read(read.GetCallback());
    EXPECT_EQ(read.Get().status, StoreStatus::kConflict);
    EXPECT_FALSE(read.Get().value);
  }
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
  std::string current;
  ASSERT_TRUE(base::ReadFileToString(dir.GetPath().AppendASCII("access.db"), &current));
  EXPECT_EQ(current, original);
}
}  // namespace aegis::access
