// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_network_context_transport.h"

#include <utility>
#include <vector>

#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "chrome/browser/aegis/access/access_private_context_client.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/schemeful_site.h"
#include "net/proxy_resolution/proxy_info.h"
#include "services/network/network_service_proxy_delegate.h"
#include "services/network/test/test_network_context.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_MAC)
#include "base/functional/bind.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_proxying_url_loader_factory.h"
#include "chrome/browser/aegis/access/access_service_coordinator.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#endif

namespace aegis::access {
namespace {

class HeldPrivateContext final : public network::TestNetworkContext,
                                 public network::mojom::AegisAccessContext {
 public:
  void BindAegisAccessContext(
      mojo::PendingReceiver<network::mojom::AegisAccessContext> receiver,
      BindAegisAccessContextCallback callback) override {
    receivers.Add(this, std::move(receiver));
#if BUILDFLAG(IS_MAC)
    if (hold_bind) {
      CHECK(!bind_reply);
      bind_reply = std::move(callback);
      return;
    }
#endif
    std::move(callback).Run(incarnation);
  }
  void InstallSnapshot(network::mojom::AegisAccessSnapshotPtr value,
                       InstallSnapshotCallback callback) override {
    ++install_count;
    snapshot = std::move(value);
    reply = std::move(callback);
  }
  void Acknowledge() {
    CHECK(reply);
    auto ack = network::mojom::AegisAccessSnapshotAck::New();
    ack->accepted = true;
    ack->owner = snapshot->owner.Clone();
    ack->incarnation = snapshot->incarnation;
    ack->sequence = snapshot->sequence;
    ack->common_generations = snapshot->common_generations.Clone();
    for (const auto& endpoint : snapshot->endpoints) {
      ack->registrations.push_back(
          network::mojom::AegisAccessRegistrationAck::New(
              endpoint->proxy_group_id, endpoint->registration_id,
              endpoint->selection_generation, endpoint->transport,
              endpoint->numeric_loopback_host, endpoint->port, endpoint->listener_binding.Clone()));
    }
    for (const auto& unavailable : snapshot->unavailable_groups)
      ack->unavailable_groups.push_back(unavailable.Clone());
    std::move(reply).Run(std::move(ack));
  }
  void BeginRestore(network::mojom::AegisAccessOwnerPtr, const std::string&,
                    BeginRestoreCallback callback) override {
    std::move(callback).Run(true);
  }
  void CreateFactory(
      mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory> receiver,
      network::mojom::URLLoaderFactoryParamsPtr,
      network::mojom::AegisAccessFactoryBindingPtr,
      CreateFactoryCallback callback) override {
#if BUILDFLAG(IS_MAC)
    factory_receivers.push_back(std::move(receiver));
    if (hold_factory) {
      CHECK(!factory_reply);
      factory_reply = std::move(callback);
      return;
    }
#endif
    std::move(callback).Run(true);
  }

#if BUILDFLAG(IS_MAC)
  mojo::PendingRemote<network::mojom::NetworkContext> BindNetworkContext() {
    return network_receiver.BindNewPipeAndPassRemote();
  }
  void CreateURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory>,
      network::mojom::URLLoaderFactoryParamsPtr) override {
    ++legacy_factory_count;
  }

  bool hold_bind = false;
  bool hold_factory = false;
  BindAegisAccessContextCallback bind_reply;
  CreateFactoryCallback factory_reply;
  std::vector<mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory>>
      factory_receivers;
  size_t legacy_factory_count = 0;
  mojo::Receiver<network::mojom::NetworkContext> network_receiver{this};
#endif
  std::string incarnation = "actual-context";
  size_t install_count = 0;
  network::mojom::AegisAccessSnapshotPtr snapshot;
  InstallSnapshotCallback reply;
  mojo::ReceiverSet<network::mojom::AegisAccessContext> receivers;
};

class AccessPrivatePublicationTest : public testing::Test {
 protected:
  using TimeSource = base::test::TaskEnvironment::TimeSource;
  explicit AccessPrivatePublicationTest(TimeSource time_source = TimeSource::SYSTEM_TIME)
      : environment_(time_source) {}

