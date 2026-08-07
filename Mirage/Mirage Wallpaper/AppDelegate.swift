//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

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

        // The renderer keeps a black window up when it cannot decode, so this
        // alert is the only thing that tells the user why.
        wallpaperViewModel.renderer.onVideoError = { [weak self] screen, wallpaper, message in
            VideoTranscodeProgressModel.shared.finish(screen: screen)
            self?.contentViewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("视频无法播放"),
                message: L("“%@”无法播放：%@", wallpaper.project.title, message)
            )
        }

        wallpaperViewModel.renderer.onVideoTranscodeProgress = { screen, wallpaper, progress, done in
            let model = VideoTranscodeProgressModel.shared
            if done {
                model.finish(screen: screen)
                NSLog("[Mirage] 视频转码结束: \(wallpaper.project.title)")
            } else {
                model.update(screen: screen,
                             title: wallpaper.project.title,
                             progress: progress)
            }
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Before any wallpaper is applied and before the screen-saver check can
        // restart WallpaperAgent: undoes an override the previous run died
        // holding, and clears the pre-2026-08 staticWP_ placeholder cache.
        DesktopOverrideService.shared.recoverAtLaunch()

        DispatchQueue.main.async {
            self.contentViewModel.refresh()
        }

        DispatchQueue.global(qos: .utility).async {
            SteamCMDManager.shared.refreshSessionIfNeeded()
        }

        let w = wallpaperViewModel.currentWallpaper
        if w.isValid {
            wallpaperViewModel.restoreAllDisplays()
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

        PlaylistManager.shared.startRotators(wallpaperViewModel: wallpaperViewModel)

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
        // This method returns directly into process exit, so the renderers must
        // be reaped here and now. Anything deferred would never run and a hung
        // renderer would outlive the app as an orphan still drawing on the
        // desktop. Bounded to roughly two seconds in total.
        wallpaperViewModel.renderer.stopAllAndWait()
        rmskinViewModel.stopAll()

        // Same constraint: a transient override is one Mirage only took for
        // tint consistency, so the user's own picture goes back synchronously
        // before this returns. A persistent override is left in place.
        DesktopOverrideService.shared.restoreIfTransient()
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

    func restoreMainWindowFocus() {
        guard let window = mainWindowController?.window, window.isVisible else { return }
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
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
}
