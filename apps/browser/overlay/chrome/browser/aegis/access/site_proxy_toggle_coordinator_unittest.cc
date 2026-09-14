// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/site_proxy_toggle_coordinator.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {

aegis_access::OwnershipKey Owner(aegis_access::ChannelNamespace channel =
                                     aegis_access::ChannelNamespace::kBeta) {
  return {channel, "profile-A", ""};
}

BrowserConfirmedSite ConfirmedSite() {
  return {.storage_partition_token = "default",
          .canonical_host = "news.example",
          .http_top_level_site = "http://news.example",
          .https_top_level_site = "https://news.example"};
}

SiteToggleRequest ToggleRequest(std::string id,
                                bool enabled,
                                uint64_t expected_revision = 0) {
  return {.operation_id = id,
          .request_fingerprint = "fingerprint-" + id,
          .site_toggle_id = "site-news",
          .site = ConfirmedSite(),
          .enabled = enabled,
          .proxy_group_id = enabled ? "proxy-A" : "",
          .expected_revision = expected_revision,
          .service_incarnation = 19,
          .required_contexts = {{"default", 23}, {"media", 29}}};
}

OperationEventIdentity Identity(const PendingSiteOperation& operation) {
  return {.operation_id = operation.request.operation_id,
          .request_fingerprint = operation.request.request_fingerprint,
          .owner = operation.owner,
          .expected_revision = operation.request.expected_revision,
          .policy_generation = operation.policy_generation,
          .operation_sequence = operation.operation_sequence,
          .service_incarnation = operation.request.service_incarnation};
}

RouteReadyEvent RouteEvent(const PendingSiteOperation& operation) {
  RouteReadyEvent event;
  static_cast<OperationEventIdentity&>(event) = Identity(operation);
  return event;
}

PolicyAckEvent AckEvent(const PendingSiteOperation& operation, size_t index) {
  PolicyAckEvent event;
  static_cast<OperationEventIdentity&>(event) = Identity(operation);
  event.context_id = operation.required_contexts[index].context_id;
  event.context_incarnation = operation.required_contexts[index].incarnation;
  return event;
}

class FakeRoutePreparer final : public RoutePreparer {
 public:
  void PrepareRoute(const PendingSiteOperation& operation) override {
    prepared.push_back(operation);
  }
  std::vector<PendingSiteOperation> prepared;
};

class FakePolicyPublisher final : public PolicyPublisher {
 public:
  void PublishPolicy(const PendingSiteOperation& operation) override {
    published.push_back(operation);
  }
  void CommitAndRelease(const PendingSiteOperation& operation) override {
    released.push_back(operation);
  }
  void EnterFailClosed(const BrowserConfirmedSite& site,
                       uint64_t policy_generation,
                       const std::string& reason) override {
    fail_closed_sites.push_back(site);
    fail_closed_generations.push_back(policy_generation);
    fail_closed_reasons.push_back(reason);
  }
  void EnterStoreFailClosed(const aegis_access::OwnershipKey& owner,
                            const std::string& reason) override {
    store_fail_closed_owners.push_back(owner);
    store_fail_closed_reasons.push_back(reason);
  }

  std::vector<PendingSiteOperation> published;
  std::vector<PendingSiteOperation> released;
  std::vector<BrowserConfirmedSite> fail_closed_sites;
  std::vector<uint64_t> fail_closed_generations;
  std::vector<std::string> fail_closed_reasons;
  std::vector<aegis_access::OwnershipKey> store_fail_closed_owners;
  std::vector<std::string> store_fail_closed_reasons;
};

