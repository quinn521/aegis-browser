// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_site_proxy_bubble_controller.h"
#include "chrome/browser/aegis/access/access_async_rule_store.h"
#include "chrome/browser/aegis/access/access_browser_request_adapter.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_buildflags.h"
#include "chrome/browser/aegis/access/access_navigation_retry_tab_helper.h"
#include "chrome/browser/aegis/access/access_proxying_url_loader_factory.h"

#include <algorithm>
#include <string_view>

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/pending_task.h"
#include "base/task/current_thread.h"
#include "base/task/task_observer.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/scoped_command_line.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/test_future.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "net/base/net_errors.h"
#include "net/url_request/redirect_info.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_network_context.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#if BUILDFLAG(IS_MAC)
#include <map>

#include "base/auto_reset.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/test/task_environment.h"
#include "base/uuid.h"
#include "chrome/browser/aegis/access/access_options_service.h"
#include "components/aegis_access/access_test_node_test_fixture.h"
#endif

namespace aegis::access {
class AccessSiteProxyProductTestPeer {
 public:
  static void LoseRuntime(AccessSiteProxyBubbleController* bubble,
                          const std::string& registration) {
    bubble->local_core_.reset();
    bubble->OnLocalCoreStopped(registration);
  }
  static bool HasCore(AccessSiteProxyBubbleController* bubble) {
    return !!bubble->local_core_;
  }
  static bool IsPreparing(AccessSiteProxyBubbleController* bubble) {
    return bubble->publication_in_flight_;
  }
#if BUILDFLAG(IS_MAC)
  static void PublishFakeLegacyRoute(
      AccessSiteProxyBubbleController* bubble, const AccessSiteHost& host,
      const OwnershipKey& owner, const std::string& registration,
      base::OnceCallback<void(AccessMutationTransactionResult)> completion,
      base::OnceClosure before_verification = {}) {
    const AccessLocalCoreReady ready{
        .ready = true,
        .port = 23457,
        .registration_id = registration,
        .local_credentials = net::AuthCredentials(u"test-user", u"test-password"),
    };
    ASSERT_TRUE(bubble->RegisterPreparedCore(owner, ready));
    bubble->BeginEnablePublication(
        host, {owner, AccessSiteProxyBubbleController::LocalProxyGroup(owner)},
        base::BindOnce(
            [](base::WeakPtr<AccessSiteProxyBubbleController> bubble,
               AccessSiteHost host, AccessLocalCoreReady ready,
               base::OnceClosure before_verification,
               base::OnceCallback<void(AccessMutationTransactionResult)> done,
               AccessMutationTransactionResult result) {
              if (result.status == AccessMutationTransactionStatus::kCommitted) {
                bubble->AdoptWebsiteIntent(host, true);
                // Supply the probe result after the real mutation and Mojo ACK.
                bubble->verifying_host_ = host;
                const auto attempt = ++bubble->committed_verification_attempt_;
                if (before_verification) std::move(before_verification).Run();
                bubble->OnCommittedCoreVerified(host, attempt, std::move(ready));
              } else {
                bubble->OnPublicationComplete(host, true, result);
              }
              std::move(done).Run(result);
            }, bubble->weak_factory_.GetWeakPtr(), host, ready,
            std::move(before_verification), std::move(completion)));
  }

  static base::OneShotTimer& SavedTestProfilePublicationTimer(
      AccessSiteProxyBubbleController* bubble) {
    return bubble->saved_test_profile_publication_timer_;
  }
  static void NotifyTestProfileCoreStopped(AccessOptionsService* options,
                                           const std::string& registration) {
    options->OnTestProfileCoreStopped(registration);
  }
  static auto OverrideSavedTestProfilePublicationClock(
      AccessSiteProxyBubbleController* bubble, const base::TickClock* clock) {
    return base::AutoReset<raw_ptr<const base::TickClock>>(
        &bubble->saved_test_profile_publication_clock_, clock);
  }
#endif
  static void StopCore(AccessSiteProxyBubbleController* bubble,
                       base::OnceClosure completion) {
    bubble->local_core_->Stop(std::move(completion));
  }
};
class AccessServiceCoordinatorTestPeer {
 public:
  static base::OneShotTimer& PublicationTimer(AccessServiceCoordinator* coordinator) {
    return coordinator->publication_timeout_;
  }

  static void SetFailure(AccessServiceCoordinator* coordinator,
                        AccessRuleStore::FailurePointForTesting point) {
    CHECK(coordinator->async_store_);
    coordinator->async_store_->SetFailurePointForTesting(point);
  }
};
class AccessNetworkContextTransportTestPeer {
 public:
  static void AdvanceNetwork(AccessNetworkContextTransport* transport) {
    transport->OnNetworkChanged(net::NetworkChangeNotifier::CONNECTION_WIFI);
  }
#if BUILDFLAG(IS_MAC)
  static void ExpireInstall(AccessNetworkContextTransport* transport,
      const OwnershipKey& owner, const std::string& incarnation) {
    auto& entry = transport->partitions_.at(owner.storage_partition_token)
                      .private_contexts.at(incarnation);
    entry.install_deadline = base::TimeTicks::Now();
    entry.install_timer.FireNow();
  }
#endif
};
namespace {

// Act on either side of the task delivering the real transport's callback.
class RunAtQueuedPolicyCompletion final : public base::TaskObserver {
 public:
  enum class Phase { kBefore, kAfter };

  explicit RunAtQueuedPolicyCompletion(base::OnceClosure action,
                                       Phase phase = Phase::kBefore)
      : action_(std::move(action)), phase_(phase) {
    base::CurrentThread::Get()->AddTaskObserver(this);
  }
  ~RunAtQueuedPolicyCompletion() override {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
  }
  void WillProcessTask(const base::PendingTask& task, bool) override {
    if (phase_ == Phase::kBefore) MaybeRun(task);
  }
  void DidProcessTask(const base::PendingTask& task) override {
    if (phase_ == Phase::kAfter) MaybeRun(task);
  }
  bool ran() const { return !action_; }

 private:
  void MaybeRun(const base::PendingTask& task) {
    if (action_ && std::string_view(task.posted_from.function_name()).find(
                       "SettlePrivatePublication") != std::string_view::npos) {
      std::move(action_).Run();
    }
  }

  base::OnceClosure action_;
  const Phase phase_;
};

// Act after the enable transaction's UI task commits ON and starts its
// asynchronous verification, before another UI task can finish that recheck.
class DisableDuringCommittedVerification final : public base::TaskObserver {
 public:
  DisableDuringCommittedVerification(base::RepeatingCallback<bool()> committed,
                                      base::OnceClosure disable)
      : committed_(std::move(committed)), disable_(std::move(disable)) {
    base::CurrentThread::Get()->AddTaskObserver(this);
  }
  ~DisableDuringCommittedVerification() override {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
  }
  void WillProcessTask(const base::PendingTask&, bool) override {}
  void DidProcessTask(const base::PendingTask&) override {
    if (disable_ && committed_.Run()) {
      disabled_ = true;
      std::move(disable_).Run();
    }
  }
  bool disabled() const { return disabled_; }

