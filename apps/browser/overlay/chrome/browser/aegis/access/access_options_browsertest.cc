// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/profiles/profile.h"
#include "base/files/file_util.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/browser_test_utils.h"

namespace aegis::access {
using AccessOptionsBrowserTest = InProcessBrowserTest;
IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest, DirectNavigationUsesNativeAdmission) {
  const bool admitted = CanUseAccessOptionsForCurrentBuild(
      aegis_access::AccessOptionsOperation::kPage);
  auto* frame = ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://aegis/access-options"));
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->IsErrorDocument(), !admitted);
  if (admitted) {
    EXPECT_EQ("Access Options", content::EvalJs(
        browser()->GetTabStripModel()->GetActiveWebContents(), "document.title"));
  }
}
IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest, DirectMessageUsesNativeAdmission) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  const bool admitted = CanUseAccessOptionsForCurrentBuild(
      aegis_access::AccessOptionsOperation::kRead);
  EXPECT_EQ(admitted, content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (id, success, result) => resolve(result.enabled)};
      chrome.send('getAccessOptionsCapabilities', ['capability-test']);
    })
  )JS"));
  if (admitted) {
    EXPECT_EQ("ready", content::EvalJs(contents, R"JS(
      new Promise(resolve => {
        window.cr = {webUIResponse: (id, success, result) => resolve(result.state)};
        chrome.send('readAccessOptions', ['admitted-read-test']);
      })
    )JS"));
  } else {
    EXPECT_EQ("channelNotAdmitted", content::EvalJs(contents, R"JS(
      new Promise(resolve => {
        window.cr = {webUIResponse: (id, success, result) => resolve(result.state)};
        chrome.send('readAccessOptions', ['read-test']);
      })
    )JS"));
  }
}
IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest, PrivateReadDoesNotCreateOptionsDatabase) {
  auto* private_browser = CreateIncognitoBrowser(browser()->GetProfile());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(private_browser, GURL("chrome://aegis")));
  auto* contents = private_browser->GetTabStripModel()->GetActiveWebContents();
  const bool admitted = CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kRead);
  EXPECT_EQ(admitted ? "ready" : "channelNotAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (id, success, result) => resolve(result.state)};
      chrome.send('readAccessOptions', ['private-read-test']);
    })
  )JS"));
  EXPECT_FALSE(base::PathExists(browser()->GetProfile()->GetPath().AppendASCII("AccessOptions")));
}
}  // namespace aegis::access