  void SetUp() override {
    profile_ = TestingProfile::Builder().Build();
    ASSERT_NO_FATAL_FAILURE(ConfigureTransport(aegis_access::ChannelNamespace::kDev));
  }
  void ConfigureTransport(aegis_access::ChannelNamespace channel) {
    network::mojom::NetworkContextParams params;
    ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(
        profile_.get(), {}, &params));
    transport_ = AccessNetworkContextTransport::Get(profile_.get());
    ASSERT_TRUE(transport_);
    const auto owner = transport_->OwnerForPartition(channel, {});
    ASSERT_TRUE(owner);
    owner_ = *owner;
  }
  AccessNetworkContextTransport::PrivateContext Bind(HeldPrivateContext& context) {
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context, bound.GetCallback());
    return bound.Take();
  }
  aegis_access::PublishedAccessPolicySnapshot Policy(uint64_t generation = 1) {
    return {owner_, generation, {}};
  }
  aegis_access::GenerationTuple Common(uint64_t generation = 1) {
    return {generation, 2, 0, transport_->network_epoch(), 5};
  }
  void AddProxyRule(aegis_access::PublishedAccessPolicySnapshot& policy,
                    const std::string& group) {
    policy.rules.push_back({.rule_id = group,
                            .owner = owner_,
                            .scope = aegis_access::PolicyScope::kProfile,
                            .destination_host = group + ".example",
                            .schemes = {aegis_access::RequestScheme::kHttps},
                            .ports = {aegis_access::PortScope::kAllBrowserPermitted, {}},
                            .mode = aegis_access::AccessMode::kProxy,
                            .proxy_group_id = group,
                            .protection_override = aegis_access::ProtectionOverride::kNone,
                            .row_revision = 1,
                            .last_operation_sequence = 1});
  }
  std::unique_ptr<network::NetworkServiceProxyDelegate> CreateLegacyDelegate() {
    network::mojom::NetworkContextParams params;
    EXPECT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(
        profile_.get(), {}, &params));
    return std::make_unique<network::NetworkServiceProxyDelegate>(
        std::move(params.initial_custom_proxy_config),
        std::move(params.custom_proxy_config_client_receiver),
        mojo::PendingRemote<network::mojom::CustomProxyConnectionObserver>());
  }
  aegis_access::RegisteredProxyEndpoint Endpoint() {
    return {"private-registration", "private-group", owner_,
            {1, 2, 3, transport_->network_epoch(), 5},
            aegis_access::RegisteredProxyTransport::kHttp, "127.0.0.1", 18080};
  }
  net::ProxyInfo ResolveFromOtherWebsite(
      network::NetworkServiceProxyDelegate& delegate) {
    net::ProxyInfo result;
    result.UseNamedProxy("http://native.example:3128;direct://");
    delegate.OnResolveProxy(GURL("https://anchor.example/resource"),
        net::NetworkAnonymizationKey::CreateSameSite(
            net::SchemefulSite(GURL("https://other.example"))),
        "GET", {}, &result);
    return result;
  }
  void ExpectOriginalConnection(network::NetworkServiceProxyDelegate& delegate) {
    const auto result = ResolveFromOtherWebsite(delegate);
    EXPECT_EQ(result.proxy_chain().First().GetHost(), "native.example");
    EXPECT_EQ(result.proxy_chain().First().GetPort(), 3128);
  }

  content::BrowserTaskEnvironment environment_;
  std::unique_ptr<TestingProfile> profile_;
  raw_ptr<AccessNetworkContextTransport> transport_ = nullptr;
  aegis_access::OwnershipKey owner_;
};

TEST_F(AccessPrivatePublicationTest, PrivateBindingClearsLegacyGlobalSelection) {
  auto legacy = CreateLegacyDelegate();
  ASSERT_TRUE(transport_->PublishProxySelection({}, {"anchor.example"}, Endpoint()));
  transport_->FlushClientsForTesting({});
  EXPECT_EQ(ResolveFromOtherWebsite(*legacy).proxy_chain().First().GetHost(),
            "127.0.0.1");
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  transport_->FlushClientsForTesting({});
  ExpectOriginalConnection(*legacy);
}

