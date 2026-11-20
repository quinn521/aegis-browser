import AccessKit
import CoreImage
import Vision
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
        let bitmap = try XCTUnwrap(CIContext().createCGImage(image, from: image.extent))
        let request = VNDetectBarcodesRequest()
        request.symbologies = [.qr]
        try VNImageRequestHandler(cgImage: bitmap).perform([request])
        let payloads = try XCTUnwrap(request.results).map(\.payloadStringValue)
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
        page.close()
        XCTAssertEqual(page.status, "连接暂不可用")

        let deniedCore = PageCore()
        let denied = AccessTestNodePage(repository: AccessTestNodeRepository(channel: nil, storage: storage),
                                       testingCore: deniedCore)
        await denied.open(URL(string: "https://example.invalid/")!)
        XCTAssertFalse(deniedCore.isRunning)
        XCTAssertNil(denied.webView)
        XCTAssertNil(denied.endpoint)
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
        for host in hosts where !reserved.contains(host) {
            let attempt = UUID().uuidString
            try append(["event":"reserved", "host":host, "attempt":attempt])
            reserved.insert(host)
            guard let url = URL(string: "https://" + host + "/") else { XCTFail("Invalid fixed hostname"); return }
            let page = AccessTestNodePage(singleNavigation: true)
            await page.open(url)
            if let view = page.webView { view.frame = window.bounds; window.addSubview(view) }
            let deadline = Date().addingTimeInterval(20)
            while page.status == "正在加载", Date() < deadline {
                try await Task.sleep(for: .milliseconds(100))
            }
            try append(["event":"result", "host":host, "attempt":attempt,
                        "state":page.status, "errorDomain":page.failureDomain ?? "", "errorCode":page.failureCode ?? 0,
                        "endpoint":page.endpoint?.identity ?? "", "generation":page.endpoint?.generation ?? 0,
                        "actualProtocol":"http"])
            page.close()
            let stopDeadline = Date().addingTimeInterval(5)
            while page.endpoint != nil, Date() < stopDeadline { try await Task.sleep(for: .milliseconds(50)) }
            page.webView?.removeFromSuperview()
            guard page.endpoint == nil else { XCTFail("Old document retirement unconfirmed; campaign stopped"); return }
        }
#endif
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
        let old = try repository.revision(isPrivate: false)
        _ = try repository.apply(AccessLocalV2RayConfigLoader.parseSingleShareURI(share),
                                 expectedRevision: old, isPrivate: false)
        let page = AccessTestNodePage(repository: repository)
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
        XCTAssertEqual(page.status, "已连接", "Controlled page did not finish through its bound profile")
        XCTAssertNotNil(page.endpoint)
        XCTAssertEqual(webView.configuration.websiteDataStore.proxyConfigurations.count, 1)
        XCTAssertEqual(webView.configuration.websiteDataStore.proxyConfigurations.first?.allowFailover, false)
    }
}
#endif
