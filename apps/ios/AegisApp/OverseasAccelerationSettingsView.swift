import AccessKit
import BrowserKit
import SwiftUI

extension AccessGeoSiteCache.Region {
    var localizedTitle: LocalizedStringKey {
        switch self {
        case .mainland: "中国大陆"
        case .outsideMainland: "中国大陆以外"
        }
    }
}

private struct AccessRegionChoices: View {
    let selected: AccessGeoSiteCache.Region?
    let choose: (AccessGeoSiteCache.Region) -> Void

    var body: some View {
        ForEach(AccessGeoSiteCache.Region.allCases, id: \.self) { region in
            Button {
                choose(region)
            } label: {
                HStack {
                    Text(region.localizedTitle)
                    Spacer()
                    if selected == region { Image(systemName: "checkmark").accessibilityHidden(true) }
                }
                .contentShape(Rectangle())
            }
            .accessibilityAddTraits(selected == region ? .isSelected : [])
            .accessibilityIdentifier("access-region-" + region.rawValue)
        }
    }
}

struct AccessRegionSettingsView: View {
    @EnvironmentObject private var browser: BrowserSession
    @EnvironmentObject private var hostStores: AccessHostStoreCoordinator
    @ObservedObject var tab: BrowserTab
    @State private var saveFailed = false

    var body: some View {
        Form {
            if browser.activeTab === tab, let selection = hostStores.websiteSelection {
                Section {
                    AccessRegionChoices(selected: selection.region) { region in
                        do {
                            try hostStores.chooseRegion(region, for: tab)
                            saveFailed = false
                        } catch { saveFailed = true }
                    }
                } footer: {
                    Text("港澳台请选择中国大陆以外。")
                }
            }
            if saveFailed { Text("设置未保存，请重试。") }
        }
        .localizedNavigationTitle("当前上网地区")
    }
}

