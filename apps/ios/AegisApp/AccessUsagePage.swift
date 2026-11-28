import SwiftUI

/// Current product usage comes from the same 3X-UI subscription as webpage routing.
struct AccessUsagePage: View {
    let isPrivate: Bool
    @EnvironmentObject private var subscription: AccessSubscriptionUsageSession
    @ObservedObject private var alphaNodes: AccessAlphaNodeCatalogue

    init(isPrivate: Bool, alphaNodes: AccessAlphaNodeCatalogue = .shared) {
        self.isPrivate = isPrivate
        self.alphaNodes = alphaNodes
    }

    var body: some View {
        Form {
            if isPrivate {
                Section {
                    Label("私密会话不显示普通账户的额度；请在普通窗口查询。", systemImage: "lock")
                        .accessibilityIdentifier("access-usage-private")
                }
            } else {
                if alphaNodes.isConfigured {
                    Section("可用连接") {
                        if alphaNodes.entries.isEmpty {
                            Text("暂不可用")
                        }
                        ForEach(alphaNodes.entries) { entry in
                            let available = alphaNodes.canSelect(entry, subscription: subscription)
                            let selected = alphaNodes.selectedIndex == entry.id
                            Button {
                                guard alphaNodes.canSelect(entry, subscription: subscription) else { return }
                                alphaNodes.select(entry.id)
                            } label: {
                                HStack {
                                    VStack(alignment: .leading) {
                                        Text(verbatim: entry.displayName)
                                        if let source = entry.source {
                                            switch source {
                                            case .panel: Text(verbatim: "3X-UI").font(.footnote).foregroundStyle(.secondary)
                                            case .downloads: Text("下载").font(.footnote).foregroundStyle(.secondary)
                                            }
                                        }
                                    }
                                    Spacer()
                                    if !available {
                                        Text("暂不可用")
                                    }
                                    if selected {
                                        Image(systemName: "checkmark").accessibilityHidden(true)
                                    }
                                }
                                .contentShape(Rectangle())
                            }
                            .disabled(!available)
                            .accessibilityValue(available ? (selected ? Text("已启用") : Text("未选择")) : Text("暂不可用"))
                            .accessibilityAddTraits(selected ? .isSelected : [])
                            .accessibilityIdentifier("access-alpha-node-\(entry.id)")
                        }
                    }
                }
                if alphaNodes.isPrivateBeta, alphaNodes.selectedEntry?.usesSubscription != true {
                    Section("流量管理") {
                        AccessUnavailableUsage()
                    }
                } else {
                    AccessSubscriptionUsageView(session: subscription, usesAlphaNodeSelection: alphaNodes.isConfigured,
                                                showsUnavailableAmount: alphaNodes.isPrivateBeta)
                }
            }
        }
        .localizedNavigationTitle("流量管理")
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("access-usage-page")
    }

}
