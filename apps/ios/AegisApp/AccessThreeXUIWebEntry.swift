import AccessKit

/// The current product uses one signed 3X-UI subscription for both webpages and
/// usage. Missing setup or a failed query cannot select a static bundled node.
@MainActor
enum AccessThreeXUIWebEntry {
    static func makePublisher(
        for usage: AccessSubscriptionUsageSession,
        build: (@MainActor (AccessSubscriptionUsageSession) -> any AccessProxyEntryPublishing)? = nil
    ) -> any AccessProxyEntryPublishing {
        guard usage.isSelected, usage.managesConnections else { return AccessFailClosedProxyEntryPublisher() }
        if let build { return build(usage) }
        return AccessEmbeddedProxyEntryFactory.makeProductPublisher(
            configPath: nil,
            parameters: { try await usage.connectionParameters() },
            connectionAllowed: { usage.permitsExistingConnection }
        )
    }
}
