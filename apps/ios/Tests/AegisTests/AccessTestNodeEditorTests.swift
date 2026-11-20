import AccessKit
import CoreImage
import WebKit
import UIKit
import CryptoKit
import Network
import Security
import Synchronization
import Darwin
import XCTest
@testable import Aegis

@MainActor
final class AccessTestNodeEditorTests: XCTestCase {
    private let link = "vless://550e8400-e29b-41d4-a716-446655440000@198.51.100.10:443"
        + "?type=tcp&security=reality&flow=xtls-rprx-vision&sni=www.example.com"
        + "&pbk=abcdefghijklmnopqrstuvwxyzABCDEFG&sid=abcd1234#synthetic-node"

    func testTransportAdmissionAndBindingTimeSelection() throws {
        XCTAssertEqual(try AccessTestNodeTransport.automatic.select(socksAdmitted: true), .socks5)
        XCTAssertEqual(try AccessTestNodeTransport.automatic.select(socksAdmitted: false), .http)
        XCTAssertEqual(try AccessTestNodeTransport.http.select(socksAdmitted: true), .http)
        XCTAssertThrowsError(try AccessTestNodeTransport.socks5.select(socksAdmitted: false))
        XCTAssertTrue(AccessInternalSOCKSAdmission.admitted(major: 26, minor: 6, patch: 1, build: "23G83", phone: true, simulator: false))
        for values in [(26, 6, 1, "23G84", true, false), (26, 6, 1, "23G83", false, false),
                       (26, 6, 1, "23G83", true, true), (27, 0, 0, "24A1", true, false)] {
            XCTAssertFalse(AccessInternalSOCKSAdmission.admitted(major: values.0, minor: values.1, patch: values.2,
                build: values.3, phone: values.4, simulator: values.5))
        }
    }

    func testNativeAdmissionRejectsMixedOrRestrictedIdentitiesEvenWithCapabilityEnabled() {
        let normal = "com.gcsa.aegis"
        let alpha = "com.gcsa.aegis.alpha.internal"
        func resolve(_ bundle: String?, _ channel: String?, _ profile: String?,
                     _ capability: String? = "YES", debug: Bool = true) -> AccessInternalOptions? {
            AccessInternalOptions.resolve(bundleID: bundle, channel: channel, releaseProfile: profile,
                                          capability: capability, debug: debug)
        }
        XCTAssertEqual(resolve(normal, "dev", "dev"), .dev)
        XCTAssertEqual(resolve(alpha, "alpha", "alpha", debug: false), .alpha)
        XCTAssertNil(resolve(normal, "dev", "dev", debug: false))
        XCTAssertNil(resolve(normal, "alpha", "alpha"))
        XCTAssertNil(resolve(alpha, "dev", "dev"))
        XCTAssertNil(resolve(alpha, "alpha", "dev"))
        XCTAssertNil(resolve(alpha, "alpha", "alpha", "NO"))
        XCTAssertNil(resolve(nil, "dev", "dev"))
        XCTAssertNil(resolve(alpha, nil, nil))
        for channel in ["beta", "release", "unknown"] {
            XCTAssertNil(resolve(alpha, channel, channel))
            XCTAssertNil(resolve(normal, channel, channel))
        }
    }

    func testTextAndScanShareWholeLinkValidationAndApplyIsExplicit() throws {
        let storage = Storage()
        let repository = AccessTestNodeRepository(channel: .dev, storage: storage)
        let editor = AccessTestNodeEditor(isPrivate: false, repository: repository)
        editor.openProfile()
        editor.preview(link + "\n" + link)
        XCTAssertEqual(editor.error, .invalidLink)
        XCTAssertFalse(editor.canApply)
        editor.apply()
        XCTAssertEqual(storage.writes, 0)

        editor.preview(link)
        XCTAssertTrue(editor.hasDraft)
        XCTAssertEqual(storage.writes, 0)
        editor.discard()
        XCTAssertFalse(editor.hasDraft)
        let scan = try XCTUnwrap(editor.beginScan())
        editor.finishScan(scan, result: .success(link + "\n" + link))
        XCTAssertEqual(editor.error, .invalidLink)
        XCTAssertFalse(editor.hasDraft)
        let singleScan = try XCTUnwrap(editor.beginScan())
        editor.finishScan(singleScan, result: .success(link))
        XCTAssertTrue(editor.hasDraft)
        XCTAssertFalse(editor.hasSavedNode)
        editor.apply()
        XCTAssertFalse(editor.hasDraft)
        XCTAssertTrue(editor.hasSavedNode)
        XCTAssertEqual(storage.writes, 1)
        XCTAssertNotNil(try repository.revision(isPrivate: false))
    }

    func testCameraDenialCancellationAndProfileSwitchDoNotReplaceSavedNode() throws {
        let storage = Storage()
        let repository = AccessTestNodeRepository(channel: .dev, storage: storage)
        let editor = AccessTestNodeEditor(isPrivate: false, repository: repository)
        editor.openProfile()
        let denied = try XCTUnwrap(editor.beginScan())
        editor.finishScan(denied, result: .failure(.cameraDenied))
        XCTAssertEqual(editor.error, .cameraDenied)
        editor.preview(link)
        editor.apply()
        let old = try repository.revision(isPrivate: false)

        let cancelled = try XCTUnwrap(editor.beginScan())
        editor.finishScan(cancelled, result: nil)
        editor.finishScan(cancelled, result: .success(link))
        XCTAssertFalse(editor.hasDraft)

        let leftProfile = try XCTUnwrap(editor.beginScan())
        editor.closeProfile()
        editor.openProfile()
        let newScan = try XCTUnwrap(editor.beginScan())
        editor.finishScan(leftProfile, result: .success(link))
        XCTAssertEqual(editor.scan, newScan)
        XCTAssertFalse(editor.hasDraft)
        editor.apply()
        XCTAssertEqual(old, try repository.revision(isPrivate: false))
        XCTAssertEqual(storage.writes, 1)
    }

    func testOfficialRealityShareWorksForTextAndScanAndRestoresNativeParameters() throws {
        let uri = link.replacingOccurrences(of: "sid=abcd1234",
            with: "sid=&spx=%2Fsynthetic-path&support-x25519mlkem768=true")
        for scanned in [false, true] {
            let storage = Storage()
            let repository = AccessTestNodeRepository(channel: .dev, storage: storage)
            let editor = AccessTestNodeEditor(isPrivate: false, repository: repository)
            editor.openProfile()
            if scanned {
                editor.finishScan(try XCTUnwrap(editor.beginScan()), result: .success(uri))
            } else { editor.preview(uri) }
            XCTAssertTrue(editor.canApply)
            XCTAssertEqual(storage.writes, 0)
            editor.apply()
            XCTAssertEqual(storage.writes, 1)
            let restored = try AccessTestNodeRepository(channel: .dev, storage: storage).savedNode().parameters()
            XCTAssertEqual(restored, try AccessLocalV2RayConfigLoader.parseSingleShareURI(uri))
            XCTAssertEqual(restored.shortID, "")
            XCTAssertEqual(restored.spiderX, "/synthetic-path")
        }
    }

