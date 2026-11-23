import Foundation
import Network
import WebKit

/// App-managed proxy entry. Ready requires an identity and generation, not merely a listening port.
public struct AccessManagedProxyEndpoint: Sendable, Equatable {
    public var identity: String
    public var generation: UInt64
    public var loopbackHost: String
    public var port: UInt16
    public var state: AccessProxyEndpointState
    public var transport: AccessInboundTransport

    public var hasAdmittedBinding: Bool {
        !identity.isEmpty && generation != 0
            && (loopbackHost == "127.0.0.1" || loopbackHost == "::1")
            && port != 0 && (transport != .socks5 || port != 1080)
    }

    public init(
        identity: String,
        generation: UInt64,
        loopbackHost: String,
        port: UInt16,
        state: AccessProxyEndpointState,
        transport: AccessInboundTransport = .http
    ) {
        self.identity = identity
        self.generation = generation
        self.loopbackHost = loopbackHost
        self.port = port
        self.state = state
        self.transport = transport
    }
}

/// Holds one non-persistent store per scope and exact host.
@MainActor
public final class AccessHostDataStoreRegistry {
    public private(set) var switchGeneration: UInt64 = 0
    private var entries: [AccessHostStoreKey: Entry] = [:]
    private var endpoints: [AccessHostStoreKey: AccessManagedProxyEndpoint] = [:]
    public var onRetire: ((AccessHostStoreKey) -> Void)?

    private struct Entry {
        var store: WKWebsiteDataStore
        var record: AccessHostStoreRecord
        var appliedGeneration: UInt64?
        var retiringDocuments: Set<UUID> = []
    }

    public init() {}

    /// Snapshot includes canonical landing stores with a tab-scoped inherited route.
    public var endpointSnapshot: [AccessHostStoreKey: AccessManagedProxyEndpoint] { endpoints }

    public func endpoint(
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool
    ) -> AccessManagedProxyEndpoint? {
        endpoints[key(scopeID, host, isPrivate)]
    }

    /// Reports whether the registry accepted the endpoint. A rejected ready
    /// generation blocks new attachments and leaves the old document's proxy intact.
    @discardableResult
    public func setEndpoint(
        _ endpoint: AccessManagedProxyEndpoint,
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool
    ) -> Bool {
        let storeKey = key(scopeID, host, isPrivate)
        if let current = endpoints[storeKey], endpoint.generation < current.generation {
            return false
        }
        if endpoint.state == .ready {
            guard let configuration = configuration(for: endpoint) else {
                var rejected = endpoint
                rejected.state = .failed
                endpoints[storeKey] = rejected
                return false
            }
            if let entry = entries[storeKey] {
                entry.store.proxyConfigurations = [configuration]
                entries[storeKey]?.appliedGeneration = endpoint.generation
            }
        }
        endpoints[storeKey] = endpoint
        return true
    }

    public func setProxyEnabled(
        _ enabled: Bool,
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool
    ) {
        switchGeneration &+= 1
        let storeKey = key(scopeID, host, isPrivate)
        if var entry = entries[storeKey] {
            entry.record.proxyEnabled = enabled
            entries[storeKey] = entry
        }
        if !enabled {
            endpoints[storeKey]?.state = .unavailable
        }
    }

    /// Returns the dedicated store when the routing policy selects or keeps one.
    /// Returns nil when the tab must stay on its original store. Does not create a
    /// store while the entry is not ready and the tab is not already bound.
    public func storeForTab(
        tabID: String,
        scopeID: String,
        isPrivate: Bool,
        targetHost: AccessSiteHost?,
        boundHost: AccessSiteHost?,
        intent: AccessProxyIntentState,
        now: Date = Date()
    ) -> WKWebsiteDataStore? {
        let endpointState = targetHost.flatMap { endpoints[key(scopeID, $0, isPrivate)]?.state } ?? .unavailable
        let decision = AccessHostRoutingPolicy.decide(AccessHostRoutingRequest(
            intent: intent,
            endpoint: endpointState,
            targetHost: targetHost,
            boundHost: boundHost
        ))
        unbind(
            tabID,
            exceptHost: targetHost,
            scopeID: scopeID,
            isPrivate: isPrivate,
            decision: decision,
            now: now
        )
        switch decision {
        case .originalStore:
            sweep(now: now)
            return nil
        case .keepBoundHostStore, .useHostStore:
            guard let targetHost else {
                sweep(now: now)
                return nil
            }
            let store = attach(
                tabID: tabID,
                scopeID: scopeID,
                host: targetHost,
                isPrivate: isPrivate,
                now: now
            )
            sweep(now: now)
            return store
        }
    }

    /// Disables the host and drops its store. Returns tab ids that were bound so
    /// the caller can send each one back to its original store first.
    public func revoke(
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool,
        now: Date = Date()
    ) -> Set<String> {
        switchGeneration &+= 1
        let storeKey = key(scopeID, host, isPrivate)
        let tabs = entries[storeKey]?.record.boundTabIDs ?? []
        if var entry = entries[storeKey] {
            entry.record.proxyEnabled = false
            entry.record.boundTabIDs = []
            entries[storeKey] = entry
        }
        endpoints[storeKey]?.state = .unavailable
        sweep(now: now)
        return tabs
    }

    public func unbind(_ tabID: String, now: Date = Date()) {
        var changed = false
        for key in entries.keys {
            guard var entry = entries[key], entry.record.boundTabIDs.remove(tabID) != nil else { continue }
            if entry.record.boundTabIDs.isEmpty {
                entry.record.idleSince = now
            }
            entries[key] = entry
            changed = true
        }
        if changed { sweep(now: now) }
    }