TEST(SiteProxyToggleCoordinatorTest, EnableWaitsForRouteAndEveryMatchingAck) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  ToggleStartResult start =
      coordinator.SetSiteProxy(ToggleRequest("enable", true));
  ASSERT_EQ(start.status, ToggleStartStatus::kPendingRoute);
  ASSERT_TRUE(start.operation);
  EXPECT_EQ(routes.prepared.size(), 1u);
  EXPECT_TRUE(policies.published.empty());

  EXPECT_EQ(coordinator.OnRouteReady(RouteEvent(*start.operation)),
            ToggleEventResult::kPublished);
  EXPECT_EQ(policies.published.size(), 1u);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*start.operation, 1)),
            ToggleEventResult::kWaiting);
  EXPECT_EQ(store.ReadSiteSelection(ConfirmedSite()).state,
            SiteSelectionReadState::kRestoring);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*start.operation, 0)),
            ToggleEventResult::kCommitted);
  EXPECT_EQ(store.ReadSiteSelection(ConfirmedSite()).selection,
            aegis_access::GroupSelection::kEnabled);
  ASSERT_EQ(policies.released.size(), 1u);
  EXPECT_EQ(policies.released[0].request.operation_id, "enable");
}

TEST(SiteProxyToggleCoordinatorTest, ClosingSkipsPreparationAndCommitsDirect) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  auto enable = coordinator.SetSiteProxy(ToggleRequest("enable", true));
  ASSERT_TRUE(enable.operation);
  coordinator.OnRouteReady(RouteEvent(*enable.operation));
  coordinator.OnPolicyAcknowledged(AckEvent(*enable.operation, 0));
  coordinator.OnPolicyAcknowledged(AckEvent(*enable.operation, 1));

  auto close = coordinator.SetSiteProxy(ToggleRequest("close", false, 1));
  ASSERT_EQ(close.status, ToggleStartStatus::kPendingPublish);
  ASSERT_TRUE(close.operation);
  EXPECT_EQ(routes.prepared.size(), 1u);
  EXPECT_EQ(policies.published.size(), 2u);
  coordinator.OnPolicyAcknowledged(AckEvent(*close.operation, 0));
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*close.operation, 1)),
            ToggleEventResult::kCommitted);
  SiteSelectionRecord selection = store.ReadSiteSelection(ConfirmedSite());
  EXPECT_EQ(selection.selection, aegis_access::GroupSelection::kDisabled);
  EXPECT_TRUE(selection.proxy_group_id.empty());
}

TEST(SiteProxyToggleCoordinatorTest,
     LaterSameSiteOperationSupersedesLateEvents) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  auto earlier = coordinator.SetSiteProxy(ToggleRequest("earlier", true));
  auto later = coordinator.SetSiteProxy(ToggleRequest("later", false));
  ASSERT_TRUE(earlier.operation);
  ASSERT_TRUE(later.operation);
  EXPECT_GT(later.operation->operation_sequence,
            earlier.operation->operation_sequence);
  EXPECT_EQ(coordinator.OnRouteReady(RouteEvent(*earlier.operation)),
            ToggleEventResult::kIgnored);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*earlier.operation, 0)),
            ToggleEventResult::kIgnored);

  coordinator.OnPolicyAcknowledged(AckEvent(*later.operation, 1));
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*later.operation, 0)),
            ToggleEventResult::kCommitted);
  EXPECT_EQ(store.ReadSiteSelection(ConfirmedSite()).selection,
            aegis_access::GroupSelection::kDisabled);
}

TEST(SiteProxyToggleCoordinatorTest,
     SupersedingPublishedOperationKeepsScopeFailClosed) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  auto earlier = coordinator.SetSiteProxy(ToggleRequest("earlier", false));
  ASSERT_TRUE(earlier.operation);
  ASSERT_EQ(policies.published.size(), 1u);
  auto later = coordinator.SetSiteProxy(ToggleRequest("later", false));
  ASSERT_TRUE(later.operation);
  ASSERT_EQ(policies.fail_closed_reasons.size(), 1u);
  EXPECT_EQ(policies.fail_closed_reasons[0],
            "published_operation_superseded");
  ASSERT_EQ(policies.published.size(), 2u);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*earlier.operation, 0)),
            ToggleEventResult::kIgnored);
}

