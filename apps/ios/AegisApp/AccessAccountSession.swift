import Combine
import Foundation
import Security

struct AccessAccountCredential: Codable, Equatable, Sendable {
    let accountID, token, endpoint: String
    var valid: Bool {
        !accountID.isEmpty && (1...256).contains(token.utf8.count) &&
        token.utf8.allSatisfy { (65...90).contains($0) || (97...122).contains($0) ||
            (48...57).contains($0) || $0 == 45 || $0 == 95 }
    }
}

struct AccessAccountKeychain {
    let service: String
    init(service: String = "com.gcsa.aegis.ios.access-account.v1") { self.service = service }
    private var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
         kSecAttrAccount as String: "current", kSecAttrSynchronizable as String: false]
    }

    func read() throws -> AccessAccountCredential? {
        var request = query
        request[kSecReturnData as String] = true
        var item: CFTypeRef?
        let status = SecItemCopyMatching(request as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw AccountQueryError.invalid }
        return try JSONDecoder().decode(AccessAccountCredential.self, from: data)
    }

    func save(_ credential: AccessAccountCredential?) throws {
        guard let credential else {
            let status = SecItemDelete(query as CFDictionary)
            guard status == errSecSuccess || status == errSecItemNotFound else { throw AccountQueryError.invalid }
            return
        }
        let attributes: [String: Any] = [kSecValueData as String: try JSONEncoder().encode(credential),
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        var status = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(query.merging(attributes) { _, value in value } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw AccountQueryError.invalid }
    }
}

/// Application-wide identity with single-flight reads. Identity changes invalidate earlier responses.
@MainActor
final class AccessAccountSession: ObservableObject {
    enum State: Equatable { case signedOut, loading, unavailable, ready(AccessAccountEntitlement) }
    @Published private(set) var state: State = .signedOut
    @Published private(set) var lastSnapshot: AccessAccountEntitlement?
    /// Receipt anchor only; never a substitute for the server's measurement watermark.
    @Published private(set) var snapshotReceivedUptime: TimeInterval?
    /// Request start conservatively includes network and MainActor delivery time.
    /// Invalid or regressing timing makes freshness unavailable, never newer.
    @Published private(set) var snapshotFreshnessAnchorUptime: TimeInterval?
    @Published private(set) var isRefreshing = false
    /// Query validity only. The UI must separately age the server's freshness metadata.
    var snapshotIsStale: Bool {
        guard lastSnapshot != nil else { return false }
        if case .ready = state { return false }
        return true
    }
    private let client: AccessAccountClient?
    private let keychain: AccessAccountKeychain
    private let uptime: () -> TimeInterval
    private let fetch: @Sendable (AccessAccountClient, AccessAccountCredential) async throws -> AccessAccountEntitlement
    private var generation = UUID()
    private var task: Task<Void, Never>?
    private var currentCredential: AccessAccountCredential?
    lazy var usageRefresh = AccessAccountUsageRefreshCoordinator(refresh: { [weak self] in
        self?.refresh()
    })

    convenience init(client: AccessAccountClient?, keychain: AccessAccountKeychain = .init(),
                     fetch: @escaping @Sendable (AccessAccountClient, AccessAccountCredential) async throws -> AccessAccountEntitlement = { try await $0.read($1) }) {
        self.init(client: client, keychain: keychain,
                  uptime: { ProcessInfo.processInfo.systemUptime }, fetch: fetch)
    }

    init(client: AccessAccountClient?, keychain: AccessAccountKeychain = .init(),
         uptime: @escaping () -> TimeInterval,
         fetch: @escaping @Sendable (AccessAccountClient, AccessAccountCredential) async throws -> AccessAccountEntitlement = { try await $0.read($1) }) {
        self.client = client
        self.keychain = keychain
        self.uptime = uptime
        self.fetch = fetch
    }