 private:
  base::RepeatingCallback<bool()> committed_;
  base::OnceClosure disable_;
  bool disabled_ = false;
};

class CountingPrivateFactory final
    : public network::mojom::AegisAccessURLLoaderFactory {
 public:
  mojo::PendingRemote<network::mojom::AegisAccessURLLoaderFactory> Bind() {
    mojo::PendingRemote<network::mojom::AegisAccessURLLoaderFactory> remote;
    Clone(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void Clone(mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory>
                 receiver) override {
    receivers_.Add(this, std::move(receiver));
  }
  void CreateLoaderAndStartWithRoute(
      mojo::PendingReceiver<network::mojom::URLLoader>,
      mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderControl>,
      int32_t, uint32_t, network::mojom::AegisAccessRoutedRequestPtr request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag&) override {
    ++starts;
    method = request->request.method;
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  }
  int starts = 0;
  std::string method;

 private:
  mojo::ReceiverSet<network::mojom::AegisAccessURLLoaderFactory> receivers_;
};

// Real public/private Mojo pipes through the production Browser wrapper. The
// controlled target counts attempted dispatch, without sending business bytes.
struct HeldBrowserRequest {
  HeldBrowserRequest(Profile* profile, content::FrameTreeNodeId frame,
      const OwnershipKey& owner,
      std::shared_ptr<AccessPrivateContextClient> context) {
    wrapper = std::make_unique<AccessProxyingURLLoaderFactory>(
        profile, frame, std::nullopt, std::nullopt, nullptr, owner, std::nullopt,
        factory.BindNewPipeAndPassReceiver(),
        mojo::PendingRemote<network::mojom::URLLoaderFactory>(), std::nullopt,
        base::BindOnce([](HeldBrowserRequest* self,
                          AccessProxyingURLLoaderFactory*) {
          self->wrapper.reset();
        }, base::Unretained(this)), target.Bind(), std::move(context));
  }
  void StartPost() {
    network::ResourceRequest request;
    request.url = GURL("https://resource.example/late-post?q=original");
    request.method = "POST";
    mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote;
    client_receiver.Bind(client_remote.InitWithNewPipeAndPassReceiver());
    factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(), 1, 0,
        request, std::move(client_remote),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
  }
  CountingPrivateFactory target;
  mojo::Remote<network::mojom::URLLoaderFactory> factory;
  network::TestURLLoaderClient client;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver{&client};
  mojo::Remote<network::mojom::URLLoader> loader;
  std::unique_ptr<AccessProxyingURLLoaderFactory> wrapper;
};

// Factory admission can close while an already-created request is waiting at
// a redirect. Its loader/client/private-control pipes remain independently live.
class RedirectingPrivateFactory final
    : public network::mojom::AegisAccessURLLoaderFactory,
      public network::mojom::URLLoader,
      public network::mojom::AegisAccessURLLoaderControl {
 public:
  mojo::PendingRemote<network::mojom::AegisAccessURLLoaderFactory> Bind() {
    mojo::PendingRemote<network::mojom::AegisAccessURLLoaderFactory> remote;
    Clone(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void Clone(mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory>
                 receiver) override {
    factories_.Add(this, std::move(receiver));
  }
  void CreateLoaderAndStartWithRoute(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderControl> control,
      int32_t, uint32_t, network::mojom::AegisAccessRoutedRequestPtr request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag&) override {
    initial = std::move(request->envelope);
    loader_.Bind(std::move(loader));
    control_.Bind(std::move(control));
    client_.Bind(std::move(client));
    net::RedirectInfo redirect;
    redirect.status_code = 302;
    redirect.new_method = "GET";
    redirect.new_url = GURL("https://cdn.example/redirect-final");
    client_->OnReceiveRedirect(redirect, network::mojom::URLResponseHead::New());
  }
  void ResetAdmission() { factories_.Clear(); }
  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {
    ++public_follows;
  }
  void FollowRedirectWithRoute(
      network::HttpRequestHeadersUpdateParams, const std::optional<GURL>&,
      network::mojom::AegisAccessRequestEnvelopePtr envelope) override {
    followed = std::move(envelope);
    client_->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  }
  void SetPriority(net::RequestPriority, int32_t) override {}

  network::mojom::AegisAccessRequestEnvelopePtr initial;
  network::mojom::AegisAccessRequestEnvelopePtr followed;
  int public_follows = 0;

 private:
  mojo::ReceiverSet<network::mojom::AegisAccessURLLoaderFactory> factories_;
  mojo::Receiver<network::mojom::URLLoader> loader_{this};
  mojo::Receiver<network::mojom::AegisAccessURLLoaderControl> control_{this};
  mojo::Remote<network::mojom::URLLoaderClient> client_;
};

struct HeldRedirectBrowserRequest {
  HeldRedirectBrowserRequest(Profile* profile, content::RenderFrameHost* frame,
      const OwnershipKey& owner,
      aegis_access::BrowserOwnedRequestMetadata metadata,
      std::shared_ptr<AccessPrivateContextClient> context,
      std::optional<int64_t> navigation_id = std::nullopt) {
    std::optional<content::WeakDocumentPtr> document;
    if (!navigation_id) document = frame->GetWeakDocumentPtr();
    wrapper = std::make_unique<AccessProxyingURLLoaderFactory>(
        profile, frame->GetFrameTreeNodeId(), navigation_id, std::nullopt,
        nullptr, owner, std::move(metadata),
        factory.BindNewPipeAndPassReceiver(),
        mojo::PendingRemote<network::mojom::URLLoaderFactory>(),
        std::move(document),
        base::BindOnce([](HeldRedirectBrowserRequest* self,
                          AccessProxyingURLLoaderFactory*) {
          self->wrapper.reset();
        }, base::Unretained(this)), target.Bind(), std::move(context));
  }
  void Start() {
    network::ResourceRequest request;
    request.url = GURL("https://news.example/redirect");
    factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(), 1, 0,
        request, client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
  }
  RedirectingPrivateFactory target;
  mojo::Remote<network::mojom::URLLoaderFactory> factory;
  network::TestURLLoaderClient client;
  mojo::Remote<network::mojom::URLLoader> loader;
  std::unique_ptr<AccessProxyingURLLoaderFactory> wrapper;
};

// Real Mojo ACK with an explicit hold at the product publication boundary.
class ProductAckContext final : public network::TestNetworkContext,
                                public network::mojom::AegisAccessContext {
 public:
  void BindAegisAccessContext(
      mojo::PendingReceiver<network::mojom::AegisAccessContext> receiver,
      BindAegisAccessContextCallback callback) override {
    receivers_.Add(this, std::move(receiver));
    std::move(callback).Run(incarnation);
  }
  void InstallSnapshot(network::mojom::AegisAccessSnapshotPtr snapshot,
                       InstallSnapshotCallback callback) override {
    auto ack = network::mojom::AegisAccessSnapshotAck::New();
    ack->accepted = true;
    ack->owner = snapshot->owner.Clone();
    ack->incarnation = snapshot->incarnation;
    ack->sequence = snapshot->sequence;
    ack->common_generations = snapshot->common_generations.Clone();
    for (const auto& endpoint : snapshot->endpoints)
      ack->registrations.push_back(
          network::mojom::AegisAccessRegistrationAck::New(
              endpoint->proxy_group_id, endpoint->registration_id,
              endpoint->selection_generation, endpoint->transport,
              endpoint->numeric_loopback_host, endpoint->port, endpoint->listener_binding.Clone()));
    for (const auto& group : snapshot->unavailable_groups)
      ack->unavailable_groups.push_back(group.Clone());
    if (hold_next ||
        (!hold_next_registration.empty() &&
         std::ranges::any_of(ack->registrations, [&](const auto& entry) {
           return entry->registration_id == hold_next_registration;
         }))) {
      hold_next = false;
      hold_next_registration.clear();
      held_ack_ = std::move(ack);
      held_reply_ = std::move(callback);
    } else {
      std::move(callback).Run(std::move(ack));
    }
  }
  bool has_reply() const { return !!held_reply_; }
  const network::mojom::AegisAccessSnapshotAck& held_ack() const {
    return *held_ack_;
  }
  bool HoldsRegistration(std::string_view registration_id) const {
    return held_ack_ &&
           std::ranges::any_of(held_ack_->registrations, [&](const auto& entry) {
             return entry->registration_id == registration_id;
           });
  }
  void Reply(bool accepted) {
    CHECK(held_reply_);
    held_ack_->accepted = accepted;
    std::move(held_reply_).Run(std::move(held_ack_));
  }
  void BeginRestore(network::mojom::AegisAccessOwnerPtr, const std::string&,
                    BeginRestoreCallback callback) override {
    std::move(callback).Run(true);
  }
  void CreateFactory(
      mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory>,
      network::mojom::URLLoaderFactoryParamsPtr,
      network::mojom::AegisAccessFactoryBindingPtr,
      CreateFactoryCallback callback) override {
    std::move(callback).Run(true);
  }
  bool hold_next = false;
  std::string hold_next_registration;
  std::string incarnation = "product-test-context";
  void Disconnect() { receivers_.Clear(); }

 private:
  network::mojom::AegisAccessSnapshotAckPtr held_ack_;
  InstallSnapshotCallback held_reply_;
  mojo::ReceiverSet<network::mojom::AegisAccessContext> receivers_;
};

class AccessSiteProxyProductTest : public testing::Test {
 protected:
  void SetUp() override {
#if !BUILDFLAG(AEGIS_PRIVATE_TEST_NODE_ENABLED)
    const auto* command = base::CommandLine::ForCurrentProcess();
    if (command->GetSwitchValuePath("aegis-access-dev-core-manifest").empty() ||
        command->GetSwitchValuePath("aegis-access-dev-node-config").empty()) {
      GTEST_SKIP() << "Real product core tests require authorized DEV inputs.";
    }
#endif
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    ASSERT_NO_FATAL_FAILURE(OpenProfile());
  }

  void OpenProfile() {
    profile_ = TestingProfile::Builder()
        .SetPath(directory_.GetPath())
        .AddTestingFactory(ProfileNetworkContextServiceFactory::GetInstance(),
            base::BindRepeating([](content::BrowserContext* context)
                -> std::unique_ptr<KeyedService> {
              return std::make_unique<ProfileNetworkContextService>(
                  Profile::FromBrowserContext(context));
            }))
        .Build();
    ASSERT_TRUE(ProfileNetworkContextServiceFactory::GetForContext(profile_.get()));
    // TestingProfile disables this service by default. Publish an actual
    // native DIRECT preference change so its monitor owns the C generation.
    profile_->GetPrefs()->SetDict(proxy_config::prefs::kProxy,
                                  ProxyConfigDictionary::CreateDirect());
    environment_.RunUntilIdle();
    network::mojom::NetworkContextParams params;
    ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(
        profile_.get(), {}, &params));
    transport_ = AccessNetworkContextTransport::Get(profile_.get());
    ASSERT_TRUE(transport_);
    const auto owner =
        transport_->OwnerForPartition(AccessChannelForCurrentBuild(), {});
    ASSERT_TRUE(owner);
    owner_ = *owner;
    coordinator_ = AccessServiceCoordinator::GetOrCreate(profile_.get());
    ASSERT_TRUE(coordinator_);
    base::test::TestFuture<bool> restored;
    coordinator_->InitializeForProfile(owner_, restored.GetCallback());
    ASSERT_TRUE(restored.Wait()) << "Profile store initialization did not complete";
    ASSERT_TRUE(restored.Get());
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    ASSERT_TRUE(bound.Wait()) << "Private NetworkContext bind did not complete";
    ASSERT_TRUE(bound.Get());
    auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
    ASSERT_TRUE(runtime);
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    ASSERT_TRUE(common);
    const auto* policy = runtime->GetPublishedPolicySnapshot(owner_);
    ASSERT_TRUE(policy);
    base::test::TestFuture<bool> published;
    transport_->PublishPrivatePolicy(*policy, *common, published.GetCallback());
    ASSERT_TRUE(published.Wait()) << "Initial private policy ACK did not arrive";
    ASSERT_TRUE(published.Get());
    bubble_ = AccessSiteProxyBubbleController::GetOrCreate(profile_.get());
    ASSERT_TRUE(bubble_);
  }
  AccessSiteProxyBubbleSnapshot Snapshot(const char* host = "news.example") {
    return bubble_->SnapshotForCommittedUrl(GURL(std::string("https://") + host));
  }
  void WaitFor(AccessSiteConnectionState state, const char* host = "news.example") {
    ASSERT_TRUE(base::test::RunUntil([&] { return Snapshot(host).connection == state; }));
  }
  void Enable() {
    context_.hold_next = true;
    ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kConnecting);
    context_.Reply(true);
    WaitFor(AccessSiteConnectionState::kReady);
  }

  content::BrowserTaskEnvironment environment_;
  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(90)};
  base::ScopedTempDir directory_;
  ProductAckContext context_;
  std::unique_ptr<TestingProfile> profile_;
  raw_ptr<AccessNetworkContextTransport> transport_ = nullptr;
  raw_ptr<AccessServiceCoordinator> coordinator_ = nullptr;
  raw_ptr<AccessSiteProxyBubbleController> bubble_ = nullptr;
  OwnershipKey owner_;
};

#if BUILDFLAG(IS_MAC)
namespace {
class SavedProfileTestVault final : public AccessTestNodeVaultMac::Backend {
 public:
  using Rows = std::map<std::pair<std::string, std::string>, std::string>;
  explicit SavedProfileTestVault(std::shared_ptr<Rows> rows)
      : rows_(std::move(rows)) {}

  AccessTestNodeVaultStatus Add(std::string_view service, std::string_view account,
                               std::string_view secret) override {
    return rows_->emplace(Key(service, account), secret).second
        ? AccessTestNodeVaultStatus::kOk : AccessTestNodeVaultStatus::kUnavailable;
  }
  std::optional<std::string> Read(std::string_view service,
                                  std::string_view account) override {
    const auto found = rows_->find(Key(service, account));
    return found == rows_->end() ? std::nullopt : std::make_optional(found->second);
  }
  AccessTestNodeVaultStatus Delete(std::string_view service,
                                   std::string_view account) override {
    rows_->erase(Key(service, account));
    return AccessTestNodeVaultStatus::kOk;
  }
  std::optional<std::vector<std::string>> ListAccounts(
      std::string_view service) override {
    std::vector<std::string> accounts;
    for (const auto& [key, secret] : *rows_) {
      if (key.first == service) accounts.push_back(key.second);
    }
    return accounts;
  }

 private:
  static std::pair<std::string, std::string> Key(
      std::string_view service, std::string_view account) {
    return {std::string(service), std::string(account)};
  }
  std::shared_ptr<Rows> rows_;
};
}  // namespace

constexpr char kUnconfirmedTestProfileSwitchPolicy[] =
    "The browser could not confirm the network policy for the test Profile "
    "switch.";

class AccessSavedTestProfileProductTest : public AccessSiteProxyProductTest {
 protected:
  void SetUp() override {
    if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate)) {
      GTEST_SKIP() << "Requires an admitted internal build.";
    }
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    ASSERT_NO_FATAL_FAILURE(OpenProfile());
    auto* options = InstallOptions();
    ASSERT_TRUE(options);
    const auto editor = options->BeginTestNodeEditor();
    ASSERT_TRUE(editor);
    base::test::TestFuture<TestNodePreviewResult> preview;
    options->PreviewTestNodeText(*editor, aegis_access::test::kSyntheticTestNodeShare,
                                 preview.GetCallback());
    ASSERT_TRUE(preview.Wait()) << "Test node preview did not complete";
    ASSERT_EQ(preview.Get().status, TestNodePreviewStatus::kPreview);
    aegis_access::AccessInboundPreferences inbound;
    inbound.protocol = aegis_access::AccessInboundProtocolPreference::kSocks5;
    base::test::TestFuture<AccessTestProfileSaveResult> saved;
    options->SaveTestNodeProfile(*editor, 0, base::Uuid::GenerateRandomV4().AsLowercaseString(),
                                 "Synthetic", inbound, saved.GetCallback());
    ASSERT_TRUE(saved.Wait()) << "Test Profile save did not complete";
    ASSERT_EQ(saved.Get().outcome, AccessOptionsCommitOutcome::kCommitted);
    ASSERT_TRUE(saved.Get().snapshot && saved.Get().snapshot->test_profile);
    options_before_ = *saved.Get().snapshot;
    options->EndTestNodeEditor(*editor);

    const auto& saved_profile = *options_before_.test_profile;
    group_ = "internal-test-profile:" + saved_profile.configuration.id + ":" +
             base::NumberToString(saved_profile.version);
    ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
    ASSERT_NO_FATAL_FAILURE(PublishInitialTestProfile(options));
    previous_registration_ = options->test_profile_runtime().registration_id;
    const auto committed =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_TRUE(committed.value && committed.value->site_groups.size() == 1u);

    base::test::TestFuture<bool> revoked;
    ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(options_before_.revision,
                                                         revoked.GetCallback()));
    ASSERT_TRUE(revoked.Wait()) << SavedProfileState(options);
    ASSERT_TRUE(revoked.Get()) << SavedProfileState(options);