    func testCampaignReorderingRequiresAnExactPermutationAndSafeIndependentIdentity() throws {
        let canonical = ["one.example", "two.example", "three.example"]
        let reordered = Data("three.example\none.example\ntwo.example\n".utf8)
        let digest = SHA256.hash(data: reordered).map { String(format: "%02x", $0) }.joined()
        XCTAssertEqual(try AccessDomainCampaignInput.order(reordered, canonical: canonical, digest: digest),
                       ["three.example", "one.example", "two.example"])
        for invalid in ["one.example\none.example\nthree.example\n", "one.example\ntwo.example\n",
                        "one.example\ntwo.example\noutside.example\n", "one.example\ntwo.example\nTHREE.example\n"] {
            let bytes = Data(invalid.utf8)
            let hash = SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
            XCTAssertThrowsError(try AccessDomainCampaignInput.order(bytes, canonical: canonical, digest: hash))
        }
        XCTAssertThrowsError(try AccessDomainCampaignInput.order(reordered, canonical: canonical, digest: "wrong"))
        XCTAssertTrue(AccessDomainCampaignInput.validID("popularity-20261121-test01-v1"))
        for invalid in ["", "../old", "a/b", "UPPER", String(repeating: "a", count: 65)] {
            XCTAssertFalse(AccessDomainCampaignInput.validID(invalid))
        }
    }

    func testTwoWindowsCompareSavedRevisionAndFailedStoragePreservesDraft() {
        let storage = Storage()
        let repository = AccessTestNodeRepository(channel: .alpha, storage: storage)
        let first = AccessTestNodeEditor(isPrivate: false, repository: repository)
        let second = AccessTestNodeEditor(isPrivate: false, repository: repository)
        for editor in [first, second] {
            editor.openProfile()
            editor.preview(link)
        }
        storage.failWrite = true
        first.apply()
        XCTAssertTrue(first.hasDraft)
        XCTAssertEqual(first.error, .storageUnavailable)
        XCTAssertFalse(first.hasSavedNode)
        storage.failWrite = false
        first.apply()
        second.apply()
        XCTAssertEqual(storage.writes, 1)
        XCTAssertTrue(second.hasDraft)
        XCTAssertEqual(second.error, .conflict)
        second.discard()
        XCTAssertTrue(second.hasSavedNode)
        XCTAssertFalse(second.canApply)
    }

    func testDirectCommandsAndRestoreNeverTouchRestrictedOrPrivateStorage() throws {
        let storage = Storage()
        let params = try AccessLocalV2RayConfigLoader.parseSingleShareURI(link)
        storage.value = AccessTestNodeRecord(params)
        for (channel, isPrivate): (AccessInternalOptions?, Bool) in [(nil, false), (.dev, true), (.alpha, true)] {
            let repository = AccessTestNodeRepository(channel: channel, storage: storage)
            XCTAssertFalse(repository.allowsEditing(isPrivate: isPrivate))
            XCTAssertThrowsError(try repository.revision(isPrivate: isPrivate))
            XCTAssertThrowsError(try repository.apply(params, expectedRevision: nil, isPrivate: isPrivate))
            XCTAssertThrowsError(try repository.remove(expectedRevision: UUID(), isPrivate: isPrivate))
            let editor = AccessTestNodeEditor(isPrivate: isPrivate, repository: repository)
            editor.openProfile()
            XCTAssertNil(editor.beginScan())
            editor.preview(link)
            editor.apply()
            XCTAssertFalse(editor.hasDraft)
            XCTAssertFalse(editor.hasSavedNode)
        }
        XCTAssertEqual(storage.reads, 0)
        XCTAssertEqual(storage.writes, 0)
        XCTAssertEqual(storage.removals, 0)
    }

    func testKeychainStoresNormalizedRecordAndSeparatesChannels() throws {
        let service = "test.access-node." + UUID().uuidString
        let storage = AccessTestNodeKeychain(service: service)
        defer {
            SecItemDelete([kSecClass as String: kSecClassGenericPassword,
                           kSecAttrService as String: service] as CFDictionary)
        }
        let first = AccessTestNodeRepository(channel: .dev, storage: storage)
        let params = try AccessLocalV2RayConfigLoader.parseSingleShareURI(link)
        let revision = try first.apply(params, expectedRevision: nil, isPrivate: false)
        let reopened = AccessTestNodeRepository(channel: .dev, storage: storage)
        XCTAssertEqual(try reopened.revision(isPrivate: false), revision)
        XCTAssertNil(try AccessTestNodeRepository(channel: .alpha, storage: storage).revision(isPrivate: false))

        let record = try XCTUnwrap(storage.read(channel: .dev))
        let serialized = String(decoding: try JSONEncoder().encode(record), as: UTF8.self)
        XCTAssertFalse(serialized.contains("synthetic-node"))
        XCTAssertFalse(serialized.contains("vless://"))
        XCTAssertThrowsError(try reopened.remove(expectedRevision: UUID(), isPrivate: false))
        XCTAssertEqual(try reopened.revision(isPrivate: false), revision)
        try reopened.remove(expectedRevision: revision, isPrivate: false)
        XCTAssertNil(try reopened.revision(isPrivate: false))
    }

    func testDecodedSyntheticQRCodeMatchesTextAndRejectsMultiplePayloads() throws {
        let generator = try XCTUnwrap(CIFilter(name: "CIQRCodeGenerator"))
        generator.setValue(Data(link.utf8), forKey: "inputMessage")
        let image = try XCTUnwrap(generator.outputImage).transformed(by: CGAffineTransform(scaleX: 5, y: 5))
        let context = CIContext(options: [.useSoftwareRenderer: true])
        let detector = try XCTUnwrap(CIDetector(ofType: CIDetectorTypeQRCode, context: context,
                                               options: [CIDetectorAccuracy: CIDetectorAccuracyHigh]))
        let payloads = detector.features(in: image).map { ($0 as? CIQRCodeFeature)?.messageString }
        let decoded = try AccessTestNodeQRCode.payload(from: payloads).get()
        XCTAssertEqual(try AccessLocalV2RayConfigLoader.parseSingleShareURI(decoded),
                       try AccessLocalV2RayConfigLoader.parseSingleShareURI(link))
        let storage = Storage()
        let editor = AccessTestNodeEditor(isPrivate: false,
            repository: AccessTestNodeRepository(channel: .dev, storage: storage))
        editor.openProfile()
        let scan = try XCTUnwrap(editor.beginScan())
        editor.finishScan(scan, result: AccessTestNodeQRCode.payload(from: payloads))
        XCTAssertTrue(editor.canApply)
        XCTAssertEqual(storage.writes, 0)
        editor.apply()
        XCTAssertEqual(storage.writes, 1)
        XCTAssertThrowsError(try AccessTestNodeQRCode.payload(from: [link, link]).get())
        XCTAssertThrowsError(try AccessTestNodeQRCode.payload(from: []).get())
        XCTAssertThrowsError(try AccessTestNodeQRCode.payload(from: [nil]).get())
    }

