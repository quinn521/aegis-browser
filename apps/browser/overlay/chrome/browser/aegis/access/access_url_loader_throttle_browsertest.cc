// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_url_loader_throttle.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/functional/bind.h"
#include "chrome/browser/aegis/access/access_identity_generation_source.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_proxy_selection_generation_source.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {

constexpr char kProxyGroup[] = "browser-test-proxy-group";
constexpr char kTargetHost[] = "target.example";
constexpr char kLoopbackHost[] = "127.0.0.1";

std::unique_ptr<net::test_server::HttpResponse> HandleOriginRequest(
    std::atomic<size_t>* counter,
    const net::test_server::HttpRequest& request) {
  if (request.method != net::test_server::METHOD_GET) {
    return nullptr;
  }
  counter->fetch_add(1, std::memory_order_relaxed);
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content("<html><body>origin</body></html>");
  response->set_content_type("text/html");
  return response;
}

std::unique_ptr<net::test_server::HttpResponse> HandleProxyRequest(
    std::atomic<size_t>* counter,
    const net::test_server::HttpRequest& request) {
  if (request.method != net::test_server::METHOD_GET) {
    return nullptr;
  }
  counter->fetch_add(1, std::memory_order_relaxed);
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content("<html><body>proxy</body></html>");
  response->set_content_type("text/html");
  return response;
}

class AccessURLLoaderThrottleBrowserTest : public InProcessBrowserTest {
 public:
  AccessURLLoaderThrottleBrowserTest()
      : origin_(net::test_server::EmbeddedTestServer::TYPE_HTTP),
        proxy_(net::test_server::EmbeddedTestServer::TYPE_HTTP) {}
  ~AccessURLLoaderThrottleBrowserTest() override = default;

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();

    origin_.RegisterRequestHandler(base::BindRepeating(
        &HandleOriginRequest, base::Unretained(&origin_requests_)));
    proxy_.RegisterRequestHandler(base::BindRepeating(
        &HandleProxyRequest, base::Unretained(&proxy_requests_)));
    ASSERT_TRUE(origin_.Start());
    ASSERT_TRUE(proxy_.Start());
    host_resolver()->AddRule(kTargetHost, kLoopbackHost);

