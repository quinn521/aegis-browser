import XCTest
@testable import BrowserKit
@testable import Aegis

@MainActor
final class AccessAccountSessionTests: XCTestCase {
    private func isolatedStore() -> AccessAccountKeychain {
        let keychain = AccessAccountKeychain(service: "test.access.account." + UUID().uuidString)
        addTeardownBlock { try keychain.save(nil) }
        return keychain
    }

    func testEndpointAllowsOnlyAServiceOriginAndExplicitLoopback() throws {
        for origin in ["http://example.org", "https://a@b", "https://example.org/?x", "https://example.org/#x",
                       "https://example.org/v1", "http://localhost", "http://127.0.0.1/"] {
            XCTAssertThrowsError(try AccessAccountClient(origin: origin))
        }
        XCTAssertEqual(try AccessAccountClient(origin: "https://account.example.test/").endpoint.absoluteString,
                       "https://account.example.test/v1/me/entitlement")
    }

    func testManagedLeaseRejectsWrongAccountPeriodContextModeAndExpiry() throws {
        let context = UUID().uuidString.lowercased(), now = Date(timeIntervalSince1970: 1)
        let fixture: [String: Any] = ["accountId": "A", "periodId": "p", "contextId": context, "mode": "regular",
            "credentialId": "node-id", "username": "node-id", "password": "synthetic", "host": "node.invalid",
            "port": 443, "expiresAt": 2000]
        func validate(_ replacements: [String: Any]) throws {
            let data = try JSONSerialization.data(withJSONObject: fixture.merging(replacements) { _, new in new })
            let value = try JSONDecoder().decode(AccessNodeLease.self, from: data)
            try value.validate(account: "A", period: "p", context: context, mode: .regular, now: now)
        }
        try validate([:])
        for invalid: [String: Any] in [["accountId": "B"], ["periodId": "other"], ["contextId": UUID().uuidString],
                                      ["mode": "private"], ["expiresAt": 1000], ["username": "other"],
                                      ["host": "user@node.invalid"], ["port": 0], ["credentialId": "../x", "username": "../x"]] {
            XCTAssertThrowsError(try validate(invalid))
        }
    }

    func testStatusFreshnessAndIntegerWireContract() throws {
        let complete: [String: Any] = ["entitlementAccountId": "A", "status": "active",
            "planId": "p", "periodId": "1", "periodStart": 0, "periodEnd": 30000, "expiresAt": 28000,
            "quotaBytes": "9007199254740997", "uploadedBytes": "4", "downloadedBytes": "5",
            "usedBytes": "9", "remainingBytes": "9007199254740988", "receivedAt": 20000,
            "measuredAt": 10000, "snapshotAgeMs": 10000, "freshness": "delayed"]
        func decode(_ fields: [String: Any]) throws -> AccessAccountEntitlement {
            let data = try JSONSerialization.data(withJSONObject: fields)
            let result = try JSONDecoder().decode(AccessAccountEntitlement.self, from: data)
            try result.validate(for: "A")
            return result
        }
        XCTAssertEqual(try decode(complete).freshness, .delayed)
        XCTAssertEqual(try decode(complete.merging(["status": "expired"]) { _, x in x }).status, .expired)
        XCTAssertEqual(try decode(["entitlementAccountId": "A", "status": "not_subscribed"]).status, .notSubscribed)
        for (key, bad): (String, Any) in [("quotaBytes", 9007199254740997 as UInt64), ("usedBytes", "09"),
                                         ("remainingBytes", "0"), ("freshness", "current"), ("status", "exhausted")] {
            XCTAssertThrowsError(try decode(complete.merging([key: bad]) { _, x in x }), key)
        }
    }

    func testPendingSwitchBackAndRevocationCannotPublishAnEarlierIdentity() async throws {
        let service = try AccessAccountClient(origin: "https://account.example.test")
        let store = isolatedStore()
        let pending = DelayedAccountReads()
        let session = AccessAccountSession(client: service, keychain: store) { _, credential in
            try await pending.fetch(credential.token)
        }
        let first = try XCTUnwrap(session.bind(accountID: "A", token: "one"))
        await pending.started("one")
        let second = try XCTUnwrap(session.bind(accountID: "B", token: "two"))
        await pending.started("two")
        let third = try XCTUnwrap(session.bind(accountID: "A", token: "three"))
        await pending.started("three")
        await pending.respond("three", with: account("A", plan: "new-plan"))
        await third.value
        await pending.respond("one", with: account("A", plan: "old-plan"))
        await pending.fail("two", error: AccountQueryError.unauthorized)
        await first.value
        await second.value
        XCTAssertEqual(session.state, .ready(account("A", plan: "new-plan")))
        XCTAssertEqual(try store.read()?.token, "three")

        let task = session.refresh()
        await pending.started("three")
        await pending.fail("three", error: AccountQueryError.unauthorized)
        await task?.value
        XCTAssertEqual(session.state, .signedOut)
        XCTAssertNil(try store.read())
    }

