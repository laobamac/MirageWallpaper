//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

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
            contentRect: NSRect(x: 0, y: 0, width: 1000, height: 640),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        win.isRestorable = false
        super.init(window: win)
        self.window.delegate = self
        self.window.isReleasedWhenClosed = false
        refreshLocalizedTitle()
        self.window.titlebarAppearsTransparent = true
        self.window.setFrameAutosaveName("MainWindow")
        self.window.isMovableByWindowBackground = true
        self.window.contentMinSize = NSSize(width: 1000, height: 640)

        // 使用 NSHostingController 作为 contentViewController，而非将
        // NSHostingView 直接设为 contentView。
        //
        // 根因: NSHostingView 直接作为窗口 contentView 时，其在每次 SwiftUI
        // graph 变更时对 setNeedsUpdateConstraints 的调用会直接进入窗口的
        // display-cycle 约束更新 pass。当窗口内含 HSplitView + 多个相互
        // 依赖的 @ObservedObject（本项目在引入第 4 个 rmskinViewModel 后
        // 依赖图更易抖动）时，会在初始 display cycle 形成
        // updateConstraints ↔ setNeedsUpdateConstraints 无限循环
        // (表现为窗口高度 668↔669 1px 振荡)。
        //
        // NSHostingController 由 AppKit 托管其 hosting view 的生命周期与
        // 约束集成，走独立的布局路径，可规避该循环。
        let hostingController = NSHostingController(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel
            ).environmentObject(AppDelegate.shared.globalSettingsViewModel)
        )
        hostingController.sizingOptions = []
        self.window.contentViewController = hostingController
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    override func windowDidLoad() {
        super.windowDidLoad()
    }
    
    func windowWillClose(_ notification: Notification) {
        AppDelegate.shared.contentViewModel.isStaging = false
        // The settings panel is now a sheet hosted by this window, so it can no
        // longer be visible on its own once the main window closes.
        DispatchQueue.main.async {
            NSApp.setActivationPolicy(.accessory)
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

    func refreshLocalizedTitle() {
        // The brand name "Mirage" is never localized. Build the title from a
        // literal so it stays "Mirage" in every language even if the string
        // generator re-harvests the "Mirage" literal into the localization table.
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0"
        window?.title = "Mirage \(version)"
    }
}
