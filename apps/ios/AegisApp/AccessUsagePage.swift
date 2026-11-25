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
                            Button {
                                alphaNodes.select(entry.id)
                            } label: {
                                HStack {
                                    Text(verbatim: entry.protocolName)
                                    Spacer()
                                    if alphaNodes.selectedIndex == entry.id {
                                        Image(systemName: "checkmark").accessibilityHidden(true)
                                    }
                                }
                                .contentShape(Rectangle())
                            }
                            .accessibilityValue(alphaNodes.selectedIndex == entry.id ? Text("已启用") : Text("未选择"))
                            .accessibilityIdentifier("access-alpha-node-\(entry.id)")
                        }
                    }
                }
                AccessSubscriptionUsageView(session: subscription, usesAlphaNodeSelection: alphaNodes.isConfigured)
            }
        }
        .localizedNavigationTitle("隐藏地理位置服务")
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("access-usage-page")
    }
}
