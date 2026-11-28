// Copyright 2026 GCSA
#include "services/network/aegis/access_request_route_state.h"
#include "components/aegis_access/access_prepared_listener_test_fixture.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
using namespace aegis_access;
class AccessRequestRouteStateTest : public testing::Test {
 protected:
  OwnershipKey owner_{ChannelNamespace::kDev, "profile", "partition"};
  GenerationTuple common_{1, 1, 0, 1, 1};
  BrowserOwnedRequestMetadata metadata_{
      "request", owner_, RequestAttributionKind::kDocument, "document", {},
      net::SchemefulSite(GURL("https://a.test"))};
  std::unique_ptr<AccessRouteRegistry> registry_ =
      std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  AccessRouteRegistry::Expectation expectation_{"incarnation", common_, {}};
  void SetUp() override {
    ASSERT_TRUE(registry_->Install(1, {{owner_, 1, {}}, common_, {}}));
  }
  scoped_refptr<AccessRequestRouteState> Create(std::string method = "GET") {
    return AccessRequestRouteState::Create(registry_->GetWeakPtr(), metadata_,
        GURL("https://a.test/start"), method, expectation_);
  }
  AccessRouteRegistry::Snapshot ManagedSnapshot() {
    AccessRouteRegistry::Snapshot snapshot{{owner_, 1, {}}, common_, {}};
    for (const auto group : {"x", "y"}) {
      const std::string site = std::string(group) == "x" ? "https://a.test" : "https://b.test";
      snapshot.policy.rules.push_back({group, owner_, PolicyScope::kSite, site,
          "cdn.test", false, {RequestScheme::kHttps}, {PortScope::kAllBrowserPermitted, {}},
          AccessMode::kProxy, group, ProtectionOverride::kNone, 1, 1});
      net::AccessRouteIsolationKey key{0, "profile", "partition", group,
          std::string("registration-") + group, "incarnation", 1, 1, 1, 1, 1};
      snapshot.routes[group] = net::AccessProxyRoute::Create(key, net::ProxyChain(
          net::ProxyServer::FromSchemeHostAndPort(net::ProxyServer::SCHEME_HTTP,
              "127.0.0.1", std::string(group) == "x" ? 12345 : 12346)));
      snapshot.listener_bindings[group] = aegis_access::test::SyntheticPreparedListenerBinding();
    }
    return snapshot;
  }
  AccessRouteRegistry::Expectation ManagedExpectation(const std::string& group) {
    auto generations = common_;
    generations.selection_generation = 1;
    return {"incarnation", generations, "registration-" + group};
  }
  scoped_refptr<AccessRequestRouteState> CreateManaged(const std::string& group) {
    auto metadata = metadata_;
    metadata.top_frame_site = net::SchemefulSite(GURL(group == "x" ? "https://a.test" : "https://b.test"));
    return AccessRequestRouteState::Create(registry_->GetWeakPtr(), metadata,
        GURL("https://cdn.test/start"), "GET", ManagedExpectation(group));
  }
};
TEST_F(AccessRequestRouteStateTest, PreflightDoesNotConsumeActualRequest) {
  auto state = Create("POST");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->BeginSend(GURL("https://a.test/start"), "OPTIONS", true).net_error,
            net::OK);
  EXPECT_EQ(state->BeginSend(GURL("https://a.test/start"), "POST", false).net_error,
            net::OK);
  EXPECT_NE(state->BeginSend(GURL("https://a.test/start"), "POST", false).net_error,
            net::OK);
}
TEST_F(AccessRequestRouteStateTest, NativeRedirectRequiresPrivateNextHopApproval) {
  auto state = Create();
  ASSERT_TRUE(state);
  ASSERT_EQ(state->BeginSend(GURL("https://a.test/start"), "GET", false).net_error,
            net::OK);
  ASSERT_TRUE(state->ObserveRedirect(GURL("https://b.test/next"), "GET"));
  EXPECT_NE(state->BeginSend(GURL("https://b.test/next"), "GET", false).net_error,
            net::OK);
  ASSERT_TRUE(state->ApproveRedirect(1, GURL("https://b.test/next"), "GET",
                                     expectation_));
  EXPECT_EQ(state->BeginSend(GURL("https://b.test/next"), "GET", false).net_error,
            net::OK);
  EXPECT_FALSE(state->ApproveRedirect(1, GURL("https://b.test/next"), "GET",
                                      expectation_));
}
TEST_F(AccessRequestRouteStateTest, RedirectRejectsWrongHopMethodOrOverrideOrigin) {
  for (int kind = 0; kind < 3; ++kind) {
    auto state = Create("POST");
    ASSERT_TRUE(state);
    ASSERT_EQ(state->BeginSend(GURL("https://a.test/start"), "POST", false).net_error,
              net::OK);
    ASSERT_TRUE(state->ObserveRedirect(GURL("https://b.test/next"), "POST"));
    EXPECT_FALSE(state->ApproveRedirect(kind == 0 ? 2 : 1,
        GURL(kind == 1 ? "https://evil.test/next" : "https://b.test/next"),
        kind == 2 ? "GET" : "POST", expectation_));
    EXPECT_NE(state->BeginSend(GURL("https://b.test/next"), "POST", false).net_error,
              net::OK);
  }
}
TEST_F(AccessRequestRouteStateTest, RechecksRegistryAfterSchedulingDelayAndDestruction) {
  auto state = Create();
  ASSERT_TRUE(state);
  registry_->EnterRestoring();
  EXPECT_NE(state->BeginSend(GURL("https://a.test/start"), "GET", false).net_error,
            net::OK);
  registry_.reset();
  EXPECT_NE(state->BeginSend(GURL("https://a.test/start"), "GET", false).net_error,
            net::OK);
}
TEST_F(AccessRequestRouteStateTest, PreflightCannotChangeTargetOrReplayActualMethod) {
  auto state = Create("POST");
  ASSERT_TRUE(state);
  EXPECT_NE(state->BeginSend(GURL("https://b.test/start"), "OPTIONS", true).net_error,
            net::OK);
  EXPECT_NE(state->BeginSend(GURL("https://a.test/start"), "POST", true).net_error,
            net::OK);
}
TEST_F(AccessRequestRouteStateTest, SubframeNavigationRetainsTrustedTopSite) {
  for (bool main_frame : {false, true}) {
    registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
    AccessPolicyRule reject{"rule", owner_, PolicyScope::kSite, "https://a.test",
        "b.test", false, {RequestScheme::kHttps},
        {PortScope::kAllBrowserPermitted, {}}, AccessMode::kReject, {},
        ProtectionOverride::kNone, 1, 1};
    ASSERT_TRUE(registry_->Install(1, {{owner_, 1, {reject}}, common_, {}}));
    metadata_.attribution_kind = RequestAttributionKind::kPendingNavigation;
    metadata_.document_token.clear();
    metadata_.pending_navigation_token = "navigation";
    if (main_frame) metadata_.top_frame_site.reset();
    auto state = Create();
    ASSERT_TRUE(state);
    ASSERT_EQ(state->BeginSend(GURL("https://a.test/start"), "GET", false).net_error,
              net::OK);
    ASSERT_TRUE(state->ObserveRedirect(GURL("https://b.test/next"), "GET"));
    EXPECT_EQ(state->ApproveRedirect(1, GURL("https://b.test/next"), "GET",
                                     expectation_), main_frame);
  }
}
TEST_F(AccessRequestRouteStateTest, ReinstalledSnapshotCannotReviveDelayedRequestAcrossRestore) {
  auto earlier = Create();
  ASSERT_TRUE(earlier);
  registry_->EnterRestoring();
  ASSERT_TRUE(registry_->Install(2, {{owner_, 1, {}}, common_, {}}));
  EXPECT_NE(earlier->BeginSend(GURL("https://a.test/start"), "GET", false).net_error, net::OK);
  auto fresh = Create();
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->BeginSend(GURL("https://a.test/start"), "GET", false).net_error, net::OK);
}
TEST_F(AccessRequestRouteStateTest, SharedPrimaryAndPreflightObserversRevokeOnlyTheirListener) {
  base::test::TaskEnvironment environment;
  registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  ASSERT_TRUE(registry_->Install(1, ManagedSnapshot()));
  auto x = CreateManaged("x"), y = CreateManaged("y");
  ASSERT_TRUE(x);
  ASSERT_TRUE(y);
  int primary = 0, preflight = 0, other = 0;
  auto first = x->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++primary; }));
  auto second = x->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++preflight; }));
  auto separate = y->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++other; }));
  ASSERT_TRUE(x->InvalidateCurrentListener());
  EXPECT_FALSE(CreateManaged("x"));
  EXPECT_EQ(y->BeginSend(GURL("https://cdn.test/start"), "GET", false).net_error, net::OK);
  environment.RunUntilIdle();
  EXPECT_EQ(primary, 1);
  EXPECT_EQ(preflight, 1);
  EXPECT_EQ(other, 0);
  EXPECT_NE(x->BeginSend(GURL("https://cdn.test/start"), "GET", false).net_error, net::OK);
}
TEST_F(AccessRequestRouteStateTest, LateSubscriptionAfterRestoreCannotAttachToFreshSameListener) {
  base::test::TaskEnvironment environment;
  registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  const auto snapshot = ManagedSnapshot();
  ASSERT_TRUE(registry_->Install(1, snapshot));
  auto earlier = CreateManaged("x");
  ASSERT_TRUE(earlier);
  registry_->EnterRestoring();
  ASSERT_TRUE(registry_->Install(2, snapshot));
  int old = 0, fresh = 0;
  auto old_subscription = earlier->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++old; }));
  auto current = CreateManaged("x");
  ASSERT_TRUE(current);
  auto current_subscription = current->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++fresh; }));
  environment.RunUntilIdle();
  EXPECT_EQ(old, 1);
  EXPECT_EQ(fresh, 0);
  EXPECT_NE(earlier->BeginSend(GURL("https://cdn.test/start"), "GET", false).net_error, net::OK);
}
TEST_F(AccessRequestRouteStateTest, NativeRedirectAttachesRevocationOnlyAfterPrivateProxyApproval) {
  base::test::TaskEnvironment environment;
  registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  ASSERT_TRUE(registry_->Install(1, ManagedSnapshot()));
  const GURL first("https://native.test/start"), next("https://cdn.test/start");
  auto state = AccessRequestRouteState::Create(registry_->GetWeakPtr(), metadata_, first, "GET", expectation_);
  ASSERT_TRUE(state);
  int canceled = 0;
  auto subscription = state->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++canceled; }));
  EXPECT_FALSE(state->InvalidateCurrentListener());
  ASSERT_EQ(state->BeginSend(first, "GET", false).net_error, net::OK);
  ASSERT_TRUE(state->ObserveRedirect(next, "GET"));
  ASSERT_TRUE(state->ApproveRedirect(1, next, "GET", ManagedExpectation("x")));
  ASSERT_TRUE(state->InvalidateCurrentListener());
  environment.RunUntilIdle();
  EXPECT_EQ(canceled, 1);
  EXPECT_NE(state->BeginSend(next, "GET", false).net_error, net::OK);
}
TEST_F(AccessRequestRouteStateTest, RevokedListenerCannotApprovePendingRedirectToNative) {
  base::test::TaskEnvironment environment;
  registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  ASSERT_TRUE(registry_->Install(1, ManagedSnapshot()));
  auto state = CreateManaged("x");
  ASSERT_TRUE(state);
  auto subscription = state->ObserveRegistrationRevocation(base::BindLambdaForTesting([] {}));
  const GURL first("https://cdn.test/start"), next("https://native.test/next");
  ASSERT_EQ(state->BeginSend(first, "GET", false).net_error, net::OK);
  ASSERT_TRUE(state->ObserveRedirect(next, "GET"));
  ASSERT_TRUE(state->InvalidateCurrentListener());
  EXPECT_FALSE(state->ApproveRedirect(1, next, "GET", expectation_));
  EXPECT_NE(state->BeginSend(next, "GET", false).net_error, net::OK);
}
TEST_F(AccessRequestRouteStateTest, CallbackMayReleaseLastExternalStateWithoutLosingOtherObservers) {
  base::test::TaskEnvironment environment;
  registry_ = std::make_unique<AccessRouteRegistry>(owner_, "incarnation");
  ASSERT_TRUE(registry_->Install(1, ManagedSnapshot()));
  auto state = CreateManaged("x");
  ASSERT_TRUE(state);
  int callbacks = 0;
  auto first = state->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { state.reset(); ++callbacks; }));
  auto second = state->ObserveRegistrationRevocation(base::BindLambdaForTesting([&] { ++callbacks; }));
  ASSERT_TRUE(state->InvalidateCurrentListener());
  environment.RunUntilIdle();
  EXPECT_FALSE(state);
  EXPECT_EQ(callbacks, 2);
}
}  // namespace
}  // namespace network
