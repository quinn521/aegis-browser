import Foundation
import Network
import Synchronization

/// Builds the product proxy entry publisher.
///
/// Stay fail-closed unless: lock is `verified_build`, the thin
/// `AegisEmbeddedCore` module is linked, a local or account-provided Reality
/// Vision config is present, and loopback CONNECT can start. Digests alone never dial.
@MainActor
public enum AccessEmbeddedProxyEntryFactory {
    public static let lockResourceName = "embedded-core-lock"
    public static let lockResourceExtension = "json"

    /// Default product publisher. Falls back to fail-closed when any gate fails.
    public static func makeProductPublisher(
        lockData: Data? = nil,
        configPath: String? = AccessLocalV2RayConfigLoader.productPath(),
        verifiedCore: (any AccessEmbeddedCoreControlling)? = nil,
        parameters: (@MainActor () async throws -> AccessRealityVisionParameters)? = nil,
        connectionAllowed: (@MainActor () -> Bool)? = nil,
        retireDocuments: (@MainActor (@escaping @MainActor () -> Void) -> Void)? = nil,
        transport: AccessInboundTransport = .http
    ) -> any AccessProxyEntryPublishing {
        do {
            guard transport == .http || transport == .socks5 else { return AccessFailClosedProxyEntryPublisher() }
            let data = try lockData ?? loadBundledLockData()
            let manifest = try AccessEmbeddedCoreManifest.load(from: data)
            guard manifest.mayLink else {
                return AccessFailClosedProxyEntryPublisher()
            }
            let core: any AccessEmbeddedCoreControlling
            if let verifiedCore {
                core = verifiedCore
            } else {
                core = try AccessGoEmbeddedCore()
            }
            let adapter = AccessEmbeddedCoreAdapter(manifest: manifest, verifiedCore: core)
            guard adapter.mayDial else {
                return AccessFailClosedProxyEntryPublisher()
            }
            let session: AccessEmbeddedProxyEntrySession
            if let parameters {
                session = AccessEmbeddedProxyEntrySession(core: core, parameters: parameters, retireDocuments: retireDocuments, transport: transport)
            } else {
                guard let configPath else { return AccessFailClosedProxyEntryPublisher() }
                let local = try AccessLocalV2RayConfigLoader.load(path: configPath)
                let outbound = try AccessRealityVisionOutboundConfigBuilder.build(parameters: local.parameters)
                session = AccessEmbeddedProxyEntrySession(core: core, outbound: outbound, transport: transport, retireDocuments: retireDocuments)
            }
            return AccessProxyEntryPublisher(
                provision: { generation, deadline in
                    do { return try await session.provision(generation: generation, deadline: deadline) }
                    catch {
                        await session.stopUnpublished()
                        throw error
                    }
                },
                shutdown: { identity in
                    session.shutdown(identity: identity)
                },
                healthCheck: {
                    session.isRuntimeHealthy && (connectionAllowed?() ?? true)
                }
            )
        } catch {
            return AccessFailClosedProxyEntryPublisher()
        }
    }

    public static func loadBundledLockData(
        bundle: Bundle = Bundle(for: AccessEmbeddedCoreAdapter.self)
    ) throws -> Data {
        guard let url = bundle.url(
            forResource: lockResourceName,
            withExtension: lockResourceExtension
        ) else {
            throw AccessEmbeddedCoreError.invalidLock
        }
        return try Data(contentsOf: url)
    }
}

/// Owns one running embedded core + loopback CONNECT listener for the shared entry.
@MainActor
final class AccessEmbeddedProxyEntrySession {
    private let core: any AccessEmbeddedCoreControlling
    private let loadOutbound: @MainActor () async throws -> AccessRealityVisionOutboundConfig
    private var listener: AccessEmbeddedInboundListener?
    private let transport: AccessInboundTransport
    private var identity: String?
    private var startedCore = false
    private var pendingStop: Task<Void, Never>?
    private var logCapture: AccessDiagnosticLog.Capture?
    private var retireDocuments: (@MainActor (@escaping @MainActor () -> Void) -> Void)?

