import Foundation
import XCTest
@testable import AccessKit

final class AccessOutboundNodeTests: XCTestCase {
    func testSharedShareLinksPreserveLegacyConfigBytesAndAdmission() throws {
        for fixture in try fixtures() {
            let key = try XCTUnwrap(AccessOutboundProfileKey(profileName: fixture.profileKey))
            XCTAssertEqual(key.rawValue, fixture.profileKey)
            for alias in fixture.aliases {
                XCTAssertEqual(AccessOutboundProfileKey(profileName: alias), key)
            }
            let node = try AccessOutboundNode.parseSingleShareURI(fixture.share)
            XCTAssertEqual(node.profileKey, key)
            XCTAssertEqual(node.kind, .vless)
            XCTAssertEqual(try JSONDecoder().decode(AccessOutboundNode.self, from: JSONEncoder().encode(node)), node)
            XCTAssertEqual(try AccessOutboundConfigRegistry.embeddedInstanceJSONData(for: node),
                           try JSONSerialization.data(withJSONObject: fixture.embeddedConfig, options: [.sortedKeys]))

            let outbounds = try XCTUnwrap(fixture.embeddedConfig["outbounds"] as? [[String: Any]])
            XCTAssertEqual(outbounds.first?["tag"] as? String, fixture.legacyOutboundTag)
            for suffix in fixture.rejectedQuerySuffixes {
                let share = fixture.share.prefix(while: { $0 != "#" })
                XCTAssertThrowsError(try AccessOutboundNode.parseSingleShareURI(String(share) + suffix))
            }
        }
        XCTAssertNil(AccessOutboundProfileKey(profileName: "vless.raw.none"))
        XCTAssertEqual(AccessOutboundProfile.realityVision.rawValue, "reality_vision")
        for wsProfile in ["ws_tls_compat", "vless_ws_tls", "vless.ws.tls"] {
            XCTAssertThrowsError(try AccessOutboundProfile.parse(rawValue: wsProfile))
        }
    }

    func testEnabledLoggingConfigMatchesFixtureWithLogSinkAndGeneration() throws {
        let path = "/private/var/mobile/aegis-diagnostics/fifo"
        for fixture in try fixtures() {
            let node = try AccessOutboundNode.parseSingleShareURI(fixture.share)
            for level in [AccessLogLevel.debug, .info, .warning, .error] {
                var expected = fixture.embeddedConfig
                expected["log"] = ["loglevel": level.rawValue, "access": path, "error": path]
                expected["aegisLogCaptureGeneration"] = 7
                let expectedData = try JSONSerialization.data(withJSONObject: expected, options: [.sortedKeys])
                XCTAssertEqual(try AccessOutboundConfigRegistry.embeddedInstanceJSONData(
                    for: node, logLevel: level, errorLogPath: path, logCaptureGeneration: 7), expectedData,
                    "\(fixture.profileKey) \(level.rawValue)")
                // Account-selected REALITY nodes assemble the same document on their own path.
                if case .realityVision(let parameters) = node {
                    let config = try AccessRealityVisionOutboundConfigBuilder.build(parameters: parameters)
                    XCTAssertEqual(try config.embeddedInstanceJSONData(
                        logLevel: level, errorLogPath: path, includeAccessLog: true, logCaptureGeneration: 7),
                        expectedData, "account path \(level.rawValue)")
                    XCTAssertEqual(try config.embeddedInstanceJSONData(),
                                   try AccessOutboundConfigRegistry.embeddedInstanceJSONData(for: node))
                }
            }
        }
    }