    bubble_ = nullptr;
    transport_ = nullptr;
    coordinator_ = nullptr;
    profile_.reset();
    environment_.RunUntilIdle();
    ASSERT_NO_FATAL_FAILURE(OpenProfile());
    options = InstallOptions();
    ASSERT_TRUE(options);
    bubble_->EnsureProfileRestored(owner_);
    const auto restored =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    // The reopened store rebinds durable policy rows to the new runtime owner.
    ASSERT_TRUE(owner_ != committed.value->owner);
    ASSERT_TRUE(restored.value && restored.value->owner == owner_);
    ASSERT_EQ(restored.value->policy_generation, committed.value->policy_generation);
    ASSERT_EQ(restored.value->site_groups.size(), 1u);
    const auto& site = restored.value->site_groups.front();
    ASSERT_EQ(site.group.revision, committed.value->site_groups.front().group.revision);
    ASSERT_FALSE(site.members.empty());
    for (const auto& rule : site.members) {
      ASSERT_EQ(rule.policy.mode, AccessMode::kProxy);
      ASSERT_TRUE(rule.policy.proxy_group_id == group_);
    }
    ASSERT_EQ(Snapshot().intent, AccessSiteIntent::kAllow)
        << SavedProfileState(options);
    ASSERT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable)
        << SavedProfileState(options);
    ASSERT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));
    ASSERT_TRUE(options->CanRestoreSavedTestNodeProfile()) << SavedProfileState(options);
  }

  AccessOptionsService* InstallOptions() {
    // Only the native core preparation is synthetic. The service first reads
    // from the persisted store and resolves the reference through the Vault.
    return AccessOptionsService::InstallForTesting(
        profile_.get(), std::make_unique<SavedProfileTestVault>(vault_rows_),
        base::BindRepeating([](const AccessInternalTestCoreBindingMac& binding) {
          return AccessLocalCoreReady{
              .ready = true,
              .port = 23456,
              .registration_id = base::Uuid::GenerateRandomV4().AsLowercaseString(),
              .transport = binding.selected_transport,
              .no_local_auth = true,
              .profile_id = binding.profile_id,
              .profile_version = binding.profile_version,
              .authorization_generation = binding.authorization_generation,
              .listener_socket_generation = binding.authorization_generation,
              .parent_listener_lease_held = true,
          };
        }));
  }

  void PrepareExplicitly(AccessOptionsService* options) {
    base::test::TestFuture<AccessLocalCoreReady> prepared;
    options->PrepareTestNodeProfile(options_before_.revision, prepared.GetCallback());
    ASSERT_TRUE(prepared.Wait()) << SavedProfileState(options);
    ASSERT_TRUE(prepared.Get().ready) << SavedProfileState(options);
  }

  void PublishInitialTestProfile(AccessOptionsService* options) {
    context_.hold_next = true;
    ASSERT_TRUE(bubble_->RequestTestProfileForSite({"news.example"}))
        << SavedProfileState(options);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return context_.has_reply() ||
             !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
    })) << "Waiting for manual publication ACK. " << SavedProfileState(options);
    ASSERT_NO_FATAL_FAILURE(AssertHeldTestProfileAck(options));

    context_.Reply(true);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
    })) << "Waiting for manual publication to settle after ACK. "
        << SavedProfileState(options);
    ASSERT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady)
        << SavedProfileState(options);
    ASSERT_EQ(Snapshot().intent, AccessSiteIntent::kAllow) << SavedProfileState(options);
    const auto active = options->test_profile_runtime();
    ASSERT_TRUE(active.ready) << SavedProfileState(options);
    ASSERT_TRUE(HasAcknowledgedListener(active)) << SavedProfileState(options);
  }

  void WaitForRestoreAck(base::test::TestFuture<bool>& completion) {
    auto* options = AccessOptionsService::GetOrCreate(profile_.get());
    ASSERT_TRUE(base::test::RunUntil([&] {
      return context_.has_reply() || completion.IsReady();
    })) << "Waiting for saved Profile publication ACK. " << SavedProfileState(options);
    ASSERT_NO_FATAL_FAILURE(AssertHeldTestProfileAck(options));
    ASSERT_FALSE(completion.IsReady()) << SavedProfileState(options);
  }

  void AssertHeldTestProfileAck(AccessOptionsService* options,
                               const char* host = "news.example") {
    const auto runtime = options->test_profile_runtime();
    ASSERT_TRUE(context_.HoldsRegistration(runtime.registration_id))
        << SavedProfileState(options);
    ASSERT_EQ(Snapshot(host).connection, AccessSiteConnectionState::kConnecting)
        << SavedProfileState(options);
    ASSERT_EQ(options->test_profile_runtime_state(),
              AccessTestProfileRuntimeState::kPublishing)
        << SavedProfileState(options);
    ASSERT_FALSE(runtime.ready) << SavedProfileState(options);
    ASSERT_FALSE(HasAcknowledgedListener(runtime)) << SavedProfileState(options);
  }

  bool HasAcknowledgedListener(const AccessLocalCoreReady& runtime) {
    const auto* published = AccessPublishedRequestRuntime::Get(profile_.get());
    const auto common = published
        ? published->CaptureCommonGenerationTupleOnUiThread(owner_)
        : std::nullopt;
    return common && transport_->HasAcknowledgedTestListener(
                         owner_, *common, group_, runtime.registration_id);
  }

  std::string SavedProfileState(AccessOptionsService* options) {
    const auto snapshot = Snapshot();
    const auto runtime = options->test_profile_runtime();
    return (testing::Message()
            << "site=" << AccessSiteConnectionStateToString(snapshot.connection)
            << " intent=" << static_cast<int>(snapshot.intent)
            << " runtime_state="
            << static_cast<int>(options->test_profile_runtime_state())
            << " runtime_ready=" << runtime.ready
            << " ack_held=" << context_.has_reply()
            << " registration_in_held_ack="
            << context_.HoldsRegistration(runtime.registration_id)
            << " listener_acknowledged=" << HasAcknowledgedListener(runtime)
            << " can_dispatch=" << coordinator_->CanDispatch(owner_))
        .GetString();
  }

  void ExpectBlockedWithoutRestoring() {
    base::test::TestFuture<bool> blocked;
    bubble_->EnsureLocalCoreForRequest(owner_, group_, blocked.GetCallback());
    ASSERT_TRUE(blocked.Wait())
        << SavedProfileState(AccessOptionsService::GetOrCreate(profile_.get()));
    EXPECT_FALSE(blocked.Get());
    EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));
    EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
    EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  }

  void HoldRevokedLegacyRollback(const AccessSiteHost& host,
                                 const std::string& registration) {
    auto* options = AccessOptionsService::GetOrCreate(profile_.get());
    ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
    context_.hold_next = true;
    ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    ASSERT_NO_FATAL_FAILURE(AssertHeldTestProfileAck(options, host.value.c_str()));
    ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(
        options_before_.revision, base::DoNothing()));

    // Retirement can publish before rollback. Hold the install that restores
    // this legacy registration specifically.
    context_.hold_next_registration = registration;
    context_.Reply(true);
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    ASSERT_TRUE(context_.HoldsRegistration(registration));
    ASSERT_TRUE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
  }

  void PublishLegacyRoute(const AccessSiteHost& host,
                          const std::string& registration,
                          base::OnceClosure before_verification = {}) {
    base::test::TestFuture<AccessMutationTransactionResult> published;
    ASSERT_NO_FATAL_FAILURE(AccessSiteProxyProductTestPeer::PublishFakeLegacyRoute(
        bubble_, host, owner_, registration, published.GetCallback(),
        std::move(before_verification)));
    ASSERT_TRUE(published.Wait());
    ASSERT_EQ(published.Get().status, AccessMutationTransactionStatus::kCommitted);
    ASSERT_EQ(Snapshot(host.value.c_str()).connection,
              AccessSiteConnectionState::kReady);
  }

  enum class SwitchFailure {
    kRejectedAck,
    kRejectedAckWithLateRollback,
    kCandidateStopped,
    kCommitFailed,
  };

  void ExpectFailedTestProfileSwitchAfterLegacyRollback(SwitchFailure failure) {
    const AccessSiteHost host{"legacy-example.com"};
    const std::string registration = "legacy-local-registration";
    const auto legacy_group =
        AccessSiteProxyBubbleController::LocalProxyGroup(owner_);
    ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, registration));
    const auto before =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_TRUE(before.value);

    auto* options = AccessOptionsService::GetOrCreate(profile_.get());
    ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
    context_.hold_next = true;
    ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    ASSERT_NO_FATAL_FAILURE(
        AssertHeldTestProfileAck(options, host.value.c_str()));
    const auto candidate_registration =
        options->test_profile_runtime().registration_id;
    const bool fail_commit = failure == SwitchFailure::kCommitFailed;
    const char* expected_failure = kUnconfirmedTestProfileSwitchPolicy;
    if (fail_commit) {
      AccessServiceCoordinatorTestPeer::SetFailure(
          coordinator_,
          AccessRuleStore::FailurePointForTesting::kCommitAfterFirstMemberInsert);
      expected_failure = "The test Profile switch could not be saved.";
    }
    context_.hold_next_registration = registration;
    context_.Reply(/*accepted=*/fail_commit);
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    ASSERT_TRUE(context_.HoldsRegistration(registration));

    const auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    ASSERT_TRUE(common);
    ASSERT_GT(common->policy_generation, before.value->policy_generation);
    ASSERT_EQ(context_.held_ack().common_generations->policy_generation,
              common->policy_generation);
    const auto legacy_acknowledged = [&] {
      return transport_->HasAcknowledgedLegacyProxy(owner_, *common,
                                                    legacy_group, registration);
    };
    EXPECT_FALSE(legacy_acknowledged());
    if (failure == SwitchFailure::kCandidateStopped) {
      ASSERT_FALSE(context_.HoldsRegistration(candidate_registration));
      ASSERT_EQ(options->test_profile_runtime_state(),
                AccessTestProfileRuntimeState::kPublishing);
      AccessSiteProxyProductTestPeer::NotifyTestProfileCoreStopped(
          options, candidate_registration);
      ASSERT_EQ(options->test_profile_runtime_state(),
                AccessTestProfileRuntimeState::kRevoking);
      EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));
      // A UI Revoke can join the retirement the core stop already started.
      ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(
          options_before_.revision, base::DoNothing()));
      EXPECT_TRUE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
    }
    if (failure == SwitchFailure::kRejectedAckWithLateRollback) {
      const auto expired_sequence = context_.held_ack().sequence;
      AccessNetworkContextTransportTestPeer::ExpireInstall(
          transport_, owner_, context_.incarnation);
      ASSERT_TRUE(base::test::RunUntil([&] {
        return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
      }));
      EXPECT_EQ(Snapshot(host.value.c_str()).connection,
                AccessSiteConnectionState::kUnavailable);

      context_.hold_next_registration = registration;
      context_.Reply(true);
      ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
      ASSERT_TRUE(context_.HoldsRegistration(registration));
      EXPECT_GT(context_.held_ack().sequence, expired_sequence);
      EXPECT_EQ(context_.held_ack().common_generations->policy_generation,
                common->policy_generation);
      EXPECT_FALSE(legacy_acknowledged());
      EXPECT_EQ(Snapshot(host.value.c_str()).connection,
                AccessSiteConnectionState::kUnavailable);
    }

    context_.Reply(true);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_) &&
             legacy_acknowledged();
    }));
    if (fail_commit) {
      AccessServiceCoordinatorTestPeer::SetFailure(
          coordinator_, AccessRuleStore::FailurePointForTesting::kNone);
    }
    const auto after =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    EXPECT_EQ(after.value, before.value);
    const auto snapshot = Snapshot(host.value.c_str());
    EXPECT_EQ(snapshot.intent, AccessSiteIntent::kAllow);
    EXPECT_EQ(snapshot.connection, AccessSiteConnectionState::kReady);
    EXPECT_EQ(snapshot.failure_reason, expected_failure);

    AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
    EXPECT_EQ(Snapshot(host.value.c_str()).connection,
              AccessSiteConnectionState::kUnavailable);
  }

  void ExpectFaultSurvivesLegacyRollback(bool wait,
                                         bool expire_rollback,
                                         bool fault_before_switch = false) {
    const AccessSiteHost host{"legacy-example.com"};
    const std::string registration = "legacy-local-registration";
    const auto legacy_group =
        AccessSiteProxyBubbleController::LocalProxyGroup(owner_);
    ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, registration));
    const auto before =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_TRUE(before.value);

    const auto expected_state = wait ? AccessSiteConnectionState::kConnecting
                                     : AccessSiteConnectionState::kUnavailable;
    const auto expected_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kRuntimeFault,
        wait ? "waiting for node recovery" : "node or runtime unavailable");
    const auto expect_fault = [&] {
      const auto snapshot = Snapshot(host.value.c_str());
      EXPECT_EQ(snapshot.intent, AccessSiteIntent::kAllow);
      EXPECT_EQ(snapshot.connection, expected_state);
      EXPECT_EQ(snapshot.failure_reason, expected_reason);
    };
    if (fault_before_switch) {
      ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault(host, wait));
      expect_fault();
    }

    ASSERT_NO_FATAL_FAILURE(HoldRevokedLegacyRollback(host, registration));
    auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    ASSERT_TRUE(common);
    ASSERT_GT(common->policy_generation, before.value->policy_generation);
    ASSERT_EQ(context_.held_ack().common_generations->policy_generation,
              common->policy_generation);

    if (!fault_before_switch) {
      ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault(host, wait));
      expect_fault();
    }
    EXPECT_FALSE(bubble_->RequestHostProxySession(host, false));
    if (!fault_before_switch) expect_fault();

    if (expire_rollback) {
      const auto expired_sequence = context_.held_ack().sequence;
      AccessNetworkContextTransportTestPeer::ExpireInstall(
          transport_, owner_, context_.incarnation);
      ASSERT_TRUE(base::test::RunUntil([&] {
        return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
      }));
      ASSERT_TRUE(coordinator_->CanDispatch(owner_));
      expect_fault();

      context_.hold_next_registration = registration;
      context_.Reply(true);
      ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
      ASSERT_TRUE(context_.HoldsRegistration(registration));
      EXPECT_GT(context_.held_ack().sequence, expired_sequence);
      EXPECT_EQ(context_.held_ack().common_generations->policy_generation,
                common->policy_generation);
      EXPECT_FALSE(transport_->HasAcknowledgedLegacyProxy(
          owner_, *common, legacy_group, registration));
      expect_fault();
    }

    context_.Reply(true);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_) &&
             transport_->HasAcknowledgedLegacyProxy(
                 owner_, *common, legacy_group, registration);
    }));
    expect_fault();
    EXPECT_TRUE(coordinator_->CanDispatch(owner_));
    EXPECT_EQ(Snapshot("www.legacy-example.com").failure_reason, expected_reason);
    const auto after =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    EXPECT_EQ(after.value, before.value);
    const auto original = AccessRuleStore::AdaptMatcherSnapshot(*before.value);
    ASSERT_TRUE(original.value);
    const auto* published = runtime->GetPublishedPolicySnapshot(owner_);
    ASSERT_TRUE(published);
    EXPECT_EQ(published->rules, original.value->rules);
    EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));
    EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
    EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  }

  // Keep SQLite and Mojo startup on the real clock.
  base::test::ScopedRunLoopTimeout saved_profile_timeout_{FROM_HERE,
                                                        base::Seconds(15)};
  std::shared_ptr<SavedProfileTestVault::Rows> vault_rows_ =
      std::make_shared<SavedProfileTestVault::Rows>();
  AccessOptionsSnapshot options_before_;
  std::string group_;
  std::string previous_registration_;
};

TEST_F(AccessSavedTestProfileProductTest, PersistedBindingWaitsForExactAckAfterRestart) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  base::test::TestFuture<bool> wrong_group;
  bubble_->EnsureLocalCoreForRequest(owner_,
      "internal-test-profile:550e8400-e29b-41d4-a716-446655440000:1",
      wrong_group.GetCallback());
  ASSERT_TRUE(wrong_group.Wait()) << SavedProfileState(options);
  EXPECT_FALSE(wrong_group.Get());
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kIdle);

  context_.hold_next = true;
  base::test::TestFuture<bool> first, second;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, first.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(first));
  bubble_->EnsureLocalCoreForRequest(owner_, group_, second.GetCallback());
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(second.IsReady());
  EXPECT_FALSE(options->test_profile_runtime().ready);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kConnecting);
  const auto endpoint = transport_->PreparedProxyForGroup(owner_, group_);
  ASSERT_TRUE(endpoint);
  EXPECT_NE(endpoint->registration_id, previous_registration_);
  EXPECT_EQ(endpoint->transport, aegis_access::RegisteredProxyTransport::kSocks5);

  context_.Reply(true);
  ASSERT_TRUE(first.Wait()) << SavedProfileState(options);
  ASSERT_TRUE(second.Wait()) << SavedProfileState(options);
  EXPECT_TRUE(first.Get());
  EXPECT_TRUE(second.Get());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady)
      << SavedProfileState(options);
  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(after.value->policy_generation, before.value->policy_generation);
  EXPECT_EQ(after.value->site_groups, before.value->site_groups);
  base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> stored;
  options->Read(stored.GetCallback());
  ASSERT_TRUE(stored.Wait()) << "Saved test Profile read did not complete";
  ASSERT_TRUE(stored.Get().value);
  EXPECT_EQ(stored.Get().value->revision, options_before_.revision);
  EXPECT_EQ(stored.Get().value->test_profile, options_before_.test_profile);

  base::test::TestFuture<bool> revoked;
  ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(options_before_.revision,
                                                       revoked.GetCallback()));
  ASSERT_TRUE(revoked.Wait()) << SavedProfileState(options);
  ASSERT_TRUE(revoked.Get());
  ExpectBlockedWithoutRestoring();
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
}