TEST_F(AccessPrivatePublicationTest,
       RecreatedPrivateContextAndOffDoNotInstallGlobalHostProxy) {
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  auto before = transport_->CurrentSelection(owner_);
  ASSERT_TRUE(before);
  ASSERT_TRUE(transport_->ReplaceSelection(owner_, *before,
      {Endpoint(), {"anchor.example"}}));
  auto recreated = CreateLegacyDelegate();
  ExpectOriginalConnection(*recreated);
  const auto enabled = transport_->CurrentSelection(owner_);
  ASSERT_TRUE(enabled);
  ASSERT_TRUE(transport_->ReplaceSelection(owner_, *enabled, {}));
  transport_->FlushClientsForTesting({});
  ExpectOriginalConnection(*recreated);
}

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
TEST_F(AccessPrivatePublicationTest, AccountSitesCoexistWithPrivateLocalSelection) {
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  const auto before = transport_->CurrentSelection(owner_);
  ASSERT_TRUE(before);
  const AccessTransportSelection local{Endpoint(), {"anchor.example"}};
  ASSERT_TRUE(transport_->ReplaceSelection(owner_, *before, local));
  ASSERT_TRUE(transport_->SetAccountNodeSites({"paid.example"}, "account.example", 443, "Basic test"));
  auto delegate = CreateLegacyDelegate();
  ExpectOriginalConnection(*delegate);
  net::ProxyInfo paid;
  paid.UseDirect();
  delegate->OnResolveProxy(GURL("https://paid.example./"), {}, "GET", {}, &paid);
  ASSERT_TRUE(paid.proxy_chain().is_aegis_account_node());
  ASSERT_TRUE(transport_->SetAccountNodeSites({}, "", 0, ""));
  EXPECT_EQ(transport_->CurrentSelection(owner_), local);
}
#endif

TEST_F(AccessPrivatePublicationTest, ChangedCommonTupleRequiresFreshPrivateAck) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  HeldPrivateContext context;
  auto client = Bind(context);
  ASSERT_TRUE(client);
  const auto policy = Policy();
  const auto old_common = Common();
  base::test::TestFuture<bool> first;
  transport_->PublishPrivatePolicy(policy, old_common, first.GetCallback());
  environment_.RunUntilIdle();
  context.Acknowledge();
  ASSERT_TRUE(first.Get());
  EXPECT_TRUE(transport_->HasCurrentPrivatePolicy(owner_, old_common));
  net::NetworkChangeNotifier::NotifyObserversOfNetworkChangeForTests(
      net::NetworkChangeNotifier::CONNECTION_WIFI);
  environment_.RunUntilIdle();
  auto new_common = old_common;
  new_common.network_epoch = transport_->network_epoch();
  EXPECT_NE(old_common.network_epoch, new_common.network_epoch);
  EXPECT_FALSE(transport_->HasCurrentPrivatePolicy(owner_, new_common));
  base::test::TestFuture<bool> changed;
  transport_->PublishPrivatePolicy(policy, new_common, changed.GetCallback());
  environment_.RunUntilIdle();
  EXPECT_FALSE(changed.IsReady());
  EXPECT_FALSE(transport_->HasCurrentPrivatePolicy(owner_, new_common));
  ASSERT_TRUE(context.snapshot);
  EXPECT_EQ(context.snapshot->common_generations->network_epoch,
            new_common.network_epoch);
  context.Acknowledge();
  ASSERT_TRUE(changed.Get());
  EXPECT_TRUE(transport_->HasCurrentPrivatePolicy(owner_, new_common));
  EXPECT_FALSE(transport_->HasCurrentPrivatePolicy(owner_, old_common));
}

TEST_F(AccessPrivatePublicationTest, FactoriesShareOneContextAndExactPublication) {
  HeldPrivateContext context;
  auto first = Bind(context);
  auto second = Bind(context);
  ASSERT_TRUE(first);
  EXPECT_EQ(first, second);
  base::test::TestFuture<bool> one;
  base::test::TestFuture<bool> two;
  transport_->PublishPrivatePolicy(Policy(), Common(), one.GetCallback());
  transport_->PublishPrivatePolicy(Policy(), Common(), two.GetCallback());
  environment_.RunUntilIdle();
  EXPECT_EQ(context.install_count, 1u);
  EXPECT_FALSE(one.IsReady());
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  context.Acknowledge();
  EXPECT_TRUE(one.Get());
  EXPECT_TRUE(two.Get());
  EXPECT_TRUE(transport_->HasReadyPrivateContext(owner_));
}

