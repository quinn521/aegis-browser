import AccessKit
import CoreImage
import WebKit
import UIKit
import CryptoKit
import Network
import Security
import Synchronization
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

    func testSupersededNavigationFailuresCannotCloseTheReplacement() {
        let page = AccessTestNodePage()
        let view = WKWebView()
        let first = WKNavigation()
        let replacement = WKNavigation()
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
        let hosts = String(decoding: data, as: UTF8.self).split(separator: "\n").map(String.init)
        guard hosts.count == 4372, Set(hosts).count == hosts.count else { XCTFail("Campaign input is incomplete"); return }
        let repository = AccessTestNodeRepository.shared
        guard repository.allowsEditing(isPrivate: false) else { XCTFail("Native admission denied"); return }
        let parameters = try AccessLocalV2RayConfigLoader.parseSingleShareURI(node)
        if (try? repository.savedNode().parameters()) != parameters {
            _ = try repository.apply(parameters, expectedRevision: repository.revision(isPrivate: false), isPrivate: false)
        }
        let file = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ios208-domain-dispatch.jsonl")
        if !FileManager.default.fileExists(atPath: file.path) {
            try Data().write(to: file, options: .atomic)
        }
        let prior = try Data(contentsOf: file)
        var reserved = Set<String>()
        for line in String(decoding: prior, as: UTF8.self).split(separator: "\n") {
            guard let json = line.data(using: .utf8), let record = try JSONSerialization.jsonObject(with: json) as? [String: Any],
                  record["dataset"] as? String == digest else { XCTFail("Dispatch ledger is damaged"); return }
            if record["event"] as? String == "reserved", let host = record["host"] as? String { reserved.insert(host) }
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
        for host in hosts where !reserved.contains(host) {
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
                        "navigationPolicy":page.failureDomain == "AdditionalTopLevelNavigation" ? "subsequent-top-level-blocked" : "not-blocked",
                        "endpoint":page.initialBinding?.identity ?? "", "generation":page.initialBinding?.generation ?? 0,
                        "port":page.initialBinding?.port ?? 0,
                        "phase":page.initialBinding == nil ? "preparation" : "navigation",
                        "actualProtocol":page.initialBinding.map { $0.transport == .socks5 ? "socks5" : "http" } ?? "unbound",
                        "network":network.0])
            consecutiveNetworkFailures = timedOut || page.failureDomain == NSURLErrorDomain ? consecutiveNetworkFailures + 1 : 0
            preparationFailures = page.webView == nil ? preparationFailures + 1 : 0
            page.close()
            let stopDeadline = Date().addingTimeInterval(5)
            while page.endpoint != nil, Date() < stopDeadline { try await Task.sleep(for: .milliseconds(50)) }
            page.webView?.removeFromSuperview()
            guard page.endpoint == nil else { XCTFail("Old document retirement unconfirmed; campaign stopped"); return }
            guard preparationFailures < 3 else { XCTFail("Repeated preparation failures; campaign stopped"); return }
            if consecutiveNetworkFailures >= 10 {
                try append(["event":"checkpoint", "reason":"consecutive-network-failures", "reservedCount":reserved.count])
                return
            }
        }
#endif

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
        let core = try AccessGoEmbeddedCore(isolated: true)
        let singleNavigation = environment["AEGIS_TEST_SINGLE_NAVIGATION"] == "1"
        let expectPolicyBlock = environment["AEGIS_TEST_EXPECT_POLICY_BLOCK"] == "1"
        let page = AccessTestNodePage(repository: repository, testingCore: core,
                                     singleNavigation: singleNavigation,
                                     transport: preference, socksAdmitted: capabilityOverride)
        var receipt: [String: Any] = ["sourceSHA": environment["AEGIS_TEST_SOURCE_SHA"] ?? "unknown",
            "device": "ios-device-2d62695fc7", "protocol": expectedTransport == .socks5 ? "socks5" : "http",
            "nonce": URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "nonce" })?.value ?? ""]
        defer {
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
        while page.status == "正在加载", Date() < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        if singleNavigation { try await Task.sleep(for: .seconds(1)) }
        receipt["singleNavigation"] = singleNavigation
        receipt["state"] = page.status
        receipt["failureDomain"] = page.failureDomain ?? ""
        receipt["initialHTTPStatus"] = page.initialHTTPStatus ?? 0
        receipt["initialResponseObserved"] = page.initialHTTPStatus != nil
        receipt["serverRedirectCount"] = page.serverRedirectCount
        if expectPolicyBlock {
            XCTAssertEqual(page.failureDomain, "AdditionalTopLevelNavigation")
            XCTAssertEqual(page.status, "连接暂不可用")
        } else {
            XCTAssertEqual(page.status, "已连接", "Controlled page did not finish through its bound profile")
            XCTAssertNotNil(page.endpoint)
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
        if let target = environment["AEGIS_TEST_REVOKED_URL"] {
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
        page.close()
        let retirementDeadline = Date().addingTimeInterval(10)
        while page.endpoint != nil, Date() < retirementDeadline { try await Task.sleep(for: .milliseconds(50)) }
        XCTAssertNil(page.endpoint, "The real proxy-bound document must commit its inert replacement")
        receipt["retired"] = page.endpoint == nil
        XCTAssertNotNil(page.initialBinding)
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
