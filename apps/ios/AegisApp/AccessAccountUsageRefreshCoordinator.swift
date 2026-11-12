import Foundation

/// One application-wide display refresh loop shared by all visible usage surfaces.
/// Cancelling this loop never cancels the session-owned refresh task it awaits.
@MainActor
final class AccessAccountUsageRefreshCoordinator {
    private let refresh: () -> Task<Void, Never>?
    private let interval: Duration
    private let sleep: @Sendable (Duration) async throws -> Void
    private var owners: Set<UUID> = []
    private var active = false
    private var generation = UUID()
    private var loop: Task<Void, Never>?

    init(
        refresh: @escaping () -> Task<Void, Never>?,
        interval: Duration = .seconds(5),
        sleep: @escaping @Sendable (Duration) async throws -> Void = { try await Task.sleep(for: $0) }
    ) {
        precondition(interval > .zero, "Refresh interval must be positive")
        self.refresh = refresh
        self.interval = interval
        self.sleep = sleep
    }

    func setVisible(_ visible: Bool, owner: UUID) {
        if visible { owners.insert(owner) }
        else { owners.remove(owner) }
        reconcile()
    }

    func setActive(_ active: Bool) {
        self.active = active
        reconcile()
    }

    private func reconcile() {
        guard active, !owners.isEmpty else {
            generation = UUID()
            loop?.cancel()
            loop = nil
            return
        }
        guard loop == nil else { return }
        generation = UUID()
        let generation = generation
        let sleep = sleep
        let interval = interval
        loop = Task { [weak self] in
            defer {
                if self?.generation == generation { self?.loop = nil }
            }
            while !Task.isCancelled, self?.mayRefresh(generation) == true {
                // Keep no strong coordinator reference across the await.
                let request = self?.refresh()
                await request?.value
                guard !Task.isCancelled, self?.mayRefresh(generation) == true else { return }
                do { try await sleep(interval) }
                catch { return }
            }
        }
    }

    private func mayRefresh(_ generation: UUID) -> Bool {
        self.generation == generation && active && !owners.isEmpty
    }
}
