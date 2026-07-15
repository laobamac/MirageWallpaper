//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

// MARK: - 防约束循环的 NSHostingView 子类
// SwiftUI 的 NSHostingView 在 hitTest 期间会 flush graph transactions，
// 触发 setNeedsUpdateConstraints，与窗口的 display cycle 产生无限循环。
// 通过节流 updateConstraints 调用频率来打断循环。
class StableHostingView<Content: View>: NSHostingView<Content> {
    private var isUpdatingConstraints = false

    override func updateConstraints() {
        guard !isUpdatingConstraints else { return }
        isUpdatingConstraints = true
        super.updateConstraints()
        isUpdatingConstraints = false
    }

    // 在 display cycle 期间抑制约束更新请求向上传播
    override var needsUpdateConstraints: Bool {
        get { super.needsUpdateConstraints }
        set {
            guard !isUpdatingConstraints else { return }
            super.needsUpdateConstraints = newValue
        }
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

        let hostingView = StableHostingView(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel
            ).environmentObject(AppDelegate.shared.globalSettingsViewModel)
        )
        self.window.contentView = hostingView
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