    profile_ = browser()->profile();
    ASSERT_NE(profile_, nullptr);
    transport_ = AccessNetworkContextTransport::Get(profile_);
    ASSERT_NE(transport_, nullptr)
        << "the real Profile NetworkContext must own the Access transport";
  }

  void TearDownOnMainThread() override {
    EXPECT_TRUE(proxy_.ShutdownAndWaitUntilComplete());
    EXPECT_TRUE(origin_.ShutdownAndWaitUntilComplete());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  aegis_access::OwnershipKey DefaultOwner() {
    const auto owner = transport_->OwnerForPartition(
        aegis_access::ChannelNamespace::kDev, base::FilePath());
    CHECK(owner.has_value());
    return *owner;
  }

  StoredPolicySnapshot ProxyPolicy(
      const aegis_access::OwnershipKey& owner) const {
    StoredAccessRule rule;
    rule.policy.rule_id = "browser-test-rule";
    rule.policy.owner = owner;
    rule.policy.scope = aegis_access::PolicyScope::kProfile;
    rule.policy.destination_host = kTargetHost;
    rule.policy.include_subdomains = false;
    rule.policy.schemes = {aegis_access::RequestScheme::kHttp};
    rule.policy.ports.scope =
        aegis_access::PortScope::kAllBrowserPermitted;
    rule.policy.mode = aegis_access::AccessMode::kProxy;
    rule.policy.proxy_group_id = kProxyGroup;
    rule.policy.protection_override =
        aegis_access::ProtectionOverride::kNone;
    rule.policy.row_revision = 1;
    rule.policy.last_operation_sequence = 1;
    rule.source = StoredRuleSource::kTestFixture;
    rule.lifetime = StoredRuleLifetime::kProfileSession;

    StoredPolicySnapshot snapshot;
    snapshot.owner = owner;
    snapshot.policy_generation = 1;
    snapshot.independent_rules.push_back(std::move(rule));
    return snapshot;
  }

  aegis_access::GenerationTuple PublishReadyAccessState(
      bool stale_endpoint_generation) {
    const aegis_access::OwnershipKey owner = DefaultOwner();

    AccessIdentityGenerationSource* identity =
        AccessIdentityGenerationSource::GetOrCreate(profile_);
    CHECK(identity);
    const auto identity_result = identity->CommitIdentity(
        {aegis_access::AccessIdentityKind::kInstallationGuest,
         "browser-test-guest", "browser-test-entitlement", "dev",
         "browser-test-realm"});
    EXPECT_EQ(identity_result.status,
              aegis_access::IdentityGenerationCommitStatus::kCommitted);

    AccessProxySelectionGenerationSource* selection =
        AccessProxySelectionGenerationSource::GetOrCreate(profile_);
    CHECK(selection);
    const auto selection_result = selection->CommitSelection(
        {kProxyGroup, "endpoint-local", "lease-local", "assignment-local", 1});
    EXPECT_EQ(selection_result.status,
              aegis_access::ProxySelectionGenerationCommitStatus::kCommitted);

    AccessPublishedRequestRuntime* runtime =
        AccessPublishedRequestRuntime::GetOrCreate(profile_);
    CHECK(runtime);
    const auto publication =
        runtime->PublishCommittedPolicySnapshot(ProxyPolicy(owner));
    EXPECT_EQ(publication.status, AccessPolicyPublicationStatus::kPublished);

    ProfileNetworkContextService* network_service =
        ProfileNetworkContextServiceFactory::GetForContext(profile_);
    CHECK(network_service);
    EXPECT_GT(network_service->GetAegisBaseProxyConfigGeneration(), 0u);

    const auto tuple_result =
        runtime->BuildProxyGenerationTuple(owner, kProxyGroup);
    CHECK(tuple_result.generations.has_value());
    const aegis_access::GenerationTuple current = *tuple_result.generations;

    aegis_access::GenerationTuple endpoint_generations = current;
    if (stale_endpoint_generation) {
      ++endpoint_generations.policy_generation;
    }
    const aegis_access::RegisteredProxyEndpoint endpoint{
        "browser-test-registration",
        kProxyGroup,
        owner,
        endpoint_generations,
        aegis_access::RegisteredProxyTransport::kHttp,
        kLoopbackHost,
        static_cast<uint16_t>(proxy_.port())};
    CHECK(transport_->PublishProxySelection(
        base::FilePath(), {kTargetHost}, endpoint));
    transport_->FlushClientsForTesting(base::FilePath());
    return current;
  }

  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<AccessNetworkContextTransport> transport_ = nullptr;
  std::atomic<size_t> origin_requests_{0};
  std::atomic<size_t> proxy_requests_{0};
  net::test_server::EmbeddedTestServer origin_;
  net::test_server::EmbeddedTestServer proxy_;
};

IN_PROC_BROWSER_TEST_F(AccessURLLoaderThrottleBrowserTest,
                       OffKeepsRealBrowserOnNativePath) {
  const GURL target = origin_.GetURL(kTargetHost, "/native");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), target));
  EXPECT_EQ("origin",
            content::EvalJs(web_contents(), "document.body.textContent"));
  EXPECT_EQ(origin_requests_.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(proxy_requests_.load(std::memory_order_relaxed), 0u);
}

IN_PROC_BROWSER_TEST_F(AccessURLLoaderThrottleBrowserTest,
                       ExactHostProxyRequestReachesLocalProxy) {
  PublishReadyAccessState(false);

  const GURL target = origin_.GetURL(kTargetHost, "/selected");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), target));
  EXPECT_EQ("proxy",
            content::EvalJs(web_contents(), "document.body.textContent"));
  EXPECT_EQ(origin_requests_.load(std::memory_order_relaxed), 0u);
  EXPECT_GE(proxy_requests_.load(std::memory_order_relaxed), 1u);
}

IN_PROC_BROWSER_TEST_F(AccessURLLoaderThrottleBrowserTest,
                       StalePublishedEndpointFailsBeforeNetworkSend) {
  PublishReadyAccessState(true);

  const GURL target = origin_.GetURL(kTargetHost, "/stale");
  EXPECT_FALSE(ui_test_utils::NavigateToURL(browser(), target));
  EXPECT_EQ(origin_requests_.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(proxy_requests_.load(std::memory_order_relaxed), 0u);
}

}  // namespace
}  // namespace aegis::access
