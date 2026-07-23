//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI
import AVKit
import CryptoKit

class AppDelegate: NSObject, NSApplicationDelegate {

    var statusItem: NSStatusItem!

    var mainWindowController: MainWindowController!

    var contentViewModel = ContentViewModel()
    var wallpaperViewModel = WallpaperViewModel()
    var globalSettingsViewModel = GlobalSettingsViewModel()
    var workshopViewModel = WorkshopViewModel()
    var rmskinViewModel = RmskinViewModel()

    var importOpenPanel: NSOpenPanel!
    private var localizationObserver: NSObjectProtocol?
    private let placeholderLock = NSLock()
    private var placeholderIdentity: String?
    private var placeholderInFlightIdentity: String?
    private var placeholderGeneration: UInt64 = 0
    private var placeholderCachePruneScheduled = false

    static var shared = AppDelegate()

    func applicationWillFinishLaunching(_ notification: Notification) {
        // 禁用窗口状态恢复，防止 restoreWindowWithIdentifier 错误
        UserDefaults.standard.set(false, forKey: "NSQuitAlwaysKeepsWindows")
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
        wallpaperViewModel.saveRuntime()
        wallpaperViewModel.renderer.stopAll()
        rmskinViewModel.stopAll()

        // NSScreen.main can be nil (all displays asleep / disconnected / app in
        // background). Never force-unwrap it or the app crashes on quit.
        if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper"),
           let screen = NSScreen.main {
            try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: screen)
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
        guard wallpaper.kind == .video,
              globalSettingsViewModel.settings.adjustMenuBarTint else { return }
        let values = try? wallpaper.entryURL.resourceValues(
            forKeys: [.contentModificationDateKey, .fileSizeKey])
        let screen = NSScreen.main
        let targetSize = CGSize(
            width: (screen?.frame.width ?? 1920) * (screen?.backingScaleFactor ?? 1),
            height: (screen?.frame.height ?? 1080) * (screen?.backingScaleFactor ?? 1))
        let identity = "\(wallpaper.entryURL.path)#\(values?.contentModificationDate?.timeIntervalSince1970 ?? 0)#\(values?.fileSize ?? 0)#\(Int(targetSize.width))x\(Int(targetSize.height))"
        let digest = SHA256.hash(data: Data(identity.utf8))
            .map { String(format: "%02x", $0) }.joined()
        let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "staticWP_\(digest).tiff")

        let fileExists = FileManager.default.fileExists(atPath: url.path)
        placeholderLock.lock()
        if placeholderIdentity != identity {
            placeholderIdentity = identity
            placeholderGeneration &+= 1
            placeholderInFlightIdentity = nil
        }
        let generation = placeholderGeneration
        let alreadyGenerating = placeholderInFlightIdentity == identity
        if !fileExists && !alreadyGenerating {
            placeholderInFlightIdentity = identity
        }
        placeholderLock.unlock()

        if fileExists {
            try? FileManager.default.setAttributes(
                [.modificationDate: Date()], ofItemAtPath: url.path)
            schedulePlaceholderCachePrune(keeping: url)
            applyPlaceholderIfCurrent(url, wallpaper: wallpaper, generation: generation)
            return
        }
        guard !alreadyGenerating else { return }

        let asset = AVAsset(url: wallpaper.entryURL)
        let imageGenerator = AVAssetImageGenerator(asset: asset)
        imageGenerator.appliesPreferredTrackTransform = true
        imageGenerator.maximumSize = targetSize
        let time = CMTimeMake(value: 1, timescale: 1)
        imageGenerator.generateCGImagesAsynchronously(forTimes: [NSValue(time: time)]) {
            [weak self] _, cgImage, _, _, error in
            guard let self else { return }
            self.placeholderLock.lock()
            let current = self.placeholderGeneration == generation &&
                self.placeholderIdentity == identity
            if self.placeholderInFlightIdentity == identity {
                self.placeholderInFlightIdentity = nil
            }
            self.placeholderLock.unlock()
            guard current, error == nil, let cgImage else { return }
            let nsImage = NSImage(cgImage: cgImage, size: .zero)
            guard let data = nsImage.tiffRepresentation else { return }
            try? data.write(to: url, options: .atomic)
            self.schedulePlaceholderCachePrune(keeping: url)
            DispatchQueue.main.async {
                self.applyPlaceholderIfCurrent(url, wallpaper: wallpaper,
                                               generation: generation)
            }
        }
    }

    private func schedulePlaceholderCachePrune(keeping keepURL: URL) {
        placeholderLock.lock()
        guard !placeholderCachePruneScheduled else {
            placeholderLock.unlock()
            return
        }
        placeholderCachePruneScheduled = true
        placeholderLock.unlock()
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            defer {
                self.placeholderLock.lock()
                self.placeholderCachePruneScheduled = false
                self.placeholderLock.unlock()
            }
            let directory = keepURL.deletingLastPathComponent()
            let keys: Set<URLResourceKey> = [.contentModificationDateKey, .fileSizeKey]
            guard let urls = try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: Array(keys),
                options: .skipsHiddenFiles) else { return }
            let entries = urls.filter { $0.lastPathComponent.hasPrefix("staticWP_") }
                .compactMap { url -> (URL, Date, Int)? in
                    let values = try? url.resourceValues(forKeys: keys)
                    return (url, values?.contentModificationDate ?? .distantPast,
                            values?.fileSize ?? 0)
                }
                .sorted { $0.1 > $1.1 }
            var retainedBytes = 0
            var retainedCount = 0
            for entry in entries {
                let fits = retainedCount < 8 &&
                    retainedBytes + entry.2 <= 256 * 1024 * 1024
                if entry.0 == keepURL || fits {
                    retainedBytes += entry.2
                    retainedCount += 1
                } else {
                    try? FileManager.default.removeItem(at: entry.0)
                }
            }
        }
    }

    private func applyPlaceholderIfCurrent(_ url: URL, wallpaper: WEWallpaper,
                                           generation: UInt64) {
        placeholderLock.lock()
        let current = placeholderGeneration == generation && placeholderIdentity != nil
        placeholderLock.unlock()
        guard current,
              globalSettingsViewModel.settings.adjustMenuBarTint,
              wallpaperViewModel.currentWallpaper.id == wallpaper.id,
              let screen = NSScreen.main else { return }
        try? NSWorkspace.shared.setDesktopImageURL(url, for: screen)
    }
}