TEST_F(AccessSavedTestProfileProductTest,
       ManualBindingOffersOriginalTabRetryWithoutLegacyInput) {
  base::test::ScopedCommandLine command;
  auto* line = command.GetProcessCommandLine();
  line->RemoveSwitch("aegis-access-dev-node-config");
  line->RemoveSwitch("aegis-access-dev-core-manifest");
  line->RemoveSwitch("aegis-access-dev-canary-base-url");
  ASSERT_FALSE(ResolveAccessLocalCoreInput());
  // No manual launch options: the legacy-only behavior is unchanged.
  EXPECT_TRUE(bubble_->GetFailedNavigationRetryHandler().is_null());

  line->AppendSwitchPath("aegis-access-dev-core-manifest",
                         directory_.GetPath().AppendASCII("core-manifest.json"));
  line->AppendSwitchASCII("aegis-access-dev-canary-base-url",
                          "https://canary.example/probe");
  if (!CanResolveAccessInternalTestCoreInput()) {
    GTEST_SKIP() << "Requires an admitted internal DEV build.";
  }
  ASSERT_FALSE(ResolveAccessLocalCoreInput());
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  const auto handler = bubble_->GetFailedNavigationRetryHandler();
  ASSERT_FALSE(handler.is_null()) << SavedProfileState(options);

  // A site without a manual binding is refused without touching its state.
  base::test::TestFuture<bool> unrelated;
  handler.Run(GURL("https://other.example/path"), unrelated.GetCallback());
  ASSERT_TRUE(unrelated.Wait());
  EXPECT_FALSE(unrelated.Get());
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kIdle);
  EXPECT_FALSE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));

  // The bound site still needs its exact acknowledged route.
  base::test::TestFuture<bool> before_ack;
  handler.Run(GURL("https://news.example/story"), before_ack.GetCallback());
  ASSERT_TRUE(before_ack.Wait()) << SavedProfileState(options);
  EXPECT_FALSE(before_ack.Get());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));

  context_.hold_next = true;
  base::test::TestFuture<bool> restored;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, restored.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(restored));
  context_.Reply(true);
  ASSERT_TRUE(restored.Wait()) << SavedProfileState(options);
  ASSERT_TRUE(restored.Get()) << SavedProfileState(options);

  base::test::TestFuture<bool> after_ack;
  handler.Run(GURL("https://news.example/story"), after_ack.GetCallback());
  ASSERT_TRUE(after_ack.Wait()) << SavedProfileState(options);
  EXPECT_TRUE(after_ack.Get()) << SavedProfileState(options);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
  EXPECT_TRUE(HasAcknowledgedListener(options->test_profile_runtime()));
}

TEST_F(AccessSavedTestProfileProductTest, RejectedAckKeepsSavedProxyUnavailable) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  context_.hold_next = true;
  base::test::TestFuture<bool> result;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, result.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(result));
  context_.Reply(false);
  ASSERT_TRUE(result.Wait()) << SavedProfileState(options);
  EXPECT_FALSE(result.Get());
  environment_.RunUntilIdle();
  ExpectBlockedWithoutRestoring();
}

TEST_F(AccessSavedTestProfileProductTest, MissingVaultBindingKeepsSavedProxyUnavailable) {
  vault_rows_->clear();
  ExpectBlockedWithoutRestoring();
  EXPECT_EQ(AccessOptionsService::GetOrCreate(profile_.get())->test_profile_runtime_state(),
            AccessTestProfileRuntimeState::kIdle);
  ExpectBlockedWithoutRestoring();
}

TEST_F(AccessSavedTestProfileProductTest, ChangedNetworkTupleFencesHeldAck) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  context_.hold_next = true;
  base::test::TestFuture<bool> first, changed;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, first.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(first));
  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  bubble_->EnsureLocalCoreForRequest(owner_, group_, changed.GetCallback());
  ASSERT_TRUE(changed.IsReady());
  EXPECT_FALSE(changed.Get());
  EXPECT_FALSE(first.IsReady());

  context_.Reply(true);
  ASSERT_TRUE(first.Wait()) << SavedProfileState(options);
  EXPECT_FALSE(first.Get());
  environment_.RunUntilIdle();
  ExpectBlockedWithoutRestoring();
}

TEST_F(AccessSavedTestProfileProductTest, MissingAckFailsColdRestoreAndLaterRequests) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  context_.hold_next = true;
  base::test::TestFuture<bool> result;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, result.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(result));

  AccessNetworkContextTransportTestPeer::ExpireInstall(
      transport_, owner_, context_.incarnation);
  ASSERT_TRUE(result.Wait()) << SavedProfileState(options);
  EXPECT_FALSE(result.Get());
  ExpectBlockedWithoutRestoring();

  const auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(common);
  const auto* policy = runtime->GetPublishedPolicySnapshot(owner_);
  ASSERT_TRUE(policy);
  base::test::TestFuture<bool> later;
  transport_->PublishPrivatePolicy(*policy, *common, later.GetCallback());
  ASSERT_TRUE(later.Wait());
  EXPECT_FALSE(later.Get());

  base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
  transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
  ASSERT_TRUE(bound.Wait());
  ASSERT_TRUE(bound.Get());
  // The metadata-free fake has no live frame and only tests publication failure.
  HeldBrowserRequest request(profile_.get(),
                             content::FrameTreeNodeId::FromUnsafeValue(1),
                             owner_, bound.Take());
  request.StartPost();
  ASSERT_TRUE(base::test::RunUntil([&] { return request.client.has_received_completion(); }));
  EXPECT_EQ(request.client.completion_status().error_code, net::ERR_NETWORK_CHANGED);
  EXPECT_EQ(request.target.starts, 0);

  EXPECT_TRUE(context_.has_reply());
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kUncertain);
  EXPECT_TRUE(options->test_profile_runtime().parent_listener_lease_held);
}

TEST_F(AccessSavedTestProfileProductTest, MissingAckTimesOutAndLateAckCannotDispatch) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  context_.hold_next = true;
  base::test::TestFuture<bool> result;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, result.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(result));

  auto& timer =
      AccessSiteProxyProductTestPeer::SavedTestProfilePublicationTimer(bubble_);
  ASSERT_TRUE(timer.IsRunning()) << SavedProfileState(options);
  timer.FireNow();
  ASSERT_TRUE(result.IsReady()) << SavedProfileState(options);
  EXPECT_FALSE(result.Get());
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kRevoking);
  AccessNetworkContextTransportTestPeer::ExpireInstall(
      transport_, owner_, context_.incarnation);
  environment_.RunUntilIdle();
  ASSERT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kUncertain);
  ASSERT_TRUE(context_.has_reply());
  const auto expired_sequence = context_.held_ack().sequence;

  context_.hold_next = true;
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  EXPECT_GT(context_.held_ack().sequence, expired_sequence);
  EXPECT_TRUE(context_.held_ack().registrations.empty());
  ASSERT_EQ(context_.held_ack().unavailable_groups.size(), 1u);
  EXPECT_EQ(context_.held_ack().unavailable_groups.front()->proxy_group_id, group_);
  EXPECT_NE(context_.held_ack().unavailable_groups.front()->selection_generation, 0u);
  EXPECT_FALSE(transport_->HasReadyPrivateContext(owner_));
  EXPECT_TRUE(options->test_profile_runtime().parent_listener_lease_held);
  base::test::TestFuture<AccessLocalCoreReady> blocked_preparation;
  options->PrepareTestNodeProfile(options_before_.revision,
                                  blocked_preparation.GetCallback());
  EXPECT_FALSE(blocked_preparation.Get().ready);

  base::test::TestFuture<bool> revoked;
  ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(options_before_.revision,
                                                       revoked.GetCallback()));
  EXPECT_FALSE(revoked.IsReady());
  context_.Reply(true);
  ASSERT_TRUE(revoked.Wait());
  EXPECT_TRUE(revoked.Get());
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kIdle);
  ExpectBlockedWithoutRestoring();
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
}

TEST_F(AccessSavedTestProfileProductTest, AckAfterDeadlineBeforeColdTimerCannotActivate) {
  base::SimpleTestTickClock publication_clock;
  publication_clock.SetNowTicks(base::TimeTicks::Now());
  auto clock_override = AccessSiteProxyProductTestPeer::OverrideSavedTestProfilePublicationClock(
      bubble_, &publication_clock);
  context_.hold_next = true;
  base::test::TestFuture<bool> result;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, result.GetCallback());
  ASSERT_NO_FATAL_FAILURE(WaitForRestoreAck(result));
  publication_clock.Advance(base::Seconds(30));
  ASSERT_TRUE(AccessSiteProxyProductTestPeer::SavedTestProfilePublicationTimer(bubble_)
                  .IsRunning());
  context_.Reply(true);
  ASSERT_TRUE(result.Wait());
  EXPECT_FALSE(result.Get());
  EXPECT_FALSE(bubble_->AcknowledgedTestProfileListener());
  ExpectBlockedWithoutRestoring();
}

TEST_F(AccessSavedTestProfileProductTest, NetworkChangeHidesActiveListenerButKeepsRevocation) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  base::test::TestFuture<bool> restored;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, restored.GetCallback());
  ASSERT_TRUE(restored.Wait());
  ASSERT_TRUE(restored.Get());
  ASSERT_TRUE(bubble_->AcknowledgedTestProfileListener());

  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kActive);
  EXPECT_FALSE(bubble_->AcknowledgedTestProfileListener());
  EXPECT_TRUE(options->CanRevokeTestNodeProfileAtRevision(options_before_.revision));
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
}

TEST_F(AccessSavedTestProfileProductTest,
       ActiveProfileServesSecondSiteWithoutReregistration) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  base::test::TestFuture<bool> restored;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, restored.GetCallback());
  ASSERT_TRUE(restored.Wait());
  ASSERT_TRUE(restored.Get());
  const auto active = bubble_->AcknowledgedTestProfileListener();
  ASSERT_TRUE(active);
  ASSERT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
  const AccessSiteHost other{"other.example"};
  EXPECT_FALSE(bubble_->HasPreparedTestNode());
  EXPECT_FALSE(bubble_->CanSelectTestProfileForSite({"news.example"}));
  ASSERT_TRUE(bubble_->CanSelectTestProfileForSite(other));

  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite(other));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  // The listener stays active and acknowledged for the first site meanwhile.
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kActive);
  EXPECT_EQ(options->test_profile_runtime().registration_id, active->registration_id);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kConnecting);
  EXPECT_FALSE(bubble_->CanSelectTestProfileForSite(other));
  context_.Reply(true);
  ASSERT_NO_FATAL_FAILURE(WaitFor(AccessSiteConnectionState::kReady, "other.example"));

  const auto after = bubble_->AcknowledgedTestProfileListener();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->registration_id, active->registration_id);
  EXPECT_EQ(after->authorization_generation, active->authorization_generation);
  EXPECT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kActive);
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
  EXPECT_FALSE(bubble_->CanSelectTestProfileForSite(other));
  const auto committed =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(committed.value);
  EXPECT_EQ(committed.value->site_groups.size(), 2u);
  for (const auto& site : committed.value->site_groups) {
    ASSERT_FALSE(site.members.empty());
    EXPECT_EQ(site.members.front().policy.proxy_group_id, group_);
  }

  // Closing protection on the second site restores its entry and leaves the
  // first site's connection untouched.
  ASSERT_TRUE(bubble_->RequestHostProxySession(other, false));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kOff);
  EXPECT_TRUE(bubble_->CanSelectTestProfileForSite(other));
  ASSERT_TRUE(bubble_->AcknowledgedTestProfileListener());
  EXPECT_EQ(bubble_->AcknowledgedTestProfileListener()->registration_id,
            active->registration_id);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
}

TEST_F(AccessSavedTestProfileProductTest,
       FailedSecondSiteSelectionKeepsActiveListener) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  base::test::TestFuture<bool> restored;
  bubble_->EnsureLocalCoreForRequest(owner_, group_, restored.GetCallback());
  ASSERT_TRUE(restored.Wait());
  ASSERT_TRUE(restored.Get());
  const auto active = bubble_->AcknowledgedTestProfileListener();
  ASSERT_TRUE(active);
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite({"other.example"}));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(false);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  EXPECT_NE(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  // A failed reuse never revokes the listener other sites still use.
  EXPECT_NE(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kRevoking);
  EXPECT_EQ(options->test_profile_runtime().registration_id, active->registration_id);
  EXPECT_TRUE(transport_->PreparedProxyForGroup(owner_, group_));
}

TEST_F(AccessSavedTestProfileProductTest, RevokingHeldExplicitPublicationSettlesOriginalSite) {
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite({"other.example"}));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_EQ(options->test_profile_runtime_state(), AccessTestProfileRuntimeState::kPublishing);
  ASSERT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kConnecting);
  base::test::TestFuture<bool> revoked;
  EXPECT_FALSE(options->CanRevokeTestNodeProfileAtRevision(options_before_.revision + 1));
  ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(options_before_.revision,
                                                       revoked.GetCallback()));
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kOff);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kIdle);
  EXPECT_TRUE(Snapshot("other.example").failure_reason.empty());
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
}

TEST_F(AccessSavedTestProfileProductTest,
       RejectedTestProfileSwitchReportsFailureAfterLegacyRollback) {
  ExpectFailedTestProfileSwitchAfterLegacyRollback(SwitchFailure::kRejectedAck);
}

TEST_F(AccessSavedTestProfileProductTest,
       RejectedTestProfileSwitchReportsFailureAfterLateLegacyAck) {
  ExpectFailedTestProfileSwitchAfterLegacyRollback(
      SwitchFailure::kRejectedAckWithLateRollback);
}

TEST_F(AccessSavedTestProfileProductTest,
       CandidateStopDuringRejectedSwitchRollbackStillReportsFailure) {
  // The held rollback can acknowledge the legacy route as-is once the
  // candidate stops because no other website uses the test Profile.
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, false));
  ASSERT_NO_FATAL_FAILURE(WaitFor(AccessSiteConnectionState::kIdle));
  ASSERT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  ExpectFailedTestProfileSwitchAfterLegacyRollback(
      SwitchFailure::kCandidateStopped);
}

TEST_F(AccessSavedTestProfileProductTest,
       AcknowledgedTestProfileSwitchReportsDurableSaveFailure) {
  ExpectFailedTestProfileSwitchAfterLegacyRollback(SwitchFailure::kCommitFailed);
}

TEST_F(AccessSavedTestProfileProductTest,
       StalePositiveSwitchAckReportsUnconfirmedPolicy) {
  const AccessSiteHost host{"other.example"};
  const auto before =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_NO_FATAL_FAILURE(AssertHeldTestProfileAck(options, host.value.c_str()));
  const auto candidate = options->test_profile_runtime();

  RunAtQueuedPolicyCompletion advance_epoch(base::BindLambdaForTesting([&] {
    EXPECT_TRUE(HasAcknowledgedListener(candidate));
    AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  }));
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  ASSERT_TRUE(advance_epoch.ran());
  const auto after =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  EXPECT_EQ(after.value, before.value);
  const auto snapshot = Snapshot(host.value.c_str());
  EXPECT_EQ(snapshot.intent, AccessSiteIntent::kOff);
  EXPECT_EQ(snapshot.connection, AccessSiteConnectionState::kUnavailable);
  EXPECT_EQ(snapshot.failure_reason, kUnconfirmedTestProfileSwitchPolicy);
}