    func testNewScanInvalidatesOldDraftAndCorruptRestoreCannotApply() throws {
        let storage = Storage()
        let editor = AccessTestNodeEditor(isPrivate: false,
            repository: AccessTestNodeRepository(channel: .dev, storage: storage))
        editor.openProfile()
        editor.preview(link)
        XCTAssertTrue(editor.canApply)
        let scan = try XCTUnwrap(editor.beginScan())
        editor.finishScan(scan, result: AccessTestNodeQRCode.payload(from: [nil]))
        XCTAssertFalse(editor.canApply)
        editor.apply()
        XCTAssertEqual(storage.writes, 0)

        let record = AccessTestNodeRecord(try AccessLocalV2RayConfigLoader.parseSingleShareURI(link))
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(record)) as? [String: Any])
        for version: Int? in [nil, 0, 2] {
            json["version"] = version
            XCTAssertThrowsError(try JSONDecoder().decode(AccessTestNodeRecord.self,
                from: JSONSerialization.data(withJSONObject: json)))
        }
    }

    func testPageBindsOneHTTPStoreFromExplicitSavedProfileAndRefusesRestrictedRestore() async throws {
        let storage = Storage()
        let repository = AccessTestNodeRepository(channel: .dev, storage: storage)
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(link),
                                 expectedRevision: nil, isPrivate: false)
        let core = PageCore()
        let page = AccessTestNodePage(repository: repository, testingCore: core)
        await page.open(URL(string: "https://example.invalid/single")!)
        XCTAssertTrue(core.isRunning)
        XCTAssertNotNil(page.endpoint)
        XCTAssertEqual(page.webView?.configuration.websiteDataStore.proxyConfigurations.count, 1)
        XCTAssertEqual(page.webView?.configuration.websiteDataStore.proxyConfigurations.first?.allowFailover, false)
        let original = page.webView
        await page.open(URL(string: "https://second.invalid/")!)
        XCTAssertTrue(original === page.webView)
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(link),
                                 expectedRevision: repository.revision(isPrivate: false), isPrivate: false)
        XCTAssertEqual(page.status, "连接暂不可用", "Apply revokes old pages before returning")

        let deniedCore = PageCore()
        let denied = AccessTestNodePage(repository: AccessTestNodeRepository(channel: nil, storage: storage),
                                       testingCore: deniedCore)
        await denied.open(URL(string: "https://example.invalid/")!)
        XCTAssertFalse(deniedCore.isRunning)
        XCTAssertNil(denied.webView)
        XCTAssertNil(denied.endpoint)
    }

    func testExplicitSOCKSRequiresAdmissionAndPublishesItsActualTransport() async throws {
        let repository = AccessTestNodeRepository(channel: .dev, storage: Storage())
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(link), expectedRevision: nil, isPrivate: false)
        let rejectedCore = PageCore()
        let rejected = AccessTestNodePage(repository: repository, testingCore: rejectedCore,
                                         transport: .socks5, socksAdmitted: false)
        await rejected.open(URL(string: "https://example.invalid/")!)
        XCTAssertFalse(rejectedCore.isRunning)
        XCTAssertNil(rejected.webView)
        let admitted = AccessTestNodePage(repository: repository, testingCore: PageCore(),
                                         transport: .socks5, socksAdmitted: true)
        await admitted.open(URL(string: "https://example.invalid/")!)
        XCTAssertEqual(admitted.endpoint?.transport, .socks5)
        XCTAssertNotEqual(admitted.endpoint?.port, 0)
        XCTAssertNotEqual(admitted.endpoint?.port, 1080)
        XCTAssertEqual(admitted.webView?.configuration.websiteDataStore.proxyConfigurations.count, 1)
        XCTAssertEqual(admitted.webView?.configuration.websiteDataStore.proxyConfigurations.first?.allowFailover, false)
        admitted.close()
    }

    func testPageExposesBothWebKitPolicySelectors() {
        let page = AccessTestNodePage()
        XCTAssertTrue(page.responds(to: NSSelectorFromString("webView:decidePolicyForNavigationAction:decisionHandler:")))
        XCTAssertTrue(page.responds(to: NSSelectorFromString("webView:decidePolicyForNavigationResponse:decisionHandler:")))
    }

    func testSupersededNavigationFailuresCannotCloseTheReplacement() throws {
        let page = AccessTestNodePage()
        let view = WKWebView()
        let first = try XCTUnwrap(view.loadHTMLString("<p>first</p>", baseURL: URL(string: "https://aegis.invalid/first")))
        let replacement = try XCTUnwrap(view.loadHTMLString("<p>replacement</p>", baseURL: URL(string: "https://aegis.invalid/replacement")))
        page.webView(view, didStartProvisionalNavigation: first)
        page.webView(view, didStartProvisionalNavigation: replacement)
        for error in [URLError(.cancelled), URLError(.cannotConnectToHost)] {
            page.webView(view, didFailProvisionalNavigation: first, withError: error)
            XCTAssertEqual(page.status, "正在加载")
        }
        page.webView(view, didFinish: replacement)
        page.webView(view, didFail: first, withError: URLError(.serverCertificateUntrusted))
        XCTAssertEqual(page.status, "已连接")
        page.webView(view, didFail: replacement, withError: URLError(.cannotConnectToHost))
        XCTAssertEqual(page.status, "连接暂不可用")
    }

    func testPolicyInterruptionAttributionRemainsUnknownWithoutAnObservedDecision() throws {
        let page = AccessTestNodePage()
        XCTAssertEqual(page.navigationPolicyObservation, "no-local-rejection-observed")
        let view = WKWebView()
        let navigation = try XCTUnwrap(view.loadHTMLString("<p>policy</p>", baseURL: URL(string: "https://aegis.invalid/policy")))
        page.webView(view, didStartProvisionalNavigation: navigation)
        page.webView(view, didFailProvisionalNavigation: navigation,
                     withError: NSError(domain: "WebKitErrorDomain", code: 102))
        XCTAssertNil(page.localPolicyRejection)
        XCTAssertEqual(page.navigationPolicyObservation, "policy-interruption-unattributed")
        XCTAssertEqual(page.status, "连接暂不可用")
    }

    func testClosingPendingPageCommitsInertDocumentAndReleasesEndpoint() async throws {
        let repository = AccessTestNodeRepository(channel: .dev, storage: Storage())
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(link),
                                 expectedRevision: nil, isPrivate: false)
        let page = AccessTestNodePage(repository: repository, testingCore: PageCore())
        await page.open(URL(string: "https://example.invalid/pending")!)
        let view = try XCTUnwrap(page.webView)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 390, height: 844))
        window.windowScene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        window.addSubview(view)
        view.frame = window.bounds
        window.makeKeyAndVisible()
        defer { view.removeFromSuperview(); window.isHidden = true }
        XCTAssertNotNil(page.endpoint)
        page.close()
        let deadline = Date().addingTimeInterval(10)
        while page.endpoint != nil, Date() < deadline { try await Task.sleep(for: .milliseconds(50)) }
        XCTAssertNil(page.endpoint, "A proxy-bound pending document must retire before its port is released")
        XCTAssertNotNil(page.initialBinding, "Retirement must retain its immutable receipt identity")
        XCTAssertEqual(view.url?.host, "aegis.invalid")
        XCTAssertTrue(view.url?.path.hasPrefix("/retired/") == true)
    }

    func testContentProcessTerminationRequiresInertCommitInsteadOfReplayingTarget() async throws {
        let repository = AccessTestNodeRepository(channel: .dev, storage: Storage())
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(link), expectedRevision: nil, isPrivate: false)
        let core = PageCore()
        let page = AccessTestNodePage(repository: repository, testingCore: core)
        await page.open(URL(string: "https://example.invalid/pending")!)
        let view = try XCTUnwrap(page.webView)
        let binding = page.initialBinding
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 390, height: 844))
        window.windowScene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        window.addSubview(view); view.frame = window.bounds; window.makeKeyAndVisible()
        defer { view.removeFromSuperview(); window.isHidden = true }
        page.webViewWebContentProcessDidTerminate(WKWebView())
        XCTAssertNotNil(page.endpoint, "Another view cannot revoke this binding")
        page.webViewWebContentProcessDidTerminate(view)
        XCTAssertEqual(page.failureDomain, WKError.errorDomain)
        XCTAssertEqual(page.failureCode, WKError.Code.webContentProcessTerminated.rawValue)
        XCTAssertNotNil(page.endpoint, "Process notification alone cannot release the port")
        let deadline = Date().addingTimeInterval(10)
        while page.endpoint != nil, Date() < deadline { try await Task.sleep(for: .milliseconds(50)) }
        XCTAssertNil(page.endpoint)
        XCTAssertEqual(page.initialBinding, binding)
        XCTAssertTrue(view.url?.path.hasPrefix("/retired/") == true)
        XCTAssertTrue(page.retirementEvents.contains("replacement-committed"))
    }

    private final class PageCore: AccessEmbeddedCoreControlling, @unchecked Sendable {
        let running = Mutex(false)
        var isRunning: Bool { running.withLock { $0 } }
        func start(configData: Data) async throws { running.withLock { $0 = true } }
        func stop() async { running.withLock { $0 = false } }
        func dial(host: String, port: UInt16) async throws -> any AccessRealityPathStream {
            throw URLError(.cannotConnectToHost)
        }
    }

    private final class Storage: AccessTestNodeStoring {
        var value: AccessTestNodeRecord?
        var reads = 0
        var writes = 0
        var removals = 0
        var failWrite = false

        func read(channel: AccessInternalOptions) throws -> AccessTestNodeRecord? {
            reads += 1
            return value
        }

        func write(_ record: AccessTestNodeRecord, channel: AccessInternalOptions) throws {
            if failWrite { throw AccessTestNodeError.storageUnavailable }
            writes += 1
            value = record
        }

        func remove(channel: AccessInternalOptions) throws {
            removals += 1
            value = nil
        }
    }
}