    public func endPrivateSession(scopeID: String, now: Date = Date()) {
        switchGeneration &+= 1
        let keys = entries.keys.filter { $0.isPrivate && $0.scopeID == scopeID }
        for key in keys {
            entries[key]?.record.boundTabIDs = []
            entries[key]?.record.proxyEnabled = false
        }
        sweep(now: now, endingPrivateScopeID: scopeID)
    }

    public func sweep(now: Date, endingPrivateScopeID: String? = nil) {
        let evictions = AccessHostStoreEvictionPolicy.evictions(
            records: entries.values.map { entry in
                var record = entry.record
                if !entry.retiringDocuments.isEmpty { record.boundTabIDs.insert("retiring-document") }
                return record
            },
            now: now,
            endingPrivateScopeID: endingPrivateScopeID
        )
        for eviction in evictions {
            retire(eviction.key)
        }
    }

    public func ordinaryStores() -> [WKWebsiteDataStore] {
        entries.values.filter { !$0.record.key.isPrivate }.map(\.store)
    }

    /// The registry can select a replacement before WebKit acknowledges that
    /// the previous realm is inert. Such a store is not idle yet.
    public func retainRetiringDocument(scopeID: String, host: AccessSiteHost, isPrivate: Bool) -> (@MainActor () -> Void)? {
        let storeKey = key(scopeID, host, isPrivate)
        guard entries[storeKey] != nil else { return nil }
        let token = UUID()
        entries[storeKey]?.retiringDocuments.insert(token)
        return { [weak self] in
            guard let self else { return }
            self.entries[storeKey]?.retiringDocuments.remove(token)
            if self.entries[storeKey]?.retiringDocuments.isEmpty == true,
               self.entries[storeKey]?.record.boundTabIDs.isEmpty == true {
                self.entries[storeKey]?.record.idleSince = Date()
            }
            self.sweep(now: Date())
        }
    }

    public func removeUnstoredEndpoint(for key: AccessHostStoreKey) {
        guard entries[key] == nil else { return }
        endpoints[key] = nil
    }

    public func releaseIdleStores() {
        for (key, entry) in entries where !entry.record.isInUse { retire(key) }
    }

    public func store(
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool
    ) -> WKWebsiteDataStore? {
        entries[key(scopeID, host, isPrivate)]?.store
    }

    private func attach(
        tabID: String,
        scopeID: String,
        host: AccessSiteHost,
        isPrivate: Bool,
        now: Date
    ) -> WKWebsiteDataStore? {
        let storeKey = key(scopeID, host, isPrivate)
        let endpoint = endpoints[storeKey]
        if let endpoint, !endpoint.hasAdmittedBinding { return nil }
        if entries[storeKey] == nil {
            guard let endpoint, let configuration = configuration(for: endpoint) else { return nil }
            let store = WKWebsiteDataStore.nonPersistent()
            store.proxyConfigurations = [configuration]
            entries[storeKey] = Entry(
                store: store,
                record: AccessHostStoreRecord(
                    key: storeKey,
                    boundTabIDs: [],
                    proxyEnabled: true,
                    idleSince: nil
                ),
                appliedGeneration: endpoint.generation
            )
        }
        guard var entry = entries[storeKey] else { return nil }
        entry.record.boundTabIDs.insert(tabID)
        entry.record.idleSince = nil
        if let endpoint, endpoint.state == .ready, entry.appliedGeneration != endpoint.generation {
            guard let configuration = configuration(for: endpoint) else { return nil }
            entry.store.proxyConfigurations = [configuration]
            entry.appliedGeneration = endpoint.generation
        }
        entries[storeKey] = entry
        _ = now
        return entry.store
    }

    private func unbind(
        _ tabID: String,
        exceptHost: AccessSiteHost?,
        scopeID: String,
        isPrivate: Bool,
        decision: AccessHostRoutingDecision,
        now: Date
    ) {
        let keep: AccessSiteHost?
        switch decision {
        case .originalStore:
            keep = nil
        case .keepBoundHostStore, .useHostStore:
            keep = exceptHost
        }
        for key in entries.keys {
            let sameScope = key.scopeID == scopeID && key.isPrivate == isPrivate
            guard sameScope, key.host != keep, var entry = entries[key] else { continue }
            if entry.record.boundTabIDs.remove(tabID) != nil, entry.record.boundTabIDs.isEmpty {
                entry.record.idleSince = now
            }
            entries[key] = entry
        }
    }

    private func retire(_ key: AccessHostStoreKey) {
        guard entries[key]?.retiringDocuments.isEmpty == true,
              let entry = entries.removeValue(forKey: key) else { return }
        entry.store.proxyConfigurations = []
        endpoints[key] = nil
        onRetire?(key)
    }

    private func configuration(for endpoint: AccessManagedProxyEndpoint) -> ProxyConfiguration? {
        guard endpoint.state == .ready,
              endpoint.hasAdmittedBinding,
              let port = NWEndpoint.Port(rawValue: endpoint.port)
        else { return nil }
        let address = NWEndpoint.hostPort(host: NWEndpoint.Host(endpoint.loopbackHost), port: port)
        var configuration: ProxyConfiguration
        switch endpoint.transport {
        case .http: configuration = ProxyConfiguration(httpCONNECTProxy: address)
        case .https: configuration = ProxyConfiguration(httpCONNECTProxy: address, tlsOptions: NWProtocolTLS.Options())
        case .socks5: configuration = ProxyConfiguration(socksv5Proxy: address)
        }
        configuration.allowFailover = false
        return configuration
    }

    private func key(
        _ scopeID: String,
        _ host: AccessSiteHost,
        _ isPrivate: Bool
    ) -> AccessHostStoreKey {
        AccessHostStoreKey(scopeID: scopeID, host: host, isPrivate: isPrivate)
    }
}