TEST_F(AccessSavedTestProfileProductTest,
       CandidateStopAfterPositiveSwitchAckStillReportsSaved) {
  const AccessSiteHost host{"legacy-example.com"};
  ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, "legacy-local-registration"));
  const auto before =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_NO_FATAL_FAILURE(AssertHeldTestProfileAck(options, host.value.c_str()));
  const auto candidate = options->test_profile_runtime();
  const auto generation =
      context_.held_ack().common_generations->policy_generation;
  ASSERT_GT(generation, before.value->policy_generation);

  {
    // Deliver the positive ACK on UI while the real SQLite commit is queued.
    base::test::TaskEnvironment::ParallelExecutionFence hold_commit;
    base::RunLoop ack_consumed;
    RunAtQueuedPolicyCompletion after_ack(
        ack_consumed.QuitClosure(), RunAtQueuedPolicyCompletion::Phase::kAfter);
    context_.Reply(true);
    ack_consumed.Run();
    ASSERT_TRUE(after_ack.ran());
    ASSERT_TRUE(HasAcknowledgedListener(candidate));
    ASSERT_TRUE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
    ASSERT_EQ(options->test_profile_runtime_state(),
              AccessTestProfileRuntimeState::kPublishing);
    const auto still_committed =
        coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_EQ(still_committed.value, before.value);

    AccessSiteProxyProductTestPeer::NotifyTestProfileCoreStopped(
        options, candidate.registration_id);
    EXPECT_EQ(options->test_profile_runtime_state(),
              AccessTestProfileRuntimeState::kRevoking);
    EXPECT_FALSE(HasAcknowledgedListener(candidate));
  }

  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_) &&
           options->test_profile_runtime_state() ==
               AccessTestProfileRuntimeState::kIdle;
  }));
  const auto after =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(after.value->policy_generation, generation);
  const auto saved =
      std::ranges::find_if(after.value->site_groups, [&](const auto& row) {
        return row.group.website_context && row.group.canonical_host == host.value;
      });
  ASSERT_NE(saved, after.value->site_groups.end());
  ASSERT_FALSE(saved->members.empty());
  for (const auto& member : saved->members) {
    EXPECT_EQ(member.policy.mode, AccessMode::kProxy);
    EXPECT_EQ(member.policy.proxy_group_id, group_);
  }
  EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group_));
  EXPECT_FALSE(bubble_->AcknowledgedTestProfileListener());
  const auto snapshot = Snapshot(host.value.c_str());
  EXPECT_EQ(snapshot.intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(snapshot.connection, AccessSiteConnectionState::kUnavailable);
  EXPECT_EQ(snapshot.failure_reason,
            "The test Profile switch was saved. Its original connection "
            "could not be confirmed.");

  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
  ASSERT_TRUE(bubble_->RequestTestProfileForSite({"other.example"}));
  ASSERT_NO_FATAL_FAILURE(
      WaitFor(AccessSiteConnectionState::kReady, "other.example"));
  const auto successor = bubble_->AcknowledgedTestProfileListener();
  ASSERT_TRUE(successor);
  EXPECT_NE(successor->registration_id, candidate.registration_id);
  EXPECT_FALSE(HasAcknowledgedListener(candidate));
  const auto with_successor = Snapshot(host.value.c_str());
  EXPECT_EQ(with_successor.connection, AccessSiteConnectionState::kReady);
  EXPECT_EQ(with_successor.failure_reason, snapshot.failure_reason);
}

TEST_F(AccessSavedTestProfileProductTest, RevokingPendingSwitchRestoresAcknowledgedLegacyRoute) {
  const AccessSiteHost host{"other.example"};
  const std::string registration = "legacy-local-registration";
  base::test::TestFuture<AccessMutationTransactionResult> initial_legacy;
  ASSERT_NO_FATAL_FAILURE(AccessSiteProxyProductTestPeer::PublishFakeLegacyRoute(
      bubble_, host, owner_, registration, initial_legacy.GetCallback()));
  ASSERT_TRUE(initial_legacy.Wait());
  ASSERT_EQ(initial_legacy.Get().status, AccessMutationTransactionStatus::kCommitted);
  ASSERT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);

  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kConnecting);
  ASSERT_TRUE(context_.HoldsRegistration(options->test_profile_runtime().registration_id));
  base::test::TestFuture<bool> revoked;
  ASSERT_TRUE(options->RevokeTestNodeProfileAtRevision(options_before_.revision,
                                                       revoked.GetCallback()));
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  ASSERT_TRUE(revoked.Wait());
  EXPECT_TRUE(revoked.Get());

  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(after.value->site_groups, before.value->site_groups);
  EXPECT_EQ(after.value->policy_generation, before.value->policy_generation);
  const auto common = AccessPublishedRequestRuntime::Get(profile_.get())
                          ->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(common);
  EXPECT_GT(common->policy_generation, after.value->policy_generation);
  EXPECT_TRUE(transport_->HasAcknowledgedLegacyProxy(
      owner_, *common, AccessSiteProxyBubbleController::LocalProxyGroup(owner_), registration));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  EXPECT_TRUE(Snapshot("other.example").failure_reason.empty());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);

  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  const auto new_common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(new_common);
  ASSERT_GT(new_common->network_epoch, common->network_epoch);
  const auto* policy = runtime->GetPublishedPolicySnapshot(owner_);
  ASSERT_TRUE(policy);
  const auto legacy_group = AccessSiteProxyBubbleController::LocalProxyGroup(owner_);
  context_.hold_next = true;
  base::test::TestFuture<bool> published;
  transport_->PublishPrivatePolicy(*policy, *new_common, published.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  EXPECT_FALSE(context_.HoldsRegistration(registration));
  EXPECT_TRUE(std::ranges::any_of(
      context_.held_ack().unavailable_groups,
      [&](const auto& entry) { return entry->proxy_group_id == legacy_group; }));
  context_.Reply(true);
  ASSERT_TRUE(published.Wait());
  ASSERT_TRUE(published.Get());
  ASSERT_TRUE(transport_->HasCurrentPrivatePolicy(owner_, *new_common));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kUnavailable);
}

TEST_F(AccessSavedTestProfileProductTest,
       RevokingPendingSwitchWaitsForFreshLegacyAckAfterRollbackTimeout) {
  const AccessSiteHost host{"other.example"};
  const std::string registration = "legacy-local-registration";
  const auto legacy_group = AccessSiteProxyBubbleController::LocalProxyGroup(owner_);
  base::test::TestFuture<AccessMutationTransactionResult> initial_legacy;
  ASSERT_NO_FATAL_FAILURE(AccessSiteProxyProductTestPeer::PublishFakeLegacyRoute(
      bubble_, host, owner_, registration, initial_legacy.GetCallback()));
  ASSERT_TRUE(initial_legacy.Wait());
  ASSERT_EQ(initial_legacy.Get().status, AccessMutationTransactionStatus::kCommitted);
  ASSERT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);

  ASSERT_NO_FATAL_FAILURE(HoldRevokedLegacyRollback(host, registration));
  const auto expired_sequence = context_.held_ack().sequence;
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(common);
  ASSERT_GT(common->policy_generation, before.value->policy_generation);
  ASSERT_EQ(context_.held_ack().common_generations->policy_generation,
            common->policy_generation);
  AccessNetworkContextTransportTestPeer::ExpireInstall(
      transport_, owner_, context_.incarnation);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  ASSERT_TRUE(coordinator_->CanDispatch(owner_));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kUnavailable);

  context_.hold_next_registration = registration;
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_TRUE(context_.HoldsRegistration(registration));
  EXPECT_GT(context_.held_ack().sequence, expired_sequence);
  EXPECT_EQ(context_.held_ack().common_generations->policy_generation,
            common->policy_generation);
  EXPECT_FALSE(transport_->HasAcknowledgedLegacyProxy(
      owner_, *common, legacy_group, registration));
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kUnavailable);
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport_->HasAcknowledgedLegacyProxy(
        owner_, *common, legacy_group, registration);
  }));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  EXPECT_TRUE(Snapshot("other.example").failure_reason.empty());

  // Hold a different site's DIRECT candidate after the legacy ACK is current.
  // Reject that candidate so both sites must still have their original rules.
  const AccessSiteHost unrelated{"unrelated.example"};
  context_.hold_next_registration = registration;
  base::test::TestFuture<void> unrelated_off;
  ASSERT_TRUE(bubble_->RequestHostProxySession(unrelated, false,
                                              unrelated_off.GetCallback()));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_TRUE(context_.HoldsRegistration(registration));
  ASSERT_TRUE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
  ASSERT_FALSE(coordinator_->CanDispatch(owner_));
  const auto pending = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(pending);
  ASSERT_GT(pending->policy_generation, common->policy_generation);
  ASSERT_EQ(context_.held_ack().common_generations->policy_generation,
            pending->policy_generation);
  const auto* candidate = runtime->GetPublishedPolicySnapshot(owner_);
  ASSERT_TRUE(candidate);
  ASSERT_TRUE(std::ranges::any_of(candidate->rules, [&](const auto& rule) {
    return rule.destination_host == unrelated.value &&
           rule.mode == AccessMode::kDirect;
  }));

  base::test::TestFuture<void> rejected_off;
  EXPECT_FALSE(bubble_->RequestHostProxySession(host, false,
                                              rejected_off.GetCallback()));
  EXPECT_EQ(Snapshot("other.example").failure_reason,
            AccessSiteProxySessionErrorReadableMessage(
                AccessSiteProxySessionError::kBusy));
  context_.Reply(false);
  ASSERT_TRUE(base::test::RunUntil([&] {
    const auto current = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_) && current &&
           current->policy_generation > pending->policy_generation &&
           transport_->HasAcknowledgedLegacyProxy(
               owner_, *current, legacy_group, registration);
  }));
  EXPECT_FALSE(rejected_off.IsReady());
  EXPECT_FALSE(unrelated_off.IsReady());
  EXPECT_EQ(Snapshot("unrelated.example").intent, AccessSiteIntent::kOff);
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kReady);
  EXPECT_TRUE(Snapshot("other.example").failure_reason.empty());
  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  EXPECT_EQ(after.value, before.value);
  const auto original = AccessRuleStore::AdaptMatcherSnapshot(*before.value);
  ASSERT_TRUE(original.value);
  const auto* published = runtime->GetPublishedPolicySnapshot(owner_);
  ASSERT_TRUE(published);
  EXPECT_EQ(published->rules, original.value->rules);

  ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault(host, false));
  EXPECT_EQ(Snapshot("other.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("other.example").connection, AccessSiteConnectionState::kUnavailable);
}

TEST_F(AccessSavedTestProfileProductTest,
       RevokingPendingSwitchPreservesConnectingFaultWithRollbackAck) {
  ExpectFaultSurvivesLegacyRollback(/*wait=*/true, /*expire_rollback=*/false);
}

TEST_F(AccessSavedTestProfileProductTest,
       RevokingPendingSwitchPreservesUnavailableFaultAfterLateRollbackAck) {
  ExpectFaultSurvivesLegacyRollback(/*wait=*/false, /*expire_rollback=*/true);
}

TEST_F(AccessSavedTestProfileProductTest,
       RevokingPendingSwitchPreservesLegacyFaultFromBeforeAdmission) {
  ExpectFaultSurvivesLegacyRollback(/*wait=*/false, /*expire_rollback=*/false,
                                   /*fault_before_switch=*/true);
}

TEST_F(AccessSavedTestProfileProductTest,
       SwitchRejectedBeforePublicationPreservesOriginalLegacyFault) {
  const AccessSiteHost host{"legacy-example.com"};
  ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, "legacy-local-registration"));
  ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault(host, /*wait=*/true));
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));

  // Invalidate the current policy ACK before the restore callback can publish.
  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
  ASSERT_FALSE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
  EXPECT_EQ(options->test_profile_runtime_state(),
            AccessTestProfileRuntimeState::kPrepared);
  const auto after = Snapshot(host.value.c_str());
  EXPECT_EQ(after.intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(after.connection, AccessSiteConnectionState::kConnecting);
  EXPECT_EQ(after.failure_reason, AccessSiteProxySessionErrorReadableMessage(
                                     AccessSiteProxySessionError::kRuntimeFault,
                                     "waiting for node recovery"));
}

TEST_F(AccessSavedTestProfileProductTest,
       SwitchRejectedBeforePublicationReportsFailureAfterLegacyAck) {
  const AccessSiteHost host{"legacy-example.com"};
  const std::string registration = "legacy-local-registration";
  ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, registration));
  auto* options = AccessOptionsService::GetOrCreate(profile_.get());
  ASSERT_NO_FATAL_FAILURE(PrepareExplicitly(options));

  context_.Disconnect();
  environment_.RunUntilIdle();
  ProductAckContext replacement;
  replacement.incarnation = "replacement-context";
  base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
  transport_->BindPrivateContext(owner_, &replacement, bound.GetCallback());
  ASSERT_TRUE(bound.Get());
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  const auto* policy = runtime->GetPublishedPolicySnapshot(owner_);
  ASSERT_TRUE(common && policy);
  replacement.hold_next_registration = registration;
  base::test::TestFuture<bool> ack;
  transport_->PublishPrivatePolicy(*policy, *common, ack.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return replacement.has_reply(); }));
  ASSERT_TRUE(replacement.HoldsRegistration(registration));

  ASSERT_TRUE(bubble_->RequestTestProfileForSite(host));
  ASSERT_FALSE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
  EXPECT_EQ(options->test_profile_runtime_state(),
            AccessTestProfileRuntimeState::kPrepared);
  EXPECT_EQ(Snapshot(host.value.c_str()).connection,
            AccessSiteConnectionState::kUnavailable);
  replacement.Reply(true);
  ASSERT_TRUE(ack.Get());
  const auto snapshot = Snapshot(host.value.c_str());
  EXPECT_EQ(snapshot.intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(snapshot.connection, AccessSiteConnectionState::kReady);
  EXPECT_EQ(snapshot.failure_reason,
            "The test Profile switch failed because the website policy could "
            "not be confirmed.");
}

