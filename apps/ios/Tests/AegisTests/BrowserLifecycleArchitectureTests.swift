import AgentKit
import Combine
import CryptoKit
import UIKit
import WebKit
import XCTest
@testable import Aegis
@testable import BrowserKit

@MainActor
final class BrowserPresentationCoordinatorTests: XCTestCase {
    func testFIFOIncludesDistinctSameURLInputsAndRejectsOldDismissal() throws {
        let route = BrowserPresentationCoordinator()
        route.request(.reading)
        let first = try XCTUnwrap(route.current)
        let url = URL(string: "https://example.test/same")!
        route.request(.downloads(url, nil)); route.request(.downloads(url, nil))
        route.request(.external(url)); route.request(.markdown(MarkdownDocument(title: "One", text: "One")))
        XCTAssertFalse(route.isDismissing, "An unpresented request must not wait for an impossible dismissal")
        XCTAssertEqual(route.pending.count, 4)
        route.dismiss(first.id); route.didDismiss(first.id)
        let download = try XCTUnwrap(route.current)
        XCTAssertNotEqual(first.id, download.id)
        route.didDismiss(first.id)
        XCTAssertEqual(route.current?.id, download.id)
        route.dismiss(download.id); route.didDismiss(download.id)
        let second = try XCTUnwrap(route.current)
        XCTAssertNotEqual(download.id, second.id)
        guard case .downloads = second.route else { return XCTFail("Second same URL event was lost") }
        route.dismiss(second.id); route.didDismiss(second.id)
        guard case .external = route.current?.route else { return XCTFail("External event order changed") }
    }

    func testUnshownInvalidationProgressesWithoutImpossibleUIKitDismissCallback() throws {
        let route = BrowserPresentationCoordinator()
        route.request(.assistant)
        let neverShown = try XCTUnwrap(route.current)
        route.request(.settings)
        route.invalidate { $0.isDocumentBound }
        let settings = try XCTUnwrap(route.current)
        guard case .settings = settings.route else { return XCTFail("Unshown invalidation stalled the queue") }
        XCTAssertFalse(route.isDismissing)
        route.didDismiss(neverShown.id)
        XCTAssertEqual(route.current?.id, settings.id)
        route.didPresent(settings.id); route.dismiss(settings.id)
        XCTAssertTrue(route.isDismissing)
        route.didDismiss(neverShown.id)
        XCTAssertEqual(route.current?.id, settings.id)
        route.didDismiss(settings.id)
        XCTAssertNil(route.current)
    }