TEST_F(AccessPrivatePublicationTest, WholeSnapshotIncludesReadyAndUnavailableGroups) {
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  auto policy = Policy();
  AddProxyRule(policy, "ready");
  AddProxyRule(policy, "cold");
  auto generations = Common();
  generations.selection_generation = 3;
  ASSERT_TRUE(transport_->RegisterPreparedProxy({
      {"real-registration", "ready", owner_, generations,
       aegis_access::RegisteredProxyTransport::kHttp, "127.0.0.1", 18080},
      net::AuthCredentials(u"local", u"private") }));
  base::test::TestFuture<bool> published;
  transport_->PublishPrivatePolicy(policy, Common(), published.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(context.snapshot);
  ASSERT_EQ(context.snapshot->endpoints.size(), 1u);
  EXPECT_EQ(context.snapshot->endpoints[0]->registration_id, "real-registration");
  ASSERT_EQ(context.snapshot->unavailable_groups.size(), 1u);
  EXPECT_EQ(context.snapshot->unavailable_groups[0]->proxy_group_id, "cold");
  EXPECT_EQ(context.snapshot->unavailable_groups[0]->selection_generation, 0u);
  EXPECT_EQ(context.snapshot->rules.size(), 2u);
  context.Acknowledge();
  EXPECT_TRUE(published.Get());
}

TEST_F(AccessPrivatePublicationTest, SupersededAckCannotReleaseNewCandidate) {
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  base::test::TestFuture<bool> previous;
  base::test::TestFuture<bool> current;
  transport_->PublishPrivatePolicy(Policy(), Common(), previous.GetCallback());
  environment_.RunUntilIdle();
  transport_->PublishPrivatePolicy(Policy(2), Common(2), current.GetCallback());
  EXPECT_FALSE(previous.Get());
  context.Acknowledge();
  environment_.RunUntilIdle();
  EXPECT_FALSE(current.IsReady());
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  ASSERT_EQ(context.install_count, 2u);
  EXPECT_EQ(context.snapshot->sequence, 2u);
  EXPECT_EQ(context.snapshot->common_generations->policy_generation, 2u);
  context.Acknowledge();
  EXPECT_TRUE(current.Get());
}

#if BUILDFLAG(IS_MAC)
class AccessPrivatePublicationDeadlineTest : public AccessPrivatePublicationTest {
 protected:
  AccessPrivatePublicationDeadlineTest() : AccessPrivatePublicationTest(TimeSource::MOCK_TIME) {}
};

TEST_F(AccessPrivatePublicationDeadlineTest, ExpiredAckOfUnchangedPolicyRequiresFreshSequence) {
  HeldPrivateContext context;
  ASSERT_TRUE(Bind(context));
  base::test::TestFuture<bool> first, later;
  transport_->PublishPrivatePolicy(Policy(), Common(), first.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(context.reply);
  environment_.FastForwardBy(base::Seconds(30));
  ASSERT_TRUE(first.IsReady());
  EXPECT_FALSE(first.Get());

  transport_->PublishPrivatePolicy(Policy(), Common(), later.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(later.IsReady());
  EXPECT_FALSE(later.Get());
  EXPECT_EQ(context.install_count, 1u);
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));

  context.Acknowledge();
  environment_.RunUntilIdle();
  EXPECT_EQ(context.install_count, 2u);
  ASSERT_TRUE(context.reply);
  EXPECT_EQ(context.snapshot->sequence, 2u);
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  context.Acknowledge();
  environment_.RunUntilIdle();
  EXPECT_TRUE(transport_->HasCurrentPrivatePolicy(owner_, Common()));
}

TEST_F(AccessPrivatePublicationDeadlineTest, BindReplyAtDeadlineBeforeTimerIsRejected) {
  HeldPrivateContext context;
  context.hold_bind = true;
  base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
  transport_->BindPrivateContext(owner_, &context, bound.GetCallback());
  ASSERT_TRUE(context.bind_reply);
  environment_.AdvanceClock(base::Seconds(30));
  ASSERT_FALSE(bound.IsReady());
  std::move(context.bind_reply).Run(context.incarnation);
  ASSERT_TRUE(bound.IsReady());
  EXPECT_FALSE(bound.Get());
  environment_.RunUntilIdle();
  EXPECT_TRUE(context.receivers.empty());
}

class AccessPrivateFactoryDeadlineTest : public AccessPrivatePublicationDeadlineTest {
 protected:
  void SetUp() override {
    // The private terminal captures the base proxy configuration generation.
    profile_ = TestingProfile::Builder()
        .AddTestingFactory(ProfileNetworkContextServiceFactory::GetInstance(),
            base::BindRepeating([](content::BrowserContext* context)
                -> std::unique_ptr<KeyedService> {
              return std::make_unique<ProfileNetworkContextService>(
                  Profile::FromBrowserContext(context));
            }))
        .Build();
    ASSERT_TRUE(ProfileNetworkContextServiceFactory::GetForContext(profile_.get()));
    profile_->GetPrefs()->SetDict(proxy_config::prefs::kProxy,
                                  ProxyConfigDictionary::CreateDirect());
    environment_.RunUntilIdle();
    ASSERT_NO_FATAL_FAILURE(ConfigureTransport(AccessChannelForCurrentBuild()));
    auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_.get());
    ASSERT_TRUE(coordinator);
    base::test::TestFuture<bool> restored;
    coordinator->InitializeForProfile(owner_, restored.GetCallback());
    environment_.RunUntilIdle();
    ASSERT_TRUE(restored.IsReady());
    ASSERT_TRUE(restored.Get());
    network_.Bind(context_.BindNetworkContext());
  }

  void TearDown() override {
    // Release Browser factories and clients while the fake context is alive.
    transport_ = nullptr;
    profile_.reset();
    AccessPrivatePublicationDeadlineTest::TearDown();
  }

  mojo::Remote<network::mojom::URLLoaderFactory> StartBrowserFactory() {
    network::URLLoaderFactoryBuilder builder;
    AccessProxyingURLLoaderFactory::BindPrivateFactoryForTesting(
        profile_.get(), owner_, builder);
    EXPECT_TRUE(builder.RequiresFreshFactory());
    auto params = network::mojom::URLLoaderFactoryParams::New();
    params->process_id = network::OriginatingProcessId::browser();
    return mojo::Remote<network::mojom::URLLoaderFactory>(
        std::move(builder).Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
            network_.get(), std::move(params)));
  }

  HeldPrivateContext context_;
  mojo::Remote<network::mojom::NetworkContext> network_;
};

