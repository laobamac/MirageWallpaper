//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

class MainWindowController: NSWindowController, NSWindowDelegate {
    private let minimumContentSize = NSSize(width: 1000, height: 640)

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
        refreshLocalizedTitle()
        self.window.titlebarAppearsTransparent = true
        self.window.setFrameAutosaveName("MainWindow")
        self.window.isMovableByWindowBackground = false
        let hostingView = NSHostingView(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel,
                navigationModel: AppDelegate.shared.navigationModel
            ).environmentObject(AppDelegate.shared.globalSettingsViewModel)
        )
        hostingView.sizingOptions = []
        self.window.contentView = hostingView
        enforceMinimumSize()
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    override func windowDidLoad() {
        super.windowDidLoad()
        enforceMinimumSize()
    }

    func windowWillResize(_ sender: NSWindow, to frameSize: NSSize) -> NSSize {
        let minimumFrameSize = sender.frameRect(
            forContentRect: NSRect(origin: .zero, size: minimumContentSize)
        ).size
        return NSSize(
            width: max(frameSize.width, minimumFrameSize.width),
            height: max(frameSize.height, minimumFrameSize.height)
        )
    }

    func windowDidEndLiveResize(_ notification: Notification) {
        enforceMinimumSize()
    }

    private func enforceMinimumSize() {
        guard let window else { return }
        window.contentMinSize = minimumContentSize
        window.minSize = window.frameRect(
            forContentRect: NSRect(origin: .zero, size: minimumContentSize)
        ).size

        let currentContentSize = window.contentRect(forFrameRect: window.frame).size
        let clampedSize = NSSize(
            width: max(currentContentSize.width, minimumContentSize.width),
            height: max(currentContentSize.height, minimumContentSize.height)
        )
        if currentContentSize != clampedSize {
            window.setContentSize(clampedSize)
        }
    }
    
    func windowWillClose(_ notification: Notification) {
        AppDelegate.shared.contentViewModel.isStaging = false
        AppDelegate.shared.enterMenuBarMode()
    }
    
    func windowDidResignKey(_ notification: Notification) { }

    func windowDidResignMain(_ notification: Notification) { }
    
    func windowDidBecomeKey(_ notification: Notification) {
        DispatchQueue.main.async {
            guard !AppDelegate.shared.contentViewModel.isStaging else { return }
            withAnimation {
                AppDelegate.shared.contentViewModel.isStaging = true
            }
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