    func testSessionEventsPreserveSameURLAndMarkdownBeforeAnyRootRender() throws {
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil))
        defer { browser.closeWindow() }
        let url = URL(string: "https://example.test/file")!
        browser.pendingDownloadURL = url; browser.pendingDownloadURL = url
        XCTAssertEqual(browser.consumePendingDownload()?.url, url)
        XCTAssertEqual(browser.consumePendingDownload()?.url, url)
        XCTAssertNil(browser.consumePendingDownload())
        let first = MarkdownDocument(title: "Same", text: "First"), second = MarkdownDocument(title: "Same", text: "Second")
        browser.pendingMarkdown = first; browser.pendingMarkdown = second
        XCTAssertEqual(browser.consumePendingMarkdown()?.id, first.id)
        XCTAssertEqual(browser.consumePendingMarkdown()?.id, second.id)
        XCTAssertNil(browser.consumePendingMarkdown())
    }

    func testActualWindowCommandsCannotPenetrateReadingMarkdownOrHandoff() throws {
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil))
        let other = BrowserSession(dataStore: browser.dataStore)
        defer { browser.closeWindow(); other.closeWindow() }
        let route = BrowserPresentationCoordinator(), secondRoute = BrowserPresentationCoordinator()
        let tab = try XCTUnwrap(browser.activeTab)
        let original = browser.visibleTabs.count
        let oldCommands = try XCTUnwrap(BrowserKeyboardActions.forWindow(browser, presentation: route))
        route.request(.reading)
        XCTAssertNil(BrowserKeyboardActions.forWindow(browser, presentation: route))
        oldCommands.closeTab(); oldCommands.newTab()
        XCTAssertEqual(browser.visibleTabs.count, original)
        XCTAssertTrue(browser.activeTab === tab)
        XCTAssertNotNil(BrowserKeyboardActions.forWindow(other, presentation: secondRoute))
        route.request(.markdown(MarkdownDocument(title: "Private", text: "Synthetic")))
        let reading = try XCTUnwrap(route.current)
        route.dismiss(reading.id); route.didDismiss(reading.id)
        XCTAssertNil(BrowserKeyboardActions.forWindow(browser, presentation: route))
        oldCommands.closeTab()
        XCTAssertTrue(browser.activeTab === tab)
        let markdown = try XCTUnwrap(route.current)
        route.dismiss(markdown.id); route.didDismiss(markdown.id)
        let commands = try XCTUnwrap(BrowserKeyboardActions.forWindow(browser, presentation: route))
        commands.newTab()
        XCTAssertEqual(browser.visibleTabs.count, original + 1)
        oldCommands.closeTab() // Its captured tab is no longer current.
        XCTAssertEqual(browser.visibleTabs.count, original + 1)
    }

    func testPresentationWebKitInstallsEnabledPriorityCloseCommand() throws {
        let configuration = WKWebViewConfiguration(); configuration.websiteDataStore = .nonPersistent()
        let viewer = BrowserPresentationWebView(frame: .zero, configuration: configuration)
        let close = try XCTUnwrap(viewer.keyCommands?.first { $0.input == "w" && $0.modifierFlags == .command })
        let action = try XCTUnwrap(close.action)
        XCTAssertTrue(close.wantsPriorityOverSystemBehavior)
        XCTAssertTrue(viewer.canPerformAction(action, withSender: close))
        XCTAssertTrue(viewer.target(forAction: action, withSender: close) as AnyObject? === viewer)
    }

    func testEmptyGroupOnlyAdvertisesWorkingNewTabAction() throws {
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil))
        defer { browser.closeWindow() }
        _ = try browser.createGroup(name: "Empty")
        browser.moveTab(try XCTUnwrap(browser.activeTab?.id), to: nil)
        XCTAssertNil(browser.activeTab)
        let commands = try XCTUnwrap(BrowserKeyboardActions.forWindow(browser, presentation: BrowserPresentationCoordinator()))
        XCTAssertFalse(commands.canCloseTab); XCTAssertFalse(commands.canUsePageCommands)
        commands.closeTab(); commands.reload(); commands.find()
        XCTAssertNil(browser.activeTab)
        commands.newTab()
        XCTAssertNotNil(browser.activeTab)
    }

    func testConsumedProtectedDownloadRetainsOnlyLiveScopedAuthorityAndOwnAcknowledgment() throws {
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil))
        defer { browser.closeWindow() }
        let tab = try XCTUnwrap(browser.activeTab), url = URL(string: "https://example.test/file")!
        tab.prepareForRestore(url: url); tab.selectedAccessHost = "example.test"
        tab.onDownload?(url)
        let first = try XCTUnwrap(browser.consumePendingDownload()?.access)
        XCTAssertNil(browser.pendingDownloadURL); XCTAssertNil(browser.pendingAccessDownload)
        XCTAssertTrue(browser.ownsAccessDownload(first))
        tab.onDownload?(url)
        let second = try XCTUnwrap(browser.consumePendingDownload()?.access)
        XCTAssertNotEqual(first, second)
        XCTAssertTrue(browser.ownsAccessDownload(first)); XCTAssertTrue(browser.ownsAccessDownload(second))
        browser.releasePresentedAccessDownload(first)
        XCTAssertFalse(browser.ownsAccessDownload(first)); XCTAssertTrue(browser.ownsAccessDownload(second))
        // Same URL, same tab UUID, a different document: a frozen route grants no authority.
        tab.load(url)
        XCTAssertFalse(browser.ownsAccessDownload(second))
        browser.releasePresentedAccessDownload(first)
        XCTAssertFalse(browser.ownsAccessDownload(second))
    }

    func testProfileInvalidationDiscardsSensitiveQueuedContentButKeepsOrdinarySettings() throws {
        let route = BrowserPresentationCoordinator()
        route.request(.settings)
        route.request(.markdown(MarkdownDocument(title: "Private", text: "Private")))
        route.request(.reading); route.request(.settings)
        route.invalidate { $0.isProfileBound }
        XCTAssertEqual(route.pending.count, 1)
        let first = try XCTUnwrap(route.current)
        route.dismiss(first.id); route.didDismiss(first.id)
        guard case .settings = route.current?.route else { return XCTFail("Non-sensitive route was lost") }
    }
}