TEST_F(AccessPrivateFactoryDeadlineTest, HeldBindClosesFactoryWithoutRetiringStalledContext) {
  auto installing = StartBrowserFactory();
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.reply);
  ASSERT_EQ(context_.receivers.size(), 1u);
  ASSERT_TRUE(installing.is_connected());

  context_.hold_bind = true;
  auto waiting = StartBrowserFactory();
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.bind_reply);
  ASSERT_EQ(context_.receivers.size(), 2u);
  ASSERT_EQ(context_.install_count, 1u);
  EXPECT_TRUE(waiting.is_connected());
  environment_.FastForwardBy(base::Seconds(29));
  EXPECT_TRUE(waiting.is_connected());
  environment_.FastForwardBy(base::Seconds(1));
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_FALSE(installing.is_connected());
  EXPECT_TRUE(network_.is_connected());
  EXPECT_EQ(context_.receivers.size(), 1u);
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));

  std::move(context_.bind_reply).Run(context_.incarnation);
  environment_.RunUntilIdle();
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_EQ(context_.install_count, 1u);
  EXPECT_TRUE(context_.factory_receivers.empty());
  EXPECT_EQ(context_.legacy_factory_count, 0u);

  context_.Acknowledge();
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.reply);
  ASSERT_EQ(context_.install_count, 2u);
  EXPECT_EQ(context_.snapshot->incarnation, context_.incarnation);
  EXPECT_EQ(context_.snapshot->sequence, 2u);
  EXPECT_TRUE(context_.factory_receivers.empty());
  context_.Acknowledge();
  environment_.RunUntilIdle();
  ASSERT_TRUE(transport_->HasReadyPrivateContext(owner_));
  context_.hold_bind = false;
  auto rebound = StartBrowserFactory();
  environment_.RunUntilIdle();
  EXPECT_TRUE(rebound.is_connected());
  ASSERT_EQ(context_.factory_receivers.size(), 1u);
  EXPECT_EQ(context_.receivers.size(), 1u);
  EXPECT_EQ(context_.install_count, 2u);
  EXPECT_EQ(context_.legacy_factory_count, 0u);

  auto target_pipe = context_.factory_receivers.front().PassPipe();
  ASSERT_TRUE(target_pipe.is_valid());
  EXPECT_FALSE(target_pipe->QuerySignalsState().peer_closed());
  rebound.reset();
  environment_.RunUntilIdle();
  EXPECT_TRUE(target_pipe->QuerySignalsState().peer_closed());
  EXPECT_TRUE(transport_->HasReadyPrivateContext(owner_));
}