    init(core: any AccessEmbeddedCoreControlling, outbound: AccessRealityVisionOutboundConfig,
         transport: AccessInboundTransport = .http,
         retireDocuments: (@MainActor (@escaping @MainActor () -> Void) -> Void)? = nil) {
        self.core = core
        self.transport = transport
        self.retireDocuments = retireDocuments
        self.loadOutbound = { outbound }
    }

    init(core: any AccessEmbeddedCoreControlling,
         parameters: @escaping @MainActor () async throws -> AccessRealityVisionParameters,
         retireDocuments: (@MainActor (@escaping @MainActor () -> Void) -> Void)? = nil,
         transport: AccessInboundTransport = .http) {
        self.core = core
        self.transport = transport
        self.loadOutbound = { try AccessRealityVisionOutboundConfigBuilder.build(parameters: await parameters()) }
        self.retireDocuments = retireDocuments
    }

    func provision(generation: UInt64, deadline: Duration) async throws -> AccessProxyEntryPublisher.Provision {
        _ = deadline
        // Synchronous publisher shutdown retains its teardown barrier. A new
        // generation must not observe the old core as running and reuse it.
        if let pendingStop {
            await pendingStop.value
            self.pendingStop = nil
        }
        try Task.checkCancellation()
        if let listener, let identity, listener.isListening,
           let actualPort = listener.boundPort, actualPort != 0, core.isRunning {
            return AccessProxyEntryPublisher.Provision(
                endpoint: AccessManagedProxyEndpoint(
                    identity: identity,
                    generation: generation,
                    loopbackHost: AccessEndpoint.loopbackHost,
                    port: actualPort,
                    state: .ready,
                    transport: transport
                ),
                listenerReady: true,
                runtimeHealthy: true,
                bridgeInstalled: true
            )
        }

        if !core.isRunning {
            // Fetch account-selected credentials only when a new core generation is needed.
            // Failure propagates; never reuse a bundled static node in this path.
            let outbound = try await loadOutbound()
            try Task.checkCancellation()
            do {
                logCapture = try await Self.startCoreWithDiagnostics(
                    core: core, outbound: outbound, diagnostics: .shared,
                    level: AccessDiagnosticLog.selectedLevel()
                )
            } catch {
                if let logCapture { AccessDiagnosticLog.shared.stop(capture: logCapture) }
                logCapture = nil
                throw AccessSiteSessionError.prepareFailed("core.start: \(error)")
            }
            startedCore = true
        }
        guard core.isRunning else {
            throw AccessSiteSessionError.runtimeNotReady
        }

        let boundSocket: NWListener
        do {
            boundSocket = try await Self.reserveLoopbackSocket()
        } catch let error as AccessSiteSessionError {
            throw error
        } catch {
            throw AccessSiteSessionError.prepareFailed("port.reserve: \(error)")
        }
        var transferredSocket = false
        defer { if !transferredSocket { boundSocket.cancel() } }
        try Task.checkCancellation()
        guard let port = boundSocket.port?.rawValue, port != 0,
              transport != .socks5 || port != 1080 else {
            throw AccessSiteSessionError.runtimeNotReady
        }

        let registration: AccessRegistration
        do {
            registration = try AccessRegistration(
                id: try AccessRegistrationID("embedded-entry-\(generation)-\(port)"),
                owner: try AccessOwner(channel: 1, profile: "ordinary", partition: "default"),
                transport: transport,
                endpoint: try AccessEndpoint(host: AccessEndpoint.loopbackHost, port: port),
                credentialSlot: try AccessCredentialSlot(id: "embedded-entry"),
                lease: try AccessLease(
                    generation: max(generation, 1),
                    expiresAt: Date().addingTimeInterval(60 * 60)
                )
            )
        } catch {
            throw AccessSiteSessionError.prepareFailed("registration: \(error)")
        }

        let opener = AccessEmbeddedTunnelOpener(dialer: AccessEmbeddedRealityDialer(core: core))
        let next: AccessEmbeddedInboundListener
        do {
            switch transport {
            case .http: next = .http(try AccessHTTPInboundListener(registration: registration, preparedSocket: boundSocket, tunnels: opener))
            case .socks5: next = .socks5(try AccessSOCKS5InboundListener(registration: registration, preparedSocket: boundSocket, tunnels: opener))
            case .https: throw AccessSiteSessionError.runtimeNotReady
            }
            try await next.start(at: Date())
        } catch {
            throw AccessSiteSessionError.prepareFailed("listener.start: \(error)")
        }
        guard let actualPort = next.boundPort, actualPort == port, actualPort != 0,
              transport != .socks5 || actualPort != 1080 else {
            try? next.stop()
            throw AccessSiteSessionError.runtimeNotReady
        }
        try Task.checkCancellation()
        let entryIdentity = "embedded-\(transport.rawValue)-\(registration.id.rawValue)"
        transferredSocket = true
        listener = next
        identity = entryIdentity
        return AccessProxyEntryPublisher.Provision(
            endpoint: AccessManagedProxyEndpoint(
                identity: entryIdentity,
                generation: generation,
                loopbackHost: AccessEndpoint.loopbackHost,
                port: actualPort,
                state: .ready,
                transport: transport
            ),
            listenerReady: true,
            runtimeHealthy: true,
            bridgeInstalled: true
        )
    }