TEST(SiteProxyToggleCoordinatorTest, AckBeforePublicationIsIgnored) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  auto start = coordinator.SetSiteProxy(ToggleRequest("enable", true));
  ASSERT_TRUE(start.operation);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*start.operation, 0)),
            ToggleEventResult::kIgnored);
  ASSERT_EQ(coordinator.OnRouteReady(RouteEvent(*start.operation)),
            ToggleEventResult::kPublished);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*start.operation, 1)),
            ToggleEventResult::kWaiting);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*start.operation, 0)),
            ToggleEventResult::kCommitted);
}

TEST(SiteProxyToggleCoordinatorTest,
     DifferentSitesPublishInGlobalGenerationOrder) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  SiteToggleRequest first_request = ToggleRequest("first", true);
  SiteToggleRequest second_request = ToggleRequest("second", false);
  second_request.site_toggle_id = "site-other";
  second_request.site.canonical_host = "other.example";
  second_request.site.http_top_level_site = "http://other.example";
  second_request.site.https_top_level_site = "https://other.example";
  auto first = coordinator.SetSiteProxy(first_request);
  auto second = coordinator.SetSiteProxy(second_request);
  ASSERT_TRUE(first.operation);
  ASSERT_TRUE(second.operation);
  EXPECT_TRUE(policies.published.empty());

  EXPECT_EQ(coordinator.OnRouteReady(RouteEvent(*first.operation)),
            ToggleEventResult::kPublished);
  ASSERT_EQ(policies.published.size(), 1u);
  EXPECT_EQ(policies.published[0].request.operation_id, "first");
  coordinator.OnPolicyAcknowledged(AckEvent(*first.operation, 0));
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*first.operation, 1)),
            ToggleEventResult::kCommitted);
  ASSERT_EQ(policies.published.size(), 2u);
  EXPECT_EQ(policies.published[1].request.operation_id, "second");
  EXPECT_LT(policies.published[0].policy_generation,
            policies.published[1].policy_generation);
}

TEST(SiteProxyToggleCoordinatorTest, RouteFailureAdvancesPublicationLane) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  SiteToggleRequest second_request = ToggleRequest("second", false);
  second_request.site_toggle_id = "site-other";
  second_request.site.canonical_host = "other.example";
  second_request.site.http_top_level_site = "http://other.example";
  second_request.site.https_top_level_site = "https://other.example";
  auto first = coordinator.SetSiteProxy(ToggleRequest("first", true));
  auto second = coordinator.SetSiteProxy(second_request);
  ASSERT_TRUE(first.operation);
  ASSERT_TRUE(second.operation);

  EXPECT_EQ(coordinator.OnRouteFailed(RouteEvent(*first.operation)),
            ToggleEventResult::kFailed);
  ASSERT_EQ(policies.published.size(), 1u);
  EXPECT_EQ(policies.published[0].request.operation_id, "second");
}

TEST(SiteProxyToggleCoordinatorTest, RejectsWrongOwnerRevisionAndIncarnations) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
  auto start = coordinator.SetSiteProxy(ToggleRequest("enable", true));
  ASSERT_TRUE(start.operation);

  RouteReadyEvent wrong_service = RouteEvent(*start.operation);
  ++wrong_service.service_incarnation;
  EXPECT_EQ(coordinator.OnRouteReady(wrong_service),
            ToggleEventResult::kIgnored);
  RouteReadyEvent wrong_owner = RouteEvent(*start.operation);
  wrong_owner.owner.profile_token = "profile-B";
  EXPECT_EQ(coordinator.OnRouteReady(wrong_owner), ToggleEventResult::kIgnored);
  RouteReadyEvent wrong_revision = RouteEvent(*start.operation);
  ++wrong_revision.expected_revision;
  EXPECT_EQ(coordinator.OnRouteReady(wrong_revision),
            ToggleEventResult::kIgnored);
  RouteReadyEvent wrong_fingerprint = RouteEvent(*start.operation);
  wrong_fingerprint.request_fingerprint = "not-the-request";
  EXPECT_EQ(coordinator.OnRouteReady(wrong_fingerprint),
            ToggleEventResult::kIgnored);
  RouteReadyEvent wrong_sequence = RouteEvent(*start.operation);
  ++wrong_sequence.operation_sequence;
  EXPECT_EQ(coordinator.OnRouteReady(wrong_sequence),
            ToggleEventResult::kIgnored);

  ASSERT_EQ(coordinator.OnRouteReady(RouteEvent(*start.operation)),
            ToggleEventResult::kPublished);
  PolicyAckEvent wrong_context = AckEvent(*start.operation, 0);
  ++wrong_context.context_incarnation;
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(wrong_context),
            ToggleEventResult::kIgnored);
  PolicyAckEvent wrong_generation = AckEvent(*start.operation, 0);
  ++wrong_generation.policy_generation;
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(wrong_generation),
            ToggleEventResult::kIgnored);
}