TEST_F(AccessPrivateFactoryDeadlineTest, PrivatePipeClosureBeforeBindReplyClosesFactory) {
  context_.hold_bind = true;
  base::test::TestFuture<void> disconnected;
  auto waiting = StartBrowserFactory();
  waiting.set_disconnect_handler(disconnected.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.bind_reply);
  ASSERT_TRUE(waiting.is_connected());

  context_.receivers.Clear();
  environment_.RunUntilIdle();
  EXPECT_TRUE(disconnected.IsReady());
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_TRUE(network_.is_connected());
  std::move(context_.bind_reply).Run(context_.incarnation);
  environment_.RunUntilIdle();
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_EQ(context_.install_count, 0u);
  EXPECT_TRUE(context_.factory_receivers.empty());
  EXPECT_EQ(context_.legacy_factory_count, 0u);
}

TEST_F(AccessPrivateFactoryDeadlineTest, HeldCreateClosesFactoryAndFencesLateSuccess) {
  context_.hold_factory = true;
  auto waiting = StartBrowserFactory();
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.reply);
  context_.Acknowledge();
  environment_.RunUntilIdle();
  ASSERT_TRUE(context_.factory_reply);
  ASSERT_EQ(context_.factory_receivers.size(), 1u);
  ASSERT_TRUE(waiting.is_connected());
  auto target_pipe = context_.factory_receivers.front().PassPipe();
  ASSERT_TRUE(target_pipe.is_valid());
  EXPECT_FALSE(target_pipe->QuerySignalsState().peer_closed());

  environment_.FastForwardBy(base::Seconds(29));
  EXPECT_TRUE(waiting.is_connected());
  environment_.FastForwardBy(base::Seconds(1));
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_TRUE(transport_->HasReadyPrivateContext(owner_));
  EXPECT_TRUE(target_pipe->QuerySignalsState().peer_closed());
  std::move(context_.factory_reply).Run(true);
  environment_.RunUntilIdle();
  EXPECT_FALSE(waiting.is_connected());
  EXPECT_TRUE(target_pipe->QuerySignalsState().peer_closed());
  EXPECT_TRUE(network_.is_connected());
  EXPECT_EQ(context_.receivers.size(), 1u);
  EXPECT_TRUE(transport_->HasReadyPrivateContext(owner_));
  EXPECT_EQ(context_.legacy_factory_count, 0u);
}
#endif

TEST_F(AccessPrivatePublicationTest, LostContextRequiresFullNewIncarnationAck) {
  HeldPrivateContext old;
  ASSERT_TRUE(Bind(old));
  base::test::TestFuture<bool> previous;
  transport_->PublishPrivatePolicy(Policy(), Common(), previous.GetCallback());
  environment_.RunUntilIdle();
  old.receivers.Clear();
  EXPECT_FALSE(previous.Get());
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  HeldPrivateContext replacement;
  replacement.incarnation = "new-context";
  ASSERT_TRUE(Bind(replacement));
  base::test::TestFuture<bool> restored;
  transport_->PublishPrivatePolicy(Policy(), Common(), restored.GetCallback());
  environment_.RunUntilIdle();
  EXPECT_FALSE(restored.IsReady());
  EXPECT_EQ(replacement.snapshot->incarnation, "new-context");
  EXPECT_EQ(replacement.snapshot->sequence, 1u);
  replacement.Acknowledge();
  EXPECT_TRUE(restored.Get());
  EXPECT_TRUE(transport_->HasReadyPrivateContext(owner_));
}

}  // namespace
}  // namespace aegis::access
