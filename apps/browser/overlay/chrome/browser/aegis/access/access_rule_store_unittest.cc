// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_rule_store.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "sql/database.h"
#include "sql/meta_table.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {

aegis_access::OwnershipKey StoreOwner(aegis_access::ChannelNamespace channel =
                                          aegis_access::ChannelNamespace::kBeta,
                                      std::string profile = "profile-A") {
  return {channel, std::move(profile), ""};
}

BrowserConfirmedSite Site(std::string partition = "partition-A",
                          std::string host = "news.example") {
  return {.storage_partition_token = std::move(partition),
          .canonical_host = std::move(host),
          .http_top_level_site = "http://news.example",
          .https_top_level_site = "https://news.example"};
}

SiteToggleRequest Request(std::string operation_id,
                          bool enabled,
                          uint64_t expected_revision = 0) {
  return {.operation_id = operation_id,
          .request_fingerprint = "fingerprint-" + operation_id,
          .site_toggle_id = "toggle-news",
          .site = Site(),
          .enabled = enabled,
          .proxy_group_id = enabled ? "managed-proxy" : "",
          .expected_revision = expected_revision,
          .service_incarnation = 7,
          .required_contexts = {{"default", 11}, {"isolated", 13}}};
}

void AcknowledgeAndCommit(AccessRuleStore* store,
                          PendingSiteOperation operation) {
  if (operation.request.enabled) {
    ASSERT_TRUE(store->MarkRouteReady(operation));
    operation.route_ready = true;
  }
  ASSERT_TRUE(store->MarkPublishStarted(operation));
  operation.publish_started = true;
  for (const auto& context : operation.required_contexts) {
    ASSERT_TRUE(store->MarkContextAcknowledged(operation, context));
  }
  EXPECT_EQ(store->CommitOperation(operation),
            CommitOperationStatus::kCommitted);
}

TEST(AccessRuleStoreTest, InMemoryPrepareCommitAndSelectionAreAtomic) {
  AccessRuleStore store(base::FilePath(), StoreOwner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  EXPECT_EQ(store.ReadSiteSelection(Site()).state,
            SiteSelectionReadState::kMissing);

  PrepareOperationResult prepared =
      store.PrepareSiteToggleOperation(Request("enable", true));
  ASSERT_EQ(prepared.status, PrepareOperationStatus::kPrepared);
  ASSERT_TRUE(prepared.operation);
  SiteSelectionRecord restoring = store.ReadSiteSelection(Site());
  EXPECT_EQ(restoring.state, SiteSelectionReadState::kRestoring);
  EXPECT_EQ(restoring.selection, aegis_access::GroupSelection::kUnknown);

  AcknowledgeAndCommit(&store, *prepared.operation);
  SiteSelectionRecord active = store.ReadSiteSelection(Site());
  EXPECT_EQ(active.state, SiteSelectionReadState::kValid);
  EXPECT_EQ(active.selection, aegis_access::GroupSelection::kEnabled);
  EXPECT_EQ(active.revision, 1u);
  EXPECT_EQ(active.proxy_group_id, "managed-proxy");
}

TEST(AccessRuleStoreTest, FileDatabaseReopensWithExactOwner) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath path = temp_dir.GetPath().AppendASCII("access.db");
  {
    AccessRuleStore store(path, StoreOwner());
    ASSERT_TRUE(store.Initialize());
    auto prepared = store.PrepareSiteToggleOperation(Request("persist", true));
    ASSERT_TRUE(prepared.operation);
    AcknowledgeAndCommit(&store, *prepared.operation);
  }
  {
    AccessRuleStore reopened(path, StoreOwner());
    ASSERT_TRUE(reopened.Initialize());
    EXPECT_EQ(reopened.ReadSiteSelection(Site()).selection,
              aegis_access::GroupSelection::kEnabled);
  }
  AccessRuleStore wrong_profile(
      path, StoreOwner(aegis_access::ChannelNamespace::kBeta, "profile-B"));
  EXPECT_FALSE(wrong_profile.Initialize());
  AccessRuleStore wrong_channel(
      path, StoreOwner(aegis_access::ChannelNamespace::kRelease));
  EXPECT_FALSE(wrong_channel.Initialize());
}