struct RegionalAccelerationSettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @EnvironmentObject private var browser: BrowserSession
    @EnvironmentObject private var hostStores: AccessHostStoreCoordinator
    @ObservedObject var tab: BrowserTab
    var openedFromRegion = false
    @State private var region: AccessGeoSiteCache.Region?
    @State private var scope: AccessOverseasSelection.Mode = .all
    @State private var overseasEnabled = true
    @State private var mainlandEnabled = true
    @State private var overseas: Set<AccessOverseasCategory> = []
    @State private var mainland: Set<AccessGeoSiteCategory> = []
    @State private var confirmation: AccessHostStoreCoordinator.RegionalConfirmation?
    @State private var operation: Task<Void, Never>?
    @State private var message: LocalizedStringKey?

    private var canEdit: Bool {
        browser.activeTab === tab && hostStores.offersWebsiteAcceleration(for: tab)
    }

    var body: some View {
        Form {
            if canEdit, hostStores.websiteSelection != nil {
                Section {
                    AccessRegionChoices(selected: region) { region = $0 }
                } header: {
                    Text("当前上网地区")
                } footer: {
                    Text("港澳台请选择中国大陆以外。")
                }
                Section {
                    scopeChoice(.all, title: "全量")
                    scopeChoice(.categories, title: "按分类")
                } header: {
                    Text("加速范围")
                } footer: {
                    Text("跨地区命中规则时加速，同地区优先使用原有连接。")
                }
                overseasSection
                mainlandSection
                if hostStores.websiteSelection?.regionalCategoryDraft.contains(.debian) == true {
                    Section {
                        Toggle("Debian", isOn: Binding(get: { mainland.contains(.debian) },
                                                      set: { if $0 { mainland.insert(.debian) } else { mainland.remove(.debian) } }))
                    }
                }
                Section {
                    Button("检查并应用", action: prepare)
                        .disabled(region == nil || operation != nil || scenePhase != .active)
                        .accessibilityIdentifier("regional-review-choice")
                    if let message { Text(message).foregroundStyle(.secondary) }
                }
            }
        }
        .localizedNavigationTitle(openedFromRegion ? "当前上网地区" : "自动")
        .onChange(of: hostStores.websiteSelection, initial: true) { _, selection in populate(selection) }
        .onChange(of: hostStores.overseasSettings.selection) { _, _ in populate(hostStores.websiteSelection) }
        .onChange(of: browser.tabSelectionRevision) { _, _ in invalidateConfirmation() }
        .onChange(of: scenePhase) { _, phase in if phase != .active { invalidateConfirmation() } }
        .onDisappear(perform: invalidateConfirmation)
        .sheet(item: $confirmation, onDismiss: { operation?.cancel() }) { confirmation in
            confirmationDetails(confirmation)
        }
    }

    private func scopeChoice(_ choice: AccessOverseasSelection.Mode, title: LocalizedStringKey) -> some View {
        Button {
            scope = choice
        } label: {
            HStack {
                Text(title)
                Spacer()
                if scope == choice { Image(systemName: "checkmark").accessibilityHidden(true) }
            }
            .contentShape(Rectangle())
        }
        .accessibilityAddTraits(scope == choice ? .isSelected : [])
        .accessibilityIdentifier("regional-scope-" + choice.rawValue)
    }

    private var overseasSection: some View {
        Section {
            Toggle("大陆以外网站", isOn: $overseasEnabled)
                .accessibilityIdentifier("regional-overseas-enabled")
            if scope == .categories && overseasEnabled {
                ForEach(AccessOverseasCategory.allCases, id: \.self) { category in
                    Toggle(category.localizedTitle, isOn: Binding(
                        get: { overseas.contains(category) },
                        set: { if $0 { overseas.insert(category) } else { overseas.remove(category) } }
                    ))
                    .accessibilityIdentifier("overseas-category-" + category.rawValue)
                }
            }
            if hostStores.overseasSettings.remotelyPaused { Text("海外加速已暂停。") }
            if hostStores.overseasSettings.awaitingExpansion { Text("等待确认") }
            if let preview = hostStores.overseasPreview {
                NavigationLink("分类范围") {
                    OverseasScopeDetails(version: preview.release, policy: preview.fullPolicy, bundled: preview.bundled,
                                         counts: preview.catalogue.counts, projections: preview.catalogue.hosts,
                                         unused: preview.catalogue.unusedRules)
                        .localizedNavigationTitle("分类范围")
                }
            }
            if hostStores.canFetchOverseasPreview {
                if hostStores.overseasSettings.previewRequested {
                    Button("取消") { hostStores.cancelOverseasPreview(for: tab) }
                    Text(hostStores.overseasSettings.refreshFailed ? "下载失败" : "正在更新规则…")
                } else {
                    Button("获取官方规则") {
                        do { try hostStores.requestOverseasPreview(for: tab) }
                        catch { message = "连接暂不可用" }
                    }
                }
            }
        }
    }

    private var mainlandSection: some View {
        Section {
            Toggle("中国大陆网站", isOn: $mainlandEnabled)
                .accessibilityIdentifier("regional-mainland-enabled")
            if scope == .categories && mainlandEnabled {
                ForEach(AccessGeoSiteCategory.allCases.filter { AccessGeoSiteCache.Selection.mainlandCategories.contains($0) }, id: \.self) { category in
                    Toggle(String(localized: category.displayName), isOn: Binding(
                        get: { mainland.contains(category) },
                        set: { if $0 { mainland.insert(category) } else { mainland.remove(category) } }
                    ))
                    .accessibilityIdentifier("website-acceleration-category-" + category.rawValue)
                }
            }
            NavigationLink("分类范围") { WebsiteCategoriesView(tab: tab) }
                .accessibilityIdentifier("website-acceleration-categories")
        }
    }

    private func populate(_ selection: AccessGeoSiteCache.Selection?) {
        guard let selection else { return }
        invalidateConfirmation()
        let state = hostStores.overseasSettings
        region = selection.region
        scope = selection.automaticScope
            ?? (state.hasSavedSelection ? state.selection.mode : (selection.mode == .smart ? .categories : .all))
        overseasEnabled = !state.hasSavedSelection || state.selection.enabled
        overseas = state.hasSavedSelection ? Set(state.selection.categories) : Set(AccessOverseasCategory.allCases)
        mainlandEnabled = selection.mainlandEnabled ?? true
        mainland = selection.regionalCategoryDraft
    }

    private func prepare() {
        guard let region, canEdit, operation == nil, scenePhase == .active else { return }
        let website = AccessGeoSiteCache.Selection(mode: .smart, categories: mainland, region: region,
                                                   automaticScope: scope, mainlandEnabled: mainlandEnabled)
        let choice = AccessOverseasSelection(enabled: overseasEnabled, mode: scope, categories: overseas)
        operation = Task { @MainActor in
            defer { operation = nil }
            do {
                let result = try await hostStores.regionalConfirmation(website, overseas: choice, for: browser)
                try Task.checkCancellation()
                guard self.region == region, scope == choice.mode, overseasEnabled == choice.enabled,
                      overseas == Set(choice.categories), mainland == website.categories, mainlandEnabled == website.mainlandEnabled
                else { return }
                confirmation = result
                message = nil
            } catch is CancellationError {
                return
            } catch AccessGeoSiteCache.Failure.expansionTooLarge {
                message = "新增范围过大，无法批准。"
            } catch {
                message = "连接暂不可用"
            }
        }
    }

    private func save(_ offer: AccessHostStoreCoordinator.RegionalConfirmation) {
        guard operation == nil, canEdit, scenePhase == .active else { return }
        operation = Task { @MainActor in
            defer { operation = nil }
            do {
                try await hostStores.confirmRegional(offer, for: browser) {
                    canEdit && scenePhase == .active && confirmation?.id == offer.id
                }
                confirmation = nil
                dismiss()
            } catch is CancellationError {
                return
            } catch {
                confirmation = nil
                message = "设置未保存，请重试。"
            }
        }
    }

    private func invalidateConfirmation() {
        operation?.cancel()
        confirmation = nil
    }

    private func confirmationDetails(_ confirmation: AccessHostStoreCoordinator.RegionalConfirmation) -> some View {
        NavigationStack {
            Form {
                Section {
                    if let region = confirmation.website.region {
                        LabeledContent("当前上网地区") { Text(region.localizedTitle) }
                    }
                    LabeledContent("加速范围") { Text(confirmation.overseas.mode == .all ? "全量" : "按分类") }
                }
                Section("大陆以外网站") {
                    if confirmation.overseas.enabled {
                        if let version = confirmation.overseasVersion {
                            LabeledContent("版本", value: version)
                        } else {
                            Text("暂不可用")
                        }
                        if confirmation.overseas.mode == .categories {
                            if confirmation.overseas.categories.isEmpty { Text("未选择") }
                            ForEach(confirmation.overseas.categories, id: \.self) { Text($0.localizedTitle) }
                        }
                        if let offer = confirmation.offer {
                            NavigationLink("分类范围") {
                                OverseasScopeDetails(version: offer.release, policy: offer.policy, bundled: offer.bundled,
                                                     addedRules: offer.newRules, removedExclusions: offer.removedExclusions)
                                    .localizedNavigationTitle("分类范围")
                            }
                        }
                    } else { Text("未启用") }
                }
                Section("中国大陆网站") {
                    if confirmation.website.mainlandEnabled != false {
                        if confirmation.website.selectedMainlandCategories.isEmpty { Text("未选择") }
                        ForEach(AccessGeoSiteCategory.allCases.filter { confirmation.website.selectedMainlandCategories.contains($0) }, id: \.self) { category in
                            LabeledContent {
                                if let version = confirmation.domesticVersions[category] { Text(verbatim: version) }
                                else { Text("暂不可用") }
                            } label: { Text(String(localized: category.displayName)) }
                        }
                    } else { Text("未启用") }
                }
            }
            .localizedNavigationTitle("确认启用")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("取消", action: invalidateConfirmation) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("确认") { save(confirmation) }
                        .disabled(operation != nil)
                        .accessibilityIdentifier("regional-confirm")
                }
            }
        }
    }
}