TEST(SiteProxyToggleCoordinatorTest, SaveFailuresRespectPublishBoundary) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);

  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kPrepareOperation);
  EXPECT_EQ(
      coordinator.SetSiteProxy(ToggleRequest("prepare-fail", true)).status,
      ToggleStartStatus::kSaveFailed);
  EXPECT_TRUE(routes.prepared.empty());
  EXPECT_TRUE(policies.published.empty());

  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kNone);
  auto route_save = coordinator.SetSiteProxy(ToggleRequest("route-save", true));
  ASSERT_TRUE(route_save.operation);
  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kMarkRouteReady);
  EXPECT_EQ(coordinator.OnRouteReady(RouteEvent(*route_save.operation)),
            ToggleEventResult::kSaveFailed);
  EXPECT_TRUE(policies.published.empty());
  EXPECT_TRUE(policies.fail_closed_sites.empty());

  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kNone);
  auto close =
      coordinator.SetSiteProxy(ToggleRequest("published-close", false));
  ASSERT_TRUE(close.operation);
  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kMarkAcknowledged);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*close.operation, 0)),
            ToggleEventResult::kSaveFailed);
  ASSERT_EQ(policies.fail_closed_sites.size(), 1u);
  EXPECT_EQ(policies.fail_closed_reasons.back(), "ack_save_failed");
}

TEST(SiteProxyToggleCoordinatorTest, CommitFailureAfterPublishFailsClosed) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
  auto close = coordinator.SetSiteProxy(ToggleRequest("close", false));
  ASSERT_TRUE(close.operation);
  coordinator.OnPolicyAcknowledged(AckEvent(*close.operation, 0));
  store.SetFailurePointForTesting(
      AccessRuleStore::FailurePointForTesting::kCommitOperation);
  EXPECT_EQ(coordinator.OnPolicyAcknowledged(AckEvent(*close.operation, 1)),
            ToggleEventResult::kSaveFailed);
  ASSERT_EQ(policies.fail_closed_sites.size(), 1u);
  EXPECT_EQ(policies.fail_closed_reasons.back(), "commit_save_failed");
  EXPECT_EQ(store.ReadSiteSelection(ConfirmedSite()).state,
            SiteSelectionReadState::kRestoring);
}

TEST(SiteProxyToggleCoordinatorTest, CrashRecoveryFailsClosedAndResumes) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath path = temp_dir.GetPath().AppendASCII("access.db");
  PendingSiteOperation before_crash;
  {
    AccessRuleStore store(path, Owner());
    ASSERT_TRUE(store.Initialize());
    FakeRoutePreparer routes;
    FakePolicyPublisher policies;
    SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
    auto started = coordinator.SetSiteProxy(ToggleRequest("crash", true));
    ASSERT_TRUE(started.operation);
    before_crash = *started.operation;
  }

  AccessRuleStore reopened(path, Owner());
  ASSERT_TRUE(reopened.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator recovered(&reopened, &routes, &policies);
  recovered.RecoverPendingOperations(
      31, {{"default", 37}, {"media", 41}});
  EXPECT_EQ(reopened.ReadSiteSelection(ConfirmedSite()).state,
            SiteSelectionReadState::kRestoring);
  EXPECT_EQ(routes.prepared.size(), 1u);
  EXPECT_EQ(routes.prepared[0].request.service_incarnation, 31u);
  EXPECT_EQ(routes.prepared[0].required_contexts[0].incarnation, 37u);
  ASSERT_EQ(policies.fail_closed_sites.size(), 1u);
  EXPECT_EQ(policies.fail_closed_reasons[0], "pending_operation_recovery");
  EXPECT_EQ(recovered.OnRouteReady(RouteEvent(before_crash)),
            ToggleEventResult::kIgnored);
}

