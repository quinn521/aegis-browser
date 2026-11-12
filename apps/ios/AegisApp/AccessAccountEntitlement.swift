import Foundation

/// The server sends byte counts as canonical decimal strings (some exceed 2^53).
struct AccessAccountEntitlement: Decodable, Equatable, Sendable {
    enum Status: String, Decodable { case notSubscribed = "not_subscribed", active, exhausted, expired }
    enum Freshness: String, Decodable { case current, delayed, unavailable }
    let entitlementAccountId: String
    let status: Status
    let planId, periodId: String?
    let periodStart, periodEnd, expiresAt: Int64?
    let quotaBytes, uploadedBytes, downloadedBytes, usedBytes, remainingBytes: String?
    let measuredAt, receivedAt, snapshotAgeMs: Int64?
    let freshness: Freshness?
    enum QuotaSource: String, Decodable { case threeXUIAlpha = "3xui_alpha" }
    enum PanelExpiryMode: String, Decodable { case none, absolute }
    let quotaSource: QuotaSource?
    let panelExpiryMode: PanelExpiryMode?
    let panelExpiresAt: Int64?

    init(entitlementAccountId: String, status: Status, planId: String?, periodId: String?,
         periodStart: Int64?, periodEnd: Int64?, expiresAt: Int64?, quotaBytes: String?,
         uploadedBytes: String?, downloadedBytes: String?, usedBytes: String?, remainingBytes: String?,
         measuredAt: Int64?, receivedAt: Int64?, snapshotAgeMs: Int64?, freshness: Freshness?,
         quotaSource: QuotaSource? = nil, panelExpiryMode: PanelExpiryMode? = nil,
         panelExpiresAt: Int64? = nil) {
        self.entitlementAccountId = entitlementAccountId
        self.status = status
        self.planId = planId
        self.periodId = periodId
        self.periodStart = periodStart
        self.periodEnd = periodEnd
        self.expiresAt = expiresAt
        self.quotaBytes = quotaBytes
        self.uploadedBytes = uploadedBytes
        self.downloadedBytes = downloadedBytes
        self.usedBytes = usedBytes
        self.remainingBytes = remainingBytes
        self.measuredAt = measuredAt
        self.receivedAt = receivedAt
        self.snapshotAgeMs = snapshotAgeMs
        self.freshness = freshness
        self.quotaSource = quotaSource
        self.panelExpiryMode = panelExpiryMode
        self.panelExpiresAt = panelExpiresAt
    }

    private enum CodingKeys: String, CodingKey {
        case entitlementAccountId, status, planId, periodId, periodStart, periodEnd, expiresAt
        case quotaBytes, uploadedBytes, downloadedBytes, usedBytes, remainingBytes
        case measuredAt, receivedAt, snapshotAgeMs, freshness
        case quotaSource, panelExpiryMode, panelExpiresAt
    }

    init(from decoder: any Decoder) throws {
        let fields = try decoder.container(keyedBy: CodingKeys.self)
        let alphaKeys: [CodingKeys] = [.quotaSource, .panelExpiryMode, .panelExpiresAt]
        let hasAlphaMetadata = alphaKeys.contains { fields.contains($0) }
        let status = try fields.decode(Status.self, forKey: .status)
        if hasAlphaMetadata {
            // Explicit null is not absence. An opt-in response must carry the whole group.
            guard alphaKeys.allSatisfy({ fields.contains($0) }), status != .notSubscribed else {
                throw AccountQueryError.invalid
            }
        }
        self.init(
            entitlementAccountId: try fields.decode(String.self, forKey: .entitlementAccountId),
            status: status,
            planId: try fields.decodeIfPresent(String.self, forKey: .planId),
            periodId: try fields.decodeIfPresent(String.self, forKey: .periodId),
            periodStart: try fields.decodeIfPresent(Int64.self, forKey: .periodStart),
            periodEnd: try fields.decodeIfPresent(Int64.self, forKey: .periodEnd),
            expiresAt: try fields.decodeIfPresent(Int64.self, forKey: .expiresAt),
            quotaBytes: try fields.decodeIfPresent(String.self, forKey: .quotaBytes),
            uploadedBytes: try fields.decodeIfPresent(String.self, forKey: .uploadedBytes),
            downloadedBytes: try fields.decodeIfPresent(String.self, forKey: .downloadedBytes),
            usedBytes: try fields.decodeIfPresent(String.self, forKey: .usedBytes),
            remainingBytes: try fields.decodeIfPresent(String.self, forKey: .remainingBytes),
            measuredAt: try fields.decodeIfPresent(Int64.self, forKey: .measuredAt),
            receivedAt: try fields.decodeIfPresent(Int64.self, forKey: .receivedAt),
            snapshotAgeMs: try fields.decodeIfPresent(Int64.self, forKey: .snapshotAgeMs),
            freshness: try fields.decodeIfPresent(Freshness.self, forKey: .freshness),
            quotaSource: hasAlphaMetadata ? try fields.decode(QuotaSource.self, forKey: .quotaSource) : nil,
            panelExpiryMode: hasAlphaMetadata ? try fields.decode(PanelExpiryMode.self, forKey: .panelExpiryMode) : nil,
            panelExpiresAt: try fields.decodeIfPresent(Int64.self, forKey: .panelExpiresAt)
        )
    }

