//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

class SteamSetupWindowController: NSWindowController, NSWindowDelegate {
    convenience init(viewModel: SteamSetupViewModel) {
        let window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 520, height: 560),
            styleMask: [.titled, .closable, .fullSizeContentView, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.title = L("Steam 创意工坊设置")
        window.titlebarAppearsTransparent = true
        window.isMovableByWindowBackground = true
        window.isReleasedWhenClosed = false
        window.level = .floating
        window.isFloatingPanel = true
        window.center()

        let rootView = SteamSetupView(viewModel: viewModel)
        window.contentView = NSHostingView(rootView: rootView)

        self.init(window: window)
        window.delegate = self
    }

    func windowWillClose(_ notification: Notification) {
    }
}
