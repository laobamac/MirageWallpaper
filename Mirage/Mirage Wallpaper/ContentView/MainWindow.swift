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

    required init(rootView: Content) {
        super.init(rootView: rootView)
        translatesAutoresizingMaskIntoConstraints = true
    }

    @available(*, unavailable)
    required dynamic init?(coder: NSCoder) { fatalError() }

    // ===== 彻底切断约束循环 =====
    //
    // 堆栈分析显示循环路径:
    //   NSWindow display cycle → updateConstraintsForSubtreeIfNeeded
    //   → NSHostingView.updateConstraints() [frame 15-16]
    //   → 内部 defer 闭包调用子视图 updateConstraints [frame 14]
    //   → _prepareForTwoPassConstraintsUpdateIfNeeded [frame 13]
    //   → setNeedsUpdateConstraints: [frame 12]
    //   → _informContainerThatSubviewsNeedUpdateConstraints 向上传播
    //   → 窗口再次标记需要约束更新 → 循环
    //
    // 解决: 完全不调用 super.updateConstraints()。
    // 我们使用 autoresizingMask 管理布局，不需要 Auto Layout 约束。
    // NSHostingView 会通过自身的 layout()/draw() 正确渲染 SwiftUI 内容，
    // 约束系统并非其渲染的必要条件。

    // 彻底跳过 NSHostingView 的 updateConstraints 实现。
    // NSHostingView.updateConstraints() 内部会触发子视图约束更新，
    // 子视图再次标记 setNeedsUpdateConstraints，形成无限循环。
    // 我们跳过它，只保留 NSView 的基础实现来满足 AppKit 协议。
    override func updateConstraints() {
        // 跳过 NSHostingView 的实现，直接调用 NSView 的版本
        // NSView.updateConstraints() 只是一个空壳/标记已更新
        let sel = #selector(NSView.updateConstraints)
        if let imp = NSView.instanceMethod(for: sel) {
            typealias Fn = @convention(c) (AnyObject, Selector) -> Void
            let fn = unsafeBitCast(imp, to: Fn.self)
            fn(self, sel)
        }
    }

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
