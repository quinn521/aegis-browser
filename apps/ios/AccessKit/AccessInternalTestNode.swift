import Foundation
import Network

/// Outbound choices for the isolated native DEV/internal Alpha test Profile.
public enum AccessInternalTestNode: Equatable, Sendable {
    case realityVision(AccessRealityVisionParameters)
    case vlessWebSocketTLS(AccessVLESSWebSocketTLSParameters)

    public static func parseSingleShareURI(_ input: String) throws -> Self {
        let link = input.trimmingCharacters(in: .whitespacesAndNewlines)
        let forbidden = CharacterSet.whitespacesAndNewlines.union(.controlCharacters)
        guard input.utf8.count <= 8_192,
              link.lowercased().hasPrefix("vless://"),
              link.unicodeScalars.allSatisfy({ !forbidden.contains($0) }),
              !link.dropFirst("vless://".count).contains("://"),
              let components = URLComponents(string: link),
              components.password == nil,
              components.path.isEmpty || components.path == "/" else {
            throw AccessLocalV2RayConfigError.invalidJSON
        }
        let items = components.queryItems ?? []
        guard items.contains(where: { $0.name == "type" && $0.value == "ws" }) else {
            return .realityVision(try AccessLocalV2RayConfigLoader.parseSingleShareURI(input))
        }
        let permitted: Set<String> = ["type", "security", "host", "sni", "fp", "path", "encryption", "alpn"]
        let names = Set(items.map(\.name))
        guard names.count == items.count, names.isSubset(of: permitted),
              let address = components.host, let rawPort = components.port, let port = UInt16(exactly: rawPort),
              let id = components.user else { throw AccessLocalV2RayConfigError.invalidJSON }
        let query = Dictionary(uniqueKeysWithValues: items.map { ($0.name, $0.value ?? "") })
        guard query["security"] == "tls", [nil, "", "none"].contains(query["encryption"]),
              let serverName = query["sni"] else {
            throw AccessLocalV2RayConfigError.invalidJSON
        }
        return .vlessWebSocketTLS(try AccessVLESSWebSocketTLSParameters(
            address: address, port: port, userID: id, serverName: serverName,
            requestHost: query["host"] ?? serverName, path: query["path"] ?? "/",
            fingerprint: query["fp"] ?? "", alpn: query["alpn"] ?? ""
        ))
    }
}

public struct AccessVLESSWebSocketTLSParameters: Equatable, Sendable {
    public let address: String
    public let port: UInt16
    public let userID: String
    public let serverName: String
    public let requestHost: String
    public let path: String
    public let fingerprint: String
    public let alpn: String

    public init(address: String, port: UInt16, userID: String, serverName: String,
                requestHost: String, path: String, fingerprint: String, alpn: String) throws {
        let address = address.hasPrefix("[") && address.hasSuffix("]") ? String(address.dropFirst().dropLast()) : address
        guard Self.isDNSName(address) || IPv4Address(address) != nil || IPv6Address(address) != nil else {
            throw AccessLocalV2RayConfigError.fieldNotCanonical("address")
        }
        guard port != 0, let uuid = UUID(uuidString: userID) else { throw AccessLocalV2RayConfigError.invalidJSON }
        guard Self.isDNSName(serverName), Self.isDNSName(requestHost) else {
            throw AccessLocalV2RayConfigError.fieldNotCanonical("tls-host")
        }
        guard path.hasPrefix("/"), !path.hasPrefix("//"), !path.contains("#"), !path.contains("\\"),
              path.utf8.count <= 2_048, path.utf8.allSatisfy({ (33...126).contains($0) }) else {
            throw AccessLocalV2RayConfigError.fieldNotCanonical("path")
        }
        guard ["", "chrome", "firefox", "safari", "ios", "android", "edge"].contains(fingerprint),
              ["", "http/1.1"].contains(alpn) else { throw AccessLocalV2RayConfigError.invalidJSON }
        self.address = address
        self.port = port
        self.userID = uuid.uuidString.lowercased()
        self.serverName = serverName
        self.requestHost = requestHost
        self.path = path
        self.fingerprint = fingerprint
        self.alpn = alpn
    }

    private static func isDNSName(_ name: String) -> Bool {
        let labels = name.split(separator: ".", omittingEmptySubsequences: false)
        return name.utf8.count <= 253 && labels.count >= 2
            && labels.last?.utf8.contains(where: { (65...90).contains($0) || (97...122).contains($0) }) == true
            && labels.allSatisfy { label in
                (1...63).contains(label.utf8.count) && label.first != "-" && label.last != "-"
                    && label.utf8.allSatisfy { (65...90).contains($0) || (97...122).contains($0)
                        || (48...57).contains($0) || $0 == 45 }
            }
    }

    /// Emits one typed outbound and requires normal TLS certificate verification.
    func embeddedInstanceJSONData() throws -> Data {
        var tls: [String: Any] = ["serverName": serverName]
        if !fingerprint.isEmpty { tls["fingerprint"] = fingerprint }
        if !alpn.isEmpty { tls["alpn"] = [alpn] }
        let outbound: [String: Any] = [
            "tag": "internal_vless_ws_tls",
            "protocol": "vless",
            "settings": [
                "vnext": [["address": address, "port": Int(port),
                           "users": [["id": userID, "encryption": "none"]]]],
            ],
            "streamSettings": [
                "network": "ws",
                "security": "tls",
                "tlsSettings": tls,
                "wsSettings": ["host": requestHost, "path": path],
            ],
        ]
        return try JSONSerialization.data(withJSONObject: [
            "log": ["loglevel": "none", "access": "none", "error": "none"],
            "inbounds": [],
            "outbounds": [outbound],
        ], options: [.sortedKeys])
    }
}
