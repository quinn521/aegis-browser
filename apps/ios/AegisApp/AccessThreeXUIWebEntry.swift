import AccessKit
import Combine
import CryptoKit
import Foundation

/// Bounded catalogue of test routes packaged for internal distribution.
@MainActor
final class AccessAlphaNodeCatalogue: ObservableObject {
    enum Source: String, Decodable, Equatable {
        case panel = "3x-ui"
        case downloads
    }

    private struct PackagedRoute: Decodable {
        let source: Source?
        let uri: String
        var quotaBytes: Int64? = nil
    }

    struct Entry: Identifiable, Equatable {
        let id: Int
        let protocolName: String
        let source: Source?
        let usesSubscription: Bool
        var quotaBytes: Int64? = nil
        /// Routes may share a protocol; the nonsecret ordinal tells them apart without exposing the relay.
        var displayName: String { "\(id + 1) · \(protocolName)" }
    }

    private static let fingerprintKey = "access.alpha.nodes.fingerprint.v1"
    private static let selectionKey = "access.alpha.nodes.selection.v1"
    static let shared = product()

    let isConfigured: Bool
    let isPrivateBeta: Bool
    let isExternalBeta: Bool
    var isBetaCatalogue: Bool { isPrivateBeta || isExternalBeta }
    @Published private(set) var entries: [Entry] = []
    @Published private(set) var selectedIndex: Int?
    private let defaults: UserDefaults
    private var candidates: [AccessInternalTestNode] = []
    private var fingerprint = ""
    private var revision = UUID()
    private var activeRevision: UUID?
    fileprivate var selectionDidChange: (() -> Void)?

    var enablesWebsiteAcceleration: Bool { isConfigured && !entries.isEmpty }
    var routes: [AccessInternalTestNode] { candidates }
    var selectedEntry: Entry? { selectedIndex.map { entries[$0] } }

    static func product(bundle: Bundle = .main, defaults: UserDefaults = .standard) -> AccessAlphaNodeCatalogue {
        let channel = bundle.object(forInfoDictionaryKey: "AegisAccessReleaseChannel") as? String
        let profile = bundle.object(forInfoDictionaryKey: "AegisReleaseProfile") as? String
        let bundleID = bundle.bundleIdentifier
        let internalTesting = bundle.object(forInfoDictionaryKey: "TFInternalTestingOnly") as? Bool == true
        let beta = allowsBetaNodes(channel: channel, profile: profile, bundleID: bundleID, internalTesting: internalTesting)
        let external = !internalTesting && channel == "beta" && profile == "beta" && bundleID == "com.gcsa.aegis"
        let externalData = external ? bundle.url(forResource: "AegisExternalBetaNodes", withExtension: "json").map {
            (try? Data(contentsOf: $0, options: .mappedIfSafe)) ?? Data()
        } : nil
        let resourceName = beta ? "AegisPrivateBetaTestNodes" : "AegisAlphaTestNodes"
        let isEligible = beta || allowsPackagedNodes(channel: channel, profile: profile, bundleID: bundleID)
        let resource = isEligible ? bundle.url(forResource: resourceName, withExtension: "json") : nil
        // A present but unreadable resource remains configured and fails closed.
        let data = resource.map { (try? Data(contentsOf: $0, options: .mappedIfSafe)) ?? Data() }
        return .init(channel: channel, profile: profile, bundleID: bundleID, data: beta ? nil : data,
                     privateBetaData: beta ? data : nil, externalBetaData: externalData, internalTesting: internalTesting, defaults: defaults)
    }

    private static func allowsPackagedNodes(channel: String?, profile: String?, bundleID: String?) -> Bool {
        (channel == "dev" || channel == "alpha") && profile == channel
            && ["com.gcsa.aegis", "com.gcsa.aegis.alpha.internal"].contains(bundleID ?? "")
    }

    private static func allowsBetaNodes(channel: String?, profile: String?, bundleID: String?, internalTesting: Bool) -> Bool {
        channel == "beta" && profile == "beta" && bundleID == "com.gcsa.aegis" && internalTesting
    }