TEST(SiteProxyToggleCoordinatorTest, CorruptPendingJournalFailsClosedByStore) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
  ASSERT_TRUE(
      coordinator.SetSiteProxy(ToggleRequest("broken", true)).operation);
  ASSERT_TRUE(store.ExecuteSqlForTesting(
      "UPDATE access_pending_operations SET target_mode=999 WHERE "
      "operation_id='broken'"));

  coordinator.RecoverPendingOperations(31, {{"default", 37}});
  ASSERT_EQ(policies.store_fail_closed_reasons.size(), 1u);
  EXPECT_EQ(policies.store_fail_closed_reasons[0],
            "pending_journal_corrupt");
  EXPECT_TRUE(routes.prepared.empty());
}

TEST(SiteProxyToggleCoordinatorTest,
     NewToggleCannotSilentlySupersedeCorruptPendingJournal) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
  ASSERT_TRUE(
      coordinator.SetSiteProxy(ToggleRequest("broken", true)).operation);
  ASSERT_TRUE(store.ExecuteSqlForTesting(
      "UPDATE access_pending_operations SET target_mode=999 WHERE "
      "operation_id='broken'"));

  EXPECT_EQ(
      coordinator.SetSiteProxy(ToggleRequest("replacement", false)).status,
      ToggleStartStatus::kCorruptState);
  ASSERT_EQ(policies.store_fail_closed_reasons.size(), 1u);
  EXPECT_EQ(policies.store_fail_closed_reasons[0],
            "pending_journal_corrupt");
}

TEST(SiteProxyToggleCoordinatorTest, RuntimeFailureDoesNotRewriteSelection) {
  AccessRuleStore store(base::FilePath(), Owner(), /*in_memory=*/true);
  ASSERT_TRUE(store.Initialize());
  FakeRoutePreparer routes;
  FakePolicyPublisher policies;
  SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
  auto enable = coordinator.SetSiteProxy(ToggleRequest("enable", true));
  ASSERT_TRUE(enable.operation);
  coordinator.OnRouteReady(RouteEvent(*enable.operation));
  coordinator.OnPolicyAcknowledged(AckEvent(*enable.operation, 0));
  coordinator.OnPolicyAcknowledged(AckEvent(*enable.operation, 1));

  coordinator.SetRuntimeState(ConfirmedSite(),
                              aegis_access::ProxyRuntimeState::kOffline);
  SiteToggleState state = coordinator.GetSiteToggleState(ConfirmedSite());
  EXPECT_EQ(state.committed.selection, aegis_access::GroupSelection::kEnabled);
  EXPECT_EQ(state.runtime_state, aegis_access::ProxyRuntimeState::kOffline);
}

TEST(SiteProxyToggleCoordinatorTest,
     NarrowCoordinatorRejectsNonProductChannels) {
  for (aegis_access::ChannelNamespace channel :
       {aegis_access::ChannelNamespace::kDev,
        aegis_access::ChannelNamespace::kAlpha,
        aegis_access::ChannelNamespace::kInvalid}) {
    AccessRuleStore store(base::FilePath(), Owner(channel),
                          /*in_memory=*/true);
    if (channel == aegis_access::ChannelNamespace::kInvalid) {
      EXPECT_FALSE(store.Initialize());
      continue;
    }
    ASSERT_TRUE(store.Initialize());
    FakeRoutePreparer routes;
    FakePolicyPublisher policies;
    SiteProxyToggleCoordinator coordinator(&store, &routes, &policies);
    EXPECT_EQ(coordinator.SetSiteProxy(ToggleRequest("toggle", true)).status,
              ToggleStartStatus::kRejectedChannel);
  }
}

}  // namespace
}  // namespace aegis::access