    /// Retries only a recognized Xray log-initialization failure with the same
    /// outbound and quiet logging; ordinary core failures are never hidden.
    static func startCoreWithDiagnostics(
        core: any AccessEmbeddedCoreControlling,
        outbound: AccessRealityVisionOutboundConfig,
        diagnostics: AccessDiagnosticLog,
        level: AccessLogLevel
    ) async throws -> AccessDiagnosticLog.Capture? {
        let parameters = outbound.parameters
        let capture = try? diagnostics.acquireShared(
            level: level, secrets: [parameters.userID, parameters.publicKey, parameters.shortID]
        )
        if level != .none && capture == nil { diagnostics.markUnavailable() }
        do {
            let config = try outbound.embeddedInstanceJSONData(
                logLevel: capture?.level ?? .none,
                errorLogPath: capture?.path, includeAccessLog: true
            )
            try await core.start(configData: config)
            return capture
        } catch {
            if let capture { diagnostics.stop(capture: capture) }
            let text = (error as NSError).localizedDescription
            let loggerFailure = text.contains("failed to initialize access logger") ||
                text.contains("failed to initialize error logger")
            guard capture != nil, !core.isRunning, loggerFailure else { throw error }
            diagnostics.markUnavailable()
            try await core.start(configData: outbound.embeddedInstanceJSONData())
            return nil
        }
    }

    func stopUnpublished() async {
        guard identity == nil, startedCore else { return }
        startedCore = false
        await core.stop()
        if let logCapture { AccessDiagnosticLog.shared.stop(capture: logCapture) }
        logCapture = nil
    }

    var isRuntimeHealthy: Bool {
        guard let listener, identity != nil else { return false }
        return listener.isListening && core.isRunning
    }

    func shutdown(identity: String) {
        guard self.identity == identity else { return }
        if let listener {
            if let retireDocuments {
                listener.suspend()
                retireDocuments { try? listener.stop() }
            } else {
                try? listener.stop()
            }
        }
        listener = nil
        self.identity = nil
        if startedCore {
            startedCore = false
            let capture = logCapture
            logCapture = nil
            // Publisher shutdown is synchronous; stop the core without blocking callers.
            pendingStop = Task {
                await core.stop()
                if let capture { AccessDiagnosticLog.shared.stop(capture: capture) }
            }
        }
    }

