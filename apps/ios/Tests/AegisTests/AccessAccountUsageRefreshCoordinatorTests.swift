import XCTest
@testable import Aegis

@MainActor
final class AccessAccountUsageRefreshCoordinatorTests: XCTestCase {
    func testMultipleOwnersShareOneLoopAndWaitForRequestBeforeSleeping() async {
        let clock = UsageRefreshManualClock()
        let request = UsageRefreshManualClock()
        var count = 0
        var task: Task<Void, Never>?
        let coordinator = AccessAccountUsageRefreshCoordinator(refresh: {
            count += 1
            let owned = Task<Void, Never> { try? await request.sleep(.zero) }
            task = owned
            return owned
        }, sleep: { try await clock.sleep($0) })
        let a = UUID(), b = UUID()
        coordinator.setVisible(true, owner: a)
        await drain()
        XCTAssertEqual(count, 0)
        coordinator.setActive(true)
        await request.started(1)
        coordinator.setVisible(true, owner: b)
        coordinator.setVisible(true, owner: a)
        coordinator.setActive(true)
        await drain()
        XCTAssertEqual(count, 1)
        let before = await clock.count
        XCTAssertEqual(before, 0)
        await request.resume(1)
        await clock.started(1)
        let interval = await clock.duration(1)
        XCTAssertEqual(interval, .seconds(5))
        coordinator.setVisible(false, owner: a)
        await clock.resume(1)
        await request.started(2)
        XCTAssertEqual(count, 2)
        coordinator.setVisible(false, owner: b)
        XCTAssertFalse(task?.isCancelled ?? true)
        await request.resume(2)
        await task?.value
        await drain()
        XCTAssertEqual(count, 2)
        let after = await clock.count
        XCTAssertEqual(after, 1)
    }

    func testInactiveAndLastOwnerClosingStopFurtherRefreshWithoutCancellingRequest() async {
        for deactivate in [false, true] {
            let clock = UsageRefreshManualClock()
            let request = UsageRefreshManualClock()
            var count = 0
            var task: Task<Void, Never>?
            let coordinator = AccessAccountUsageRefreshCoordinator(refresh: {
                count += 1
                let owned = Task<Void, Never> { try? await request.sleep(.zero) }
                task = owned
                return owned
            }, sleep: { try await clock.sleep($0) })
            let owner = UUID()
            coordinator.setVisible(true, owner: owner)
            coordinator.setActive(true)
            await request.started(1)
            if deactivate { coordinator.setActive(false) }
            else { coordinator.setVisible(false, owner: owner) }
            XCTAssertFalse(task?.isCancelled ?? true)
            await request.resume(1)
            await task?.value
            await drain()
            XCTAssertEqual(count, 1)
            let sleeps = await clock.count
            XCTAssertEqual(sleeps, 0)
        }
    }

    func testOldCancelledLoopCannotClearReplacementLoop() async {
        let clock = UsageRefreshManualClock()
        var count = 0
        let coordinator = AccessAccountUsageRefreshCoordinator(refresh: {
            count += 1
            return nil
        }, interval: .seconds(7), sleep: { try await clock.sleep($0) })
        let owner = UUID()
        coordinator.setVisible(true, owner: owner)
        coordinator.setActive(true)
        await clock.started(1)
        coordinator.setActive(false)
        coordinator.setActive(true)
        await clock.started(2)
        XCTAssertEqual(count, 2)
        // The test clock deliberately ignores cancellation, delivering the old wake late.
        await clock.resume(1)
        await drain()
        coordinator.setVisible(true, owner: UUID())
        coordinator.setActive(true)
        await drain()
        XCTAssertEqual(count, 2, "Old defer must not erase the replacement loop")
        await clock.resume(2)
        await clock.started(3)
        XCTAssertEqual(count, 3)
        let interval = await clock.duration(3)
        XCTAssertEqual(interval, .seconds(7))
        coordinator.setActive(false)
        await clock.resume(3)
        await drain()
        XCTAssertEqual(count, 3)
    }

    func testClosingSleepingLoopDoesNotRefreshOnLateWake() async {
        let clock = UsageRefreshManualClock()
        var count = 0
        let coordinator = AccessAccountUsageRefreshCoordinator(refresh: {
            count += 1
            return nil
        }, sleep: { try await clock.sleep($0) })
        let owner = UUID()
        coordinator.setActive(true)
        coordinator.setVisible(true, owner: owner)
        await clock.started(1)
        coordinator.setVisible(false, owner: owner)
        await clock.resume(1)
        await drain()
        XCTAssertEqual(count, 1)
    }

    private func drain() async {
        // No wall-clock sleeps: let released continuations and MainActor tasks run.
        for _ in 0..<20 { await Task.yield() }
    }
}

private actor UsageRefreshManualClock {
    private var pending: [Int: CheckedContinuation<Void, Never>] = [:]
    private var waiters: [Int: CheckedContinuation<Void, Never>] = [:]
    private var durations: [Int: Duration] = [:]
    private(set) var count = 0

    func sleep(_ duration: Duration) async throws {
        count += 1
        let id = count
        durations[id] = duration
        await withCheckedContinuation { continuation in
            pending[id] = continuation
            waiters.removeValue(forKey: id)?.resume()
        }
    }

    func started(_ id: Int) async {
        if pending[id] != nil { return }
        await withCheckedContinuation { waiters[id] = $0 }
    }

    func resume(_ id: Int) { pending.removeValue(forKey: id)?.resume() }
    func duration(_ id: Int) -> Duration? { durations[id] }
}
