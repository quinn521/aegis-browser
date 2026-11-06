import XCTest
import WebKit
import SwiftUI
@testable import Aegis
@testable import BrowserKit
@testable import AgentKit

@MainActor
final class AppLanguageTests: XCTestCase {
    private var streamedText = ""
    private final class Defaults: Aegis.AegisLanguageDefaults {
        var values: [String: Any] = ["unrelated": "keep"]
        func string(forKey key: String) -> String? { values[key] as? String }
        func set(_ value: Any?, forKey key: String) { values[key] = value }
        func removeObject(forKey key: String) { values.removeValue(forKey: key) }
    }

    private struct PolicyCase: Decodable {
        let name: String
        let selected: String?
        let preferences: [String]
        let systemLocale: String
        let family: String
        let dialect: String
        let appResource: String
        let agentResource: String
    }

    func testIndependentPolicyFixturesAndResourceOwners() throws {
        let url = try XCTUnwrap(Bundle(for: Self.self).url(forResource: "language-policy", withExtension: "json",
                                                         subdirectory: "Fixtures/Localization"))
        let cases = try JSONDecoder().decode([PolicyCase].self, from: Data(contentsOf: url))
        XCTAssertEqual(cases.count, 45)
        let agent = try XCTUnwrap(Bundle(identifier: "com.gcsa.aegis.agentkit"))
        for item in cases {
            let snapshot = Aegis.AegisLocalizationSnapshot.resolve(
                family: item.selected.flatMap(Aegis.AegisLanguageFamily.init(rawValue:)),
                preferences: item.preferences, systemLocale: Locale(identifier: item.systemLocale))
            XCTAssertEqual(snapshot.family.rawValue, item.family, item.name)
            XCTAssertEqual(snapshot.dialect, item.dialect, item.name)
            XCTAssertEqual(Aegis.AegisLocalization.resourceLocalization(in: .main, snapshot: snapshot), item.appResource, item.name)
            XCTAssertEqual(Aegis.AegisLocalization.resourceLocalization(in: agent, snapshot: snapshot), item.agentResource, item.name)
        }
    }

    func testControllerPersistsOnlyFamilyAndKeepsUnrelatedPreferences() {
        let defaults = Defaults()
        let store = Aegis.AegisLanguagePreferenceStore(defaults: defaults)
        let controller = AppLanguageSettings(store: store)
        for family in Aegis.AegisLanguageFamily.allCases {
            controller.select(family)
            XCTAssertEqual(controller.family, family)
            XCTAssertEqual(defaults.string(forKey: Aegis.AegisLanguagePreferenceStore.key), family.rawValue)
            XCTAssertEqual(AppLanguageSettings(store: store).family, family)
        }
        controller.select(.en)
        for (region, dialect) in [("en_GB", "en-GB"), ("en_HK", "en-HK")] {
            let resolved = Aegis.AegisLocalization.snapshot(store: store, preferences: ["en"], systemLocale: Locale(identifier: region))
            XCTAssertEqual(resolved.dialect, dialect)
            XCTAssertEqual(store.family, .en)
        }
        defaults.set("en-GB", forKey: Aegis.AegisLanguagePreferenceStore.key)
        controller.refresh()
        XCTAssertNil(controller.family)
        controller.select(nil)
        XCTAssertEqual(defaults.values.count, 1)
        XCTAssertEqual(defaults.string(forKey: "unrelated"), "keep")
    }

    func testFormattingOverridesStayIndependentOfDialect() {
        let system = Locale(identifier: "th-TH-u-ca-buddhist-hc-h23-nu-thai-tz-usnyc")
        let snapshot = Aegis.AegisLocalizationSnapshot.resolve(family: .en, preferences: ["en-GB"], systemLocale: system)
        XCTAssertEqual(snapshot.locale.language.region?.identifier, "GB")
        XCTAssertEqual(snapshot.locale.region, system.region)
        XCTAssertEqual(snapshot.locale.calendar.identifier, system.calendar.identifier)
        XCTAssertEqual(snapshot.locale.hourCycle, system.hourCycle)
        XCTAssertEqual(snapshot.locale.numberingSystem, system.numberingSystem)
        XCTAssertEqual(snapshot.locale.timeZone, system.timeZone)
    }

