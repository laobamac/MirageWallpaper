//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

class MainWindowController: NSWindowController, NSWindowDelegate {
    private let minimumContentSize = NSSize(width: 1300, height: 640)

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
            contentRect: NSRect(x: 0, y: 0, width: 1100, height: 640),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered, defer: false))
        self.window.delegate = self
        self.window.isReleasedWhenClosed = false
        refreshLocalizedTitle()
        self.window.titlebarAppearsTransparent = true
        self.window.titlebarSeparatorStyle = .none
        self.window.setFrameAutosaveName("MainWindow")
        self.window.isMovableByWindowBackground = false
        let hostingView = NSHostingView(rootView: ContentView(
                viewModel: AppDelegate.shared.contentViewModel,
                wallpaperViewModel: AppDelegate.shared.wallpaperViewModel,
                navigationModel: AppDelegate.shared.navigationModel
            ).environment(AppDelegate.shared.globalSettingsViewModel)
        )
        hostingView.sizingOptions = []
        hostingView.safeAreaRegions = []
        self.window.contentView = MainWindowBackgroundView(
            hostedView: hostingView,
            blurEnabled: AppDelegate.shared.globalSettingsViewModel.settings.shouldBlurMainWindow
        )
        enforceMinimumSize()
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func setBackgroundBlurEnabled(_ enabled: Bool) {
        (window?.contentView as? MainWindowBackgroundView)?.setBlurEnabled(enabled)
    }

    func prepareForPresentation() {
        AppDelegate.shared.contentViewModel.isStaging = true
    }

    func synchronizeVisibility(refreshDisplay: Bool = false) {
        let viewModel = AppDelegate.shared.contentViewModel
        let wasVisible = viewModel.isWindowVisible
        let visible = viewModel.isStaging && !NSApp.isHidden && window?.isVisible == true &&
            window?.isMiniaturized == false && window?.occlusionState.contains(.visible) == true
        viewModel.isWindowVisible = visible

        if visible && (!wasVisible || refreshDisplay), let contentView = window?.contentView {
            contentView.needsLayout = true
            invalidateDisplay(in: contentView)
        }
    }

    private func invalidateDisplay(in view: NSView) {
        view.needsDisplay = true
        for subview in view.subviews {
            invalidateDisplay(in: subview)
        }
    }
    
    override func windowDidLoad() {
        super.windowDidLoad()
        enforceMinimumSize()
    }

    func windowWillResize(_ sender: NSWindow, to frameSize: NSSize) -> NSSize {
        let minimumFrameSize = minimumFrameSize(for: sender)
        return NSSize(
            width: max(frameSize.width, minimumFrameSize.width),
            height: max(frameSize.height, minimumFrameSize.height)
        )
    }

    func windowDidEndLiveResize(_ notification: Notification) {
        enforceMinimumSize()
    }

    func windowDidEnterFullScreen(_ notification: Notification) {
        enforceMinimumSize()
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        enforceMinimumSize()
    }

    private func minimumFrameSize(for window: NSWindow) -> NSSize {
        let layoutSize = window.contentLayoutRect.size
        return NSSize(
            width: minimumContentSize.width + max(0, window.frame.width - layoutSize.width),
            height: minimumContentSize.height + max(0, window.frame.height - layoutSize.height)
        )
    }

    private func enforceMinimumSize() {
        guard let window else { return }
        let minimumFrameSize = minimumFrameSize(for: window)
        let minimumWindowContentSize = window.contentRect(
            forFrameRect: NSRect(origin: .zero, size: minimumFrameSize)
        ).size
        window.contentMinSize = minimumWindowContentSize
        window.minSize = minimumFrameSize

        let currentContentSize = window.contentRect(forFrameRect: window.frame).size
        let clampedSize = NSSize(
            width: max(currentContentSize.width, minimumWindowContentSize.width),
            height: max(currentContentSize.height, minimumWindowContentSize.height)
        )
        if currentContentSize != clampedSize {
            window.setContentSize(clampedSize)
        }
    }
    
    func windowWillClose(_ notification: Notification) {
        AppDelegate.shared.contentViewModel.isWindowVisible = false
        AppDelegate.shared.contentViewModel.isStaging = false
        AppDelegate.shared.enterMenuBarMode()
    }
    
    func windowDidResignKey(_ notification: Notification) { }

    func windowDidResignMain(_ notification: Notification) { }
    
    func windowDidBecomeKey(_ notification: Notification) {
        synchronizeVisibility(refreshDisplay: true)
    }

    func windowDidChangeOcclusionState(_ notification: Notification) {
        synchronizeVisibility()
    }

    func windowDidMiniaturize(_ notification: Notification) {
        AppDelegate.shared.contentViewModel.isWindowVisible = false
    }

    func windowDidDeminiaturize(_ notification: Notification) {
        synchronizeVisibility(refreshDisplay: true)
    }

    func refreshLocalizedTitle() {
        // The brand name "Mirage" is never localized. Build the title from a
        // literal so it stays "Mirage" in every language even if the string
        // generator re-harvests the "Mirage" literal into the localization table.
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0"
        window?.title = "Mirage \(version)"
    }
}
