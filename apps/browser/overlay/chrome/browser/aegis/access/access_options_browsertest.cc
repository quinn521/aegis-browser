// Copyright 2026 GCSA
#include <string>
#include <vector>

#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_buildflags.h"
#include "chrome/browser/aegis/access/access_options_service.h"
#include "chrome/common/channel_info.h"
#include "chrome/browser/aegis/access/access_site_proxy_bubble_controller.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/profiles/profile.h"
#include "base/files/file_util.h"
#include "base/threading/thread_restrictions.h"
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
  base::FilePath legacy;
  if (!admitted) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    legacy = browser()->GetProfile()->GetPath().AppendASCII("AccessOptions")
        .AppendASCII("alpha").AppendASCII("options.db");
    ASSERT_TRUE(base::CreateDirectory(legacy.DirName()));
    ASSERT_TRUE(base::WriteFile(legacy, "legacy-internal-options"));
  }
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
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string original;
    ASSERT_TRUE(base::ReadFileToString(legacy, &original));
    EXPECT_EQ(original, "legacy-internal-options");
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
  base::ScopedAllowBlockingForTesting allow_blocking;
  EXPECT_FALSE(base::PathExists(browser()->GetProfile()->GetPath().AppendASCII("AccessOptions")));
}
IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       ReadReplyContainsOnlyOptionsPresentationFields) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kPage)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://aegis/access-options")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("ready,formatVersion,revision,state,storeStatus", content::EvalJs(
      contents, R"JS(
        (async () => {
          const {sendWithPromise} = await import('chrome://resources/js/cr.js');
          const reply = await sendWithPromise('readAccessOptions');
          return reply.state + ',' + Object.keys(reply).sort().join(',');
        })()
      )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       ApplyRejectsSiblingDocumentDespiteRewrittenUrl) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::ExecJs(contents,
      "history.replaceState(null, '', '/access-options')"));
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (_, __, result) => resolve(result.state)};
      chrome.send('applyAccessTestNode', [
        'apply-test', '0', '01234567-89ab-4def-8a01-234567890123',
        'socks5', 'fixed', '10806',
      ]);
    })
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       RevokeRequiresTrustedDocumentAndOwnedRevision) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kActivate)) {
    GTEST_SKIP() << "Requires an admitted internal build";
  }
  constexpr char kRevoke[] = R"JS(
    (async () => {
      const {sendWithPromise} = await import('chrome://resources/js/cr.js');
      return (await sendWithPromise('revokeAccessTestNode', '1')).state;
    })()
  )JS";
  auto* options = AccessOptionsService::GetOrCreate(browser()->GetProfile());
  ASSERT_TRUE(options);
  std::vector<std::string> editors;
  for (size_t i = 0; i < 16; ++i) {
    auto editor = options->BeginTestNodeEditor();
    ASSERT_TRUE(editor);
    editors.push_back(*editor);
  }
  ASSERT_FALSE(options->BeginTestNodeEditor());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis/access-options")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("connectionChanged", content::EvalJs(contents, kRevoke));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  ASSERT_TRUE(content::ExecJs(contents,
      "history.replaceState(null, '', '/access-options')"));
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, kRevoke));
  for (const auto& editor : editors) options->EndTestNodeEditor(editor);
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodePreviewRequiresExactOptionsPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (_, __, result) => resolve(result.state)};
      chrome.send('previewAccessTestNode', ['test', 'text', 'invalid']);
    })
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodePreviewRejectsRewrittenSiblingDocument) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::ExecJs(contents,
      "history.replaceState(null, '', '/access-options')"));
  EXPECT_EQ(GURL("chrome://aegis/access-options"), contents->GetLastCommittedURL());
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (_, __, result) => resolve(result.state)};
      chrome.send('previewAccessTestNode', ['test', 'text', 'invalid']);
    })
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodeDiscardRejectsRewrittenSiblingDocument) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://aegis")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::ExecJs(contents,
      "history.replaceState(null, '', '/access-options')"));
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (_, __, result) => resolve(result.state)};
      chrome.send('discardAccessTestNodeDraft', ['discard-test']);
    })
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodePreviewKeepsPayloadOutOfWebUIReplyAndStore) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "The test requires an admitted internal build";
  }
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://aegis/access-options")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("preview,protocol,state", content::EvalJs(contents, content::JsReplace(
      R"JS(
        (async () => {
          const {sendWithPromise} = await import('chrome://resources/js/cr.js');
          const reply = await sendWithPromise('previewAccessTestNode', 'text', $1);
          return reply.state + ',' + Object.keys(reply).sort().join(',');
        })()
      )JS",
      "vless://550e8400-e29b-41d4-a716-446655440000@198.51.100.10:443"
      "?security=reality&type=tcp&flow=xtls-rprx-vision&sni=www.example.com"
      "&pbk=abcdefghijklmnopqrstuvwxyzABCDEFG&sid=abcd1234#demo-reality-vision")));
  base::ScopedAllowBlockingForTesting allow_blocking;
  EXPECT_FALSE(base::PathExists(
      browser()->GetProfile()->GetPath().AppendASCII("AccessOptions")));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodePreviewRejectsOffTheRecordProfile) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) {
    GTEST_SKIP() << "The test requires an admitted internal build";
  }
  auto* private_browser = CreateIncognitoBrowser(browser()->GetProfile());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      private_browser, GURL("chrome://aegis/access-options")));
  auto* contents = private_browser->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    (async () => {
      const {sendWithPromise} = await import('chrome://resources/js/cr.js');
      return (await sendWithPromise('previewAccessTestNode', 'text', 'input')).state;
    })()
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest, LegacyDiagnosticPanelUsesResolvedChannel) {
  const auto channel = AccessChannelForCurrentBuild();
  const auto identity = AccessBuildIdentityForCurrentBuild();
  bool internal = !identity.conflict &&
      (channel == aegis_access::ChannelNamespace::kDev ||
       channel == aegis_access::ChannelNamespace::kAlpha);
#if !BUILDFLAG(AEGIS_DEV_LOCAL_NODE_ALLOWED)
  if (identity.product == aegis_access::AccessProductChannel::kUnknown &&
      chrome::GetChannel() == version_info::Channel::UNKNOWN)
    internal = false;
#endif
  auto* controller = AccessSiteProxyBubbleController::GetOrCreate(browser()->GetProfile());
  ASSERT_TRUE(controller);
  EXPECT_EQ(controller->IsDevAllowBlockPanelRegistered(), internal);
  const auto result = controller->ApplyDevAllowBlock("example.test", AccessDevAllowBlockAction::kBlock);
  EXPECT_EQ(result.status, internal ? AccessDevAllowBlockApplyStatus::kApplied
                                   : AccessDevAllowBlockApplyStatus::kChannelNotAdmitted);
  EXPECT_EQ(controller->DevAllowBlockSnapshotForCommittedUrl(GURL("https://example.test")).panel_registered, internal);
}
}  // namespace aegis::access
