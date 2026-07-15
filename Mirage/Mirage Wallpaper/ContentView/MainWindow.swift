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
        super.init(window: NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1000, height: 640),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false))
        self.window.delegate = self
        self.window.isReleasedWhenClosed = false
        self.window.title = "Mirage \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0")"
        self.window.titlebarAppearsTransparent = true
        self.window.setFrameAutosaveName("MainWindow")
        self.window.isMovableByWindowBackground = true
        self.window.contentMinSize = NSSize(width: 1000, height: 640)
        let hostingView = NSHostingView(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel
            ).environmentObject(AppDelegate.shared.globalSettingsViewModel)
        )
        hostingView.sizingOptions = []
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
        DispatchQueue.main.async {
            withAnimation {
                AppDelegate.shared.contentViewModel.isStaging = true
            }
        }
    }
}
