// Copyright 2026 GCSA
#include <string>
#include <vector>

#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
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
#include "components/aegis_access/access_test_node_test_fixture.h"
#include "content/public/test/browser_test.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/browser_test_utils.h"
#include "ui/base/window_open_disposition.h"

namespace aegis::access {
namespace {

void NavigateAndRewriteSiblingAsOptions(BrowserWindowInterface* browser) {
  // The bare Aegis page redirects to chrome://settings on desktop.
  const GURL sibling("chrome://aegis/?workspace=protection");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser, sibling));
  auto* contents = browser->GetTabStripModel()->GetActiveWebContents();
  ASSERT_EQ(sibling, contents->GetLastCommittedURL());
  ASSERT_TRUE(content::ExecJs(
      contents, "history.replaceState(null, '', '/access-options')"));
  const GURL expected("chrome://aegis/access-options");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return contents->GetLastCommittedURL() == expected; }));
}

}  // namespace

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
  ASSERT_NO_FATAL_FAILURE(NavigateAndRewriteSiblingAsOptions(browser()));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
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
      return (await sendWithPromise('revokeAccessTestNode', '1', 'stale-token')).state;
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

  ASSERT_NO_FATAL_FAILURE(NavigateAndRewriteSiblingAsOptions(browser()));
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
  ASSERT_NO_FATAL_FAILURE(NavigateAndRewriteSiblingAsOptions(browser()));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  EXPECT_EQ("notAdmitted", content::EvalJs(contents, R"JS(
    new Promise(resolve => {
      window.cr = {webUIResponse: (_, __, result) => resolve(result.state)};
      chrome.send('previewAccessTestNode', ['test', 'text', 'invalid']);
    })
  )JS"));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       TestNodeDiscardRejectsRewrittenSiblingDocument) {
  ASSERT_NO_FATAL_FAILURE(NavigateAndRewriteSiblingAsOptions(browser()));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
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
  // Initial page reads can create the empty store. Compare the persisted
  // configuration before and after preview.
  auto* options = AccessOptionsService::GetOrCreate(browser()->GetProfile());
  ASSERT_TRUE(options);
  base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> before;
  options->Read(before.GetCallback());
  const auto& initial = before.Get();
  ASSERT_EQ(StoreStatus::kValid, initial.status);
  ASSERT_TRUE(initial.value);
  ASSERT_FALSE(initial.value->test_profile);

  EXPECT_EQ("preview,protocol,state", content::EvalJs(contents, content::JsReplace(
      R"JS(
        (async () => {
          const {sendWithPromise} = await import('chrome://resources/js/cr.js');
          const reply = await sendWithPromise('previewAccessTestNode', 'text', $1);
          return reply.state + ',' + Object.keys(reply).sort().join(',');
        })()
      )JS",
      aegis_access::test::kSyntheticTestNodeShare)));
  base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> after;
  options->Read(after.GetCallback());
  const auto& saved = after.Get();
  ASSERT_EQ(StoreStatus::kValid, saved.status);
  ASSERT_TRUE(saved.value);
  EXPECT_EQ(initial.value->revision, saved.value->revision);
  EXPECT_EQ(initial.value->configuration, saved.value->configuration);
  EXPECT_FALSE(saved.value->test_profile);
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       CommittedApplyQueryClearsOnlyMatchingNativeDraft) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kWrite)) {
    GTEST_SKIP() << "The test requires an admitted internal build";
  }
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://aegis/access-options")));
  auto* contents = browser()->GetTabStripModel()->GetActiveWebContents();
  auto* options = AccessOptionsService::GetOrCreate(browser()->GetProfile());
  ASSERT_TRUE(options);
  using Failure = AccessOptionsStore::SaveFailurePointForTesting;
  options->SetSaveFailureForTesting(Failure::kAfterCommit);
  const auto operation = base::Uuid::GenerateRandomV4().AsLowercaseString();

  ASSERT_EQ("saved", content::EvalJs(contents, content::JsReplace(R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const initial = await send('readAccessOptions');
      const preview = await send('previewAccessTestNode', 'text', $1);
      if (preview.state !== 'preview') return preview.state;
      const apply = await send('applyAccessTestNode', initial.revision, $2,
          'socks5', 'random', '');
      if (apply.state !== 'uncertain') return apply.state;
      return (await send('queryAccessTestNodeApply', $2)).state;
    })()
  )JS", aegis_access::test::kSyntheticTestNodeShare, operation)));

  options->SetSaveFailureForTesting(Failure::kNone);
  EXPECT_EQ("2,http", content::EvalJs(contents, R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const recovered = await send('readAccessOptions');
      const prefs = await send('applyAccessTestNodePreferences', recovered.revision,
          crypto.randomUUID(), 'http', 'random', '');
      if (prefs.state !== 'prepared' && prefs.state !== 'savedUnavailable') {
        return 'preferences:' + prefs.state;
      }
      const committed = await send('readAccessOptions');
      if (committed.revision !== prefs.revision) return 'revisionMismatch';
      return [committed.revision, committed.testProfile?.protocol].join(',');
    })()
  )JS"));
  base::test::TestFuture<StoreResult<AccessOptionsSnapshot>> read;
  options->Read(read.GetCallback());
  ASSERT_EQ(read.Get().status, StoreStatus::kValid);
  ASSERT_TRUE(read.Get().value && read.Get().value->test_profile);
  EXPECT_EQ(read.Get().value->revision, 2u);
  EXPECT_EQ(read.Get().value->test_profile->committed_revision,
            read.Get().value->revision);
  EXPECT_EQ(read.Get().value->test_profile->configuration.inbound.protocol,
            aegis_access::AccessInboundProtocolPreference::kHttp);

  // A receipt for the previous node Apply cannot clear a newly staged draft.
  EXPECT_EQ("saved,preview,preserved", content::EvalJs(contents, content::JsReplace(R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const committed = await send('readAccessOptions');
      const fresh = await send('previewAccessTestNode', 'text', $1);
      const confirmed = await send('queryAccessTestNodeApply', $2);
      const pending = await send('applyAccessTestNodePreferences', committed.revision,
          crypto.randomUUID(), 'socks5', 'random', '');
      return [confirmed.state, fresh.state,
        pending.state === 'previewPending' ? 'preserved' : pending.state].join(',');
    })()
  )JS", aegis_access::test::kSyntheticTestNodeShare, operation)));
}

IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       CommittedApplyQueryClearsDraftAfterAnotherTabSaves) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kWrite)) {
    GTEST_SKIP() << "The test requires an admitted internal build";
  }
  const GURL url("chrome://aegis/access-options");
  auto* first = ui_test_utils::NavigateToURL(browser(), url);
  ASSERT_TRUE(first);
  auto* options = AccessOptionsService::GetOrCreate(browser()->GetProfile());
  ASSERT_TRUE(options);
  options->SetSaveFailureForTesting(
      AccessOptionsStore::SaveFailurePointForTesting::kAfterCommit);
  constexpr char kApply[] = R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const initial = await send('readAccessOptions');
      if (initial.pendingApplyRecovery) return 'previousApplyPending';
      const preview = await send('previewAccessTestNode', 'text', $1);
      if (preview.state !== 'preview') return preview.state;
      return (await send('applyAccessTestNode', initial.revision, $2,
          'socks5', 'random', '')).state;
    })()
  )JS";
  const auto operation = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const auto other_operation =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  constexpr auto share = aegis_access::test::kSyntheticTestNodeShare;
  ASSERT_EQ("uncertain", content::EvalJs(
      first, content::JsReplace(kApply, share, operation)));

  auto* second = ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::kNewForegroundTab,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);
  ASSERT_TRUE(second);
  ASSERT_EQ("uncertain", content::EvalJs(
      second, content::JsReplace(kApply, share, other_operation)));
  ASSERT_EQ("saved", content::EvalJs(second, content::JsReplace(R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      return (await send('queryAccessTestNodeApply', $1)).state;
    })()
  )JS", other_operation)));

  EXPECT_EQ("saved,uncertain", content::EvalJs(first, content::JsReplace(R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const recovered = await send('queryAccessTestNodeApply', $1);
      const next = await send('applyAccessTestNodePreferences', recovered.revision,
          crypto.randomUUID(), 'http', 'random', '');
      return [recovered.state, next.state].join(',');
    })()
  )JS", operation)));
}


