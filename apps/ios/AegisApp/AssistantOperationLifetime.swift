import Combine
import UIKit

/// A synchronous notification revokes consent even when SwiftUI coalesces a rapid
/// inactive/active transition. A service that ignores cancellation cannot publish a stale reply.
@MainActor
final class AssistantOperationLifetime: ObservableObject {
    private(set) var generation = UUID()
    var operation: Task<Void, Never>?
    private var foreground: Bool
    private let applicationIsActive: @MainActor () -> Bool
    private var observations: Set<AnyCancellable> = []

    init(applicationIsActive: @escaping @MainActor () -> Bool = { UIApplication.shared.applicationState == .active }) {
        self.applicationIsActive = applicationIsActive
        foreground = applicationIsActive()
        NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)
            .sink { [weak self] _ in self?.foreground = false; self?.invalidate() }
            .store(in: &observations)
        NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)
            .sink { [weak self] _ in self?.foreground = true }
            .store(in: &observations)
        NotificationCenter.default.publisher(for: UIScene.willDeactivateNotification)
            .sink { [weak self] _ in self?.invalidate() }
            .store(in: &observations)
    }

    func isCurrent(_ token: UUID?) -> Bool {
        token == generation && foreground && applicationIsActive()
    }

    func invalidate() {
        generation = UUID()
        operation?.cancel(); operation = nil
    }

    deinit { operation?.cancel() }
}