TEST(AccessRuleStoreTest, PartitionIsolationAndIndependentRulesSurviveReplace) {
  AccessRuleStore store(base::FilePath(), StoreOwner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  IndependentAccessRule debug_rule{
      .rule_id = "debug-reject",
      .storage_partition_token = "partition-A",
      .top_level_site = "https://news.example",
      .exact_host = "news.example",
      .schemes = {aegis_access::RequestScheme::kHttps},
      .ports = aegis_access::PortScope::kExplicitSubset,
      .include_subdomains = false,
      .mode = aegis_access::AccessMode::kReject,
      .proxy_group_id = "",
      .protection_override = aegis_access::ProtectionOverride::kNone,
      .row_revision = 3,
      .last_operation_sequence = 9,
  };
  ASSERT_TRUE(store.SaveIndependentRule(debug_rule));
  auto enabled = store.PrepareSiteToggleOperation(Request("enable", true));
  ASSERT_TRUE(enabled.operation);
  AcknowledgeAndCommit(&store, *enabled.operation);
  auto disabled =
      store.PrepareSiteToggleOperation(Request("disable", false, 1));
  ASSERT_TRUE(disabled.operation);
  AcknowledgeAndCommit(&store, *disabled.operation);
  SiteToggleRequest partition_b = Request("partition-b", true);
  partition_b.site = Site("partition-B");
  auto enabled_partition_b = store.PrepareSiteToggleOperation(partition_b);
  ASSERT_TRUE(enabled_partition_b.operation);
  AcknowledgeAndCommit(&store, *enabled_partition_b.operation);

  EXPECT_EQ(store.ReadSiteSelection(Site()).selection,
            aegis_access::GroupSelection::kDisabled);
  EXPECT_TRUE(store.ReadSiteSelection(Site()).has_independent_rule);
  EXPECT_TRUE(store.LoadIndependentRule("partition-A", "debug-reject"));
  EXPECT_EQ(store.ReadSiteSelection(Site("partition-B")).selection,
            aegis_access::GroupSelection::kEnabled);
}

TEST(AccessRuleStoreTest, PartialCorruptAndReadErrorsNeverBecomeDisabled) {
  AccessRuleStore store(base::FilePath(), StoreOwner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  auto prepared = store.PrepareSiteToggleOperation(Request("enable", true));
  ASSERT_TRUE(prepared.operation);
  AcknowledgeAndCommit(&store, *prepared.operation);
  ASSERT_TRUE(store.ExecuteSqlForTesting(
      "DELETE FROM access_rules WHERE rule_id='toggle-news:https'"));
  SiteSelectionRecord partial = store.ReadSiteSelection(Site());
  EXPECT_EQ(partial.state, SiteSelectionReadState::kCorrupt);
  EXPECT_EQ(partial.selection, aegis_access::GroupSelection::kUnknown);

  store.CloseDatabaseForTesting();
  SiteSelectionRecord read_error = store.ReadSiteSelection(Site());
  EXPECT_EQ(read_error.state, SiteSelectionReadState::kStorageError);
  EXPECT_EQ(read_error.selection, aegis_access::GroupSelection::kUnknown);
}

TEST(AccessRuleStoreTest, CasDuplicateAndCountersAreMonotonic) {
  AccessRuleStore store(base::FilePath(), StoreOwner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  auto first = store.PrepareSiteToggleOperation(Request("operation-1", true));
  ASSERT_EQ(first.status, PrepareOperationStatus::kPrepared);
  auto duplicate =
      store.PrepareSiteToggleOperation(Request("operation-1", true));
  EXPECT_EQ(duplicate.status, PrepareOperationStatus::kDuplicate);

  SiteToggleRequest conflict = Request("operation-1", true);
  conflict.request_fingerprint = "different";
  EXPECT_EQ(store.PrepareSiteToggleOperation(conflict).status,
            PrepareOperationStatus::kDuplicateConflict);

  auto second = store.PrepareSiteToggleOperation(Request("operation-2", false));
  ASSERT_EQ(second.status, PrepareOperationStatus::kPrepared);
  EXPECT_GT(second.operation->operation_sequence,
            first.operation->operation_sequence);
  EXPECT_GT(second.operation->policy_generation,
            first.operation->policy_generation);
  EXPECT_FALSE(store.LoadPendingOperation("operation-1"));
  EXPECT_TRUE(store.LoadPendingOperation("operation-2"));

  SiteToggleRequest wrong_revision = Request("operation-3", true, 9);
  EXPECT_EQ(store.PrepareSiteToggleOperation(wrong_revision).status,
            PrepareOperationStatus::kRevisionConflict);
}

TEST(AccessRuleStoreTest, RejectsNonCanonicalHostAndToggleIdReuse) {
  AccessRuleStore store(base::FilePath(), StoreOwner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  SiteToggleRequest uppercase = Request("uppercase", true);
  uppercase.site.canonical_host = "News.Example";
  EXPECT_EQ(store.PrepareSiteToggleOperation(uppercase).status,
            PrepareOperationStatus::kInvalidRequest);

  SiteToggleRequest mismatched_site = Request("mismatched-site", true);
  mismatched_site.site.https_top_level_site = "https://other.example";
  EXPECT_EQ(store.PrepareSiteToggleOperation(mismatched_site).status,
            PrepareOperationStatus::kInvalidRequest);

  auto first = store.PrepareSiteToggleOperation(Request("first", false));
  ASSERT_TRUE(first.operation);
  AcknowledgeAndCommit(&store, *first.operation);
  SiteToggleRequest reused = Request("reused", false);
  reused.site = {.storage_partition_token = "partition-A",
                 .canonical_host = "other.example",
                 .http_top_level_site = "http://other.example",
                 .https_top_level_site = "https://other.example"};
  EXPECT_EQ(store.PrepareSiteToggleOperation(reused).status,
            PrepareOperationStatus::kInvalidRequest);
}

TEST(AccessRuleStoreTest, PendingAfterImageSurvivesCrashAndReadsRestoring) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath path = temp_dir.GetPath().AppendASCII("access.db");
  uint64_t sequence = 0;
  {
    AccessRuleStore store(path, StoreOwner());
    ASSERT_TRUE(store.Initialize());
    auto pending = store.PrepareSiteToggleOperation(Request("pending", true));
    ASSERT_TRUE(pending.operation);
    sequence = pending.operation->operation_sequence;
  }
  AccessRuleStore reopened(path, StoreOwner());
  ASSERT_TRUE(reopened.Initialize());
  EXPECT_EQ(reopened.ReadSiteSelection(Site()).state,
            SiteSelectionReadState::kRestoring);
  auto recovered = reopened.LoadPendingOperation("pending");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->operation_sequence, sequence);
  EXPECT_EQ(recovered->after_members.size(), 2u);
  EXPECT_EQ(recovered->after_members[0].exact_host, "news.example");
  EXPECT_EQ(recovered->after_members[0].schemes.size(), 4u);
}

TEST(AccessRuleStoreTest, FutureSchemaAndCorruptFileFailInitialization) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath future_path =
      temp_dir.GetPath().AppendASCII("future.db");
  {
    AccessRuleStore store(future_path, StoreOwner());
    ASSERT_TRUE(store.Initialize());
  }
  {
    sql::Database future_database("AegisAccess");
    ASSERT_TRUE(future_database.Open(future_path));
    sql::MetaTable meta;
    ASSERT_TRUE(meta.Init(&future_database, 99, 99));
    ASSERT_TRUE(meta.SetVersionNumber(99));
    ASSERT_TRUE(meta.SetCompatibleVersionNumber(99));
  }
  AccessRuleStore future(future_path, StoreOwner());
  EXPECT_FALSE(future.Initialize());

  const base::FilePath corrupt_path =
      temp_dir.GetPath().AppendASCII("corrupt.db");
  ASSERT_TRUE(base::WriteFile(corrupt_path, "not sqlite"));
  AccessRuleStore corrupt(corrupt_path, StoreOwner());
  EXPECT_FALSE(corrupt.Initialize());

  const base::FilePath partial_path =
      temp_dir.GetPath().AppendASCII("partial.db");
  {
    AccessRuleStore store(partial_path, StoreOwner());
    ASSERT_TRUE(store.Initialize());
  }
  {
    sql::Database partial_database("AegisAccess");
    ASSERT_TRUE(partial_database.Open(partial_path));
    ASSERT_TRUE(partial_database.Execute("DROP TABLE access_rules"));
  }
  AccessRuleStore partial(partial_path, StoreOwner());
  EXPECT_FALSE(partial.Initialize());
}

}  // namespace
}  // namespace aegis::access
