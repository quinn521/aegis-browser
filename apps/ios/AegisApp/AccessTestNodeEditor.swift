import AccessKit
import Foundation
import Observation
import Security
import WebKit

/// Package metadata is resolved by native code. Firebase is independent of Access admission.
enum AccessInternalOptions: String {
    case dev, alpha

    static var installed: Self? {
        let bundle = Bundle.main
#if DEBUG
        let debug = true
#else
        let debug = false
#endif
        return resolve(
            bundleID: bundle.bundleIdentifier,
            channel: bundle.object(forInfoDictionaryKey: "AegisAccessReleaseChannel") as? String,
            releaseProfile: bundle.object(forInfoDictionaryKey: "AegisReleaseProfile") as? String,
            capability: bundle.object(forInfoDictionaryKey: "AegisAccessInternalOptions") as? String,
            debug: debug
        )
    }

    static func resolve(
        bundleID: String?, channel: String?, releaseProfile: String?, capability: String?, debug: Bool
    ) -> Self? {
        guard let channel, channel == releaseProfile, capability == "YES" else { return nil }
        switch (bundleID, channel) {
        case ("com.gcsa.aegis", "dev") where debug: return .dev
        case ("com.gcsa.aegis.alpha.internal", "alpha"): return .alpha
        default: return nil
        }
    }
}

enum AccessTestNodeError: Error {
    case unavailable, invalidLink, cameraDenied, cameraUnavailable, conflict, storageUnavailable
}

struct AccessTestNodeRecord: Codable {
    let version = 1
    let revision: UUID
    private let address: String
    private let port: UInt16
    private let userID: String
    private let serverName: String
    private let publicKey: String
    private let shortID: String
    private let fingerprint: String
    private let spiderX: String

    // Use an explicit encoding schema. Unknown or missing versions are rejected at restore.
    private enum CodingKeys: String, CodingKey {
        case version, revision, address, port, userID, serverName, publicKey, shortID, fingerprint, spiderX
    }

    init(_ node: AccessRealityVisionParameters) {
        revision = UUID()
        address = node.address
        port = node.port
        userID = node.userID
        serverName = node.serverName
        publicKey = node.publicKey
        shortID = node.shortID
        fingerprint = node.fingerprint
        spiderX = node.spiderX
    }

    func parameters() throws -> AccessRealityVisionParameters {
        try AccessRealityVisionParameters(address: address, port: port, userID: userID,
            serverName: serverName, publicKey: publicKey, shortID: shortID,
            fingerprint: fingerprint, spiderX: spiderX)
    }

    init(from decoder: any Decoder) throws {
        let fields = try decoder.container(keyedBy: CodingKeys.self)
        guard try fields.decode(Int.self, forKey: .version) == version else {
            throw AccessTestNodeError.storageUnavailable
        }
        revision = try fields.decode(UUID.self, forKey: .revision)
        address = try fields.decode(String.self, forKey: .address)
        port = try fields.decode(UInt16.self, forKey: .port)
        userID = try fields.decode(String.self, forKey: .userID)
        serverName = try fields.decode(String.self, forKey: .serverName)
        publicKey = try fields.decode(String.self, forKey: .publicKey)
        shortID = try fields.decode(String.self, forKey: .shortID)
        fingerprint = try fields.decode(String.self, forKey: .fingerprint)
        spiderX = try fields.decode(String.self, forKey: .spiderX)
        _ = try AccessRealityVisionParameters(
            address: address, port: port, userID: userID, serverName: serverName, publicKey: publicKey,
            shortID: shortID, fingerprint: fingerprint, spiderX: spiderX
        )
    }
}

@MainActor
protocol AccessTestNodeStoring {
    func read(channel: AccessInternalOptions) throws -> AccessTestNodeRecord?
    func write(_ record: AccessTestNodeRecord, channel: AccessInternalOptions) throws
    func remove(channel: AccessInternalOptions) throws
}

