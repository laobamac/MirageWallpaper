import AppKit
import SwiftUI
@testable import Mirage_Wallpaper

@main
@MainActor
struct PlaybackUIRegression {
    static func require(_ value: @autoclosure () -> Bool, _ message: String) { precondition(value(), message) }
    static func main() async throws {
        NSApplication.shared.setActivationPolicy(.accessory)
        NSApplication.shared.finishLaunching()
        MirageLocalization.shared.apply(.zh_CN)
        let key = DisplayKey(rawValue: "uuid:test")
        func summary(_ policy: GSPlayback = .keepRunning, running: Bool = true,
                     pending: Bool = false, assigned: String? = "assigned", paused: Bool = false,
                     locked: Bool = false, muted: Bool = false) -> DisplayPlaybackSummary {
            .resolve(id: key, name: "主屏", activeTitle: running ? "active" : nil,
                     assignedTitle: assigned, pending: pending, running: running, policy: policy,
                     sessionPaused: paused, lockPaused: locked, muted: muted, runtime: WallpaperRuntimeState())
        }
        require(summary().wallpaperTitle == "active", "selection substituted for actual playback")
        require(summary(pending: true).wallpaperTitle == "active", "switch hid the currently playing wallpaper")
        require(summary(pending: true).status == L("正在切换…"), "pending state")
        require(summary(running: false).status == L("未运行"), "configured state claimed playback")
        require(summary(.stop, running: false).status == L("按规则停止"), "policy stop state")
        require(summary(.pause).status == L("按规则暂停"), "policy pause state")
        require(summary(paused: true).status == L("已暂停"), "manual pause state")
        require(summary(locked: true).status == L("锁屏暂停"), "lock state")
        require(summary(muted: true).status == L("播放中（静音）"), "global mute state")
        require(summary(running: false, assigned: nil).status == L("未设置"), "empty state")
        print("PASS: active/configured/pending playback, policy, pause, lock and mute labels")

        let model = ContentViewModel(observeLibrary: false)
        model.isStaging = true
        model.isWindowVisible = false
        let wallpaper = WallpaperViewModel(initialStates: [:])
        let workshop = WorkshopViewModel(subscriptionCatalog: [])
        let settings = GlobalSettingsViewModel()
        settings.isFirstLaunch = false
        settings.settings.language = .zh_CN
        let navigation = MainNavigationModel()
        let content = ContentView(viewModel: model, wallpaperViewModel: wallpaper,
                                  workshopViewModel: workshop, navigationModel: navigation).environment(settings)
        for width in [800, 1000, 1300] {
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: width, height: 640),
                                  styleMask: [.titled], backing: .buffered, defer: false)
            let view = NSHostingView(rootView: content)
            view.sizingOptions = []
            window.contentView = view
            view.setFrameSize(NSSize(width: width, height: 640))
            view.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(150))
            view.layoutSubtreeIfNeeded()
            require(view.frame.width == CGFloat(width), "layout forced a wider window")
            let representation = view.bitmapImageRepForCachingDisplay(in: view.bounds)!
            view.cacheDisplay(in: view.bounds, to: representation)
            let output = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appending(path: "window-\(width).png")
            try representation.representation(using: .png, properties: [:])!.write(to: output)
            print("RENDER: \(output.path)")
        }
        print("PASS: actual main content rendered at 800, 1000 and 1300 points in an isolated home")
    }
}