enum AccessDomainCampaignInput {
    static func validID(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 64 && value.utf8.allSatisfy {
            (97...122).contains($0) || (48...57).contains($0) || $0 == 45
        }
    }

    static func order(_ data: Data, canonical: [String], digest: String) throws -> [String] {
        let actual = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard actual == digest, let text = String(data: data, encoding: .utf8) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let hosts = text.split(separator: "\n").map(String.init)
        guard hosts.count == canonical.count, Set(hosts).count == hosts.count,
              Set(hosts) == Set(canonical) else { throw CocoaError(.fileReadCorruptFile) }
        return hosts
    }
}

#if DEBUG
@MainActor
final class AccessTestNodeDeviceTests: XCTestCase {
    func testPhysicalDomainCampaign() async throws {
#if targetEnvironment(simulator)
        throw XCTSkip("This campaign requires its designated physical device")
#else
        let env = ProcessInfo.processInfo.environment
        guard env["AEGIS_TEST_CAMPAIGN_DEVICE"] == "ios-device-2d62695fc7",
              let node = env["AEGIS_TEST_AUTHORIZED_NODE"],
              let input = Bundle.main.url(forResource: "domains", withExtension: "txt") else {
            throw XCTSkip("Explicit physical campaign inputs are required")
        }
        var system = utsname()
        uname(&system)
        let model = withUnsafePointer(to: &system.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        guard model == "iPhone14,3" else { XCTFail("Wrong physical device model"); return }
        let data = try Data(contentsOf: input)
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard digest == "84a004adf2537fde3befb724fe77512cf3a4acb1df8ec350cd898a6cf8428374" else {
            XCTFail("Campaign input digest differs"); return
        }
        var hosts = String(decoding: data, as: UTF8.self).split(separator: "\n").map(String.init)
        guard hosts.count == 4372, Set(hosts).count == hosts.count else { XCTFail("Campaign input is incomplete"); return }
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let runID = env["AEGIS_TEST_CAMPAIGN_RUN_ID"]
        let orderDigest = env["AEGIS_TEST_CAMPAIGN_ORDER_SHA256"]
        if let runID {
            guard AccessDomainCampaignInput.validID(runID), let orderDigest else {
                XCTFail("Independent campaign identity or order digest missing"); return
            }
            let ordered = try Data(contentsOf: documents.appendingPathComponent("ios208-" + runID + ".domains.txt"))
            hosts = try AccessDomainCampaignInput.order(ordered, canonical: hosts, digest: orderDigest)
        } else if orderDigest != nil { XCTFail("Order requires an independent campaign identity"); return }
        let limit: Int
        if let value = env["AEGIS_TEST_CAMPAIGN_LIMIT"] {
            guard let count = Int(value), (1...hosts.count).contains(count) else {
                XCTFail("Invalid campaign phase limit"); return
            }
            limit = count
        } else { limit = hosts.count }
        let validHosts = Set(hosts)
        let repository = AccessTestNodeRepository.shared
        guard repository.allowsEditing(isPrivate: false) else { XCTFail("Native admission denied"); return }
        let parameters = try AccessLocalV2RayConfigLoader.parseSingleShareURI(node)
        if (try? repository.savedNode().parameters()) != parameters {
            _ = try repository.apply(parameters, expectedRevision: repository.revision(isPrivate: false), isPrivate: false)
        }
        let file = documents.appendingPathComponent(runID.map { "ios208-" + $0 + "-dispatch.jsonl" }
            ?? "ios208-domain-dispatch.jsonl")
        if !FileManager.default.fileExists(atPath: file.path) {
            try Data().write(to: file, options: .atomic)
        }
        let prior = try Data(contentsOf: file)
        var reserved = Set<String>()
        for line in String(decoding: prior, as: UTF8.self).split(separator: "\n") {
            guard let json = line.data(using: .utf8), let record = try JSONSerialization.jsonObject(with: json) as? [String: Any],
                  record["dataset"] as? String == digest,
                  record["device"] as? String == "ios-device-2d62695fc7",
                  record["bundle"] as? String == Bundle.main.bundleIdentifier,
                  record["campaignID"] as? String == runID,
                  record["orderSHA256"] as? String == orderDigest else { XCTFail("Dispatch ledger is damaged"); return }
            if record["event"] as? String == "reserved" {
                guard let host = record["host"] as? String, validHosts.contains(host),
                      reserved.insert(host).inserted else { XCTFail("Duplicate or foreign dispatch key"); return }
            }
        }
        let output = try FileHandle(forWritingTo: file)
        defer { try? output.close() }
        try output.seekToEnd()
        func append(_ item: [String: Any]) throws {
            var record = item
            record["dataset"] = digest
            record["device"] = "ios-device-2d62695fc7"
            record["bundle"] = Bundle.main.bundleIdentifier
            record["sourceSHA"] = env["AEGIS_TEST_SOURCE_SHA"] ?? "unknown"
            record["campaignID"] = runID
            record["orderSHA256"] = orderDigest
            record["nodeInputSHA256"] = SHA256.hash(data: Data(node.utf8)).map { String(format: "%02x", $0) }.joined()
            record["os"] = ProcessInfo.processInfo.operatingSystemVersionString
            record["time"] = Date().timeIntervalSince1970
            var bytes = try JSONSerialization.data(withJSONObject: record, options: .sortedKeys)
            bytes.append(10)
            try output.write(contentsOf: bytes)
            try output.synchronize()
        }
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 390, height: 844))
        window.windowScene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        window.makeKeyAndVisible()
        defer { window.isHidden = true }
        let pathState = Mutex((label: "unknown", reachable: false, reported: false))
        let monitor = NWPathMonitor()
        let network: String = await withCheckedContinuation { continuation in
            monitor.pathUpdateHandler = { path in
                let label = path.usesInterfaceType(.wifi) ? "wifi" :
                    path.usesInterfaceType(.cellular) ? "cellular" :
                    path.usesInterfaceType(.wiredEthernet) ? "wired" : "other"
                let first = pathState.withLock { state in
                    let first = !state.reported
                    state = (label, path.status == .satisfied, true)
                    return first
                }
                if first { continuation.resume(returning: label) }
            }
            monitor.start(queue: DispatchQueue(label: "ios208-campaign-network"))
        }
        defer { monitor.cancel() }
        try append(["event":"network", "interface":network])
        var preparationFailures = 0
        var consecutiveNetworkFailures = 0
        let control = file.deletingLastPathComponent().appendingPathComponent("ios208-campaign-control")
        for host in hosts.prefix(limit) where !reserved.contains(host) {
            if (try? String(contentsOf: control, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)) == "stop" {
                try append(["event":"checkpoint", "reservedCount":reserved.count])
                return
            }
            let network = pathState.withLock { ($0.label, $0.reachable) }
            guard network.1 else {
                try append(["event":"checkpoint", "reason":"network-unavailable", "reservedCount":reserved.count])
                return
            }
            let attempt = UUID().uuidString
            try append(["event":"reserved", "host":host, "attempt":attempt, "network":network.0])
            reserved.insert(host)
            guard let url = URL(string: "https://" + host + "/") else { XCTFail("Invalid fixed hostname"); return }
            let page = AccessTestNodePage(singleNavigation: true)
            await page.open(url)
            if let view = page.webView { view.frame = window.bounds; window.addSubview(view) }
            let deadline = Date().addingTimeInterval(20)
            while page.status == "正在加载", Date() < deadline {
                try await Task.sleep(for: .milliseconds(100))
            }
            let timedOut = page.status == "正在加载"
            let documentDigest = !timedOut && page.status == "已连接"
                ? await Self.documentPrefixDigest(page.webView) : ""
            try append(["event":"result", "host":host, "attempt":attempt,
                        "state":timedOut ? "timeout" : page.status,
                        "errorDomain":page.failureDomain ?? "", "errorCode":page.failureCode ?? 0,
                        "httpStatus":page.httpStatus ?? 0, "documentPrefixSHA256":documentDigest,
                        "initialHTTPStatus":page.initialHTTPStatus ?? 0,
                        "initialResponseObserved":page.initialHTTPStatus != nil,
                        "serverRedirectCount":page.serverRedirectCount,
                        "navigationPolicy":page.navigationPolicyObservation,
                        "localPolicyRejection":page.localPolicyRejection ?? "",
                        "endpoint":page.initialBinding?.identity ?? "", "generation":page.initialBinding?.generation ?? 0,
                        "port":page.initialBinding?.port ?? 0,
                        "phase":page.initialBinding == nil ? "preparation" : "navigation",
                        "actualProtocol":page.initialBinding.map { $0.transport == .socks5 ? "socks5" : "http" } ?? "unbound",
                        "network":network.0])
            consecutiveNetworkFailures = timedOut || page.failureDomain == NSURLErrorDomain ? consecutiveNetworkFailures + 1 : 0
            preparationFailures = page.webView == nil ? preparationFailures + 1 : 0
            let retired = try await Self.observeRetirement(page) { supplement, confirmed, elapsed, events in
                try append(["event":supplement ? "retirement-supplement" : "retirement",
                            "host":host, "attempt":attempt, "retired":confirmed,
                            "observationSeconds":elapsed, "events":events])
            }
            guard retired else { XCTFail("Old document retirement unconfirmed; campaign stopped"); return }
            page.webView?.removeFromSuperview()
            guard preparationFailures < 3 else { XCTFail("Repeated preparation failures; campaign stopped"); return }
            if consecutiveNetworkFailures >= 10 {
                try append(["event":"checkpoint", "reason":"consecutive-network-failures", "reservedCount":reserved.count])
                return
            }
        }
        try append(["event":"checkpoint", "reason":"phase-complete", "phaseLimit":limit, "reservedCount":reserved.count])
#endif

    }

    /// The initial observation and eventual acknowledgement are separate facts.
    /// Keep the old view mounted and its port owned until the exact inert commit.
    /// Never retry the target or release anything to make a deadline appear green.
    private static func observeRetirement(
        _ page: AccessTestNodePage,
        afterClose: () async throws -> Void = {},
        checkpoint: (Bool, Bool, TimeInterval, [String]) throws -> Void
    ) async throws -> Bool {
        let started = Date()
        page.close()
        try await afterClose()
        for (index, limit) in [5.0, 10.0].enumerated() {
            while page.endpoint != nil, Date().timeIntervalSince(started) < limit {
                try await Task.sleep(for: .milliseconds(50))
            }
            let confirmed = page.endpoint == nil
            try checkpoint(index > 0, confirmed, Date().timeIntervalSince(started), page.retirementEvents)
            if confirmed { return true }
        }
        return false
    }

    /// Observation must not hold up the next checkpoint when WebContent stops responding.
    private static func documentPrefixDigest(_ view: WKWebView?) async -> String {
        guard let view else { return "" }
        return await withCheckedContinuation { continuation in
            let result = DocumentDigestResult(continuation)
            view.evaluateJavaScript("document.documentElement.outerHTML.slice(0, 65536)") { value, _ in
                let text = value as? String
                Task { @MainActor in result.finish(text) }
            }
            Task {
                try? await Task.sleep(for: .seconds(2))
                result.finish(nil)
            }
        }
    }

    private final class DocumentDigestResult {
        var continuation: CheckedContinuation<String, Never>?
        init(_ continuation: CheckedContinuation<String, Never>) { self.continuation = continuation }
        func finish(_ text: String?) {
            guard let continuation else { return }
            self.continuation = nil
            continuation.resume(returning: text.map { SHA256.hash(data: Data($0.utf8))
                .map { String(format: "%02x", $0) }.joined() } ?? "")
        }
    }

    /// Explicit operator-supplied inputs only. Never uses any member of the domain campaign as a probe.
    func testAuthorizedAppPath() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard let share = environment["AEGIS_TEST_AUTHORIZED_NODE"],
              let rawURL = environment["AEGIS_TEST_CONTROLLED_URL"], let url = URL(string: rawURL) else {
            throw XCTSkip("Explicit authorized node and independent controlled URL are required")
        }
        let repository = AccessTestNodeRepository.shared
        guard repository.allowsEditing(isPrivate: false) else {
            XCTFail("Native internal channel admission rejected this package")
            return
        }
        let editor = AccessTestNodeEditor(isPrivate: false, repository: repository)
        editor.openProfile()
        editor.preview(share)
        guard editor.canApply else { XCTFail("Authorized node did not produce a valid native draft"); return }
        editor.apply()
        guard editor.error == nil, editor.hasSavedNode else { XCTFail("Native Apply did not save the authorized node"); return }
        let preference = AccessTestNodeTransport(rawValue: environment["AEGIS_TEST_LOCAL_TRANSPORT"] ?? "http") ?? .http
        let capabilityOverride: Bool? = environment["AEGIS_TEST_FORCE_SOCKS_UNADMITTED"] == "1" ? false : nil
        let expectedTransport = try preference.select(socksAdmitted: capabilityOverride ?? AccessInternalSOCKSAdmission.current)
        let core = try ObservedCore(mainHost: url.host ?? "")
        let singleNavigation = environment["AEGIS_TEST_SINGLE_NAVIGATION"] == "1"
        let expectPolicyBlock = environment["AEGIS_TEST_EXPECT_POLICY_BLOCK"] == "1"
        let page = AccessTestNodePage(repository: repository, testingCore: core,
                                     singleNavigation: singleNavigation,
                                     transport: preference, socksAdmitted: capabilityOverride)
        var receipt: [String: Any] = ["sourceSHA": environment["AEGIS_TEST_SOURCE_SHA"] ?? "unknown",
            "device": "ios-device-2d62695fc7", "protocol": expectedTransport == .socks5 ? "socks5" : "http",
            "nonce": URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "nonce" })?.value ?? ""]
        defer {
            receipt["dnsObservation"] = core.observation
            if let bytes = try? JSONSerialization.data(withJSONObject: receipt, options: [.sortedKeys, .prettyPrinted]) {
                let file = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                    .appendingPathComponent("ios208-controlled-path.json")
                try? bytes.write(to: file, options: .atomic)
            }
        }
        await page.open(url)
        guard let webView = page.webView else { XCTFail("Authorized publisher did not bind a DataStore"); return }
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 390, height: 844))
        window.windowScene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        window.addSubview(webView)
        webView.frame = window.bounds
        window.makeKeyAndVisible()
        defer { page.close(); window.isHidden = true }
        let deadline = Date().addingTimeInterval(30)
        let expectedPath = environment["AEGIS_TEST_EXPECT_FINAL_PATH"]
        while (page.status == "正在加载" || expectedPath.map { webView.url?.path != $0 } == true), Date() < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        if singleNavigation { try await Task.sleep(for: .seconds(1)) }
        receipt["singleNavigation"] = singleNavigation
        receipt["state"] = page.status
        receipt["failureDomain"] = page.failureDomain ?? ""
        receipt["navigationPolicy"] = page.navigationPolicyObservation
        receipt["localPolicyRejection"] = page.localPolicyRejection ?? ""
        receipt["initialHTTPStatus"] = page.initialHTTPStatus ?? 0
        receipt["initialResponseObserved"] = page.initialHTTPStatus != nil
        receipt["serverRedirectCount"] = page.serverRedirectCount
        if expectPolicyBlock {
            let expectedPolicy = environment["AEGIS_TEST_EXPECT_POLICY_REASON"] ?? "additional-top-level-navigation"
            XCTAssertEqual(page.localPolicyRejection, expectedPolicy)
            XCTAssertEqual(page.navigationPolicyObservation, expectedPolicy)
            if expectedPolicy == "additional-top-level-navigation" {
                XCTAssertEqual(page.failureDomain, "AdditionalTopLevelNavigation")
            }
            XCTAssertEqual(page.status, "连接暂不可用")
        } else {
            XCTAssertEqual(page.status, "已连接", "Controlled page did not finish through its bound profile")
            XCTAssertNotNil(page.endpoint)
            XCTAssertEqual(page.httpStatus, 200)
            if let expectedPath { XCTAssertEqual(webView.url?.path, expectedPath) }
            let nonce = try XCTUnwrap(receipt["nonce"] as? String)
            XCTAssertFalse(nonce.isEmpty)
            let containsNonce = try await webView.callAsyncJavaScript(
                "return document.documentElement.textContent.includes(nonce);",
                arguments: ["nonce": nonce], in: nil, contentWorld: .page) as? Bool
            receipt["documentContainsNonce"] = containsNonce == true
            XCTAssertEqual(containsNonce, true)
        }
        XCTAssertNotNil(page.initialBinding)
        XCTAssertEqual(page.selectedTransport, expectedTransport)
        receipt["port"] = page.initialBinding?.port
        receipt["endpoint"] = page.initialBinding?.identity
        receipt["generation"] = page.initialBinding?.generation
        receipt["httpStatus"] = page.httpStatus
        XCTAssertNotEqual(page.initialBinding?.port, 0)
        if expectedTransport == .socks5 { XCTAssertNotEqual(page.initialBinding?.port, 1080) }
        XCTAssertEqual(webView.configuration.websiteDataStore.proxyConfigurations.count, 1)
        XCTAssertEqual(webView.configuration.websiteDataStore.proxyConfigurations.first?.allowFailover, false)
        let normalCloseProbe = environment["AEGIS_TEST_NORMAL_CLOSE_PROBE"] == "1"
        var closeSession: URLSession?
        var oldFlow: LocalProxyPeer?
        if normalCloseProbe {
            XCTAssertEqual(environment["AEGIS_TEST_BUSY_CONTENT_BEFORE_CLOSE"], "1")
            let config = URLSessionConfiguration.ephemeral
            config.proxyConfigurations = webView.configuration.websiteDataStore.proxyConfigurations
            config.urlCache = nil
            config.requestCachePolicy = .reloadIgnoringLocalCacheData
            config.waitsForConnectivity = false
            config.timeoutIntervalForRequest = 3
            let session = URLSession(configuration: config)
            closeSession = session
            let (data, response) = try await session.data(from: url)
            XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
            XCTAssertTrue(String(decoding: data, as: UTF8.self).contains(receipt["nonce"] as? String ?? "missing"))
            let peer = LocalProxyPeer(port: try XCTUnwrap(page.initialBinding?.port))
            try await peer.connect(host: try XCTUnwrap(url.host), transport: expectedTransport)
            oldFlow = peer
            receipt["normalCloseProbe"] = true
        }
        defer { closeSession?.invalidateAndCancel(); oldFlow?.cancel() }
        if let target = environment["AEGIS_TEST_REVOKED_URL"], !normalCloseProbe {
            await core.stop()
            receipt["coreStopped"] = !core.isRunning
            do {
                let result = try await webView.callAsyncJavaScript(
                    "return await Promise.race([fetch(target, {cache: 'no-store'}).then(() => 'received').catch(() => 'blocked'), new Promise(resolve => setTimeout(() => resolve('timeout'), 3000))]);",
                    arguments: ["target": target], in: nil, contentWorld: .page)
                receipt["revokedFetch"] = result as? String ?? "unknown"
                XCTAssertEqual(result as? String, "blocked", "Stopped protection must not fall back to direct networking")
            } catch {
                receipt["revokedFetch"] = "document-retired"
                XCTAssertEqual(page.status, "连接暂不可用", "A script failure counts only after protection retired the document")
            }
        }
        let busyStarted = XCTestExpectation(description: "controlled WebContent busy loop started")
        let busy = RetirementSignal { busyStarted.fulfill() }
        let probeBusyContent = environment["AEGIS_TEST_BUSY_CONTENT_BEFORE_CLOSE"] == "1"
        if probeBusyContent {
            webView.configuration.userContentController.add(busy, name: "aegisRetirementSignal")
            webView.evaluateJavaScript("window.webkit.messageHandlers.aegisRetirementSignal.postMessage('started'); const until = performance.now()+8000; while(performance.now()<until){}; window.webkit.messageHandlers.aegisRetirementSignal.postMessage('ended');", completionHandler: nil)
            await fulfillment(of: [busyStarted], timeout: 2)
        }
        defer { if probeBusyContent { webView.configuration.userContentController.removeScriptMessageHandler(forName: "aegisRetirementSignal") } }
        if normalCloseProbe {
            receipt["coreRunningBeforeProductClose"] = core.isRunning
            XCTAssertTrue(core.isRunning)
        }
        var observations: [[String: Any]] = []
        let retired = try await Self.observeRetirement(page, afterClose: {
            if normalCloseProbe {
                let port = try XCTUnwrap(page.initialBinding?.port)
                receipt["endpointHeldDuringCloseFence"] = page.endpoint != nil
                receipt["portHeldDuringCloseFence"] = Self.portIsOwned(port)
                XCTAssertNotNil(page.endpoint)
                XCTAssertTrue(Self.portIsOwned(port))
                let oldClosed = await oldFlow?.waitForClose() == true
                receipt["oldAcceptedFlowClosed"] = oldClosed
                XCTAssertTrue(oldClosed)
                let newcomer = LocalProxyPeer(port: port)
                defer { newcomer.cancel() }
                var admitted = false
                do { try await newcomer.connect(host: try XCTUnwrap(url.host), transport: expectedTransport); admitted = true }
                catch { }
                receipt["newProxyCONNECTAdmitted"] = admitted
                XCTAssertFalse(admitted)
                if let target = environment["AEGIS_TEST_REVOKED_URL"], let targetURL = URL(string: target), let session = closeSession {
                    var received = false
                    do { _ = try await session.data(from: targetURL); received = true } catch { }
                    receipt["normalCloseMarkedRequestReceived"] = received
                    XCTAssertFalse(received)
                } else { XCTFail("Normal-close probe requires its independent negative marker") }
                receipt["coreStoppedByProductClose"] = !core.isRunning
                XCTAssertFalse(core.isRunning)
            }
        }) { supplement, confirmed, elapsed, events in
            var observation: [String: Any] = ["supplement":supplement, "confirmed":confirmed,
                                             "observationSeconds":elapsed, "events":events]
            if normalCloseProbe, !confirmed, let port = page.initialBinding?.port {
                observation["portStillOwned"] = Self.portIsOwned(port)
                observation["sameEndpointStillHeld"] = page.endpoint == page.initialBinding
                XCTAssertTrue(Self.portIsOwned(port))
                XCTAssertEqual(page.endpoint, page.initialBinding)
            }
            observations.append(observation)
        }
        receipt["retirementObservations"] = observations
        receipt["retirementWithinFirst5Seconds"] = observations.first?["confirmed"]
        receipt["retirementObservationSeconds"] = observations.last?["observationSeconds"]
        receipt["retirementEvents"] = page.retirementEvents
        receipt["busyContentProbe"] = probeBusyContent
        XCTAssertTrue(retired, "The real proxy-bound document must commit its inert replacement")
        receipt["retired"] = retired
        XCTAssertNotNil(page.initialBinding)
    }

    private static func portIsOwned(_ port: UInt16) -> Bool {
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { Darwin.close(fd) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        return result != 0 && errno == EADDRINUSE
    }

    /// App-native connection probes supplement the real WebKit page; they do not
    /// replace its publisher/DataStore or navigate any corpus member.
    private final class LocalProxyPeer: @unchecked Sendable {
        let connection: NWConnection
        init(port: UInt16) {
            connection = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
            connection.start(queue: DispatchQueue(label: "ios208-owned-proxy-probe"))
        }
        func cancel() { connection.cancel() }
        func send(_ data: Data) async throws {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                connection.send(content: data, completion: .contentProcessed { error in
                    if let error { c.resume(throwing: error) } else { c.resume() }
                })
            }
        }
        func receive(_ count: Int) async throws -> Data {
            try await withCheckedThrowingContinuation { c in
                connection.receive(minimumIncompleteLength: count, maximumLength: count) { data, _, _, error in
                    if let error { c.resume(throwing: error) }
                    else if let data, data.count == count { c.resume(returning: data) }
                    else { c.resume(throwing: URLError(.networkConnectionLost)) }
                }
            }
        }
        func connect(host: String, transport: AccessInboundTransport) async throws {
            let timeout = Task { try? await Task.sleep(for: .seconds(2)); if !Task.isCancelled { connection.cancel() } }
            defer { timeout.cancel() }
            if transport == .socks5 {
                try await send(Data([5, 1, 0]))
                guard try await receive(2) == Data([5, 0]), host.utf8.count < 256 else { throw URLError(.badServerResponse) }
                var request = Data([5, 1, 0, 3, UInt8(host.utf8.count)])
                request.append(contentsOf: host.utf8); request.append(contentsOf: [1, 187])
                try await send(request)
                let reply = try await receive(10)
                guard reply[0] == 5, reply[1] == 0 else { throw URLError(.cannotConnectToHost) }
            } else {
                try await send(Data("CONNECT \(host):443 HTTP/1.1\r\nHost: \(host):443\r\n\r\n".utf8))
                var header = Data()
                while header.count < 4096, !header.suffix(4).elementsEqual([13, 10, 13, 10]) {
                    header.append(try await receive(1))
                }
                guard String(decoding: header, as: UTF8.self).hasPrefix("HTTP/1.1 200 ") else { throw URLError(.cannotConnectToHost) }
            }
        }
        func waitForClose() async -> Bool {
            let timedOut = Mutex(false)
            let timeout = Task {
                try? await Task.sleep(for: .seconds(2))
                if !Task.isCancelled { timedOut.withLock { $0 = true }; connection.cancel() }
            }
            defer { timeout.cancel() }
            return await withCheckedContinuation { c in
                connection.receive(minimumIncompleteLength: 1, maximumLength: 1) { data, _, complete, error in
                    c.resume(returning: !timedOut.withLock { $0 } && (complete || error != nil) && (data?.isEmpty ?? true))
                }
            }
        }
    }

    private final class RetirementSignal: NSObject, WKScriptMessageHandler {
        let started: () -> Void
        init(started: @escaping () -> Void) { self.started = started }
        func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
            if message.body as? String == "started" { started() }
        }
    }

    /// Observes arguments delivered by the real WebKit inbound to the real core.
    /// Does not resolve names, alter configuration or persist target names.
    private final class ObservedCore: AccessEmbeddedCoreControlling, @unchecked Sendable {
        let base: AccessGoEmbeddedCore
        let mainHost: String
        let state = Mutex((domains: 0, ips: 0, mainName: false, dnsOverride: false))
        init(mainHost: String) throws {
            self.mainHost = mainHost
            base = try AccessGoEmbeddedCore(isolated: true)
        }
        var isRunning: Bool { base.isRunning }
        func start(configData: Data) async throws {
            let config = try JSONSerialization.jsonObject(with: configData) as? [String: Any]
            state.withLock { $0.dnsOverride = config?["dns"] != nil }
            try await base.start(configData: configData)
        }
        func dial(host: String, port: UInt16) async throws -> any AccessRealityPathStream {
            state.withLock {
                if IPv4Address(host) != nil || IPv6Address(host) != nil { $0.ips += 1 }
                else { $0.domains += 1 }
                if host == mainHost { $0.mainName = true }
            }
            return try await base.dial(host: host, port: port)
        }
        func stop() async { await base.stop() }
        var observation: [String: Any] {
            state.withLock { ["domainTargets": $0.domains, "ipTargets": $0.ips,
                             "controlledMainNameForwarded": $0.mainName, "clientDNSOverride": $0.dnsOverride] }
        }
    }
}
#endif