IN_PROC_BROWSER_TEST_F(AccessOptionsBrowserTest,
                       StaleWindowPreservesListenerDraftAfterConflict) {
  if (!CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kWrite)) {
    GTEST_SKIP() << "The test requires an admitted internal build";
  }
  const GURL url("chrome://aegis/access-options");
  auto* first = ui_test_utils::NavigateToURL(browser(), url);
  ASSERT_TRUE(first);
  ASSERT_EQ("1", content::EvalJs(first, content::JsReplace(R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const initial = await send('readAccessOptions');
      const preview = await send('previewAccessTestNode', 'text', $1);
      if (preview.state !== 'preview') return preview.state;
      const applied = await send('applyAccessTestNode', initial.revision,
          crypto.randomUUID(), 'socks5', 'random', '');
      return applied.revision ?? applied.state;
    })()
  )JS", aegis_access::test::kSyntheticTestNodeShare)));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  first = ui_test_utils::NavigateToURL(browser(), url);
  ASSERT_TRUE(first);
  auto* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_NE(second_browser, browser());
  auto* second = ui_test_utils::NavigateToURL(second_browser, url);
  ASSERT_TRUE(second);

  constexpr char kEditPreferences[] = R"JS(
    (async () => {
      const get = id => document.getElementById(id);
      const fields = get('test-node-fields');
      if (fields.disabled) {
        await new Promise(resolve => {
          const observer = new MutationObserver(() => {
            if (fields.disabled) return;
            observer.disconnect();
            resolve();
          });
          observer.observe(fields, {attributes: true, attributeFilter: ['disabled']});
        });
      }
      const set = (id, value, event) => {
        get(id).value = value;
        get(id).dispatchEvent(new Event(event, {bubbles: true}));
      };
      set('inbound-protocol', $1, 'change');
      set('inbound-port-mode', 'fixed', 'change');
      set('inbound-port', $2, 'input');
      return !get('apply-node').disabled && get('saved-revision').textContent;
    })()
  )JS";
  ASSERT_EQ("1", content::EvalJs(first, content::JsReplace(
      kEditPreferences, "http", "41071")));
  ASSERT_EQ("1", content::EvalJs(second, content::JsReplace(
      kEditPreferences, "socks5", "10806")));

  constexpr char kApplyPreferences[] = R"JS(
    (async () => {
      const get = id => document.getElementById(id);
      const fields = get('test-node-fields');
      const completed = new Promise(resolve => {
        const observer = new MutationObserver(() => {
          if (fields.disabled) return;
          observer.disconnect();
          resolve();
        });
        observer.observe(fields, {attributes: true, attributeFilter: ['disabled']});
      });
      get('apply-node').click();
      await completed;
      const status = get('node-status');
      const conflict = !get('review-revision').hidden &&
          status.textContent.startsWith('Another window saved changes.');
      return [get('saved-revision').textContent, get('draft-state').textContent,
          get('inbound-protocol').value, get('inbound-port-mode').value,
          get('inbound-port').value, status.dataset.error, conflict].join(',');
    })()
  )JS";
  EXPECT_EQ("2,Saved preferences,http,fixed,41071,false,false",
            content::EvalJs(first, kApplyPreferences));
  EXPECT_EQ("1,Unsaved draft,socks5,fixed,10806,true,true",
            content::EvalJs(second, kApplyPreferences));

  CloseBrowserSynchronously(second_browser);
  auto* reopened = ui_test_utils::NavigateToURL(
      CreateBrowser(browser()->GetProfile()), url);
  ASSERT_TRUE(reopened);
  EXPECT_EQ("2,http,fixed,41071", content::EvalJs(reopened, R"JS(
    (async () => {
      const {sendWithPromise: send} = await import('chrome://resources/js/cr.js');
      const saved = await send('readAccessOptions');
      const {protocol, portMode, requestedPort} = saved.testProfile;
      return [saved.revision, protocol, portMode, requestedPort].join(',');
    })()
  )JS"));
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
