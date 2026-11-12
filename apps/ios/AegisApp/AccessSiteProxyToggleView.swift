import AccessKit
import BrowserKit
import SwiftUI

/// Current-page 「本站代理」 toggle: host + switch + intent/connection lines.
///
/// Switch state ≠ connectivity. Closing copy states the browser's original
/// connection is used — never “direct success” without verification.
struct AccessSiteProxyToggleView: View {
    @ObservedObject var store: AccessSiteProxySessionStore
    @ObservedObject var tab: BrowserTab
    @EnvironmentObject private var account: AccessAccountSession
    @State private var authorizing = false
    @State private var accountID = ""
    @State private var sessionToken = ""
    private var url: URL? { tab.url }

    private func select(_ enabled: Bool) {
        store.setProxyEnabled(enabled, for: tab, account: account, privateAccount: accountID, privateToken: sessionToken)
    }

    @Environment(\.locale) private var localizationLocale

    private var snapshot: AccessSiteProxyToggleSnapshot {
        store.snapshot(for: url)
    }

    var body: some View {
        let _ = localizationLocale
        let snap = snapshot
        Section {
            if let host = snap.host {
                Text(host.rawValue)
                    .font(.headline)
                    .textSelection(.enabled)
                    .accessibilityIdentifier("access-site-proxy-host")
            } else {
                Label("当前页不是可代理的网站", systemImage: "globe")
                    .accessibilityIdentifier("access-site-proxy-no-host")
            }

            Toggle(
                "使用本服务代理",
                isOn: Binding(
                    get: { snap.host.map { account.hasSelectedProxy(for: tab.id, host: $0.rawValue) } ?? false },
                    set: { enabled in
                        if enabled && tab.profile.isPrivate && account.supportsAccountAuthorization { authorizing = true } else { select(enabled) }
                    }
                )
            )
            .disabled(!snap.toggleEnabled)
            .accessibilityIdentifier("access-site-proxy-toggle")
            .sheet(isPresented: $authorizing, onDismiss: { accountID = ""; sessionToken = "" }) {
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
                            Button("确认") { select(true); authorizing = false }
                                .disabled(accountID.isEmpty || sessionToken.isEmpty)
                        }
                    }
                }
            }

            VStack(alignment: .leading, spacing: 4) {
                Text(AccessSiteProxyToggleModel.intentLine(for: snap.intentStatus))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("access-site-proxy-intent")
                Text(AccessSiteProxyToggleModel.connectionLine(for: snap.connectionStatus))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("access-site-proxy-connection")
                if let reason = snap.failureReason, !reason.isEmpty {
                    Text(reason)
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .accessibilityIdentifier("access-site-proxy-failure")
                }
            }
        } header: {
            Text("本站代理")
        } footer: {
            Text("开关表示本站是否使用本服务代理；连接状态单独显示，开启不等于已连通。关闭后使用浏览器原有连接。")
                .font(.footnote)
        }
    }
}