private struct OverseasScopeDetails: View {
    let version: String
    let policy: AccessOverseasCatalogue.Policy
    var bundled = false
    var counts: AccessOverseasCatalogue.Counts?
    var projections: [AccessOverseasCategory: Set<String>] = [:]
    var unused: [AccessOverseasCatalogue.UnusedRule] = []
    var addedRules: [AccessOverseasCatalogue.Record] = []
    var removedExclusions: [AccessOverseasCatalogue.Record] = []
    @State private var search = ""

    var body: some View {
        List {
            Section {
                LabeledContent("版本", value: version)
                if bundled { Text("内置规则") }
                if let counts {
                    LabeledContent("来源规则", value: counts.source.formatted())
                    LabeledContent("通过主机校验", value: counts.admitted.formatted())
                    LabeledContent("分类入口", value: projections.values.reduce(0) { $0 + $1.count }.formatted())
                }
                LabeledContent("范围", value: policy.rules.count.formatted())
                LabeledContent("例外规则", value: policy.exclusions.count.formatted())
            }
            if !addedRules.isEmpty { rows(addedRules, title: "新增范围") }
            if !removedExclusions.isEmpty { rows(removedExclusions, title: "取消的例外") }
            ForEach(AccessOverseasCategory.allCases, id: \.self) { category in
                let hosts = (projections[category] ?? []).sorted().filter(includes)
                if !hosts.isEmpty {
                    Section(category.localizedTitle) {
                        ForEach(hosts, id: \.self) { Text(verbatim: $0) }
                    }
                }
            }
            rows(policy.rules, title: "范围")
            rows(policy.exclusions, title: "例外规则")
            if !unused.isEmpty {
                let results = unused.filter { includes($0.value) }
                Section("未采用的规则") {
                    ForEach(Array(results.enumerated()), id: \.offset) { _, row in
                        LabeledContent {
                            Text(row.reason == .hostRejected ? "主机校验未通过" : "未支持的规则")
                        } label: { Text(verbatim: row.value) }
                    }
                }
            }
        }
        .searchable(text: $search, prompt: "搜索")
    }

    private func rows(_ rules: [AccessOverseasCatalogue.Record], title: LocalizedStringKey) -> some View {
        Section(title) {
            ForEach(rules.filter { includes($0.value) }, id: \.self) { rule in
                LabeledContent {
                    switch rule.kind {
                    case .domain: Text("包含子域名")
                    case .full: Text("仅匹配所列域名。")
                    case .keyword: Text("关键词")
                    case .regexp: Text("未支持的规则")
                    }
                } label: { Text(verbatim: rule.value) }
            }
        }
    }

    private func includes(_ value: String) -> Bool {
        search.isEmpty || value.localizedCaseInsensitiveContains(search)
    }
}

private extension AccessOverseasCategory {
    var localizedTitle: LocalizedStringKey {
        switch self {
        case .video: "视频与直播"
        case .ai: "AI 工具"
        case .web: "网页与搜索"
        case .social: "社交与通讯"
        case .news: "新闻资讯"
        case .development: "开发与代码"
        case .reference: "百科与学术"
        case .cloud: "办公与网盘"
        }
    }
}
