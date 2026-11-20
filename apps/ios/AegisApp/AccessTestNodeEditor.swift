import AccessKit
import Foundation
import Observation
import Security

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