TEST_F(AccessSavedTestProfileProductTest,
       StoppedLegacySiteRecoversAfterVerifiedReplacementForAnotherSite) {
  const AccessSiteHost host{"legacy-example.com"};
  const std::string registration = "legacy-local-registration";
  const std::string replacement = "replacement-legacy-registration";
  const auto group = AccessSiteProxyBubbleController::LocalProxyGroup(owner_);
  ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(host, registration));
  ASSERT_NO_FATAL_FAILURE(HoldRevokedLegacyRollback(host, registration));

  AccessSiteProxyProductTestPeer::LoseRuntime(bubble_, registration);
  context_.Reply(true);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  ASSERT_TRUE(base::test::RunUntil([&] {
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_) &&
           coordinator_->CanDispatch(owner_) && common &&
           transport_->HasCurrentPrivatePolicy(owner_, *common);
  }));
  EXPECT_FALSE(transport_->PreparedProxyForGroup(owner_, group));
  ASSERT_EQ(Snapshot(host.value.c_str()).intent, AccessSiteIntent::kAllow);
  ASSERT_EQ(Snapshot(host.value.c_str()).connection,
            AccessSiteConnectionState::kUnavailable);

  const AccessSiteHost other{"other.example"};
  context_.hold_next_registration = replacement;
  base::test::TestFuture<AccessMutationTransactionResult> published;
  ASSERT_NO_FATAL_FAILURE(AccessSiteProxyProductTestPeer::PublishFakeLegacyRoute(
      bubble_, other, owner_, replacement, published.GetCallback()));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_TRUE(context_.HoldsRegistration(replacement));
  EXPECT_EQ(Snapshot(host.value.c_str()).connection,
            AccessSiteConnectionState::kUnavailable);
  context_.Reply(true);
  ASSERT_TRUE(published.Wait());
  ASSERT_EQ(published.Get().status, AccessMutationTransactionStatus::kCommitted);
  ASSERT_EQ(Snapshot(other.value.c_str()).connection,
            AccessSiteConnectionState::kReady);
  const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(common);
  ASSERT_TRUE(transport_->HasAcknowledgedLegacyProxy(owner_, *common, group,
                                                     replacement));
  EXPECT_EQ(Snapshot(host.value.c_str()).intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot(host.value.c_str()).connection,
            AccessSiteConnectionState::kReady);
  EXPECT_TRUE(Snapshot(host.value.c_str()).failure_reason.empty());
}

TEST_F(AccessSavedTestProfileProductTest,
       BusyRejectionShowsNoticeAfterCommittedCoreVerificationClearsFault) {
  const AccessSiteHost host{"legacy-example.com"};
  const std::string registration = "legacy-local-registration";
  ASSERT_NO_FATAL_FAILURE(PublishLegacyRoute(
      host, registration, base::BindLambdaForTesting([&] {
        ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault(host, /*wait=*/false));
        EXPECT_EQ(Snapshot(host.value.c_str()).connection,
                  AccessSiteConnectionState::kUnavailable);
      })));
  ASSERT_TRUE(Snapshot(host.value.c_str()).failure_reason.empty());

  context_.hold_next_registration = registration;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"other.example"}, false));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  ASSERT_TRUE(context_.HoldsRegistration(registration));
  EXPECT_FALSE(bubble_->RequestHostProxySession(host, false));
  EXPECT_EQ(Snapshot(host.value.c_str()).failure_reason,
            AccessSiteProxySessionErrorReadableMessage(
                AccessSiteProxySessionError::kBusy));
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  EXPECT_EQ(Snapshot(host.value.c_str()).intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot(host.value.c_str()).connection,
            AccessSiteConnectionState::kReady);
  EXPECT_TRUE(Snapshot(host.value.c_str()).failure_reason.empty());
}
#endif

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
TEST_F(AccessSiteProxyProductTest, DualSelectionReleasesPaidOnlyAfterCommittedLocalOff) {
  Enable();
  ASSERT_TRUE(transport_->SetAccountNodeSites({"news.example"}, "", 0, ""));
  base::test::TestFuture<bool> retry;
  bubble_->RequestFailedNavigationProxy(GURL("https://news.example./"), retry.GetCallback());
  ASSERT_FALSE(retry.Get());
  EXPECT_TRUE(Snapshot().toggle_on);
  bool disabled = false;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, false, base::BindLambdaForTesting([&] {
    EXPECT_FALSE(Snapshot().toggle_on);
    EXPECT_TRUE(transport_->SetAccountNodeSites({}, "", 0, ""));
    disabled = true;
  })));
  EXPECT_FALSE(disabled);
  ASSERT_TRUE(base::test::RunUntil([&] { return disabled; }));
  EXPECT_TRUE(transport_->AccountSites().empty());
  EXPECT_FALSE(Snapshot().toggle_on);
}
#endif

TEST_F(AccessSiteProxyProductTest, ToolbarEnableCompletesOnlyAfterAcknowledgement) {
  context_.hold_next = true;
  base::test::TestFuture<void> ready;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true,
                                              ready.GetCallback()));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  EXPECT_FALSE(ready.IsReady());
  context_.Reply(true);
  ASSERT_TRUE(ready.Wait());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
}

TEST_F(AccessSiteProxyProductTest, RecoveryTimesOutWhileCandidateAckIsStillHeld) {
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));

  auto& timer = AccessServiceCoordinatorTestPeer::PublicationTimer(coordinator_);
  ASSERT_TRUE(timer.IsRunning());
  timer.FireNow();
  // The store restores its committed state on a worker. Hold the original
  // NetworkContext reply through both publication deadlines.
  ASSERT_TRUE(base::test::RunUntil([&] { return timer.IsRunning(); }));
  EXPECT_FALSE(coordinator_->CanDispatch(owner_));
  timer.FireNow();
  WaitFor(AccessSiteConnectionState::kUnavailable);
  EXPECT_TRUE(coordinator_->CanDispatch(owner_));
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);

  context_.Reply(true);
  environment_.RunUntilIdle();
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  Enable();
}

TEST_F(AccessSiteProxyProductTest, CandidateReadFailureRecoversPreparedWithoutRestart) {
  AccessServiceCoordinatorTestPeer::SetFailure(coordinator_,
      AccessRuleStore::FailurePointForTesting::kBuildPreparedCandidateBeforeRead);
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
  WaitFor(AccessSiteConnectionState::kUnavailable);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  AccessServiceCoordinatorTestPeer::SetFailure(coordinator_,
      AccessRuleStore::FailurePointForTesting::kNone);
  // The next operation must prepare and commit in the same Profile/process.
  // An orphaned durable PREPARED would instead report pending_mutation_exists.
  Enable();
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
}

TEST_F(AccessSiteProxyProductTest, OnOffOnUsesRealCoreAndCommittedRevision) {
  uint64_t previous = 0;
  for (bool enabled : {true, false, true}) {
    context_.hold_next = true;
    ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, enabled));
    ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kConnecting);
    context_.Reply(true);
    WaitFor(enabled ? AccessSiteConnectionState::kReady : AccessSiteConnectionState::kIdle);
    EXPECT_TRUE(Snapshot().failure_reason.empty());
    const auto stored = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_TRUE(stored.value);
    EXPECT_GT(stored.value->policy_generation, previous);
    previous = stored.value->policy_generation;
    EXPECT_EQ(Snapshot().intent, enabled ? AccessSiteIntent::kAllow : AccessSiteIntent::kOff);
  }
}

TEST_F(AccessSiteProxyProductTest,
       DevPreviewDoesNotRewriteCommittedWebsiteIntentOrConnection) {
  const bool admitted = owner_.channel == aegis_access::ChannelNamespace::kDev ||
                        owner_.channel == aegis_access::ChannelNamespace::kAlpha;
  EXPECT_EQ(bubble_->IsDevAllowBlockPanelRegistered(), admitted);
  Enable();
  const auto stored = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(stored.value);
  const auto endpoint = transport_->CaptureSelectedProxyEndpoint(
      owner_, AccessSiteProxyBubbleController::LocalProxyGroup(owner_), "news.example");
  ASSERT_TRUE(endpoint);
  for (const auto action : {AccessDevAllowBlockAction::kOff,
                            AccessDevAllowBlockAction::kBlock}) {
    ASSERT_EQ(bubble_->ApplyDevAllowBlock("news.example", action).status,
              admitted ? AccessDevAllowBlockApplyStatus::kApplied
                       : AccessDevAllowBlockApplyStatus::kChannelNotAdmitted);
    const auto preview = bubble_->DevAllowBlockSnapshotForCommittedUrl(
        GURL("https://news.example"));
    EXPECT_EQ(preview.panel_registered, admitted);
    if (admitted) {
      EXPECT_EQ(preview.intent, action == AccessDevAllowBlockAction::kOff
                    ? AccessSiteIntent::kOff : AccessSiteIntent::kBlock);
    }
    EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
    EXPECT_TRUE(Snapshot().toggle_on);
    EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
    const auto current = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
    ASSERT_TRUE(current.value);
    EXPECT_EQ(current.value->policy_generation, stored.value->policy_generation);
    const auto selected = transport_->CaptureSelectedProxyEndpoint(
        owner_, endpoint->proxy_group_id, "news.example");
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->registration_id, endpoint->registration_id);
  }
}

TEST_F(AccessSiteProxyProductTest, FailedAckPreservesOffAndExplicitRecoveryRepublishes) {
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(false);
  WaitFor(AccessSiteConnectionState::kUnavailable);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  ASSERT_TRUE(coordinator_->has_retained_store());
  Enable();
  ASSERT_TRUE(bubble_->NotifyHostProxyRuntimeFault({"news.example"}, false));
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
  Enable();
}

TEST_F(AccessSiteProxyProductTest, RetainedProductStoreSurvivesPrepareFailure) {
  auto plan = BuildSiteProxySessionMutationPlan(
      {"news.example"}, AccessSiteIntent::kAllow, owner_,
      "dev-authorized-local-node", "invalid-stale-revision", 99);
  ASSERT_TRUE(plan.mutation.request);
  base::test::TestFuture<AccessMutationTransactionResult> failed;
  coordinator_->CommitSiteGroupMutation(nullptr, std::move(*plan.mutation.request),
                                       plan.selector, failed.GetCallback());
  EXPECT_EQ(failed.Get().status, AccessMutationTransactionStatus::kPrepareFailed);
  ASSERT_TRUE(coordinator_->has_retained_store());
  Enable();
}

TEST_F(AccessSiteProxyProductTest, CanDisableAfterEndpointBecomesUnavailable) {
  Enable();
  ASSERT_TRUE(transport_->ClearProxySelection({}));
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, false));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(true);
  WaitFor(AccessSiteConnectionState::kIdle);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  const auto stored = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(stored.value);
  ASSERT_EQ(stored.value->site_groups.size(), 1u);
  EXPECT_EQ(stored.value->site_groups[0].members[0].policy.mode, AccessMode::kDirect);
}

TEST_F(AccessSiteProxyProductTest, HostFailuresRemainIndependent) {
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(false);
  WaitFor(AccessSiteConnectionState::kUnavailable);
  const auto failed = Snapshot();
  ASSERT_FALSE(failed.failure_reason.empty());
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"other.example"}, false));
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(true);
  WaitFor(AccessSiteConnectionState::kIdle, "other.example");
  EXPECT_EQ(Snapshot().connection, failed.connection);
  EXPECT_EQ(Snapshot().failure_reason, failed.failure_reason);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
}

TEST_F(AccessSiteProxyProductTest, SubdomainEnablePersistsCanonicalSites) {
  context_.hold_next = true;
  ASSERT_TRUE(bubble_->RequestHostProxySession({"www.example.com"}, true));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return context_.has_reply() ||
           Snapshot("www.example.com").connection ==
               AccessSiteConnectionState::kUnavailable;
  }));
  ASSERT_TRUE(context_.has_reply())
      << Snapshot("www.example.com").failure_reason;
  context_.Reply(true);
  WaitFor(AccessSiteConnectionState::kReady, "www.example.com");
  const auto stored =
      coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(stored.value);
  ASSERT_EQ(stored.value->site_groups.size(), 1u);
  const auto& group = stored.value->site_groups[0];
  EXPECT_EQ(group.group.canonical_host, "example.com");
  EXPECT_TRUE(group.group.website_context);
  EXPECT_EQ(group.group.http_top_level_site, "http://example.com");
  EXPECT_EQ(group.group.https_top_level_site, "https://example.com");
  ASSERT_EQ(group.members.size(), 2u);
  for (const auto& member : group.members) {
    EXPECT_EQ(member.policy.destination_host, "example.com");
    EXPECT_EQ(member.policy.scope, PolicyScope::kWebsite);
    EXPECT_FALSE(member.policy.include_subdomains);
  }
}

TEST_F(AccessSiteProxyProductTest, RestoredCoreWaitsForAckWithoutChangingSavedIntent) {
  Enable();
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  const auto previous = transport_->CurrentEndpoint(owner_);
  ASSERT_TRUE(previous);
  AccessSiteProxyProductTestPeer::LoseRuntime(bubble_, previous->registration_id);
  environment_.RunUntilIdle();
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  EXPECT_FALSE(transport_->CurrentEndpoint(owner_));

  context_.hold_next = true;
  base::test::TestFuture<bool> first, second;
  bubble_->EnsureLocalCoreForRequest(owner_, previous->proxy_group_id, first.GetCallback());
  bubble_->EnsureLocalCoreForRequest(owner_, previous->proxy_group_id, second.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(second.IsReady());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kConnecting);
  const auto prepared = transport_->CurrentEndpoint(owner_);
  ASSERT_TRUE(prepared);
  EXPECT_NE(prepared->registration_id, previous->registration_id);
  EXPECT_GT(prepared->generations.selection_generation,
            previous->generations.selection_generation);
  context_.Reply(true);
  ASSERT_TRUE(first.Get());
  ASSERT_TRUE(second.Get());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(after.value->policy_generation, before.value->policy_generation);
  EXPECT_EQ(after.value->site_groups, before.value->site_groups);
}