    static func product(bundle: Bundle = .main, environment: [String: String] = ProcessInfo.processInfo.environment,
                        keychain: AccessAccountKeychain = .init()) -> AccessAccountSession {
        let origin = bundle.object(forInfoDictionaryKey: "AegisAccountServiceOrigin") as? String ?? ""
#if DEBUG && targetEnvironment(simulator)
        if let controlledOrigin = environment["AEGIS_ACCOUNT_SIM_ORIGIN"] {
            let candidate = try? AccessAccountClient(origin: controlledOrigin, allowSimulatorLoopback: true)
            let client = candidate?.endpoint.host == "127.0.0.1" ? candidate : nil
            let session = AccessAccountSession(client: client, keychain: keychain)
            // Only a trusted local simulator launcher can supply these inputs.
            if environment["AEGIS_ACCOUNT_SIM_ACTION"] == "clear" { session.signOut() }
            if environment["AEGIS_ACCOUNT_SIM_ACTION"] == "bind" {
                do {
                    try session.stage(accountID: environment["AEGIS_ACCOUNT_SIM_ID"] ?? "",
                                      token: environment["AEGIS_ACCOUNT_SIM_TOKEN"] ?? "")
                } catch { session.signOut() }
            }
            return session
        }
#endif
        return AccessAccountSession(client: try? AccessAccountClient(origin: origin), keychain: keychain)
    }

    func suspend() {
        generation = UUID()
        task?.cancel()
        task = nil
        isRefreshing = false
        currentCredential = nil
        lastSnapshot = nil
        snapshotReceivedUptime = nil
        snapshotFreshnessAnchorUptime = nil
        state = .signedOut
    }

    private func stage(accountID: String, token: String) throws {
        guard let client else { throw AccountQueryError.invalid }
        let credential = AccessAccountCredential(accountID: accountID, token: token, endpoint: client.endpoint.absoluteString)
        guard credential.valid else { throw AccountQueryError.invalid }
        try keychain.save(credential)
    }

    @discardableResult
    func bind(accountID: String, token: String) -> Task<Void, Never>? {
        suspend()
        do {
            try stage(accountID: accountID, token: token)
            return refresh()
        } catch {
            signOut()
            state = .unavailable
            return nil
        }
    }

    func signOut() {
        suspend()
        do { try keychain.save(nil) } catch { state = .unavailable }
    }

    @discardableResult
    func refresh() -> Task<Void, Never>? {
        do {
            guard let credential = try keychain.read() else { suspend(); return nil }
            guard let client, credential.valid, client.endpoint.absoluteString == credential.endpoint else {
                throw AccountQueryError.invalid
            }
            if currentCredential != credential {
                suspend()
                currentCredential = credential
            }
            // A slow request survives repeated foreground/UI refresh triggers.
            if let task { return task }
            generation = UUID()
            let generation = generation
            let requestStartedUptime = uptime()
            state = .loading
            isRefreshing = true
            task = Task {
                defer {
                    // A cancelled old read must not clear a newer identity's in-flight task.
                    if self.generation == generation {
                        self.task = nil
                        self.isRefreshing = false
                    }
                }
                do {
                    let result = try await fetch(client, credential)
                    guard self.generation == generation else { return }
                    guard revalidateIdentity(credential) else { return }
                    try result.validate(for: credential.accountID)
                    lastSnapshot = result
                    let receivedUptime = uptime()
                    snapshotReceivedUptime = receivedUptime
                    snapshotFreshnessAnchorUptime = requestStartedUptime.isFinite && requestStartedUptime >= 0
                        && receivedUptime.isFinite && receivedUptime >= requestStartedUptime
                        ? requestStartedUptime : nil
                    state = .ready(result)
                } catch {
                    guard self.generation == generation else { return }
                    guard revalidateIdentity(credential) else { return }
                    if case AccountQueryError.unauthorized = error { signOut() }
                    else { state = .unavailable }
                }
            }
        } catch {
            // Unknown Keychain identity is not an HTTP outage: discard every old number.
            suspend()
            state = .unavailable
        }
        return task
    }

    private func revalidateIdentity(_ credential: AccessAccountCredential) -> Bool {
        do {
            guard try keychain.read() == credential else {
                suspend()
                state = .unavailable
                return false
            }
            return true
        } catch {
            suspend()
            state = .unavailable
            return false
        }
    }
}