@MainActor
final class MainFrameNavigationStateTests: XCTestCase {
    private func navigation() throws -> (WKWebView, WKNavigation) {
        let configuration = WKWebViewConfiguration(); configuration.websiteDataStore = .nonPersistent()
        let view = WKWebView(frame: .zero, configuration: configuration)
        return (view, try XCTUnwrap(view.loadHTMLString("<p>Identity fixture</p>", baseURL: nil)))
    }

    func testFIFOAndRetiredCallbacksCannotBorrowNewerSameURLAttempt() throws {
        let state = MainFrameNavigationState()
        let url = URL(string: "https://example.test/same")!
        state.begin(url: url, method: "POST", unsafeReplay: true, policyType: .formSubmitted)
        let old = state.current
        state.begin(url: url, method: "GET", unsafeReplay: false, policyType: .linkActivated)
        let latest = state.current
        let (firstView, first) = try navigation(), (secondView, second) = try navigation()
        defer { firstView.stopLoading(); secondView.stopLoading() }
        XCTAssertEqual(state.bindNext(first)?.navigationID, old.navigationID)
        XCTAssertFalse(state.isCurrent(first, isLoading: true))
        XCTAssertEqual(state.bindNext(second)?.navigationID, latest.navigationID)
        XCTAssertTrue(state.isCurrent(second, isLoading: true))
        XCTAssertTrue(state.retire(first)?.hasReplayableBody == true)
        XCTAssertEqual(state.current.navigationID, latest.navigationID)
        XCTAssertNil(state.retire(first))
        state.retireAll()
        XCTAssertNil(state.bindNext(second)); XCTAssertFalse(state.isCurrent(second, isLoading: true))
    }

    func testHistoryAcknowledgmentAndRedirectNeverInventSafeReplay() throws {
        let state = MainFrameNavigationState()
        let initial = URL(string: "https://example.test/form")!, target = URL(string: "https://example.test/redirect")!
        state.begin(url: initial, method: "GET", unsafeReplay: false, policyType: .backForward)
        let id = state.current.navigationID
        state.programmaticPolicyNavigationID = id
        XCTAssertTrue(state.adoptPolicy(url: initial, method: "POST", continues: false, unsafeReplay: true, policyType: .formResubmitted))
        XCTAssertEqual(state.current.navigationID, id)
        XCTAssertEqual(state.current.method, "POST")
        let (view, navigation) = try navigation(); defer { view.stopLoading() }
        state.bind(navigation, to: state.current); state.pendingNavigation = navigation
        XCTAssertTrue(state.confirmRedirect(navigation, url: target))
        XCTAssertTrue(state.adoptPolicy(url: target, method: "GET", continues: false, unsafeReplay: false, policyType: .other))
        XCTAssertEqual(state.current.method, "POST")
        XCTAssertTrue(state.current.hasReplayableBody)
        XCTAssertNil(state.uniqueCurrentUnbound)
    }
}

