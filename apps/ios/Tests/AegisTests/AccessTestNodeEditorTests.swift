import AccessKit
import Security
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