    func testRuntimeRefreshPreservesWindowsWebViewsAndExternalForm() async throws {
        let data = BrowserDataStore(persistenceURL: nil)
        let windows = BrowserWindowStore(persistenceURL: nil)
        let preferences = BrowserPreferences(defaults: UserDefaults(suiteName: "com.gcsa.aegis.ios.ui-tests")!)
        let first = BrowserSession(dataStore: data, workspaceStore: WorkspaceStore(persistenceURL: nil),
                                   windowID: windows.defaultWindowID(), windowStore: windows, preferences: preferences)
        let second = BrowserSession(dataStore: data, workspaceStore: WorkspaceStore(persistenceURL: nil),
                                    windowID: try windows.createWindow(), windowStore: windows, preferences: preferences)
        let start = try XCTUnwrap(first.activeTab)
        let external = try XCTUnwrap(second.activeTab)
        start.load(URL(string: "aegis://start")!)
        // The existing integration server supplies a real external HTTP document.
        external.load(URL(string: "http://127.0.0.1:8768/article")!)
        defer { start.stop(); external.stop() }
        try await waitUntil { !start.isLoading && start.url?.absoluteString == "aegis://start" &&
            !external.isLoading && external.url?.path == "/article" }
        let startView = start.webView, externalView = external.webView
        _ = try await externalView.evaluateJavaScript("window.languageSentinel=41;document.querySelector('main').insertAdjacentHTML('beforeend','<input id=language-draft value=KEEP_DRAFT>')")
        let epoch = external.navigationEpoch
        let backCount = externalView.backForwardList.backList.count
        let history = data.history
        let ids = (first.activeTabID, second.activeTabID)
        let snapshot = BrowserKit.AegisLocalizationSnapshot.resolve(family: .de, preferences: ["en-US"], systemLocale: Locale(identifier: "en_US"))
        first.refreshLocalizedPresentation(snapshot: snapshot)
        second.refreshLocalizedPresentation(snapshot: snapshot)
        try await waitUntil {
            (try? await startView.evaluateJavaScript("document.documentElement.lang") as? String) == "de"
        }
        XCTAssertTrue(first.activeTab === start && second.activeTab === external)
        XCTAssertTrue(start.webView === startView && external.webView === externalView)
        XCTAssertEqual(first.activeTabID, ids.0)
        XCTAssertEqual(second.activeTabID, ids.1)
        XCTAssertEqual(external.navigationEpoch, epoch)
        XCTAssertEqual(externalView.backForwardList.backList.count, backCount)
        XCTAssertEqual(data.history, history)
        let draft = try await externalView.evaluateJavaScript("document.getElementById('language-draft').value") as? String
        XCTAssertEqual(draft, "KEEP_DRAFT")
        let sentinel = try await externalView.evaluateJavaScript("window.languageSentinel") as? Int
        XCTAssertEqual(sentinel, 41)
        let original = try await startView.evaluateJavaScript("document.querySelector('[data-aegis-start=heading]').textContent") as? String
        let changed = try await startView.callAsyncJavaScript(FixtureSchemeHandler.refreshScript, arguments: [
            "expectedURL": "aegis://start", "documentID": "wrong-document", "revision": 100,
            "labels": ["heading": "WRONG_DOCUMENT"], "language": "en",
        ], in: nil, contentWorld: .defaultClient)
        XCTAssertEqual(changed as? Bool, false)
        let afterRejectedUpdate = try await startView.evaluateJavaScript("document.querySelector('[data-aegis-start=heading]').textContent") as? String
        XCTAssertEqual(afterRejectedUpdate, original)
    }

