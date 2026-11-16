import BrowserKit
import SwiftUI
import WebKit

/// 按当前窗口分派快捷键，避免只在菜单展开后才能使用。
struct BrowserKeyboardActions {
    let newTab: () -> Void
    let closeTab: () -> Void
    let editAddress: () -> Void
    let find: () -> Void
    let reload: () -> Void
    let back: () -> Void
    let forward: () -> Void
    var canCloseTab = true
    var canUsePageCommands = true

    @MainActor static func forWindow(_ browser: BrowserSession, presentation: BrowserPresentationCoordinator,
                                     editAddress: @escaping () -> Void = {}) -> Self? {
        guard !browser.isWindowClosed, !presentation.blocksBrowserCommands else { return nil }
        let tab = browser.activeTab
        func allowed(_ action: () -> Void) {
            guard !browser.isWindowClosed, !presentation.blocksBrowserCommands else { return }
            action()
        }
        func onCurrentTab(_ action: (BrowserTab) -> Void) {
            allowed {
                guard let tab, browser.activeTab === tab, !tab.isDisposed else { return }
                action(tab)
            }
        }
        return Self(newTab: { allowed { _ = browser.newTab() } },
                    closeTab: { onCurrentTab { browser.close($0.id) } },
                    editAddress: { onCurrentTab { _ in editAddress() } },
                    find: { onCurrentTab { presentation.request(.find($0.id, $0.instanceID)) } },
                    reload: { onCurrentTab { $0.reload() } }, back: { onCurrentTab { $0.goBack() } }, forward: { onCurrentTab { $0.goForward() } },
                    canCloseTab: tab != nil, canUsePageCommands: tab != nil)
    }
}

/// A document viewer owns this responder for its entire presentation. Explicitly
/// consume Cmd-W before WebKit or iPadOS can treat it as Close Window.
@MainActor
final class BrowserPresentationWebView: WKWebView {
    override var keyCommands: [UIKeyCommand]? {
        let close = UIKeyCommand(input: "w", modifierFlags: .command, action: #selector(keepPresentationOpen))
        close.wantsPriorityOverSystemBehavior = true
        return [close] + (super.keyCommands ?? [])
    }
    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(keepPresentationOpen) { return true }
        return super.canPerformAction(action, withSender: sender)
    }
    @objc private func keepPresentationOpen() { }
}

private struct BrowserProtectsWindowKey: FocusedValueKey { typealias Value = Bool }

private struct BrowserKeyboardActionsKey: FocusedValueKey {
    typealias Value = BrowserKeyboardActions
}

extension FocusedValues {
    var browserProtectsWindow: Bool? {
        get { self[BrowserProtectsWindowKey.self] }
        set { self[BrowserProtectsWindowKey.self] = newValue }
    }
    var browserKeyboardActions: BrowserKeyboardActions? {
        get { self[BrowserKeyboardActionsKey.self] }
        set { self[BrowserKeyboardActionsKey.self] = newValue }
    }
}

struct BrowserKeyboardCommands: Commands {
    @ObservedObject var language: AppLanguageSettings
    @FocusedValue(\.browserKeyboardActions) private var actions
    @FocusedValue(\.browserProtectsWindow) private var protectsWindow
    var body: some Commands {
        let _ = language.revision
        CommandGroup(replacing: .newItem) {
            Button(AegisLocalization.string("新建")) { actions?.newTab() }.keyboardShortcut("t").disabled(actions == nil)
            Button(AegisLocalization.string("关闭当前标签")) { actions?.closeTab() }.keyboardShortcut("w").disabled(actions?.canCloseTab != true && protectsWindow != true)
        }
        CommandGroup(after: .textEditing) {
            Button(AegisLocalization.string("在页面中查找")) {
                actions?.find()
            }.keyboardShortcut("f").disabled(actions?.canUsePageCommands != true)
            Button(AegisLocalization.string("编辑网址")) { actions?.editAddress() }.keyboardShortcut("l").disabled(actions?.canUsePageCommands != true)
            Button(AegisLocalization.string("刷新")) { actions?.reload() }.keyboardShortcut("r").disabled(actions?.canUsePageCommands != true)
            Button(AegisLocalization.string("后退")) { actions?.back() }.keyboardShortcut("[").disabled(actions?.canUsePageCommands != true)
            Button(AegisLocalization.string("前进")) { actions?.forward() }.keyboardShortcut("]").disabled(actions?.canUsePageCommands != true)
        }
    }
}