@MainActor
final class PageAssistantModelTests: XCTestCase {
    @MainActor private struct Fixture {
        let browser: BrowserSession
        let settings: BrowserSettings
        let tasks: AssistantTaskStore
        let defaults: UserDefaults
        let suite: String
        let directory: URL
        init() throws {
            browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil))
            suite = "page-assistant-model-" + UUID().uuidString
            defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
            settings = BrowserSettings(defaults: defaults)
            directory = FileManager.default.temporaryDirectory.appendingPathComponent(suite)
            tasks = AssistantTaskStore(url: directory.appendingPathComponent("tasks.aes"), key: SymmetricKey(size: .bits256))
            let tab = try XCTUnwrap(browser.activeTab)
            tab.adoptPolicyMainFrameNavigationForTests(url: URL(string: "https://example.test/article")!)
            tab.commitPendingMainFrameForTests(); tab.stop()
        }
        func close() { browser.closeWindow(); defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: directory) }
        func attach(_ model: PageAssistantModel) { model.attach(browser: browser, settings: settings, tasks: tasks) }
    }
    private func model(stream: @escaping PageAssistantModel.Stream) -> PageAssistantModel {
        PageAssistantModel(lifetime: AssistantOperationLifetime(applicationIsActive: { true }), credential: { _ in "fixture-key" },
                           readPage: { tab, _ in
            PageSnapshot(id: tab.id, navigationEpoch: tab.navigationEpoch, url: tab.url!, title: "Synthetic", text: "Synthetic source", capturedAt: Date(), truncated: false)
        }, stream: stream)
    }
    private func read(_ model: PageAssistantModel) async throws {
        model.readPages()
        let task = try XCTUnwrap(model.lifetime.operation)
        await task.value
        XCTAssertEqual(model.snapshots.count, 1)
    }

    func testConsentAndImmediateCancellationSendZeroRequestsAndCreateNoRecords() async throws {
        let fixture = try Fixture(); defer { fixture.close() }
        var sends = 0
        let model = model { _, _, _, _, _, _ in sends += 1; return "Unexpected" }
        fixture.attach(model); try await read(model)
        model.requestConsent(); XCTAssertNotNil(model.consent)
        model.cancel(); model.send()
        XCTAssertEqual(sends, 0); XCTAssertTrue(fixture.tasks.records.isEmpty)
        model.requestConsent(); model.send()
        let task = try XCTUnwrap(model.lifetime.operation)
        model.cancel(); await task.value
        XCTAssertEqual(sends, 0); XCTAssertTrue(fixture.tasks.records.isEmpty)
        XCTAssertFalse(model.busy)
    }

    func testSameURLReloadAndCredentialChangeRevokeApprovedSourcesBeforeSend() async throws {
        let fixture = try Fixture(); defer { fixture.close() }
        var sends = 0, key = "first-account"
        let model = PageAssistantModel(lifetime: AssistantOperationLifetime(applicationIsActive: { true }), credential: { _ in key },
                                      readPage: { tab, _ in PageSnapshot(id: tab.id, navigationEpoch: tab.navigationEpoch, url: tab.url!, title: "Synthetic", text: "Synthetic", capturedAt: Date(), truncated: false) },
                                      stream: { _, _, _, _, _, _ in sends += 1; return "Unexpected" })
        fixture.attach(model); try await read(model); model.requestConsent()
        key = "another-account"; model.send()
        XCTAssertEqual(sends, 0); XCTAssertTrue(fixture.tasks.records.isEmpty)
        model.requestConsent()
        let tab = try XCTUnwrap(fixture.browser.activeTab), url = try XCTUnwrap(fixture.browser.activeTab?.url)
        tab.load(url)
        XCTAssertNil(model.consent); XCTAssertTrue(model.snapshots.isEmpty)
        model.send(); XCTAssertEqual(sends, 0)
    }

    func testCancelledUncooperativeStreamCannotPublishOrCompleteNewerOperation() async throws {
        for oldCompletionFails in [false, true] {
            let fixture = try Fixture(); defer { fixture.close() }
            var replies: [String: CheckedContinuation<String, any Error>] = [:]
            var partials: [String: @MainActor @Sendable (String) -> Void] = [:]
            let firstStarted = expectation(description: "First request"), secondStarted = expectation(description: "Second request")
            let model = model { _, _, goal, _, _, onText in
                partials[goal] = onText
                return try await withCheckedThrowingContinuation { reply in
                    replies[goal] = reply
                    (goal == "First" ? firstStarted : secondStarted).fulfill()
                }
            }
            fixture.attach(model); try await read(model)
            model.goal = "First"; model.requestConsent(); model.send()
            let firstTask = try XCTUnwrap(model.lifetime.operation)
            await fulfillment(of: [firstStarted], timeout: 2)
            model.cancel()
            XCTAssertEqual(fixture.tasks.records.first?.state, .interrupted)
            try await read(model); model.goal = "Second"; model.requestConsent(); model.send()
            let secondTask = try XCTUnwrap(model.lifetime.operation)
            await fulfillment(of: [secondStarted], timeout: 2)
            partials["First"]?("Stale partial")
            XCTAssertTrue(model.output.isEmpty)
            partials["Second"]?("Current partial")
            XCTAssertEqual(model.output, "Current partial")
            replies["Second"]?.resume(returning: "Current [1]"); await secondTask.value
            XCTAssertTrue(model.outputComplete); XCTAssertEqual(model.output, "Current [1]")
            if oldCompletionFails { replies["First"]?.resume(throwing: ModelClientError.invalidResponse) }
            else { replies["First"]?.resume(returning: "Stale final [1]") }
            await firstTask.value
            XCTAssertTrue(model.outputComplete); XCTAssertEqual(model.output, "Current [1]")
            XCTAssertNil(model.error); XCTAssertFalse(model.busy)
            XCTAssertEqual(fixture.tasks.records.first?.state, .completed)
            XCTAssertEqual(fixture.tasks.records.last?.state, .interrupted)
        }
    }

    func testRepeatedSendWithoutRereadingRejectsPreviouslyCompletedStreamsLateCallback() async throws {
        let fixture = try Fixture(); defer { fixture.close() }
        var callbacks: [String: @MainActor @Sendable (String) -> Void] = [:]
        var secondReply: CheckedContinuation<String, any Error>?
        let secondStarted = expectation(description: "Second send against same preview")
        let model = model { _, _, goal, _, _, onText in
            callbacks[goal] = onText
            if goal == "First" { return "First [1]" }
            return try await withCheckedThrowingContinuation { secondReply = $0; secondStarted.fulfill() }
        }
        fixture.attach(model); try await read(model)
        model.goal = "First"; model.requestConsent(); model.send()
        let first = try XCTUnwrap(model.lifetime.operation); await first.value
        XCTAssertTrue(model.outputComplete)
        model.goal = "Second"; model.requestConsent(); model.send()
        let second = try XCTUnwrap(model.lifetime.operation)
        await fulfillment(of: [secondStarted], timeout: 2)
        callbacks["First"]?("Late callback after first stream already returned")
        XCTAssertTrue(model.output.isEmpty)
        callbacks["Second"]?("Current partial")
        XCTAssertEqual(model.output, "Current partial")
        secondReply?.resume(returning: "Second [1]"); await second.value
        callbacks["First"]?("Another stale callback")
        XCTAssertEqual(model.output, "Second [1]"); XCTAssertTrue(model.outputComplete)
        XCTAssertEqual(fixture.tasks.records.map(\.state), [.completed, .completed])
    }

    func testCancellingRepeatedSendBeforeTaskStartsPreservesPriorCompletedRecord() async throws {
        let fixture = try Fixture(); defer { fixture.close() }
        var sends = 0
        let model = model { _, _, _, _, _, _ in sends += 1; return "Complete [1]" }
        fixture.attach(model); try await read(model)
        model.requestConsent(); model.send()
        let first = try XCTUnwrap(model.lifetime.operation); await first.value
        XCTAssertEqual(fixture.tasks.records.first?.state, .completed)
        model.requestConsent(); model.send()
        let second = try XCTUnwrap(model.lifetime.operation)
        model.cancel(); await second.value
        XCTAssertEqual(sends, 1)
        XCTAssertEqual(fixture.tasks.records.count, 1)
        XCTAssertEqual(fixture.tasks.records.first?.state, .completed)
    }

    func testSourceClosureDuringUncooperativeReadCannotRestorePreview() async throws {
        let fixture = try Fixture(); defer { fixture.close() }
        let started = expectation(description: "Reading source")
        var reply: CheckedContinuation<PageSnapshot, any Error>?
        let tab = try XCTUnwrap(fixture.browser.activeTab)
        let snapshot = PageSnapshot(id: tab.id, navigationEpoch: tab.navigationEpoch, url: tab.url!, title: "Synthetic", text: "Private fixture", capturedAt: Date(), truncated: false)
        let model = PageAssistantModel(lifetime: AssistantOperationLifetime(applicationIsActive: { true }), readPage: { _, _ in
            try await withCheckedThrowingContinuation { reply = $0; started.fulfill() }
        })
        fixture.attach(model); model.readPages()
        let operation = try XCTUnwrap(model.lifetime.operation)
        await fulfillment(of: [started], timeout: 2)
        fixture.browser.close(tab.id)
        reply?.resume(returning: snapshot); await operation.value
        XCTAssertTrue(model.snapshots.isEmpty); XCTAssertNil(model.consent); XCTAssertFalse(model.busy)
    }

    func testOwningSceneRevokesConsentWhileAnotherWindowRemainsUsable() async throws {
        let first = try Fixture(), second = try Fixture(); defer { first.close(); second.close() }
        let firstModel = model { _, _, _, _, _, _ in "First [1]" }, secondModel = model { _, _, _, _, _, _ in "Second [1]" }
        first.attach(firstModel); second.attach(secondModel)
        firstModel.lifetime.attach(sceneID: "First", isActive: true); secondModel.lifetime.attach(sceneID: "Second", isActive: true)
        try await read(firstModel); try await read(secondModel)
        firstModel.requestConsent(); secondModel.requestConsent()
        firstModel.lifetime.deactivateScene("First"); secondModel.lifetime.deactivateScene("First")
        XCTAssertNil(firstModel.consent); XCTAssertTrue(firstModel.snapshots.isEmpty)
        XCTAssertNotNil(secondModel.consent)
        firstModel.lifetime.activateScene("First"); XCTAssertNil(firstModel.consent)
        secondModel.send(); let operation = try XCTUnwrap(secondModel.lifetime.operation); await operation.value
        XCTAssertTrue(secondModel.outputComplete)
    }
}
