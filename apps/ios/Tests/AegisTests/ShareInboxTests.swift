import XCTest
@testable import Aegis

final class ShareInboxTests: XCTestCase {
    private var temporaryURL: URL!

    override func setUpWithError() throws {
        temporaryURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("aegis-share-tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: temporaryURL, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let temporaryURL { try? FileManager.default.removeItem(at: temporaryURL) }
    }

    func testEnvelopeCanOnlyBeConsumedOnce() throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let inbox = try ShareInbox(baseURL: temporaryURL)
        let envelope = try inbox.makeEnvelope(from: "https://example.com/shared", now: now)
        try inbox.store(envelope)

        XCTAssertEqual(try inbox.consume(now: now.addingTimeInterval(1)), envelope)
        XCTAssertNil(try inbox.consume(now: now.addingTimeInterval(2)))
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: temporaryURL.path).isEmpty)
    }

    func testExpiredEnvelopeIsDeletedAndRejected() throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let inbox = try ShareInbox(baseURL: temporaryURL)
        try inbox.store(try inbox.makeEnvelope(from: "https://example.com", now: now))
        XCTAssertThrowsError(try inbox.consume(now: now.addingTimeInterval(61))) { error in
            XCTAssertEqual(error as? ShareInboxError, .expired)
        }
        XCTAssertNil(try inbox.consume(now: now.addingTimeInterval(62)))
    }

    func testRejectsFilesCredentialsCustomSchemesAndOversizeText() throws {
        let inbox = try ShareInbox(baseURL: temporaryURL)
        for value in [
            "file:///tmp/private.txt",
            "javascript:alert(1)",
            "https://user:secret@example.com/",
        ] {
            XCTAssertThrowsError(try inbox.makeEnvelope(from: value))
        }
        XCTAssertThrowsError(try inbox.makeEnvelope(from: String(repeating: "a", count: 8_193))) { error in
            XCTAssertEqual(error as? ShareInboxError, .payloadTooLarge)
        }
    }

    func testMaximumSizeURLSurvivesEnvelopeEncoding() throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let inbox = try ShareInbox(baseURL: temporaryURL)
        let prefix = "https://example.com/"
        let value = prefix + String(
            repeating: "a",
            count: ShareInbox.maximumPayloadBytes - prefix.utf8.count
        )

        try inbox.store(try inbox.makeEnvelope(from: value, now: now))
        XCTAssertEqual(try inbox.consume(now: now.addingTimeInterval(1))?.url.absoluteString, value)
    }

    func testRejectsFutureOrExtendedLifetimeEnvelope() throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let inbox = try ShareInbox(baseURL: temporaryURL)
        let invalid = ShareEnvelope(
            version: ShareEnvelope.currentVersion,
            nonce: UUID(),
            url: URL(string: "https://example.com")!,
            createdAt: now.addingTimeInterval(10),
            expiresAt: now.addingTimeInterval(120)
        )
        XCTAssertThrowsError(try inbox.store(invalid)) { error in
            XCTAssertEqual(error as? ShareInboxError, .invalidEnvelope)
        }

        let future = try inbox.makeEnvelope(
            from: "https://example.com/future",
            now: now.addingTimeInterval(10)
        )
        try inbox.store(future)
        XCTAssertThrowsError(try inbox.consume(now: now)) { error in
            XCTAssertEqual(error as? ShareInboxError, .invalidEnvelope)
        }
    }

    func testStaleCrashClaimIsRemoved() throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let inbox = try ShareInbox(baseURL: temporaryURL)
        let stale = temporaryURL.appendingPathComponent(".consuming-stale-v1.json")
        try Data("{}".utf8).write(to: stale)
        try FileManager.default.setAttributes(
            [.modificationDate: now.addingTimeInterval(-70)],
            ofItemAtPath: stale.path
        )

        XCTAssertNil(try inbox.consume(now: now))
        XCTAssertFalse(FileManager.default.fileExists(atPath: stale.path))
    }
}

@MainActor
final class MarkdownShareInboxTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    func testClaimCleanupCannotDeleteNewlyPublishedDocument() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let inbox = try MarkdownShareInbox(root: root)
        try inbox.store(title: "A", data: Data("first".utf8))
        let first = try inbox.consume(afterClaim: { try inbox.store(title: "B", data: Data("second".utf8)) })
        XCTAssertEqual(first?.title, "A")
        XCTAssertEqual(try inbox.consume()?.text, "second")
        XCTAssertNil(try inbox.consume())
    }
    func testMaximumEscapedDocumentAndUnicodeTitleRoundTrip() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let inbox = try MarkdownShareInbox(root: root)
        let text = String(repeating: "\u{1}", count: MarkdownShareInbox.maximumTextBytes)
        try inbox.store(title: String(repeating: "👨‍👩‍👧‍👦", count: 300), data: Data(text.utf8))
        let envelope = try XCTUnwrap(inbox.consume())
        XCTAssertEqual(envelope.text, text)
        XCTAssertLessThanOrEqual(envelope.title.utf8.count, MarkdownShareInbox.maximumTitleBytes)
        XCTAssertLessThanOrEqual(envelope.title.count, 300)
        XCTAssertFalse(envelope.title.isEmpty)
    }
    func testPoisonClaimsAreRemovedAndCannotBlockNextShare() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let inbox = try MarkdownShareInbox(root: root)
        let path = root.appendingPathComponent("markdown-inbox-v1.json")
        let now = Date()
        let bad = [Data("{".utf8),
            try JSONEncoder().encode(MarkdownShareEnvelope(nonce: UUID(), title: "NUL", text: "a\0b", expiresAt: now.addingTimeInterval(30))),
            try JSONEncoder().encode(MarkdownShareEnvelope(nonce: UUID(), title: "old", text: "text", expiresAt: now.addingTimeInterval(-1))),
            try JSONEncoder().encode(MarkdownShareEnvelope(nonce: UUID(), title: "future", text: "text", expiresAt: now.addingTimeInterval(61))),
            Data(repeating: 65, count: MarkdownShareInbox.maximumEnvelopeBytes + 1)]
        for data in bad {
            try data.write(to: path)
            XCTAssertThrowsError(try inbox.consume(now: now))
            XCTAssertNil(try inbox.consume(now: now))
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: root.path).isEmpty)
        }
        XCTAssertThrowsError(try inbox.store(title: "NUL", data: Data("\0".utf8)))
        try inbox.store(title: "good", data: Data("next".utf8))
        XCTAssertEqual(try inbox.consume()?.text, "next")
    }
    func testIndependentConsumersHaveOneWinner() async throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        try MarkdownShareInbox(root: root).store(title: "one", data: Data("payload".utf8))
        let winners = await withTaskGroup(of: Int.self) { group in
            for _ in 0..<12 {
                group.addTask { (try? MarkdownShareInbox(root: root).consume()) == nil ? 0 : 1 }
            }
            var count = 0
            for await value in group { count += value }
            return count
        }
        XCTAssertEqual(winners, 1)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: root.path).isEmpty)
    }
    func testLinkCleanupDoesNotRemoveAnActiveMarkdownClaim() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let markdown = try MarkdownShareInbox(root: root)
        try markdown.store(title: "safe", data: Data("value".utf8))
        let result = try markdown.consume(afterClaim: {
            _ = try ShareInbox(baseURL: root).consume(now: Date().addingTimeInterval(120))
        })
        XCTAssertEqual(result?.text, "value")
    }
}
