//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

// MARK: - 防约束循环的 NSHostingView 子类
//
// 根本原因分析:
// SwiftUI 的 NSHostingView 在 hitTest 期间会执行 flushTransactions →
// graphDidChange → requestUpdate → setNeedsUpdateConstraints。当此时窗口
// 正处于 display cycle 中时，会导致无限约束更新循环。
//
// 这个问题在 commit 80926f5 引入 RmskinViewModel 后被放大：
// RmskinViewModel.init() 异步加载数据后在主线程更新 @Published 属性，
// 产生额外的 graph transactions，增加了 hitTest 期间 flush 的概率。
//
// 解决方案: 完全禁用 NSHostingView 的 Auto Layout 约束参与，
// 改用 autoresizingMask 驱动尺寸，这样 setNeedsUpdateConstraints
// 即使被调用也不会向窗口传播约束更新请求。
class StableHostingView<Content: View>: NSHostingView<Content> {

    override init(rootView: Content) {
        super.init(rootView: rootView)
        // 关键: 使用 autoresizing 而非 Auto Layout
        // 这样 NSHostingView 内部的 setNeedsUpdateConstraints 不会
        // 触发窗口级别的约束更新 pass
        translatesAutoresizingMaskIntoConstraints = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    // 拦截任何试图切换回 Auto Layout 的行为
    override var translatesAutoresizingMaskIntoConstraints: Bool {
        get { true }
        set { /* 强制保持 true，忽略外部设置 */ }
    }

    // 覆盖 intrinsicContentSize 返回 noIntrinsicMetric
    // 防止 Auto Layout 系统根据 SwiftUI 内容尺寸创建约束
    override var intrinsicContentSize: NSSize {
        NSSize(width: NSView.noIntrinsicMetric, height: NSView.noIntrinsicMetric)
    }
}

class MainWindowController: NSWindowController, NSWindowDelegate {
    override var window: NSWindow! {
        get {
            return super.window
        }
        set {
            super.window = newValue
        }
    }
    
    override init(window: NSWindow?) {
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1029, height: 669),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        win.isRestorable = false
        super.init(window: win)
        self.window.delegate = self
        self.window.isReleasedWhenClosed = false
        self.window.title = "Mirage \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0")"
        self.window.titlebarAppearsTransparent = true
        self.window.setFrameAutosaveName("MainWindow")
        self.window.isMovableByWindowBackground = true
        self.window.contentMinSize = NSSize(width: 1000, height: 640)

        // 使用普通 NSView 作为 contentView，NSHostingView 作为子视图填充。
        // 这确保 NSHostingView 的约束更新不会直接传播到窗口的 contentView 层级。
        let container = NSView(frame: NSRect(origin: .zero, size: self.window.contentLayoutRect.size))
        container.autoresizesSubviews = true
        container.wantsLayer = true

        let hostingView = StableHostingView(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel
            ).environmentObject(AppDelegate.shared.globalSettingsViewModel)
        )
        hostingView.frame = container.bounds
        hostingView.autoresizingMask = [.width, .height]
        container.addSubview(hostingView)

        self.window.contentView = container
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    override func windowDidLoad() {
        super.windowDidLoad()
    }
    
    func windowWillClose(_ notification: Notification) {
        AppDelegate.shared.contentViewModel.isStaging = false
        if !AppDelegate.shared.settingsWindow.isVisible {
            DispatchQueue.main.async {
                NSApp.setActivationPolicy(.accessory)
            }
        }
    }
    
    func windowDidResignKey(_ notification: Notification) { }

    func windowDidResignMain(_ notification: Notification) { }
    
    func windowDidBecomeKey(_ notification: Notification) {
        // 延迟到下一个 runloop 周期，避免在 display cycle 中触发约束更新循环
        DispatchQueue.main.async {
            AppDelegate.shared.contentViewModel.isStaging = true
        }
    }
}