    func testOutboundNodeStorageLayoutIsStable() throws {
        let fixtures = try fixtures()
        let ws = try AccessOutboundNode.parseSingleShareURI(fixtures[1].share)
        let stored = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(ws)) as? NSDictionary)
        let expected: NSDictionary = [
            "kind": "vless",
            "endpoint": ["address": "192.0.2.20", "port": 8443],
            "security": ["tls": ["serverName": "cert.example.com", "fingerprint": "chrome", "alpn": "http/1.1"]],
            "transport": ["ws": ["path": "/ws?token=%74est-value&opaque=%E9%E8", "host": "ws.example.com"]],
            "credentials": ["vless": ["userID": "00000000-0000-4000-8000-000000000002", "encryption": "none"]],
        ]
        XCTAssertEqual(stored, expected)

        let reality = try AccessOutboundNode.parseSingleShareURI(fixtures[0].share)
        let fields = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(reality)) as? [String: Any])
        let security = try XCTUnwrap((fields["security"] as? [String: Any])?["reality"] as? [String: Any])
        XCTAssertEqual(Set(security.keys), ["serverName", "fingerprint", "publicKey", "shortID", "spiderX"])
        let transport = try XCTUnwrap(fields["transport"] as? [String: Any])
        XCTAssertEqual(Set(transport.keys), ["raw"])
        let credentials = try XCTUnwrap((fields["credentials"] as? [String: Any])?["vless"] as? [String: String])
        XCTAssertEqual(credentials["flow"], "xtls-rprx-vision")
        XCTAssertEqual(credentials["encryption"], "none")
    }

    func testStructuralWhitelistRejectsAddedFieldsAndWrongShapesForBothProfiles() throws {
        for fixture in try fixtures() {
            let key = try XCTUnwrap(AccessOutboundProfileKey(profileName: fixture.profileKey))
            let root = fixture.embeddedConfig
            let outbounds = try XCTUnwrap(root["outbounds"] as? [[String: Any]])
            let outbound = try XCTUnwrap(outbounds.first)
            let stream = try XCTUnwrap(outbound["streamSettings"] as? [String: Any])
            let settings = try XCTUnwrap(outbound["settings"] as? [String: Any])
            let vnext = try XCTUnwrap((settings["vnext"] as? [[String: Any]])?.first)
            let user = try XCTUnwrap((vnext["users"] as? [[String: Any]])?.first)
            let securityKey = key == .vlessRawReality ? "realitySettings" : "tlsSettings"
            let security = try XCTUnwrap(stream[securityKey] as? [String: Any])

            func replacing(_ object: [String: Any], _ key: String, _ value: Any) -> [String: Any] {
                object.merging([key: value]) { _, new in new }
            }
            func withStream(_ stream: [String: Any]) -> [String: Any] {
                replacing(root, "outbounds", [replacing(outbound, "streamSettings", stream)])
            }
            func withServer(_ server: [String: Any]) -> [String: Any] {
                replacing(root, "outbounds", [replacing(outbound, "settings", ["vnext": [server]])])
            }

            XCTAssertNoThrow(try AccessOutboundConfigRegistry.assertFailClosed(root, profile: key))
            let invalid: [[String: Any]] = [
                replacing(root, "extra", true),
                replacing(root, "routing", ["rules": []]),
                replacing(root, "inbounds", [["protocol": "socks"]]),
                replacing(root, "outbounds", [outbound, outbound]),
                replacing(root, "outbounds", [] as [Any]),
                replacing(root, "outbounds", [replacing(outbound, "mux", ["enabled": false])]),
                replacing(root, "outbounds", [replacing(outbound, "proxySettings", [:] as [String: Any])]),
                replacing(root, "outbounds", [replacing(outbound, "protocol", "freedom")]),
                withStream(replacing(stream, "fragment", [:] as [String: Any])),
                withStream(replacing(stream, "sockopt", ["dialerProxy": "another"])),
                withStream(replacing(stream, "network", 42)),
                withStream(replacing(stream, "security", "none")),
                withStream(replacing(stream, securityKey, replacing(security, "allowInsecure", false))),
                withStream(replacing(stream, securityKey, replacing(security, "disableSystemRoot", false))),
                withServer(replacing(vnext, "users", [user, user])),
                withServer(replacing(vnext, "users", [replacing(user, "futureField", "")])),
                withServer(replacing(vnext, "port", true)),
                withServer(replacing(vnext, "port", 443.5)),
                withServer(replacing(vnext, "port", 443.0)),
            ]
            for (index, object) in invalid.enumerated() {
                XCTAssertThrowsError(try AccessOutboundConfigRegistry.assertFailClosed(object, profile: key),
                                     "\(key.rawValue) mutation \(index)")
            }
            XCTAssertThrowsError(try AccessOutboundConfigRegistry.assertFailClosed(root,
                profile: key == .vlessRawReality ? .vlessWsTls : .vlessRawReality))
            if key == .vlessWsTls {
                XCTAssertThrowsError(try AccessOutboundConfigRegistry.assertFailClosed(
                    withStream(replacing(stream, securityKey, replacing(security, "useSystemCA", 1))), profile: key))
            }
        }
    }

    func testDiagnosticSecretsIncludeBothPathRepresentationsAndSuppressOverflow() throws {
        let fixtures = try fixtures()
        let reality = try AccessOutboundNode.parseSingleShareURI(fixtures[0].share)
        let ws = try AccessOutboundNode.parseSingleShareURI(fixtures[1].share)
        XCTAssertTrue(try AccessOutboundConfigRegistry.diagnosticSecrets(for: reality).contains("01020304"))
        let secrets = try AccessOutboundConfigRegistry.diagnosticSecrets(for: ws)
        XCTAssertTrue(secrets.contains("00000000-0000-4000-8000-000000000002"))
        XCTAssertTrue(secrets.contains("%74est-value"))
        XCTAssertTrue(secrets.contains("test-value"))
        XCTAssertTrue(secrets.contains("%E9%E8"))
        XCTAssertFalse(secrets.contains("/ws"))

        let query = (0..<256).map { "q=" + String(format: "%04d", $0) }.joined(separator: "&")
        let params = try AccessVLESSWebSocketTLSParameters(address: "192.0.2.20", port: 443,
            userID: "00000000-0000-4000-8000-000000000002", serverName: "cert.example.com",
            requestHost: "ws.example.com", path: "/ws?" + query, fingerprint: "", alpn: "")
        let overflow = try AccessOutboundConfigRegistry.diagnosticSecrets(for: .vlessWebSocketTLS(params))
        XCTAssertEqual(Set(overflow).count, 257)
        let log = AccessDiagnosticLog(pollInterval: .seconds(60))
        let first = log.acquireShared(level: .info, secrets: [])
        let path = try XCTUnwrap(first.path)
        let writer = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        defer { try? writer.close() }
        let blocked = log.acquireShared(level: .info, secrets: overflow)
        XCTAssertNil(blocked.path)
        XCTAssertTrue(log.unavailable)
        try writer.write(contentsOf: Data("2026/11/11 00:00:00 from local accepted //example.invalid:443\n".utf8))
        XCTAssertTrue(log.stop(lease: blocked))
        XCTAssertTrue(log.stop(lease: first))
        XCTAssertEqual(log.snapshot(), "")
        let atLimit = Set(overflow).subtracting(["0255"])
        XCTAssertEqual(atLimit.count, 256)
        let recovered = log.acquireShared(level: .info, secrets: Array(atLimit))
        defer { log.stop(lease: recovered) }
        XCTAssertNotNil(recovered.path)
    }

    func testShortQueryValuesDoNotRedactTimestampsOrEndpoints() async throws {
        let userID = "00000000-0000-4000-8000-000000000002"
        let params = try AccessVLESSWebSocketTLSParameters(address: "192.0.2.20", port: 443,
            userID: userID, serverName: "cert.example.com", requestHost: "ws.example.com",
            path: "/ws?v=1&x=%31&k=a+b&id=abc&token=long-value", fingerprint: "", alpn: "")
        let secrets = try AccessOutboundConfigRegistry.diagnosticSecrets(for: .vlessWebSocketTLS(params))
        XCTAssertEqual(Set(secrets), [userID, "long-value"])

        let log = AccessDiagnosticLog(pollInterval: .milliseconds(15))
        defer { log.stop() }
        let lease = log.acquireShared(level: .info, secrets: secrets)
        let path = try XCTUnwrap(lease.path)
        let writer = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        defer { try? writer.close() }
        try writer.write(contentsOf: Data("2026/11/11 00:00:01 from local accepted //abc.example.com:1443\n".utf8))
        let deadline = ContinuousClock.now + .seconds(2)
        while !log.snapshot().contains("abc.example.com:1443"), ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(log.snapshot().contains("2026/11/11 00:00:01 [Access] accepted abc.example.com:1443"))
        XCTAssertTrue(log.stop(lease: lease))
    }

    private struct Fixture {
        let profileKey: String
        let aliases: [String]
        let legacyOutboundTag: String
        let share: String
        let rejectedQuerySuffixes: [String]
        let embeddedConfig: [String: Any]
    }

    private func fixtures() throws -> [Fixture] {
        let bundle = Bundle(for: Self.self)
        return try ["vless.raw.reality", "vless.ws.tls"].map { name in
            let url = try XCTUnwrap(bundle.url(forResource: name, withExtension: "json", subdirectory: "share-links"))
            let root = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
            return Fixture(profileKey: try XCTUnwrap(root["profileKey"] as? String),
                           aliases: try XCTUnwrap(root["aliases"] as? [String]),
                           legacyOutboundTag: try XCTUnwrap(root["legacyOutboundTag"] as? String),
                           share: try XCTUnwrap(root["share"] as? String),
                           rejectedQuerySuffixes: try XCTUnwrap(root["rejectedQuerySuffixes"] as? [String]),
                           embeddedConfig: try XCTUnwrap(root["embeddedConfig"] as? [String: Any]))
        }
    }
}
