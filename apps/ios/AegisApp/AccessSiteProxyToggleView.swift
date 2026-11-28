import AccessKit
import BrowserKit
import SwiftUI

/// Shared presentation for a website’s requested and active location protection.
struct AccessSiteProxyToggleView: View {
    @ObservedObject var store: AccessSiteProxySessionStore
    let url: URL?
    var tab: BrowserTab?
    var showsConnectionOptions = true

    @EnvironmentObject private var account: AccessAccountSession
    @State private var authorizing = false
    @State private var accountID = ""
    @State private var sessionToken = ""
    @State private var authorizationHost: AccessSiteHost?

    @Environment(\.locale) private var localizationLocale

    private var snapshot: AccessSiteProxyToggleSnapshot {
        if let tab { return store.snapshot(for: tab, forLocationControl: true) }
        return store.snapshot(for: url)
    }

    var body: some View {
        let _ = localizationLocale
        let snap = snapshot
        Section {
            Toggle(isOn: Binding(
                get: { snap.toggleOn },
                set: { enabled in
                    guard let tab, let host = snap.host else { return }
                    store.setSiteProxyEnabled(enabled, for: tab, displayedHost: host)
                }
            )) {
                Label("隐藏地理位置", systemImage: "location.slash")
                    .font(.headline)
            }
            .disabled(!snap.toggleEnabled || tab == nil)
            .accessibilityIdentifier("access-site-proxy-toggle")

            if snap.hasTrustedSite {
                Text(LocalizedStringKey(AccessSiteProxyToggleModel.siteStatusLine(snap)))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("access-site-proxy-connection")
                if snap.toggleOn || snap.connectionStatus != .idle {
                    LabeledContent("服务连接") {
                        Text(LocalizedStringKey(AccessSiteProxyToggleModel.connectionLine(for: snap.connectionStatus)))
                            .accessibilityIdentifier("access-site-service-status")
                    }
                }
                if snap.toggleOn {
                    LabeledContent("网页定位") {
                        Text(snap.locationBlockingActive ? "已阻止" : "暂不可用")
                    }
                }
            } else {
                Text("请先打开网站。").font(.subheadline).foregroundStyle(.secondary)
                    .accessibilityIdentifier("access-site-proxy-no-host")
            }
            if showsConnectionOptions { connectionOptions(snap) }
        } header: {
            if let host = snap.host {
                Text(verbatim: AccessSiteDisplayName.title(host.rawValue))
                    .textCase(nil).accessibilityIdentifier("access-site-proxy-host")
            }
        } footer: {
            if snap.toggleEnabled { Text("启用后，网站看到服务出口 IP，网页无法读取设备位置。") }
        }
    }

    private func connectionOptions(_ snap: AccessSiteProxyToggleSnapshot) -> some View {
        DisclosureGroup("连接选项") {
            Toggle(
                LocalizedStringKey(AccessAutoRecoveryCopy.toggleTitle),
                isOn: Binding(
                    get: { snap.autoRecoveryEnabled },
                    set: { enabled in
                        guard let tab else { return }
                        _ = store.setAutoRecoveryEnabled(enabled, for: tab)
                    }
                )
            )
            .disabled(!snap.hasTrustedSite || tab == nil)
            .accessibilityIdentifier("access-site-auto-recovery-toggle")
            Text(LocalizedStringKey(AccessAutoRecoveryCopy.explanation))
                .font(.footnote).foregroundStyle(.secondary)
            Text(LocalizedStringKey(AccessAutoRecoveryCopy.scopeWarning))
                .font(.footnote).foregroundStyle(.secondary)
            if let tab, account.supportsAccountAuthorization {
                let paidHost = store.paidDownloadHost(for: tab)
                Toggle(
                    "通过账户连接下载本站文件",
                    isOn: Binding(
                        get: { paidHost.map { account.hasSelectedProxy(for: tab.id, host: $0.rawValue) } ?? false },
                        set: { enabled in
                            guard let paidHost else { return }
                            if enabled && tab.profile.isPrivate {
                                authorizationHost = paidHost
                                authorizing = true
                            } else {
                                store.setProxyEnabled(enabled, for: tab, account: account, displayedHost: paidHost)
                            }
                        }
                    )
                )
                .disabled(paidHost == nil)
                .accessibilityIdentifier("access-account-site-download-toggle")
                .sheet(isPresented: $authorizing, onDismiss: { accountID = ""; sessionToken = ""; authorizationHost = nil }) {
                    NavigationStack {
                        Form {
                            TextField("服务账户", text: $accountID).textInputAutocapitalization(.never).autocorrectionDisabled()
                            SecureField("服务账户会话令牌", text: $sessionToken)
                                .textInputAutocapitalization(.never).autocorrectionDisabled().privacySensitive()
                        }
                        .localizedNavigationTitle("授权本次私密会话")
                        .toolbar {
                            ToolbarItem(placement: .cancellationAction) { Button("取消") { authorizing = false } }
                            ToolbarItem(placement: .confirmationAction) {
                                Button("确认") {
                                    if let authorizationHost {
                                        store.setProxyEnabled(true, for: tab, account: account, displayedHost: authorizationHost, privateAccount: accountID, privateToken: sessionToken)
                                    }
                                    authorizing = false
                                }
                                    .disabled(accountID.isEmpty || sessionToken.isEmpty)
                            }
                        }
                    }
                }
            }
            if let line = snap.autoRecoveryLine {
                Text(LocalizedStringKey(line))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("access-site-auto-recovery-status")
            }
            if !snap.debugMemberHosts.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    Text("组成员（只读）")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    ForEach(snap.debugMemberHosts, id: \.self) { member in
                        Text(verbatim: AccessSiteDisplayName.title(member))
                            .font(.footnote.monospaced())
                            .textSelection(.enabled)
                    }
                }
                .accessibilityIdentifier("access-site-debug-members")
            }
            if snap.showsDebugSiteGroupDetails {
                Button(snap.probeBusy ? "正在探测…" : "探测候选（建议）") {
                    guard let tab else { return }
                    _ = store.runAutoRecoveryProbe(for: tab)
                }
                .disabled(tab == nil || snap.probeBusy || !snap.autoRecoveryEnabled)
                .accessibilityIdentifier("access-site-probe-button")
                if !snap.probeSuggestionLines.isEmpty {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(snap.probeSuggestionLines, id: \.self) { line in
                            Text(line)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .accessibilityIdentifier("access-site-probe-suggestions")
                }
            }
        }
        .disabled(tab.map { store.hostRouting?.canManageRecovery(for: $0, displayedHost: snap.host) != true } ?? true)
    }
}
