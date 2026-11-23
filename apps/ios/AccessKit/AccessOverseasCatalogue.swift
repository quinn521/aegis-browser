import AegisPolicyKit
import Foundation

public enum AccessOverseasCategory: String, CaseIterable, Codable, Hashable, Sendable {
    case video = "VIDEO"
    case ai = "AI"
    case web = "WEB"
    case social = "SOCIAL"
    case news = "NEWS"
    case development = "DEV"
    case reference = "REFERENCE"
    case cloud = "CLOUD"
}

/// One verified revision of the overseas contribution. It never authorizes a
/// page on behalf of a different source or inherits a previous page's decision.
public struct AccessOverseasCatalogue: Sendable {
    public enum Scope: Equatable, Sendable {
        case all
        case categories(Set<AccessOverseasCategory>)
    }

    public struct Counts: Equatable, Sendable {
        public let source: Int
        public let admitted: Int
        public let rejected: Int
        public let unsupported: Int
        public let exclusions: Int
    }

    public enum RuleKind: UInt64, Sendable {
        case keyword = 0, regexp = 1, domain = 2, full = 3
    }

    public struct UnusedRule: Equatable, Sendable {
        public enum Reason: Equatable, Sendable {
            case hostRejected
            case unsupportedKeyword
            case unsupportedRegexp
        }

        public let value: String
        public let kind: RuleKind
        public let reason: Reason
    }

    static let positiveTag = "GFWLIST-CN"
    static let exclusionTag = "GFWLIST-CN-EXCLUDE"
    static let positiveLimit = 8_192
    static let exclusionLimit = 512

    struct Projection: Hashable, Sendable {
        let category: AccessOverseasCategory
        let host: String
        let source: String
    }

    // Explicit product selections from the reviewed v1 catalogue. A service
    // group justifies its own listed host, not the whole group or any aliases.
    static let projection: [Projection] = [
        .init(category: .video, host: "www.youtube.com", source: "YOUTUBE"),
        .init(category: .video, host: "www.netflix.com", source: "NETFLIX"),
        .init(category: .video, host: "vimeo.com", source: "VIMEO"),
        .init(category: .video, host: "www.twitch.tv", source: "TWITCH"),
        .init(category: .ai, host: "chatgpt.com", source: "OPENAI"),
        .init(category: .ai, host: "claude.ai", source: "ANTHROPIC"),
        .init(category: .ai, host: "gemini.google.com", source: "GOOGLE-GEMINI"),
        .init(category: .ai, host: "www.perplexity.ai", source: "PERPLEXITY"),
        .init(category: .web, host: "www.google.com", source: "GOOGLE"),
        .init(category: .web, host: "duckduckgo.com", source: "DUCKDUCKGO"),
        .init(category: .web, host: "search.yahoo.com", source: "YAHOO"),
        .init(category: .social, host: "x.com", source: "TWITTER"),
        .init(category: .social, host: "www.instagram.com", source: "INSTAGRAM"),
        .init(category: .social, host: "t.me", source: "TELEGRAM"),
        .init(category: .social, host: "discord.com", source: "DISCORD"),
        .init(category: .news, host: "www.bbc.com", source: "BBC"),
        .init(category: .news, host: "www.nytimes.com", source: "NYTIMES"),
        .init(category: .news, host: "www.reuters.com", source: "REUTERS"),
        .init(category: .news, host: "www.dw.com", source: "DW"),
        .init(category: .development, host: "github.com", source: "GITHUB"),
        .init(category: .development, host: "gist.github.com", source: "GITHUB"),
        .init(category: .development, host: "raw.githubusercontent.com", source: "GITHUB"),
        .init(category: .development, host: "hub.docker.com", source: "DOCKER"),
        .init(category: .reference, host: "zh.wikipedia.org", source: "WIKIMEDIA"),
        .init(category: .reference, host: "scholar.google.com", source: "GOOGLE-SCHOLAR"),
        .init(category: .reference, host: "archive.org", source: "ARCHIVE"),
        .init(category: .cloud, host: "drive.google.com", source: "GOOGLE"),
        .init(category: .cloud, host: "docs.google.com", source: "GOOGLE"),
        .init(category: .cloud, host: "www.dropbox.com", source: "DROPBOX"),
        .init(category: .cloud, host: "onedrive.live.com", source: "MICROSOFT"),
    ]

    struct Record: Hashable, Sendable {
        let kind: RuleKind
        let value: String

        func contains(_ host: String) -> Bool {
            host == value || (kind == .domain && host.hasSuffix("." + value))
        }
    }

