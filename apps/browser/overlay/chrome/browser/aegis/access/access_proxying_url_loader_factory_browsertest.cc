// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_proxying_url_loader_factory.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/test/run_until.h"
#include "chrome/browser/aegis/access/access_browser_request_adapter.h"
#include "chrome/browser/aegis/access/access_identity_generation_source.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_proxy_selection_generation_source.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/aegis/access/access_request_dispatch_state.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/aegis_access/access_identity_generation_state.h"
#include "components/aegis_access/access_proxy_selection_generation_state.h"
#include "components/aegis_access/request_policy_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/base/net_errors.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aegis::access {
namespace {

constexpr char kTargetHost[] = "target.example";
constexpr char kProxyGroup[] = "proxy-group-browser-test";

std::unique_ptr<net::test_server::HttpResponse> CountAndReply(
    std::atomic<size_t>* counter,
    const char* body,
    const net::test_server::HttpRequest&) {
  counter->fetch_add(1, std::memory_order_relaxed);
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content(body);
  response->set_content_type("text/plain");
  return response;
}

std::unique_ptr<net::test_server::HttpResponse> ProxyReply(
    std::atomic<size_t>* counter,
    net::test_server::EmbeddedTestServer* target_origin,
    const net::test_server::HttpRequest& request) {
  counter->fetch_add(1, std::memory_order_relaxed);
  if (request.relative_url.find("/hang") != std::string::npos) {
    return std::make_unique<net::test_server::HungResponse>();
  }
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  if (request.relative_url.find("/redirect") != std::string::npos) {
    response->set_code(net::HTTP_FOUND);
    response->AddCustomHeader(
        "Location", target_origin->GetURL(kTargetHost, "/resource").spec());
    return response;
  }
  response->set_code(net::HTTP_OK);
  response->set_content("proxy");
  response->set_content_type("text/plain");
  return response;
}

class AccessProxyingURLLoaderFactoryBrowserTest : public InProcessBrowserTest {
 public:
  AccessProxyingURLLoaderFactoryBrowserTest()
      : target_origin_(net::test_server::EmbeddedTestServer::TYPE_HTTP),
        proxy_server_(net::test_server::EmbeddedTestServer::TYPE_HTTP) {}
  ~AccessProxyingURLLoaderFactoryBrowserTest() override = default;

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();

    host_resolver()->AddRule(kTargetHost, "127.0.0.1");

    target_origin_.RegisterRequestHandler(base::BindRepeating(
        &CountAndReply, base::Unretained(&origin_requests_), "origin"));
    proxy_server_.RegisterRequestHandler(base::BindRepeating(
        &ProxyReply, base::Unretained(&proxy_requests_),
        base::Unretained(&target_origin_)));
    ASSERT_TRUE(target_origin_.Start());
    ASSERT_TRUE(proxy_server_.Start());
    ASSERT_TRUE(embedded_test_server()->Start());

    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/title1.html")));

    transport_ = AccessNetworkContextTransport::Get(browser()->profile());
    ASSERT_NE(transport_, nullptr);

    owner_ = transport_->OwnerForPartition(
        aegis_access::ChannelNamespace::kDev, base::FilePath());
    ASSERT_TRUE(owner_.has_value());
    ASSERT_TRUE(transport_->OwnsConfiguredPartition(*owner_));
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  GURL target_url() const {
    return target_origin_.GetURL(kTargetHost, "/resource");
  }

  GURL redirect_url() const {
    return target_origin_.GetURL(kTargetHost, "/redirect");
  }

  GURL hanging_url() const {
    return target_origin_.GetURL(kTargetHost, "/hang");
  }

  void BuildCancellationSelector(
      const GURL& url,
      aegis_access::RequestCancellationSelector* selector) {
    ASSERT_NE(selector, nullptr);
    Profile* profile = browser()->profile();
    content::RenderFrameHost* frame = web_contents()->GetPrimaryMainFrame();
    ASSERT_NE(frame, nullptr);
    const content::FrameTreeNodeId frame_tree_node_id =
        frame->GetFrameTreeNodeId();
    auto wc_getter =