    func validate(for account: String) throws {
        guard entitlementAccountId == account else { throw AccountQueryError.invalid }
        let raw = [quotaBytes, uploadedBytes, downloadedBytes, usedBytes, remainingBytes]
        if status == .notSubscribed {
            guard raw.allSatisfy({ $0 == nil }), planId == nil, periodId == nil, periodStart == nil,
                  periodEnd == nil, expiresAt == nil, receivedAt == nil, measuredAt == nil,
                  snapshotAgeMs == nil, freshness == nil, quotaSource == nil,
                  panelExpiryMode == nil, panelExpiresAt == nil else { throw AccountQueryError.invalid }
            return
        }
        guard let planId, !planId.isEmpty, let periodId, !periodId.isEmpty,
              let periodStart, let periodEnd, periodStart >= 0, periodEnd > periodStart,
              let expiresAt, expiresAt > 0, let receivedAt, receivedAt >= periodStart else {
            throw AccountQueryError.invalid
        }
        if quotaSource != nil || panelExpiryMode != nil || panelExpiresAt != nil {
            guard quotaSource == .threeXUIAlpha, let panelExpiryMode else { throw AccountQueryError.invalid }
            switch panelExpiryMode {
            case .none:
                guard panelExpiresAt == nil, expiresAt == periodEnd else { throw AccountQueryError.invalid }
            case .absolute:
                guard let panelExpiresAt, panelExpiresAt > 0,
                      expiresAt == min(periodEnd, panelExpiresAt) else { throw AccountQueryError.invalid }
            }
        }
        let bytes = try raw.map { string -> UInt64 in
            guard let string, let value = UInt64(string), String(value) == string else {
                throw AccountQueryError.invalid
            }
            return value
        }
        let (sum, overflow) = bytes[1].addingReportingOverflow(bytes[2])
        guard !overflow, bytes[3] == sum, bytes[4] == (bytes[0] > sum ? bytes[0] - sum : 0) else {
            throw AccountQueryError.invalid
        }
        if (status == .active && sum >= bytes[0]) || (status == .exhausted && sum < bytes[0]) {
            throw AccountQueryError.invalid
        }
        if let measuredAt {
            guard measuredAt >= 0, measuredAt <= receivedAt, let age = snapshotAgeMs,
                  age >= receivedAt - measuredAt,
                  freshness == (age > 15_000 ? .unavailable : age > 5_000 ? .delayed : .current)
            else { throw AccountQueryError.invalid }
        } else if snapshotAgeMs != nil || freshness != .unavailable {
            throw AccountQueryError.invalid
        }
    }
}

enum AccountQueryError: Error { case invalid, unauthorized }

private final class AccountRedirectGuard: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

struct AccessAccountClient: Sendable {
    let endpoint: URL
    init(origin: String, allowSimulatorLoopback: Bool = false) throws {
        guard let c = URLComponents(string: origin), let host = c.host, !host.isEmpty,
              c.scheme == "https" || (allowSimulatorLoopback && c.scheme == "http" && host == "127.0.0.1"),
              c.user == nil, c.password == nil, c.query == nil, c.fragment == nil,
              c.percentEncodedPath == "" || c.percentEncodedPath == "/", let url = c.url else {
            throw AccountQueryError.invalid
        }
        endpoint = url.appendingPathComponent("v1/me/entitlement")
    }

    func read(_ credential: AccessAccountCredential) async throws -> AccessAccountEntitlement {
        guard credential.endpoint == endpoint.absoluteString, credential.valid else { throw AccountQueryError.invalid }
        let config = URLSessionConfiguration.ephemeral
        config.urlCache = nil
        config.httpCookieStorage = nil
        config.urlCredentialStorage = nil
        config.timeoutIntervalForRequest = 10
        let session = URLSession(configuration: config, delegate: AccountRedirectGuard(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        var request = URLRequest(url: endpoint, cachePolicy: .reloadIgnoringLocalAndRemoteCacheData)
        request.setValue("Bearer " + credential.token, forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        let (stream, response) = try await session.bytes(for: request)
        guard let response = response as? HTTPURLResponse, response.url == endpoint else { throw AccountQueryError.invalid }
        if response.statusCode == 401 { throw AccountQueryError.unauthorized }
        guard response.statusCode == 200 else { throw AccountQueryError.invalid }
        var data = Data()
        for try await byte in stream {
            guard data.count < 65536 else { throw AccountQueryError.invalid }
            data.append(byte)
        }
        let result = try JSONDecoder().decode(AccessAccountEntitlement.self, from: data)
        try result.validate(for: credential.accountID)
        return result
    }
}
