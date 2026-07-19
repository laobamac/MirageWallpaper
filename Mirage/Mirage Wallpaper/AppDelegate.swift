//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI
import AVKit

class AppDelegate: NSObject, NSApplicationDelegate {

    var statusItem: NSStatusItem!

    var mainWindowController: MainWindowController!

    var contentViewModel = ContentViewModel()
    var wallpaperViewModel = WallpaperViewModel()
    var globalSettingsViewModel = GlobalSettingsViewModel()
    var workshopViewModel = WorkshopViewModel()

    var importOpenPanel: NSOpenPanel!
    private var localizationObserver: NSObjectProtocol?

    static var shared = AppDelegate()

    func applicationWillFinishLaunching(_ notification: Notification) {
        setMainMenu()
        setStatusMenu()
        self.mainWindowController = MainWindowController()
        localizationObserver = NotificationCenter.default.addObserver(
            forName: MirageLocalization.didChangeNotification,
            object: MirageLocalization.shared,
            queue: .main
        ) { [weak self] _ in
            self?.refreshLocalizedChrome()
        }

        wallpaperViewModel.renderer.onProcessExit = { [weak self] screen, abnormal in
            guard abnormal, screen == 0 else { return }
            NSLog("[Mirage] 渲染子进程异常退出（屏幕 \(screen)）")
            _ = self
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        DispatchQueue.main.async {
            self.contentViewModel.refresh()
        }

        DispatchQueue.global(qos: .utility).async {
            SteamCMDManager.shared.refreshSessionIfNeeded()
        }

        let w = wallpaperViewModel.currentWallpaper
        if w.isValid {
            wallpaperViewModel.reapplyCurrent()
            setPlaceholderWallpaper(with: w)
        }

        let isDefaultLaunch = (notification.userInfo?["NSApplicationLaunchIsDefaultLaunchKey"] as? Bool) ?? true
        let launchedAtLogin = !isDefaultLaunch

        if launchedAtLogin {
            NSApp.setActivationPolicy(.accessory)
        } else if globalSettingsViewModel.isFirstLaunch {
            self.mainWindowController.window.center()
            self.mainWindowController.window.makeKeyAndOrderFront(nil)
        }

        UpdateManager.shared.start()

        DispatchQueue.global(qos: .utility).async {
            ScreenSaverManager.shared.refreshInstalledVersionIfNeeded()
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !self.mainWindowController.window.isVisible {
            openMainWindow()
        }
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        wallpaperViewModel.renderer.stopAll()

        if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper") {
            try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: .main!)
        }
        let cacheDirectory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        if let filesURL = try? FileManager.default.contentsOfDirectory(at: cacheDirectory, includingPropertiesForKeys: nil, options: .skipsHiddenFiles) {
            for url in filesURL where url.lastPathComponent.contains("staticWP") {
                try? FileManager.default.removeItem(at: url)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

    @objc func openSettingsWindow() {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        // The settings panel is a sheet that floats over the main window, so the
        // host window must exist and be on screen before we present it.
        openMainWindow()
        globalSettingsViewModel.isSettingsPresented = true
    }

    @objc func openSteamAPIKeySettings() {
        globalSettingsViewModel.selection = 1
        openSettingsWindow()
    }

    @objc func openMainWindow() {
        NSApp.setActivationPolicy(.regular)
        self.mainWindowController.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @MainActor @objc func toggleFilter() {
        self.contentViewModel.isFilterReveal.toggle()
    }

    @objc func openSteamSetup() {
        contentViewModel.isSteamSetupPresented = true
    }

    private func refreshLocalizedChrome() {
        setMainMenu()
        setStatusMenu()
        mainWindowController?.refreshLocalizedTitle()
    }

    func setPlaceholderWallpaper(with wallpaper: WEWallpaper) {
        guard wallpaper.kind == .video else { return }
        let asset = AVAsset(url: wallpaper.entryURL)
        let imageGenerator = AVAssetImageGenerator(asset: asset)
        imageGenerator.appliesPreferredTrackTransform = true
        let time = CMTimeMake(value: 1, timescale: 1)
        imageGenerator.generateCGImagesAsynchronously(forTimes: [NSValue(time: time)]) { _, cgImage, _, _, error in
            guard error == nil, let cgImage else { return }
            let nsImage = NSImage(cgImage: cgImage, size: .zero)
            guard let data = nsImage.tiffRepresentation else { return }
            let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
                .appending(path: "staticWP_\(wallpaper.wallpaperDirectory.hashValue).tiff")
            try? data.write(to: url, options: .atomic)
        }
    }
}