struct AccessTestNodeKeychain: AccessTestNodeStoring {
    var service = "com.gcsa.aegis.ios.access-test-node.v1"

    private func query(for channel: AccessInternalOptions) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: channel.rawValue + ".ordinary.fixed-test-profile",
         kSecAttrSynchronizable as String: false]
    }

    func read(channel: AccessInternalOptions) throws -> AccessTestNodeRecord? {
        var request = query(for: channel)
        request[kSecReturnData as String] = true
        var item: CFTypeRef?
        let status = SecItemCopyMatching(request as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else {
            throw AccessTestNodeError.storageUnavailable
        }
        return try JSONDecoder().decode(AccessTestNodeRecord.self, from: data)
    }

    func write(_ record: AccessTestNodeRecord, channel: AccessInternalOptions) throws {
        let request = query(for: channel)
        let attributes: [String: Any] = [
            kSecValueData as String: try JSONEncoder().encode(record),
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        ]
        var status = SecItemUpdate(request as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(request.merging(attributes) { _, value in value } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw AccessTestNodeError.storageUnavailable }
    }

    func remove(channel: AccessInternalOptions) throws {
        let status = SecItemDelete(query(for: channel) as CFDictionary)
        guard status == errSecSuccess else { throw AccessTestNodeError.storageUnavailable }
    }
}

@MainActor
final class AccessTestNodeRepository {
    static let shared = AccessTestNodeRepository()
    private let channel: AccessInternalOptions?
    private let storage: any AccessTestNodeStoring

    init(channel: AccessInternalOptions? = AccessInternalOptions.installed,
         storage: any AccessTestNodeStoring = AccessTestNodeKeychain()) {
        self.channel = channel
        self.storage = storage
    }

    func allowsEditing(isPrivate: Bool) -> Bool { channel != nil && !isPrivate }

    func savedNode() throws -> AccessTestNodeRecord {
        guard let channel, let record = try storage.read(channel: channel) else {
            throw AccessTestNodeError.unavailable
        }
        return record
    }


    func revision(isPrivate: Bool) throws -> UUID? {
        guard let channel, !isPrivate else { throw AccessTestNodeError.unavailable }
        return try storage.read(channel: channel)?.revision
    }

    func apply(_ draft: AccessRealityVisionParameters, expectedRevision: UUID?, isPrivate: Bool) throws -> UUID {
        guard let channel, !isPrivate else { throw AccessTestNodeError.unavailable }
        guard try revision(isPrivate: isPrivate) == expectedRevision else { throw AccessTestNodeError.conflict }
        let record = AccessTestNodeRecord(draft)
        try storage.write(record, channel: channel)
        return record.revision
    }

    func remove(expectedRevision: UUID, isPrivate: Bool) throws {
        guard let channel, !isPrivate else { throw AccessTestNodeError.unavailable }
        guard try revision(isPrivate: isPrivate) == expectedRevision else { throw AccessTestNodeError.conflict }
        try storage.remove(channel: channel)
    }
}

@MainActor
@Observable
final class AccessTestNodeEditor {
    struct Scan: Identifiable, Equatable {
        let id = UUID()
    }

    private let repository: AccessTestNodeRepository
    private let isPrivate: Bool
    private var draft: AccessRealityVisionParameters?
    private var baseRevision: UUID?
    private var loaded = false
    private(set) var scan: Scan?
    private(set) var error: AccessTestNodeError?

    var allowsEditing: Bool { repository.allowsEditing(isPrivate: isPrivate) }
    var hasDraft: Bool { draft != nil }
    var hasSavedNode: Bool { baseRevision != nil }
    var canApply: Bool { loaded && hasDraft && scan == nil }

    init(isPrivate: Bool, repository: AccessTestNodeRepository = .shared) {
        self.isPrivate = isPrivate
        self.repository = repository
    }

    /// Each visit to the test Profile starts a new editing lifetime.
    func openProfile() {
        closeProfile()
        do {
            baseRevision = try repository.revision(isPrivate: isPrivate)
            loaded = true
            error = nil
        } catch {
            self.error = allowsEditing ? .storageUnavailable : .unavailable
        }
    }

    func closeProfile() {
        scan = nil
        draft = nil
        baseRevision = nil
        loaded = false
        error = nil
    }

    func preview(_ input: String) {
        guard loaded else { return }
        scan = nil
        do {
            draft = try AccessLocalV2RayConfigLoader.parseSingleShareURI(input)
            error = nil
        } catch {
            // Remove an earlier candidate so Apply cannot save it after a failed preview.
            draft = nil
            self.error = .invalidLink
        }
    }

    func beginScan() -> Scan? {
        guard loaded, scan == nil else { return nil }
        let request = Scan()
        // A new input attempt cannot leave an older candidate ready to Apply.
        draft = nil
        scan = request
        error = nil
        return request
    }

    func finishScan(_ request: Scan, result: Result<String, AccessTestNodeError>?) {
        guard scan == request else { return }
        scan = nil
        switch result {
        case .success(let input): preview(input)
        case .failure(let error): self.error = error
        case nil: break
        }
    }

    func apply() {
        guard canApply, let draft else { return }
        do {
            baseRevision = try repository.apply(draft, expectedRevision: baseRevision, isPrivate: isPrivate)
            self.draft = nil
            error = nil
        } catch let failure as AccessTestNodeError {
            error = failure
        } catch {
            self.error = .storageUnavailable
        }
    }

    func discard() {
        openProfile()
    }

    func removeSavedNode() {
        guard loaded, scan == nil, let baseRevision else { return }
        do {
            try repository.remove(expectedRevision: baseRevision, isPrivate: isPrivate)
            openProfile()
        } catch let failure as AccessTestNodeError {
            error = failure
        } catch {
            self.error = .storageUnavailable
        }
    }
}

/// VisionKit and offline QR fixtures use the same whole-capture admission.
enum AccessTestNodeQRCode {
    static func payload(from items: [String?]) -> Result<String, AccessTestNodeError> {
        guard items.count == 1, let payload = items.first ?? nil else { return .failure(.invalidLink) }
        return .success(payload)
    }
}


/// A deliberately selected ordinary test Profile owns one publisher and one immutable store.
/// Each new page session rereads its saved revision and owns an isolated native core.
@MainActor
@Observable
final class AccessTestNodePage: NSObject, WKNavigationDelegate {
    let id = UUID()
    private(set) var webView: WKWebView?
    private(set) var status = "正在准备连接"
    private(set) var endpoint: AccessManagedProxyEndpoint?
    private let repository: AccessTestNodeRepository
    private let registry = AccessHostDataStoreRegistry()
    private let testingCore: (any AccessEmbeddedCoreControlling)?
    private var publisher: (any AccessProxyEntryPublishing)?
    private var key: AccessHostStoreKey?
    private var operationID: UUID?
    private var deadlineTask: Task<Void, Never>?
    private var retirement: (() -> Void)?
    private var retirementNavigation: WKNavigation?
    private var retiringOwner: AccessTestNodePage?
    private var closed = false
    private var retiring = false

    init(repository: AccessTestNodeRepository = .shared, testingCore: (any AccessEmbeddedCoreControlling)? = nil) {
        self.repository = repository
        self.testingCore = testingCore
        super.init()
    }

    func open(_ url: URL) async {
        guard publisher == nil, !closed, url.scheme == "https", url.user == nil, url.password == nil,
              let rawHost = url.host, let host = try? AccessSiteHost(rawHost),
              repository.allowsEditing(isPrivate: false) else {
            status = "连接暂不可用"
            return
        }
        do {
            let record = try repository.savedNode()
            let revision = record.revision
            let core = try testingCore ?? AccessGoEmbeddedCore(isolated: true)
            let newPublisher = AccessEmbeddedProxyEntryFactory.makeProductPublisher(
                configPath: nil, verifiedCore: core,
                parameters: { [repository] in
                    let current = try repository.savedNode()
                    guard current.revision == revision else { throw AccessTestNodeError.conflict }
                    return try current.parameters()
                },
                connectionAllowed: { [weak self, repository] in
                    guard let self, !self.closed else { return false }
                    return (try? repository.savedNode().revision) == revision
                },
                retireDocuments: { [weak self] finish in
                    guard let self else { finish(); return }
                    self.retire(finish)
                })
            publisher = newPublisher
            let operation = UUID()
            operationID = operation
            let deadline = ContinuousClock.now.advanced(by: .seconds(15))
            deadlineTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(15))
                guard !Task.isCancelled, let self, self.endpoint == nil else { return }
                self.close()
            }
            let result = try await newPublisher.prepare(.init(operationID: operation, deadline: .seconds(15)))
            guard !closed, ContinuousClock.now < deadline,
                  (try? repository.savedNode().revision) == revision,
                  newPublisher.validateReady(result) else { throw AccessTestNodeError.unavailable }
            let storeKey = AccessHostStoreKey(scopeID: "test-" + id.uuidString, host: host, isPrivate: false)
            guard newPublisher.retain(result.endpoint, for: storeKey) else { throw AccessTestNodeError.unavailable }
            key = storeKey
            registry.setEndpoint(result.endpoint, scopeID: storeKey.scopeID, host: host, isPrivate: false)
            guard let store = registry.storeForTab(tabID: id.uuidString, scopeID: storeKey.scopeID,
                isPrivate: false, targetHost: host, boundHost: nil, intent: .on),
                store.proxyConfigurations.count == 1,
                store.proxyConfigurations.first?.allowFailover == false else {
                throw AccessTestNodeError.unavailable
            }
            deadlineTask?.cancel()
            endpoint = result.endpoint
            let configuration = WKWebViewConfiguration()
            configuration.websiteDataStore = store
            let view = WKWebView(frame: .zero, configuration: configuration)
            view.navigationDelegate = self
            webView = view
            status = "正在加载"
            view.load(URLRequest(url: url))
        } catch {
            close()
        }
    }

    func close() {
        guard !closed else { return }
        closed = true
        status = "连接暂不可用"
        deadlineTask?.cancel()
        if let operationID { publisher?.cancel(operationID: operationID) }
        if let publisher = publisher as? AccessProxyEntryPublisher {
            publisher.closeCurrentGeneration()
        } else { retire {} }
    }

    private func retire(_ finish: @escaping () -> Void) {
        status = "连接暂不可用"
        retiring = true
        guard let view = webView else { finish(); release(); return }
        retirement = finish
        // Retain the suspended listener and old document until their exact stop acknowledgement.
        retiringOwner = self
        view.stopLoading()
        view.configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        retirementNavigation = view.loadHTMLString("<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'\">", baseURL: URL(string: "about:blank"))
    }

    private func release() {
        if let key { publisher?.release(for: key) }
        if let operationID { publisher?.cancel(operationID: operationID) }
        key = nil
        endpoint = nil
    }

    func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        if retiring, let navigation, navigation === retirementNavigation {
            let finish = retirement
            retirement = nil
            finish?()
            release()
            retiringOwner = nil
            return
        }
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        if !retiring { status = "已连接" }
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        close()
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        close()
    }

    func webView(_ webView: WKWebView, decidePolicyFor response: WKNavigationResponse,
                 decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        decisionHandler(response.canShowMIMEType ? .allow : .cancel)
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if retiring {
            decisionHandler(action.request.url?.absoluteString == "about:blank" ? .allow : .cancel)
        } else {
            let scheme = action.request.url?.scheme
            decisionHandler(scheme == "https" && !action.shouldPerformDownload ? .allow : .cancel)
        }
    }
}