    func testForeignAccountResponseAndUnavailableNeverExposePriorNumbers() async throws {
        let client = try AccessAccountClient(origin: "https://account.example.test")
        let pending = DelayedAccountReads()
        let session = AccessAccountSession(client: client, keychain: isolatedStore()) { _, credential in
            try await pending.fetch(credential.token)
        }
        let first = try XCTUnwrap(session.bind(accountID: "A", token: "one"))
        await pending.started("one")
        await pending.respond("one", with: account("A"))
        await first.value
        XCTAssertEqual(session.state, .ready(account("A")))
        let wrong = session.refresh()
        XCTAssertEqual(session.state, .loading)
        await pending.started("one")
        await pending.respond("one", with: account("B"))
        await wrong?.value
        XCTAssertEqual(session.state, .unavailable)
        let failed = session.refresh()
        await pending.started("one")
        await pending.fail("one", error: URLError(.timedOut))
        await failed?.value
        XCTAssertEqual(session.state, .unavailable)
        let late = session.refresh()
        await pending.started("one")
        session.signOut()
        await pending.respond("one", with: account("A"))
        await late?.value
        XCTAssertEqual(session.state, .signedOut)
    }

#if DEBUG && targetEnvironment(simulator)
    func testSimulatorProductEntryAgainstProvisionedAccountService() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let origin = env["AEGIS_S1_FIXTURE_ORIGIN"] else {
            throw XCTSkip("Start apps/ios/scripts/account-entitlement-fixture.py for the loopback service test")
        }
        let store = isolatedStore()
        let session = AccessAccountSession.product(environment: ["AEGIS_ACCOUNT_SIM_ORIGIN": origin,
            "AEGIS_ACCOUNT_SIM_ACTION": "bind", "AEGIS_ACCOUNT_SIM_ID": "A",
            "AEGIS_ACCOUNT_SIM_TOKEN": try XCTUnwrap(env["AEGIS_S1_FIXTURE_A"])], keychain: store)
        await session.refresh()?.value
        guard case let .ready(a) = session.state else { return XCTFail("A did not resolve through native product entry") }
        XCTAssertEqual(a.entitlementAccountId, "A")
        XCTAssertEqual(a.quotaBytes, "9007199254740997")
        XCTAssertEqual(a.usedBytes, "65636")
        XCTAssertEqual(a.freshness, .unavailable)
        XCTAssertNil(a.measuredAt)
        await session.bind(accountID: "B", token: try XCTUnwrap(env["AEGIS_S1_FIXTURE_B"]))?.value
        guard case let .ready(b) = session.state else { return XCTFail("B did not resolve") }
        XCTAssertEqual(b.status, .exhausted)
        XCTAssertEqual(b.remainingBytes, "0")
        await session.bind(accountID: "B", token: "unknown")?.value
        XCTAssertEqual(session.state, .signedOut)

        let redirection = try XCTUnwrap(env["AEGIS_S1_FIXTURE_REDIRECT"])
        let guarded = AccessAccountSession.product(environment: ["AEGIS_ACCOUNT_SIM_ORIGIN": redirection,
            "AEGIS_ACCOUNT_SIM_ACTION": "bind", "AEGIS_ACCOUNT_SIM_ID": "A", "AEGIS_ACCOUNT_SIM_TOKEN": "canary"], keychain: store)
        await guarded.refresh()?.value
        XCTAssertEqual(guarded.state, .unavailable)
        let (count, _) = try await URLSession.shared.data(from: XCTUnwrap(URL(string: redirection + "/visits")))
        XCTAssertEqual(String(decoding: count, as: UTF8.self), "0")
    }
#endif