#if DEBUG
/// Explicit physical protocol admission, independent of the domain campaign.
@MainActor
final class AccessPhysicalSOCKSAdmissionTests: XCTestCase {
    private static var quarantined: [(WKWebView, AccessGoEmbeddedCore, UIWindow)] = []

    func testControlledSOCKSWebKitAdmission() async throws {
#if targetEnvironment(simulator)
        throw XCTSkip("SOCKS OS admission requires the designated physical device")
#else
        let env = ProcessInfo.processInfo.environment
        guard env["AEGIS_TEST_SOCKS_PROBE"] == "1",
              env["AEGIS_TEST_CAMPAIGN_DEVICE"] == "ios-device-2d62695fc7",
              let raw = env["AEGIS_TEST_CONTROLLED_URL"], let url = URL(string: raw),
              let host = url.host,
              let nonce = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?
                .first(where: { $0.name == "nonce" })?.value else {
            throw XCTSkip("Explicit physical SOCKS admission inputs required")
        }
        var system = utsname()
        uname(&system)
        let model = withUnsafePointer(to: &system.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        guard model == "iPhone14,3" else { XCTFail("Wrong physical device model"); return }
        let repository = AccessTestNodeRepository.shared
        guard repository.allowsEditing(isPrivate: false) else { XCTFail("Internal admission required"); return }
        let parameters = try repository.savedNode().parameters()
        let outbound = try AccessRealityVisionOutboundConfigBuilder.build(parameters: parameters)
        var config = try XCTUnwrap(JSONSerialization.jsonObject(with: outbound.embeddedInstanceJSONData()) as? [String: Any])
        // Fixed DEV admission fixture only; product listeners use assigned ports.
        config["inbounds"] = [["listen": "127.0.0.1", "port": 10806, "protocol": "socks",
                              "settings": ["auth": "noauth", "udp": false]]]
        let core = try AccessGoEmbeddedCore(isolated: true)
        var result: [String: Any] = ["device": "ios-device-2d62695fc7", "model": model,
            "os": ProcessInfo.processInfo.operatingSystemVersionString,
            "sourceSHA": env["AEGIS_TEST_SOURCE_SHA"] ?? "unknown",
            "bundle": Bundle.main.bundleIdentifier ?? "unknown", "protocol": "socks5",
            "port": 10806, "nonce": nonce, "status": "probe-incomplete"]
        let file = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ios208-socks-admission.json")
        defer {
            if let bytes = try? JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys]) {
                try? bytes.write(to: file, options: .atomic)
            }
        }
        do { try await core.start(configData: JSONSerialization.data(withJSONObject: config)) }
        catch {
            result["errorDomain"] = (error as NSError).domain
            result["errorCode"] = (error as NSError).code
            await core.stop()
            XCTFail("SOCKS fixture core could not start; no OS conclusion")
            return
        }
        do {
            try await Self.nativeSOCKSConnect(host: host, port: 443)
            result["nativeConnect"] = true
        } catch {
            result["nativeConnect"] = false
            result["errorDomain"] = (error as NSError).domain
            result["errorCode"] = (error as NSError).code
            await core.stop()
            XCTFail("Native SOCKS fixture did not admit CONNECT; no OS conclusion")
            return
        }
        let store = WKWebsiteDataStore.nonPersistent()
        var proxy = ProxyConfiguration(socksv5Proxy: .hostPort(host: "127.0.0.1", port: 10806))
        proxy.allowFailover = false
        store.proxyConfigurations = [proxy]
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = store
        let view = WKWebView(frame: CGRect(x: 0, y: 0, width: 390, height: 844), configuration: configuration)
        let observer = SOCKSProbeNavigation()
        view.navigationDelegate = observer
        let window = UIWindow(frame: view.frame)
        window.windowScene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        window.addSubview(view)
        window.makeKeyAndVisible()
        view.load(URLRequest(url: url))
        let deadline = Date().addingTimeInterval(30)
        while !observer.finished, observer.failure == nil, Date() < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        let html = observer.finished ? (try? await view.evaluateJavaScript("document.documentElement.textContent") as? String) : nil
        result["proxyCount"] = store.proxyConfigurations.count
        result["allowFailover"] = proxy.allowFailover
        result["httpStatus"] = observer.httpStatus ?? 0
        result["documentContainsNonce"] = html?.contains(nonce) == true
        result["status"] = observer.finished && observer.httpStatus == 200 && html?.contains(nonce) == true
            ? "webkit-response-verified" : "webkit-not-admitted"
        if let failure = observer.failure {
            result["errorDomain"] = failure.domain
            result["errorCode"] = failure.code
        }
        // Keep ownership until the actual old document has been replaced.
        observer.retirement = nil
        view.stopLoading()
        observer.retirement = view.loadHTMLString("<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'\">",
                                                  baseURL: URL(string: "https://aegis.invalid/socks-retired"))
        let stopDeadline = Date().addingTimeInterval(10)
        while !observer.retired, Date() < stopDeadline { try await Task.sleep(for: .milliseconds(50)) }
        result["retired"] = observer.retired
        if observer.retired {
            await core.stop()
            view.removeFromSuperview()
            window.isHidden = true
        } else {
            Self.quarantined.append((view, core, window))
            XCTFail("SOCKS fixture document retirement unconfirmed; listener retained until process exit")
        }
#endif
    }

    private static func nativeSOCKSConnect(host: String, port: UInt16) async throws {
        let connection = NWConnection(host: "127.0.0.1", port: 10806, using: .tcp)
        connection.start(queue: DispatchQueue(label: "ios208-socks-admission"))
        let deadline = Task { try? await Task.sleep(for: .seconds(15)); connection.cancel() }
        defer { deadline.cancel(); connection.cancel() }
        func send(_ bytes: Data) async throws {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                connection.send(content: bytes, completion: .contentProcessed { error in
                    if let error { continuation.resume(throwing: error) } else { continuation.resume() }
                })
            }
        }
        func receive(_ count: Int) async throws -> Data {
            try await withCheckedThrowingContinuation { continuation in
                connection.receive(minimumIncompleteLength: count, maximumLength: count) { data, _, _, error in
                    if let error { continuation.resume(throwing: error) }
                    else if let data, data.count == count { continuation.resume(returning: data) }
                    else { continuation.resume(throwing: URLError(.badServerResponse)) }
                }
            }
        }
        try await send(Data([5, 1, 0]))
        guard try await receive(2) == Data([5, 0]), host.utf8.count < 256 else { throw URLError(.badServerResponse) }
        var request = Data([5, 1, 0, 3, UInt8(host.utf8.count)])
        request.append(contentsOf: host.utf8)
        request.append(contentsOf: [UInt8(port >> 8), UInt8(port & 255)])
        try await send(request)
        let reply = try await receive(10)
        guard reply[0] == 5, reply[1] == 0 else { throw URLError(.cannotConnectToHost) }
    }

    private final class SOCKSProbeNavigation: NSObject, WKNavigationDelegate {
        var finished = false
        var failure: NSError?
        var httpStatus: Int?
        var retirement: WKNavigation?
        var retired = false
        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) { finished = true }
        func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
            if let navigation, navigation === retirement { retired = true }
        }
        func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
            failure = error as NSError
        }
        func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { failure = error as NSError }
        func webView(_ webView: WKWebView, decidePolicyFor response: WKNavigationResponse,
                     decisionHandler: @escaping @MainActor (WKNavigationResponsePolicy) -> Void) {
            if response.isForMainFrame { httpStatus = (response.response as? HTTPURLResponse)?.statusCode }
            decisionHandler(.allow)
        }
    }
}
#endif