    private struct HostIndex: Sendable {
        var exact: Set<String> = []
        var domains: Set<String> = []

        mutating func insert(_ rule: Record) {
            if rule.kind == .full { exact.insert(rule.value) }
            if rule.kind == .domain { domains.insert(rule.value) }
        }

        func contains(_ host: String) -> Bool {
            if exact.contains(host) { return true }
            var suffix = host[...]
            while true {
                if domains.contains(String(suffix)) { return true }
                guard let dot = suffix.firstIndex(of: ".") else { return false }
                suffix = suffix[suffix.index(after: dot)...]
            }
        }
    }

    public let counts: Counts
    public let unusedRules: [UnusedRule]
    public let hosts: [AccessOverseasCategory: Set<String>]
    private let allProjectedHosts: Set<String>
    private let positive: HostIndex
    private let exclusions: HostIndex
    private let exclusionKeywords: [String]

    init(positive rows: [Record], exclusions negativeRows: [Record],
         provenProjections: Set<Projection>) throws {
        guard Set(rows).count == rows.count, Set(negativeRows).count == negativeRows.count else {
            throw AccessGeoSiteAdmission.Failure.malformedDatabase
        }
        var positive = HostIndex()
        var literalSource = HostIndex()
        var admitted = 0
        var rejected = 0
        var unsupported = 0
        var unusedRules: [UnusedRule] = []
        for row in rows {
            guard row.kind == .domain || row.kind == .full else {
                unsupported += 1
                let reason: UnusedRule.Reason = row.kind == .keyword ? .unsupportedKeyword : .unsupportedRegexp
                unusedRules.append(.init(value: row.value, kind: row.kind, reason: reason))
                continue
            }
            literalSource.insert(row)
            guard RegisteredDomain.allowsGeoSiteHost(row.value, includingDescendants: row.kind == .domain) else {
                rejected += 1
                unusedRules.append(.init(value: row.value, kind: row.kind, reason: .hostRejected))
                continue
            }
            positive.insert(row)
            admitted += 1
        }
        var exclusions = HostIndex()
        var keywords: [String] = []
        for row in negativeRows {
            switch row.kind {
            case .domain, .full:
                // An exception can safely cover a broad parent. Require a
                // canonical DNS spelling without applying positive PSL limits.
                guard RegisteredDomain.isCanonicalDNSName(row.value, allowsSingleLabel: row.kind == .domain),
                      row.value.split(separator: ".").last?.utf8.contains(where: { (97...122).contains($0) }) == true else {
                    throw AccessGeoSiteAdmission.Failure.malformedDatabase
                }
                exclusions.insert(row)
            case .keyword:
                guard row.value.utf8.count <= 253,
                      row.value.utf8.allSatisfy({ (97...122).contains($0) || (48...57).contains($0) || $0 == 45 || $0 == 46 }) else {
                    throw AccessGeoSiteAdmission.Failure.malformedDatabase
                }
                keywords.append(row.value)
            case .regexp:
                throw AccessGeoSiteAdmission.Failure.malformedDatabase
            }
        }
        var hosts: [AccessOverseasCategory: Set<String>] = [:]
        for entry in provenProjections {
            guard RegisteredDomain.allowsGeoSiteHost(entry.host, includingDescendants: false),
                  literalSource.contains(entry.host), !exclusions.contains(entry.host),
                  !keywords.contains(where: { entry.host.contains($0) }) else { continue }
            hosts[entry.category, default: []].insert(entry.host)
        }
        self.positive = positive
        self.exclusions = exclusions
        exclusionKeywords = keywords
        self.hosts = hosts
        self.unusedRules = unusedRules
        allProjectedHosts = Set(hosts.values.joined())
        counts = Counts(source: rows.count, admitted: admitted, rejected: rejected,
                        unsupported: unsupported, exclusions: negativeRows.count)
    }

    public func matches(_ url: URL, scope: Scope) -> Bool {
        guard url.scheme?.lowercased() == "https", url.port == nil || url.port == 443,
              url.user == nil, url.password == nil,
              let host = AccessWebsiteRoutePolicy.websiteHost(url)?.rawValue,
              !exclusions.contains(host), !exclusionKeywords.contains(where: { host.contains($0) }) else { return false }
        switch scope {
        case .all:
            return positive.contains(host) || allProjectedHosts.contains(host)
        case .categories(let categories):
            return categories.contains { hosts[$0]?.contains(host) == true }
        }
    }
}