#if DEBUG && targetEnvironment(simulator)
    func testManagedRegularAndExplicitPrivateDownloadAndLiveLogout() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let origin = env["AEGIS_S3_ORIGIN"] else { throw XCTSkip("Start the managed TLS CONNECT/Xray fixture") }
        let target = try XCTUnwrap(env["AEGIS_S3_HTTPS_URL"].flatMap(URL.init(string:)))
        let id = try XCTUnwrap(env["AEGIS_S3_ID"])
        let token = try XCTUnwrap(env["AEGIS_S3_TOKEN"])
        let client = try AccessAccountClient(origin: origin, allowSimulatorLoopback: true)
        let account = AccessAccountSession(client: client, keychain: isolatedStore())
        await account.bind(accountID: id, token: token)?.value
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: dir) }
        let downloads = DownloadManager(directory: dir, background: false)
        try await waitUntil { downloads.isReady }
        let browser = BrowserSession(dataStore: BrowserDataStore(persistenceURL: nil),
                                     workspaceStore: WorkspaceStore(persistenceURL: nil))
        let site = AccessSiteProxySessionStore()
        let regularTab = try XCTUnwrap(browser.activeTab)
        let regular = regularTab.id
        regularTab.prepareForRestore(url: target)
        let regularPrepared = await site.setProxyEnabled(true, for: regularTab, account: account)?.value
        XCTAssertEqual(regularPrepared, true)
        browser.switchProfile(to: .privateMode)
        let otr = try XCTUnwrap(browser.activeTab), privateTab = otr.id
        otr.prepareForRestore(url: target)
        let unauthorized = await site.setProxyEnabled(true, for: otr, account: account)?.value
        XCTAssertEqual(unauthorized, false)
        do {
            _ = try await account.downloadSession(for: privateTab, privateMode: true, target: target, downloadDelegate: downloads)
            XCTFail("OTR borrowed regular authorization")
        } catch AccountQueryError.unauthorized { }
        let authorized = await site.setProxyEnabled(true, for: otr, account: account, privateAccount: id, privateToken: token)?.value
        XCTAssertEqual(authorized, true)
        for tab in [regularTab, otr] {
            browser.switchProfile(to: tab.profile)
            browser.activate(tab.id)
            tab.prepareForRestore(url: target)
            XCTAssertEqual(tab.selectedAccessHost, target.host?.lowercased())
            tab.onDownload?(target)
            let captured = try XCTUnwrap(browser.pendingAccessDownload)
            let download = try await account.startProtectedDownload(.init(urls: [target]), context: captured, browser: browser,
                                                                    downloads: downloads)
            try await waitUntil { downloads.items.contains { $0.id == download && [.completed, .failed].contains($0.state) } }
            let item = try XCTUnwrap(downloads.items.first { $0.id == download })
            XCTAssertEqual(item.state, .completed, item.message ?? "")
            XCTAssertEqual(item.sha256, env["AEGIS_S3_EXPECTED_SHA256"])
        }
        let slow = try XCTUnwrap(env["AEGIS_S3_SLOW_URL"].flatMap(URL.init(string:)))
        let route = try await account.downloadSession(for: regular, privateMode: false, target: slow, downloadDelegate: downloads)
        let live = try downloads.start(.init(urls: [slow]), accessSession: route)
        try await waitUntil { downloads.items.contains { $0.id == live && $0.state == .running && $0.received > 0 } }
        let alternate = try XCTUnwrap(env["AEGIS_S3_B_ID"]), alternateToken = try XCTUnwrap(env["AEGIS_S3_B_TOKEN"])
        await account.bind(accountID: alternate, token: alternateToken)?.value
        XCTAssertThrowsError(try downloads.start(.init(urls: [slow]), accessSession: route))
        try await waitUntil { downloads.items.contains { $0.id == live && [.failed, .cancelled].contains($0.state) } }
        let secondRoute = try await account.downloadSession(for: regular, privateMode: false, target: slow, downloadDelegate: downloads)
        let secondLive = try downloads.start(.init(urls: [slow]), accessSession: secondRoute)
        try await waitUntil { downloads.items.contains { $0.id == secondLive && $0.state == .running && $0.received > 0 } }
        account.signOut()
        XCTAssertThrowsError(try downloads.start(.init(urls: [slow]), accessSession: secondRoute))
        try await waitUntil { downloads.items.contains { $0.id == secondLive && [.failed, .cancelled].contains($0.state) } }
        do {
            _ = try await account.downloadSession(for: privateTab, privateMode: true, target: target, downloadDelegate: downloads)
            XCTFail("Private bearer survived sign-out")
        } catch AccountQueryError.unauthorized { }
    }

    private func waitUntil(_ predicate: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(24)
        while !predicate() {
            guard ContinuousClock.now < deadline else { throw NSError(domain: "ManagedTransferDeadline", code: 1) }
            try await Task.sleep(for: .milliseconds(40))
        }
    }
#endif

    private func account(_ id: String, plan: String = "p") -> AccessAccountEntitlement {
        AccessAccountEntitlement(entitlementAccountId: id, status: .active, planId: plan, periodId: "current",
            periodStart: 10, periodEnd: 100, expiresAt: 90, quotaBytes: "100", uploadedBytes: "2",
            downloadedBytes: "3", usedBytes: "5", remainingBytes: "95", measuredAt: nil, receivedAt: 15,
            snapshotAgeMs: nil, freshness: .unavailable)
    }


}

private actor DelayedAccountReads {
    typealias Callback = CheckedContinuation<AccessAccountEntitlement, any Error>
    var reads: [String: Callback] = [:]
    var waiters: [String: CheckedContinuation<Void, Never>] = [:]

    func fetch(_ token: String) async throws -> AccessAccountEntitlement {
        try await withCheckedThrowingContinuation { continuation in
            reads[token] = continuation
            waiters.removeValue(forKey: token)?.resume()
        }
    }
    func started(_ token: String) async {
        if reads[token] == nil { await withCheckedContinuation { waiters[token] = $0 } }
    }
    func respond(_ token: String, with result: AccessAccountEntitlement) {
        reads.removeValue(forKey: token)?.resume(returning: result)
    }
    func fail(_ token: String, error: any Error) { reads.removeValue(forKey: token)?.resume(throwing: error) }
}