    /// The ready socket is transferred intact to the selected protocol listener.
    /// A port number obtained from a cancelled reservation is never published.
    static func reserveLoopbackSocket() async throws -> NWListener {
        try await withThrowingTaskGroup(of: NWListener.self) { group in
            group.addTask {
                try await Self.awaitEphemeralLoopbackSocket()
            }
            group.addTask {
                try await Task.sleep(for: .seconds(2))
                throw AccessSiteSessionError.prepareFailed("port reserve timed out")
            }
            guard let socket = try await group.next() else {
                throw AccessSiteSessionError.prepareFailed("port reserve returned no value")
            }
            group.cancelAll()
            return socket
        }
    }

    private static func awaitEphemeralLoopbackSocket() async throws -> NWListener {
        let nwListener: NWListener
        do {
            let parameters = NWParameters.tcp
            parameters.allowLocalEndpointReuse = false
            parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
            nwListener = try NWListener(using: parameters, on: .any)
        } catch {
            throw AccessSiteSessionError.prepareFailed(String(describing: error))
        }

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                let resume = OnceSocketResume(continuation)
                // Required before start on current Network.framework — missing handler
                // surfaces as POSIX EINVAL on some simulator runtimes.
                nwListener.newConnectionHandler = { connection in
                    connection.cancel()
                }
                nwListener.stateUpdateHandler = { state in
                    switch state {
                    case .ready:
                        if let port = nwListener.port?.rawValue, port > 0 {
                            nwListener.stateUpdateHandler = nil
                            resume.finish(returning: nwListener)
                        } else {
                            resume.finish(
                                throwing: AccessSiteSessionError.prepareFailed(
                                    "port.reserve: listener ready without port"
                                )
                            )
                            nwListener.cancel()
                        }
                    case .failed(let error):
                        nwListener.stateUpdateHandler = nil
                        resume.finish(
                            throwing: AccessSiteSessionError.prepareFailed(
                                "port.reserve: \(error)"
                            )
                        )
                        nwListener.cancel()
                    case .cancelled:
                        nwListener.stateUpdateHandler = nil
                        resume.finish(throwing: CancellationError())
                    default:
                        break
                    }
                }
                nwListener.start(queue: DispatchQueue(label: "accesskit.embedded.port.reserve"))
            }
        } onCancel: {
            nwListener.cancel()
        }
    }
}

/// One-shot continuation resume for NWListener callbacks (any queue).
///
/// `Mutex` is preferred over `NSLock` on iOS 18+/Swift 6; kept as a class so the
/// non-Copyable mutex can own the continuation.
private final class OnceSocketResume: Sendable {
    private let state: Mutex<CheckedContinuation<NWListener, Error>?>

    init(_ continuation: CheckedContinuation<NWListener, Error>) {
        state = Mutex(continuation)
    }

    func finish(returning listener: NWListener) {
        take()?.resume(returning: listener)
    }

    func finish(throwing error: Error) {
        take()?.resume(throwing: error)
    }

    private func take() -> CheckedContinuation<NWListener, Error>? {
        state.withLock { pending in
            let value = pending
            pending = nil
            return value
        }
    }
}

private final class AccessEmbeddedTunnelOpener: AccessHTTPTunnelOpening, @unchecked Sendable {
    private let dialer: AccessEmbeddedRealityDialer

    init(dialer: AccessEmbeddedRealityDialer) {
        self.dialer = dialer
    }

    func open(host: String, port: UInt16) async throws -> any AccessRealityPathStream {
        try await dialer.dial(host: host, port: port)
    }
}


private enum AccessEmbeddedInboundListener {
    case http(AccessHTTPInboundListener)
    case socks5(AccessSOCKS5InboundListener)

    var isListening: Bool {
        switch self { case .http(let value): value.isListening; case .socks5(let value): value.isListening }
    }
    var boundPort: UInt16? {
        switch self { case .http(let value): value.boundPort; case .socks5(let value): value.boundPort }
    }
    func start(at now: Date) async throws {
        switch self { case .http(let value): try await value.start(at: now); case .socks5(let value): try await value.start(at: now) }
    }
    func suspend() {
        switch self { case .http(let value): value.suspend(); case .socks5(let value): value.suspend() }
    }
    func stop() throws {
        switch self { case .http(let value): try value.stop(); case .socks5(let value): try value.stop() }
    }
}