TEST_F(AccessSiteProxyProductTest, FailedRestoreAckRetainsAllowAndCanRecover) {
  Enable();
  const auto endpoint = transport_->CurrentEndpoint(owner_);
  ASSERT_TRUE(endpoint);
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  AccessSiteProxyProductTestPeer::LoseRuntime(bubble_, endpoint->registration_id);
  environment_.RunUntilIdle();
  context_.hold_next = true;
  base::test::TestFuture<bool> rejected;
  bubble_->EnsureLocalCoreForRequest(owner_, endpoint->proxy_group_id, rejected.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
  context_.Reply(false);
  EXPECT_FALSE(rejected.Get());
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  base::test::TestFuture<bool> recovered;
  bubble_->EnsureLocalCoreForRequest(owner_, endpoint->proxy_group_id, recovered.GetCallback());
  EXPECT_TRUE(recovered.Get());
  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(before.value->site_groups, after.value->site_groups);
  EXPECT_EQ(before.value->policy_generation, after.value->policy_generation);
}

TEST_F(AccessSiteProxyProductTest, RequestReadinessRejectsForeignOwnerOrGroup) {
  EXPECT_FALSE(AccessSiteProxyProductTestPeer::HasCore(bubble_));
  auto foreign = owner_;
  foreign.profile_token += "-foreign";
  for (const auto& owner : {foreign, owner_}) {
    base::test::TestFuture<bool> denied;
    bubble_->EnsureLocalCoreForRequest(owner,
        owner == foreign ? AccessSiteProxyBubbleController::LocalProxyGroup(owner_)
                         : "unowned-group", denied.GetCallback());
    EXPECT_FALSE(denied.Get());
  }
  EXPECT_FALSE(AccessSiteProxyProductTestPeer::HasCore(bubble_));
}

TEST_F(AccessSiteProxyProductTest, CanDisableOneWebsiteWhileOtherSavedProxyHasNoCore) {
  Enable();
  ASSERT_TRUE(bubble_->RequestHostProxySession({"second.example"}, true));
  WaitFor(AccessSiteConnectionState::kReady, "second.example");
  const auto endpoint = transport_->CurrentEndpoint(owner_);
  ASSERT_TRUE(endpoint);
  AccessSiteProxyProductTestPeer::LoseRuntime(bubble_, endpoint->registration_id);
  environment_.RunUntilIdle();
  ASSERT_FALSE(transport_->CurrentEndpoint(owner_));
  ASSERT_TRUE(bubble_->RequestHostProxySession({"news.example"}, false));
  WaitFor(AccessSiteConnectionState::kIdle);
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kOff);
  EXPECT_EQ(Snapshot("second.example").intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot("second.example").connection, AccessSiteConnectionState::kUnavailable);
  const auto stored = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(stored.value);
  ASSERT_EQ(stored.value->site_groups.size(), 2u);
  EXPECT_FALSE(transport_->CurrentEndpoint(owner_));
}

TEST_F(AccessSiteProxyProductTest, ReadyUiRequiresCurrentContextAckAfterDisconnect) {
  Enable();
  context_.Disconnect();
  environment_.RunUntilIdle();
  EXPECT_EQ(Snapshot().intent, AccessSiteIntent::kAllow);
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  EXPECT_FALSE(Snapshot().failure_reason.empty());
  ProductAckContext replacement;
  replacement.incarnation = "replacement-context";
  base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
  transport_->BindPrivateContext(owner_, &replacement, bound.GetCallback());
  ASSERT_TRUE(bound.Get());
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_.get());
  const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
  ASSERT_TRUE(common);
  replacement.hold_next = true;
  base::test::TestFuture<bool> ack;
  transport_->PublishPrivatePolicy(*runtime->GetPublishedPolicySnapshot(owner_),
                                   *common, ack.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return replacement.has_reply(); }));
  EXPECT_FALSE(ack.IsReady());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kUnavailable);
  replacement.Reply(true);
  ASSERT_TRUE(ack.Get());
  EXPECT_EQ(Snapshot().connection, AccessSiteConnectionState::kReady);
}

// Unlike the helper-only tests, this fixture uses the production retry handler,
// a real pinned core, the Profile transaction and the private Mojo ACK path.
class AccessRetryNavigationProductTest : public ChromeRenderViewHostTestHarness {
 protected:
  std::unique_ptr<TestingProfile> CreateTestingProfile() override {
    return TestingProfile::Builder().SetPath(directory_.GetPath()).Build();
  }
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    ChromeRenderViewHostTestHarness::SetUp();
    // Let the RVH harness create its default test NetworkContext first.
    // TestingBrowserProcess intentionally has no SystemNetworkContextManager;
    // the Access policy ACK in this fixture belongs to context_ below.
    ProfileNetworkContextServiceFactory::GetInstance()->SetTestingFactoryAndUse(
        profile(), base::BindRepeating([](content::BrowserContext* context)
            -> std::unique_ptr<KeyedService> {
          return std::make_unique<ProfileNetworkContextService>(
              Profile::FromBrowserContext(context));
        }));
#if !BUILDFLAG(AEGIS_PRIVATE_TEST_NODE_ENABLED)
    const auto* command = base::CommandLine::ForCurrentProcess();
    if (command->GetSwitchValuePath("aegis-access-dev-core-manifest").empty() ||
        command->GetSwitchValuePath("aegis-access-dev-node-config").empty())
      GTEST_SKIP() << "Real product retry requires authorized private inputs.";
#endif
    ASSERT_TRUE(ProfileNetworkContextServiceFactory::GetForContext(profile()));
    profile()->GetPrefs()->SetDict(proxy_config::prefs::kProxy,
                                  ProxyConfigDictionary::CreateDirect());
    task_environment()->RunUntilIdle();
    network::mojom::NetworkContextParams params;
    ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile(), {}, &params));
    transport_ = AccessNetworkContextTransport::Get(profile());
    owner_ = *transport_->OwnerForPartition(AccessChannelForCurrentBuild(), {});
    coordinator_ = AccessServiceCoordinator::GetOrCreate(profile());
    base::test::TestFuture<bool> restored;
    coordinator_->InitializeForProfile(owner_, restored.GetCallback());
    ASSERT_TRUE(restored.Get());
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    ASSERT_TRUE(bound.Get());
    auto* runtime = AccessPublishedRequestRuntime::Get(profile());
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    ASSERT_TRUE(common);
    base::test::TestFuture<bool> published;
    transport_->PublishPrivatePolicy(*runtime->GetPublishedPolicySnapshot(owner_),
                                    *common, published.GetCallback());
    ASSERT_TRUE(published.Get());
    bubble_ = AccessSiteProxyBubbleController::GetOrCreate(profile());
    AccessNavigationRetryTabHelper::CreateForWebContents(web_contents());
    helper()->SetProxyRetryHandler(bubble_->GetFailedNavigationRetryHandler());
  }
  void TearDown() override {
    transport_ = nullptr;
    coordinator_ = nullptr;
    bubble_ = nullptr;
    context_.Disconnect();
    ChromeRenderViewHostTestHarness::TearDown();
  }
  AccessNavigationRetryTabHelper* helper() {
    return AccessNavigationRetryTabHelper::FromWebContents(web_contents());
  }
  void BeginHeldRetry() {
    content::NavigationSimulator::NavigateAndFailFromBrowser(
        web_contents(), failed_, net::ERR_CONNECTION_RESET);
    // The first simulated navigation creates its default NetworkContext and
    // replaces the fixture's setup-time binding. Bind and ACK that current
    // context before testing transaction completion rather than scope failure.
    task_environment()->RunUntilIdle();
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    ASSERT_TRUE(bound.Get());
    auto* runtime = AccessPublishedRequestRuntime::Get(profile());
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(owner_);
    ASSERT_TRUE(common);
    base::test::TestFuture<bool> published;
    transport_->PublishPrivatePolicy(*runtime->GetPublishedPolicySnapshot(owner_),
                                    *common, published.GetCallback());
    ASSERT_TRUE(published.Get());
    context_.hold_next = true;
    ASSERT_TRUE(helper()->RequestProxyRetry(completed_.GetCallback()));
    ASSERT_TRUE(base::test::RunUntil([&] {
      return context_.has_reply() || completed_.IsReady();
    }));
    ASSERT_TRUE(context_.has_reply())
        << bubble_->SnapshotForCommittedUrl(failed_).failure_reason;
    EXPECT_FALSE(controller().GetPendingEntry());
    EXPECT_FALSE(completed_.IsReady());
    EXPECT_EQ(bubble_->SnapshotForCommittedUrl(failed_).connection,
              AccessSiteConnectionState::kConnecting);
  }
  void DrainHeldRetry() {
    context_.Reply(true);
    // Cancellation tests must await the late transaction and core verification,
    // including an unavailable outcome if navigation changed their binding.
    ASSERT_TRUE(base::test::RunUntil([&] {
      return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
    }));
    EXPECT_NE(bubble_->SnapshotForCommittedUrl(failed_).connection,
              AccessSiteConnectionState::kConnecting);
  }
  std::unique_ptr<HeldBrowserRequest> StartPostWaitingForAck() {
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    auto client = bound.Take();
    EXPECT_TRUE(client);
    auto request = std::make_unique<HeldBrowserRequest>(
        profile(), main_rfh()->GetFrameTreeNodeId(), owner_, std::move(client));
    AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
    context_.hold_next = true;
    request->StartPost();
    EXPECT_TRUE(base::test::RunUntil([&] { return context_.has_reply(); }));
    EXPECT_EQ(request->target.starts, 0);
    return request;
  }
  std::unique_ptr<HeldRedirectBrowserRequest> StartHeldRedirect(bool proxy) {
    content::NavigationSimulator::NavigateAndCommitFromBrowser(
        web_contents(), GURL("https://news.example"));
    if (proxy) {
      EXPECT_TRUE(bubble_->RequestHostProxySession({"news.example"}, true));
      EXPECT_TRUE(base::test::RunUntil([&] {
        return bubble_->SnapshotForCommittedUrl(GURL("https://news.example"))
                   .connection == AccessSiteConnectionState::kReady;
      }));
    }
    auto metadata = BuildBrowserOwnedRequestMetadata(profile(),
        base::BindRepeating([](content::WebContents* contents) { return contents; },
                            base::Unretained(web_contents())),
        main_rfh()->GetFrameTreeNodeId(), std::nullopt);
    CHECK(metadata.metadata);
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    auto request = std::make_unique<HeldRedirectBrowserRequest>(
        profile(), main_rfh(), owner_, std::move(*metadata.metadata), bound.Take());
    request->Start();
    EXPECT_TRUE(base::test::RunUntil([&] {
      return request->client.has_received_redirect() ||
             request->client.has_received_completion();
    }));
    return request;
  }
  void SaveRedirectWebsiteAndStopCore() {
    const GURL target("https://cdn.example/redirect-final");
    ASSERT_TRUE(bubble_->RequestHostProxySession({"cdn.example"}, true));
    ASSERT_TRUE(base::test::RunUntil([&] {
      return bubble_->SnapshotForCommittedUrl(target).connection ==
             AccessSiteConnectionState::kReady;
    }));
    const auto endpoint = transport_->CurrentEndpoint(owner_);
    ASSERT_TRUE(endpoint);
    AccessSiteProxyProductTestPeer::LoseRuntime(bubble_, endpoint->registration_id);
    task_environment()->RunUntilIdle();
    ASSERT_FALSE(AccessSiteProxyProductTestPeer::HasCore(bubble_));
    ASSERT_FALSE(transport_->CurrentEndpoint(owner_));
    ASSERT_EQ(bubble_->SnapshotForCommittedUrl(target).intent,
              AccessSiteIntent::kAllow);
  }
  std::unique_ptr<HeldRedirectBrowserRequest> StartPendingNavigationRedirect(
      content::NavigationSimulator* navigation) {
    navigation->Start();
    const int64_t navigation_id =
        navigation->GetNavigationHandle()->GetNavigationId();
    auto metadata = BuildBrowserOwnedRequestMetadata(profile(),
        base::BindRepeating([](content::WebContents* contents) { return contents; },
                            base::Unretained(web_contents())),
        main_rfh()->GetFrameTreeNodeId(), navigation_id);
    CHECK(metadata.metadata);
    base::test::TestFuture<AccessNetworkContextTransport::PrivateContext> bound;
    transport_->BindPrivateContext(owner_, &context_, bound.GetCallback());
    auto request = std::make_unique<HeldRedirectBrowserRequest>(
        profile(), main_rfh(), owner_, std::move(*metadata.metadata), bound.Take(),
        navigation_id);
    request->Start();
    EXPECT_TRUE(base::test::RunUntil([&] {
      return request->client.has_received_redirect() ||
             request->client.has_received_completion();
    }));
    if (request->client.has_received_redirect())
      navigation->Redirect(GURL("https://cdn.example/redirect-final"));
    return request;
  }
  base::ScopedTempDir directory_;
  ProductAckContext context_;
  raw_ptr<AccessNetworkContextTransport> transport_ = nullptr;
  raw_ptr<AccessServiceCoordinator> coordinator_ = nullptr;
  raw_ptr<AccessSiteProxyBubbleController> bubble_ = nullptr;
  OwnershipKey owner_;
  const GURL failed_{"https://failed.example/path?q=original#fragment"};
  base::test::TestFuture<bool> completed_;
};

TEST_F(AccessRetryNavigationProductTest,
       OffDuringCommittedVerificationRevokesRetryAndPersistsOff) {
  BeginHeldRetry();
  bool accepted = false;
  DisableDuringCommittedVerification disable(base::BindLambdaForTesting([&] {
    return bubble_->SnapshotForCommittedUrl(failed_).intent ==
               AccessSiteIntent::kAllow &&
           AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }), base::BindLambdaForTesting([&] {
    EXPECT_EQ(bubble_->SnapshotForCommittedUrl(failed_).intent,
              AccessSiteIntent::kAllow);
    EXPECT_TRUE(AccessSiteProxyProductTestPeer::IsPreparing(bubble_));
    const auto host = ResolveTrustedNavigationHost(failed_);
    ASSERT_TRUE(host.host);
    accepted = bubble_->RequestHostProxySession(*host.host, false);
  }));
  context_.Reply(true);
  ASSERT_TRUE(completed_.Wait());
  ASSERT_TRUE(disable.disabled())
      << bubble_->SnapshotForCommittedUrl(failed_).failure_reason;
  EXPECT_TRUE(accepted);
  EXPECT_FALSE(completed_.Get());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !AccessSiteProxyProductTestPeer::IsPreparing(bubble_);
  }));
  // Stop runs on the core sequence after Verify. Its reply reaches UI after
  // that older Verify result, including when Stop must cancel a live probe.
  base::test::TestFuture<void> stopped;
  AccessSiteProxyProductTestPeer::StopCore(bubble_, stopped.GetCallback());
  ASSERT_TRUE(stopped.Wait());
  EXPECT_FALSE(controller().GetPendingEntry());
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), failed_);
  const auto state = bubble_->SnapshotForCommittedUrl(failed_);
  EXPECT_EQ(state.intent, AccessSiteIntent::kOff);
  EXPECT_EQ(state.connection, AccessSiteConnectionState::kIdle);
  EXPECT_TRUE(state.failure_reason.empty());
}