    func testRefreshWhileDownloadAndModelStreamAreActive() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let defaults = UserDefaults(suiteName: "com.gcsa.aegis.ios.ui-tests")!
        let downloads = DownloadManager(directory: directory, background: false, defaults: defaults)
        await downloads.retryStorage()
        try await waitUntil { downloads.isReady }
        let id = try downloads.start("http://127.0.0.1:8768/slow.bin")
        defer { downloads.cancel(id) }
        let client = ModelClient(configuration: ModelConfiguration(endpoint: "http://127.0.0.1:8768/v1", model: "stream-slow"), key: "")
        let task = Task { @MainActor in
            try await client.streamComplete(goal: "Summarize", sources: ["城市绿地观察"], language: "en-US") { [weak self] text in
                self?.streamedText = text
            }
        }
        defer { task.cancel() }
        try await waitUntil { !self.streamedText.isEmpty && (downloads.items.first { $0.id == id }?.received ?? 0) > 0 }
        let received = try XCTUnwrap(downloads.items.first { $0.id == id }).received
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil), workspaceStore: WorkspaceStore(persistenceURL: nil),
                                     preferences: BrowserPreferences(defaults: defaults))
        let tab = try XCTUnwrap(browser.activeTab)
        for family in [BrowserKit.AegisLanguageFamily.de, .traditionalChinese, .en] {
            browser.refreshLocalizedPresentation(snapshot: .resolve(family: family, preferences: ["en-US"], systemLocale: Locale(identifier: "en_US")))
        }
        let output = try await task.value
        XCTAssertFalse(output.isEmpty)
        XCTAssertTrue(browser.activeTab === tab)
        try await waitUntil { downloads.items.first { $0.id == id }?.state == .completed }
        XCTAssertGreaterThanOrEqual(try XCTUnwrap(downloads.items.first { $0.id == id }).received, received)
    }

    func testExistingUIKitFieldsRefreshWithoutLosingMarkedTextOrFocus() async throws {
        guard ProcessInfo.processInfo.arguments.contains("--ui-testing"),
              !ProcessInfo.processInfo.arguments.contains("-browser.languageFamily.v1") else {
            throw XCTSkip("Requires isolated --ui-testing host without a fixed argument-domain family")
        }
        let defaults = UserDefaults(suiteName: "com.gcsa.aegis.ios.ui-tests")!
        let key = Aegis.AegisLanguagePreferenceStore.key
        let old = defaults.persistentDomain(forName: "com.gcsa.aegis.ios.ui-tests")?[key]
        defer { if let old { defaults.set(old, forKey: key) } else { defaults.removeObject(forKey: key) } }
        let language = AppLanguageSettings(store: .init(defaults: defaults))
        language.select(.en)
        let host = UIHostingController(rootView: FieldHarness(language: language))
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let previous = scene.windows.first { $0.isKeyWindow }
        let window = UIWindow(windowScene: scene)
        window.rootViewController = host
        window.makeKeyAndVisible()
        defer { window.isHidden = true; previous?.makeKeyAndVisible() }
        try await waitUntil { self.field("address-field", in: host.view) != nil && self.field("find-text", in: host.view) != nil }
        let address = try XCTUnwrap(field("address-field", in: host.view))
        let find = try XCTUnwrap(field("find-text", in: host.view))
        address.becomeFirstResponder()
        address.setMarkedText("草稿", selectedRange: NSRange(location: 2, length: 0))
        let text = address.text
        language.select(.de)
        try await waitUntil { address.placeholder == "Suchen oder Adresse eingeben" && find.placeholder == "Suchtext" }
        XCTAssertTrue(field("address-field", in: host.view) === address)
        XCTAssertTrue(field("find-text", in: host.view) === find)
        XCTAssertEqual(address.text, text)
        XCTAssertNotNil(address.markedTextRange)
        XCTAssertTrue(address.isFirstResponder)
    }

    private struct FieldHarness: View {
        @ObservedObject var language: AppLanguageSettings
        @State private var address = "draft"
        @State private var focused = true
        @State private var find = "KEEP_FIND"
        var body: some View {
            VStack {
                BrowserAddressField(address: $address, focused: $focused, displayText: "draft", selectionRequest: 0, submit: { _ in })
                FindQueryField(text: $find)
            }.environment(\.locale, language.snapshot.locale)
        }
    }

    private func field(_ identifier: String, in view: UIView) -> UITextField? {
        if let field = view as? UITextField, field.accessibilityIdentifier == identifier { return field }
        return view.subviews.lazy.compactMap { self.field(identifier, in: $0) }.first
    }

    private func waitUntil(_ predicate: () async -> Bool) async throws {
        for _ in 0..<100 {
            if await predicate() { return }
            try await Task.sleep(for: .milliseconds(100))
        }
        XCTFail("Timed out waiting for language/document state")
        throw URLError(.timedOut)
    }
}
