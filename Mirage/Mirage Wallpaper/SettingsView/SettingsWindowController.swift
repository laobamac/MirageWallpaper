import Cocoa
import SwiftUI

final class SettingsWindowController: NSWindowController, NSWindowDelegate {
    private let minimumContentSize = NSSize(width: 720, height: 520)

    init(viewModel: GlobalSettingsViewModel) {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 780, height: 620),
            styleMask: [.titled, .resizable],
            backing: .buffered,
            defer: false)
        super.init(window: window)
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.title = L("设置")
        window.contentMinSize = minimumContentSize
        window.contentView = NSHostingView(
            rootView: SettingsView().environment(viewModel))
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func windowWillClose(_ notification: Notification) {
        let viewModel = AppDelegate.shared.globalSettingsViewModel
        if viewModel.isSettingsPresented {
            viewModel.reset()
            viewModel.isSettingsPresented = false
        }
        AppDelegate.shared.hideDockIconIfNoWindowsAreVisible()
    }
}