TEST_F(AccessRetryNavigationProductTest, NewNavigationBeforeRealAckIsNotReloaded) {
  BeginHeldRetry();
  const GURL replacement("https://replacement.example/keep");
  content::NavigationSimulator::NavigateAndCommitFromBrowser(web_contents(), replacement);
  const int entries = controller().GetEntryCount();
  DrainHeldRetry();
  EXPECT_FALSE(controller().GetPendingEntry());
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), replacement);
  EXPECT_EQ(controller().GetEntryCount(), entries);
}

TEST_F(AccessRetryNavigationProductTest, ClosedTabBeforeRealAckDoesNotReloadOtherTab) {
  BeginHeldRetry();
  auto other = CreateTestWebContents();
  const GURL replacement("https://other.example/unchanged");
  content::NavigationSimulator::NavigateAndCommitFromBrowser(other.get(), replacement);
  DeleteContents();
  const bool replied_on_close = completed_.IsReady();
  DrainHeldRetry();
  EXPECT_TRUE(replied_on_close);
  ASSERT_TRUE(completed_.IsReady());
  EXPECT_FALSE(completed_.Get());
  EXPECT_FALSE(other->GetController().GetPendingEntry());
  EXPECT_EQ(other->GetLastCommittedURL(), replacement);
}

TEST_F(AccessRetryNavigationProductTest, ImmediateReclickWaitsForItsOwnAck) {
  BeginHeldRetry();
  web_contents()->Stop();
  ASSERT_TRUE(completed_.IsReady());
  EXPECT_FALSE(completed_.Get());
  base::test::TestFuture<bool> fresh;
  ASSERT_TRUE(helper()->RequestProxyRetry(fresh.GetCallback()));
  ASSERT_FALSE(fresh.IsReady());
  context_.hold_next = true;
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return context_.has_reply() || fresh.IsReady();
  }));
  ASSERT_TRUE(context_.has_reply());
  EXPECT_FALSE(fresh.IsReady());
  EXPECT_FALSE(controller().GetPendingEntry());
  context_.Reply(true);
  ASSERT_TRUE(fresh.Get());
  ASSERT_TRUE(controller().GetPendingEntry());
  EXPECT_EQ(controller().GetPendingEntry()->GetURL(), failed_);
  content::NavigationSimulator::CreateFromPending(controller())->Commit();
  EXPECT_FALSE(controller().GetPendingEntry());
}

TEST_F(AccessRetryNavigationProductTest, SecondStopRevokesQueuedReclick) {
  BeginHeldRetry();
  web_contents()->Stop();
  base::test::TestFuture<bool> fresh;
  ASSERT_TRUE(helper()->RequestProxyRetry(fresh.GetCallback()));
  ASSERT_FALSE(fresh.IsReady());
  web_contents()->Stop();
  ASSERT_TRUE(fresh.IsReady());
  EXPECT_FALSE(fresh.Get());
  DrainHeldRetry();
  EXPECT_FALSE(controller().GetPendingEntry());
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), failed_);
}

TEST_F(AccessRetryNavigationProductTest, StopBeforeRealAckRevokesOriginalRetry) {
  BeginHeldRetry();
  web_contents()->Stop();
  const bool replied_on_stop = completed_.IsReady();
  EXPECT_FALSE(helper()->FailureForRetry());
  DrainHeldRetry();
  EXPECT_TRUE(replied_on_stop);
  ASSERT_TRUE(completed_.IsReady());
  EXPECT_FALSE(completed_.Get());
  EXPECT_FALSE(controller().GetPendingEntry());
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), failed_);

  base::test::TestFuture<bool> fresh_click;
  ASSERT_TRUE(helper()->RequestProxyRetry(fresh_click.GetCallback()));
  ASSERT_TRUE(fresh_click.Get());
  ASSERT_TRUE(controller().GetPendingEntry());
  EXPECT_EQ(controller().GetPendingEntry()->GetURL(), failed_);
  content::NavigationSimulator::CreateFromPending(controller())->Commit();
  EXPECT_FALSE(helper()->FailureForRetry());
  EXPECT_FALSE(controller().GetPendingEntry());
}

TEST_F(AccessRetryNavigationProductTest,
       DurableCommitFailureAfterRealAckCannotReloadAndCanRecover) {
  BeginHeldRetry();
  const auto before = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(before.value);
  AccessServiceCoordinatorTestPeer::SetFailure(coordinator_,
      AccessRuleStore::FailurePointForTesting::kCommitAfterFirstMemberInsert);
  context_.Reply(true);
  ASSERT_TRUE(completed_.Wait());
  EXPECT_FALSE(completed_.Get());
  EXPECT_FALSE(controller().GetPendingEntry());
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), failed_);
  const auto snapshot = bubble_->SnapshotForCommittedUrl(failed_);
  EXPECT_EQ(snapshot.intent, AccessSiteIntent::kOff);
  EXPECT_EQ(snapshot.connection, AccessSiteConnectionState::kUnavailable);
  const auto after = coordinator_->ReadCommittedSnapshot(owner_.storage_partition_token);
  ASSERT_TRUE(after.value);
  EXPECT_EQ(after.value->site_groups, before.value->site_groups);
  EXPECT_EQ(after.value->independent_rules, before.value->independent_rules);

  AccessServiceCoordinatorTestPeer::SetFailure(coordinator_,
      AccessRuleStore::FailurePointForTesting::kNone);
  base::test::TestFuture<bool> recovered;
  ASSERT_TRUE(helper()->RequestProxyRetry(recovered.GetCallback()));
  ASSERT_TRUE(recovered.Get());
  ASSERT_TRUE(controller().GetPendingEntry());
  EXPECT_EQ(controller().GetPendingEntry()->GetURL(), failed_);
  content::NavigationSimulator::CreateFromPending(controller())->Commit();
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), failed_);
  base::test::TestFuture<bool> duplicate;
  EXPECT_FALSE(helper()->RequestProxyRetry(duplicate.GetCallback()));
  EXPECT_FALSE(duplicate.Get());
  EXPECT_FALSE(controller().GetPendingEntry());
}

TEST_F(AccessRetryNavigationProductTest,
       CancelledLoaderBeforePrivateAckNeverSendsPost) {
  auto request = StartPostWaitingForAck();
  request->loader.reset();
  // Release immediately: dispatch must also notice a closed peer before the
  // queued disconnect notification runs.
  context_.Reply(true);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(request->target.starts, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       CancelledClientBeforePrivateAckNeverSendsPost) {
  auto request = StartPostWaitingForAck();
  request->client_receiver.reset();
  context_.Reply(true);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(request->target.starts, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       LivePostWaitsForPrivateAckThenSendsOnce) {
  auto request = StartPostWaitingForAck();
  context_.Reply(true);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(request->target.starts, 1);
  EXPECT_EQ(request->target.method, "POST");
}

TEST_F(AccessRetryNavigationProductTest,
       CancelledPendingRequestReleasesFactoryBeforeAck) {
  auto request = StartPostWaitingForAck();
  request->factory.reset();
  request->loader.reset();
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->wrapper);
  context_.Reply(true);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(request->target.starts, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       RedirectToSavedWebsitePreparesMissingCoreAndWaitsForAck) {
  SaveRedirectWebsiteAndStopCore();
  auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
      GURL("https://news.example/redirect"), web_contents());
  auto request = StartPendingNavigationRedirect(navigation.get());
  ASSERT_TRUE(request->client.has_received_redirect());
  ASSERT_TRUE(request->target.initial);
  EXPECT_TRUE(request->target.initial->registration_id.empty());
  EXPECT_FALSE(AccessSiteProxyProductTestPeer::HasCore(bubble_));

  context_.hold_next = true;
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return context_.has_reply() || request->client.has_received_completion();
  }));
  ASSERT_TRUE(context_.has_reply())
      << "Redirect completed before preparing its saved proxy: "
      << request->client.completion_status().error_code;
  EXPECT_FALSE(request->client.has_received_completion());
  EXPECT_FALSE(request->target.followed);
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return request->client.has_received_completion();
  }));
  EXPECT_EQ(request->client.completion_status().error_code, net::OK);
  ASSERT_TRUE(request->target.followed);
  EXPECT_FALSE(request->target.followed->registration_id.empty());
  EXPECT_EQ(request->target.followed->hop, 1u);
  EXPECT_EQ(request->target.followed->request_token,
            request->target.initial->request_token);
  EXPECT_EQ(request->target.public_follows, 0);
  navigation->Commit();
}

TEST_F(AccessRetryNavigationProductTest,
       SavedWebsiteRedirectCancelledDuringCoreAckIsNotFollowed) {
  SaveRedirectWebsiteAndStopCore();
  auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
      GURL("https://news.example/redirect"), web_contents());
  auto request = StartPendingNavigationRedirect(navigation.get());
  ASSERT_TRUE(request->client.has_received_redirect());
  context_.hold_next = true;
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return context_.has_reply() || request->client.has_received_completion();
  }));
  ASSERT_TRUE(context_.has_reply());
  request->loader.reset();
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return bubble_->SnapshotForCommittedUrl(
               GURL("https://cdn.example/redirect-final")).connection ==
           AccessSiteConnectionState::kReady;
  }));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->target.followed);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       SavedWebsiteRedirectRejectsNetworkChangeDuringCoreAck) {
  SaveRedirectWebsiteAndStopCore();
  auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
      GURL("https://news.example/redirect"), web_contents());
  auto request = StartPendingNavigationRedirect(navigation.get());
  ASSERT_TRUE(request->client.has_received_redirect());
  context_.hold_next = true;
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return context_.has_reply() || request->client.has_received_completion();
  }));
  ASSERT_TRUE(context_.has_reply());
  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  context_.Reply(true);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return request->client.has_received_completion();
  }));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return bubble_->SnapshotForCommittedUrl(
               GURL("https://cdn.example/redirect-final")).connection !=
           AccessSiteConnectionState::kConnecting;
  }));
  EXPECT_NE(request->client.completion_status().error_code, net::OK);
  EXPECT_FALSE(request->target.followed);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       RedirectLoaderCancelledAtQueuedReadyIsNotFollowed) {
  auto request = StartHeldRedirect(true);
  ASSERT_TRUE(request->client.has_received_redirect());
  ASSERT_TRUE(request->target.initial);
  ASSERT_FALSE(request->target.initial->registration_id.empty());
  RunAtQueuedPolicyCompletion cancel(base::BindOnce(
      [](HeldRedirectBrowserRequest* request) { request->loader.reset(); },
      base::Unretained(request.get())));
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] { return cancel.ran(); }));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->target.followed);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       RedirectClientCancelledAtQueuedReadyIsNotFollowed) {
  auto request = StartHeldRedirect(true);
  ASSERT_TRUE(request->client.has_received_redirect());
  ASSERT_TRUE(request->target.initial);
  ASSERT_FALSE(request->target.initial->registration_id.empty());
  RunAtQueuedPolicyCompletion cancel(base::BindOnce(
      [](HeldRedirectBrowserRequest* request) { request->client.Unbind(); },
      base::Unretained(request.get())));
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] { return cancel.ran(); }));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->target.followed);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       NativeRedirectSurvivesPrivateFactoryAdmissionReset) {
  auto request = StartHeldRedirect(false);
  ASSERT_TRUE(request->client.has_received_redirect());
  ASSERT_TRUE(request->target.initial);
  EXPECT_TRUE(request->target.initial->registration_id.empty());
  request->target.ResetAdmission();
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->factory.is_connected());
  ASSERT_TRUE(request->wrapper);
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return request->client.has_received_completion();
  }));
  EXPECT_EQ(request->client.completion_status().error_code, net::OK);
  ASSERT_TRUE(request->target.followed);
  EXPECT_TRUE(request->target.followed->registration_id.empty());
  EXPECT_EQ(request->target.followed->hop, 1u);
  EXPECT_EQ(request->target.followed->request_token,
            request->target.initial->request_token);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       ProxyRedirectSurvivesPrivateFactoryAdmissionReset) {
  auto request = StartHeldRedirect(true);
  ASSERT_TRUE(request->client.has_received_redirect());
  ASSERT_TRUE(request->target.initial);
  ASSERT_FALSE(request->target.initial->registration_id.empty());
  request->target.ResetAdmission();
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(request->factory.is_connected());
  ASSERT_TRUE(request->wrapper);
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return request->client.has_received_completion();
  }));
  EXPECT_EQ(request->client.completion_status().error_code, net::OK);
  ASSERT_TRUE(request->target.followed);
  EXPECT_EQ(request->target.followed->registration_id,
            request->target.initial->registration_id);
  EXPECT_EQ(request->target.followed->hop, 1u);
  EXPECT_EQ(request->target.followed->request_token,
            request->target.initial->request_token);
  EXPECT_EQ(request->target.public_follows, 0);
}

TEST_F(AccessRetryNavigationProductTest,
       RedirectAfterAdmissionResetStillRejectsChangedNetworkEpoch) {
  auto request = StartHeldRedirect(true);
  ASSERT_TRUE(request->client.has_received_redirect());
  request->target.ResetAdmission();
  task_environment()->RunUntilIdle();
  AccessNetworkContextTransportTestPeer::AdvanceNetwork(transport_);
  task_environment()->RunUntilIdle();
  request->loader->FollowRedirect({}, std::nullopt);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return request->client.has_received_completion();
  }));
  EXPECT_NE(request->client.completion_status().error_code, net::OK);
  EXPECT_FALSE(request->target.followed);
  EXPECT_EQ(request->target.public_follows, 0);
}

}  // namespace
}  // namespace aegis::access