    init(channel: String?, profile: String?, bundleID: String?, data: Data?, privateBetaData: Data? = nil, externalBetaData: Data? = nil,
         internalTesting: Bool = false, defaults: UserDefaults) {
        self.defaults = defaults
        isPrivateBeta = Self.allowsBetaNodes(channel: channel, profile: profile, bundleID: bundleID,
                                           internalTesting: internalTesting) && privateBetaData != nil
        isExternalBeta = !internalTesting && channel == "beta" && profile == "beta" && bundleID == "com.gcsa.aegis" && externalBetaData != nil
        let alphaData = Self.allowsPackagedNodes(channel: channel, profile: profile, bundleID: bundleID) ? data : nil
        let resource = isExternalBeta ? externalBetaData : (isPrivateBeta ? privateBetaData : alphaData)
        isConfigured = resource != nil
        guard let resource, resource.count <= 131_500 else { return }
        let decoder = JSONDecoder()
        let inputs: [PackagedRoute]
        if isExternalBeta {
            guard let routes = try? decoder.decode([PackagedRoute].self, from: resource),
                  routes.allSatisfy({ $0.source == nil && ($0.quotaBytes ?? 1) > 0 }) else { return }
            inputs = routes
        } else if isPrivateBeta {
            guard let routes = try? decoder.decode([PackagedRoute].self, from: resource),
                  routes.first?.source == .panel, routes.allSatisfy({ $0.source != nil }) else { return }
            inputs = routes
        } else {
            guard let links = try? decoder.decode([String].self, from: resource) else { return }
            inputs = links.map { PackagedRoute(source: nil, uri: $0) }
        }
        guard (1...16).contains(inputs.count) else { return }
        var loaded: [AccessInternalTestNode] = []
        var subscriptionIdentity: UUID?
        var displayed: [Entry] = []
        for (index, input) in inputs.enumerated() {
            let link = input.uri
            guard link.utf8.count <= 8_192,
                  let node = try? AccessInternalTestNode.parseSingleShareURI(link),
                  let user = node.testSubscriptionIdentity,
                  !loaded.contains(where: { $0.isWireIdentical(to: node) }) else { return }
            if node.kind == .vless,
               Self.protocolName(URLComponents(string: link)?.queryItems ?? []) == nil { return }
            if index == 0 {
                guard !isPrivateBeta || node.profileKey == .vlessRawReality else { return }
                subscriptionIdentity = user
            }
            // Only a distinct signed test identity can run independently of the default subscription.
            let usesSubscription = !isExternalBeta && user == subscriptionIdentity
            guard isBetaCatalogue || usesSubscription else { return }
            loaded.append(node)
            displayed.append(Entry(id: index, protocolName: node.protocolDisplayName, source: input.source,
                                   usesSubscription: usesSubscription, quotaBytes: isExternalBeta ? input.quotaBytes : nil))
        }
        fingerprint = SHA256.hash(data: resource).map { String(format: "%02x", $0) }.joined()
        candidates = loaded
        entries = displayed
        if defaults.string(forKey: Self.fingerprintKey) == fingerprint,
           let stored = defaults.object(forKey: Self.selectionKey) as? Int, loaded.indices.contains(stored) {
            selectedIndex = stored
        } else {
            selectedIndex = 0
        }
    }

    private static func protocolName(_ items: [URLQueryItem]) -> String? {
        let type = items.first { $0.name == "type" }?.value
        let security = items.first { $0.name == "security" }?.value
        let flow = items.first { $0.name == "flow" }?.value
        switch (type, security, flow) {
        case ("tcp", "tls", "xtls-rprx-vision"): return "VLESS / TCP / TLS / Vision"
        case ("tcp", "reality", "xtls-rprx-vision"): return "VLESS / TCP / REALITY / Vision"
        case ("ws", "tls", nil): return "VLESS / WS / TLS"
        case ("grpc", "reality", nil): return "VLESS / gRPC / REALITY"
        default: return nil
        }
    }

    func select(_ index: Int) {
        guard candidates.indices.contains(index), index != selectedIndex else { return }
        selectedIndex = index
        revision = UUID()
        activeRevision = nil
        selectionDidChange?()
        defaults.set(fingerprint, forKey: Self.fingerprintKey)
        defaults.set(index, forKey: Self.selectionKey)
    }

    func canSelect(_ entry: Entry, subscription: AccessSubscriptionUsageSession) -> Bool {
        !entry.usesSubscription || subscription.isAlphaRouteSelectable(entry.id)
    }

    fileprivate func selected() throws -> (node: AccessInternalTestNode, revision: UUID, usesSubscription: Bool) {
        guard let index = selectedIndex, candidates.indices.contains(index) else { throw AccessSubscriptionQueryError.unavailable }
        return (candidates[index], revision, entries[index].usesSubscription)
    }

    fileprivate func activate(_ expected: UUID) -> Bool {
        guard expected == revision else { return false }
        activeRevision = expected
        return true
    }

    fileprivate var isActiveSelection: Bool { activeRevision == revision }
}

/// The current product uses one signed 3X-UI subscription for both webpages and
/// usage. Alpha's selected test route still requires that same live allowance.
@MainActor
enum AccessThreeXUIWebEntry {
    static func makePublisher(
        for usage: AccessSubscriptionUsageSession,
        alphaNodes: AccessAlphaNodeCatalogue? = nil,
        lockData: Data? = nil,
        verifiedCore: (any AccessEmbeddedCoreControlling)? = nil,
        retireDocuments: @escaping @MainActor (String, @escaping @MainActor () -> Void) -> Void
    ) -> any AccessProxyEntryPublishing {
        guard alphaNodes?.isExternalBeta == true || (usage.isSelected && usage.managesConnections) else { return AccessFailClosedProxyEntryPublisher() }
        if let alphaNodes, alphaNodes.isConfigured {
            guard alphaNodes.enablesWebsiteAcceleration,
                  let core = verifiedCore ?? (try? AccessGoEmbeddedCore()) else {
                return AccessFailClosedProxyEntryPublisher()
            }
            let publisher = AccessEmbeddedProxyEntryFactory.makeInternalTestPublisher(
                lockData: lockData, verifiedCore: core,
                node: {
                    let candidate = try alphaNodes.selected()
                    if candidate.usesSubscription { try await usage.authorizeAlphaNode(candidate.node) }
                    try Task.checkCancellation()
                    guard alphaNodes.activate(candidate.revision) else { throw AccessSubscriptionQueryError.unavailable }
                    return candidate.node
                },
                connectionAllowed: {
                    alphaNodes.isActiveSelection && (alphaNodes.selectedEntry?.usesSubscription == false || usage.permitsExistingConnection)
                },
                retireDocuments: retireDocuments, transport: .http
            )
            if let publisher = publisher as? AccessProxyEntryPublisher {
                alphaNodes.selectionDidChange = { [weak publisher] in publisher?.closeCurrentGeneration() }
            }
            return publisher
        }
        return AccessEmbeddedProxyEntryFactory.makeProductPublisher(
            lockData: lockData,
            configPath: nil,
            verifiedCore: verifiedCore,
            parameters: { try await usage.connectionParameters() },
            connectionAllowed: { usage.permitsExistingConnection },
            retireDocuments: retireDocuments
        )
    }
}
