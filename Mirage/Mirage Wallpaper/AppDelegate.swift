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
    var settingsWindowController: SettingsWindowController!

    var contentViewModel = ContentViewModel()
    lazy var wallpaperViewModel = WallpaperViewModel()
    var globalSettingsViewModel = GlobalSettingsViewModel()
    @MainActor lazy var workshopFeature = WorkshopFeature()
    var navigationModel = MainNavigationModel()

    var importOpenPanel: NSOpenPanel!
    private var developerLogWindowController: DeveloperLogWindowController?
    private var developerLogWindowWasOpened = false
    private var localizationObserver: NSObjectProtocol?
    private var openWindowObserver: NSObjectProtocol?
    private var dynamicLockScreenSessionObservers: [NSObjectProtocol] = []

    static var shared = AppDelegate()

    private static let isLoginItemLaunch = ProcessInfo.processInfo.arguments.contains("--launch-at-login")

    func applicationWillFinishLaunching(_ notification: Notification) {
        LegacyWorkshopMigration.runIfNeeded()
        if Self.isLoginItemLaunch {
            enterMenuBarMode()
        }
        setMainMenu()
        setStatusMenu()
        self.mainWindowController = MainWindowController()
        self.settingsWindowController = SettingsWindowController(
            viewModel: globalSettingsViewModel)
        applyDeveloperMode(enabled: globalSettingsViewModel.settings.isDeveloperModeEnabled)
        localizationObserver = NotificationCenter.default.addObserver(
            forName: MirageLocalization.didChangeNotification,
            object: MirageLocalization.shared,
            queue: .main
        ) { [weak self] _ in
            self?.refreshLocalizedChrome()
        }

        openWindowObserver = DistributedNotificationCenter.default().addObserver(
            forName: mirageOpenWindowNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.openMainWindow()
        }

        let lockCenter = DistributedNotificationCenter.default()
        dynamicLockScreenSessionObservers = [
            lockCenter.addObserver(forName: NSNotification.Name("com.apple.screenIsLocked"), object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    let modeA = DynamicLockScreenManager.shared.isEnabled
                        && DynamicLockScreenManager.shared.isConfigured
                    let modeB = ScreenSaverDynamicLockScreenManager.shared.isEnabled
                        && ScreenSaverDynamicLockScreenManager.shared.isConfigured
                    guard modeA || modeB else { return }
                    self.wallpaperViewModel.suspendForExternalLockScreen()
                    if modeB && !ScreenSaverDynamicLockScreenManager.shared.enterLockedState() {
                        self.wallpaperViewModel.resumeAfterExternalLockScreen()
                        return
                    }
                    if modeA { self.postDynamicLockScreenState(locked: true) }
                }
            },
            lockCenter.addObserver(forName: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    let modeA = DynamicLockScreenManager.shared.isEnabled
                        && DynamicLockScreenManager.shared.isConfigured
                    let modeB = ScreenSaverDynamicLockScreenManager.shared.isEnabled
                        && ScreenSaverDynamicLockScreenManager.shared.isConfigured
                    let saverWasLocked = ScreenSaverDynamicLockScreenManager.shared.isLocked
                    guard modeA || modeB || saverWasLocked else {
                        self.wallpaperViewModel.resumeAfterExternalLockScreen()
                        return
                    }
                    if saverWasLocked && !ScreenSaverDynamicLockScreenManager.shared.leaveLockedState() { return }
                    if modeA { self.postDynamicLockScreenState(locked: false) }
                    self.wallpaperViewModel.resumeAfterExternalLockScreen()
                }
            }
        ]

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

    private func postDynamicLockScreenState(locked: Bool) {
        let name = locked ? "cn.laobamac.Mirage.dynamicLockScreen.locked" : "cn.laobamac.Mirage.dynamicLockScreen.unlocked"
        CFNotificationCenterPostNotification(
            CFNotificationCenterGetDarwinNotifyCenter(),
            CFNotificationName(name as CFString),
            nil,
            nil,
            true
        )
    }

    private func currentScreenIsLocked() -> Bool {
        guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
        for key in ["CGSSessionScreenIsLocked", "kCGSSessionScreenIsLocked"] {
            if let value = session[key] as? NSNumber { return value.boolValue }
            if let value = session[key] as? Bool { return value }
        }
        return false
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let launchedLocked = currentScreenIsLocked()
        if !launchedLocked {
            UserDefaults.standard.set(false, forKey: "Mirage.DynamicLockScreen.Locked")
            UserDefaults.standard.synchronize()
        }
        // Before any wallpaper is applied and before the screen-saver check can
        // restart WallpaperAgent: undoes an override the previous run died
        // holding, and clears the pre-2026-08 staticWP_ placeholder cache.
        DesktopOverrideService.shared.recoverAtLaunch()

        DispatchQueue.main.async {
            self.contentViewModel.refresh()
        }

        SteamServiceManager.shared.start()

        if DynamicLockScreenManager.shared.isEnabled {
            DynamicLockScreenManager.shared.setEnabled(true)
        }
        if ScreenSaverDynamicLockScreenManager.shared.isEnabled {
            ScreenSaverDynamicLockScreenManager.shared.setEnabled(true)
        }

        let dynamicLockScreenActive = (DynamicLockScreenManager.shared.isEnabled
            && DynamicLockScreenManager.shared.isConfigured)
            || (ScreenSaverDynamicLockScreenManager.shared.isEnabled
                && ScreenSaverDynamicLockScreenManager.shared.isConfigured)
        if launchedLocked && dynamicLockScreenActive {
            wallpaperViewModel.suspendForExternalLockScreen()
        } else if wallpaperViewModel.hasAnyWallpaper {
            wallpaperViewModel.restoreAllDisplays()
        }

        if !Self.isLoginItemLaunch {
            openMainWindow()
        } else {
            enterMenuBarMode()
        }

        UpdateManager.shared.start()

        PlaylistManager.shared.startRotators(wallpaperViewModel: wallpaperViewModel)

        DispatchQueue.global(qos: .utility).async {
            ScreenSaverManager.shared.refreshInstalledVersionIfNeeded()
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        DispatchQueue.main.async { [weak self] in
            self?.globalSettingsViewModel.refreshLoginItemStatus(persist: true)
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !self.mainWindowController.window.isVisible {
            openMainWindow()
        }
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        if developerLogWindowWasOpened {
            MirageLogService.shared.saveAutomatically()
        }
        wallpaperViewModel.saveRuntime()
        ScreenSaverDynamicLockScreenManager.shared.applicationWillTerminate()
        DesktopOverrideService.shared.finalizeForApplicationTermination()
        // This method returns directly into process exit, so the renderers must
        // be reaped here and now. Anything deferred would never run and a hung
        // renderer would outlive the app as an orphan still drawing on the
        // desktop. Bounded to roughly two seconds in total.
        wallpaperViewModel.renderer.stopAllAndWait()

        SteamServiceManager.shared.shutdown()

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
        openMainWindow()
        guard let parent = mainWindowController.window,
              let settingsWindow = settingsWindowController.window else { return }
        if parent.attachedSheet === settingsWindow { return }
        guard parent.attachedSheet == nil else { return }
        globalSettingsViewModel.isSettingsPresented = true
        parent.beginSheet(settingsWindow)
    }

    func closeSettingsWindow(commit: Bool) {
        if commit {
            globalSettingsViewModel.save()
        } else {
            globalSettingsViewModel.reset()
        }
        globalSettingsViewModel.isSettingsPresented = false
        guard let settingsWindow = settingsWindowController?.window else { return }
        if let parent = settingsWindow.sheetParent {
            parent.endSheet(settingsWindow)
        } else {
            settingsWindow.orderOut(nil)
        }
    }

    @objc func openSteamAPIKeySettings() {
        globalSettingsViewModel.selection = 1
        openSettingsWindow()
    }

    @objc func openMainWindow() {
        NSApp.setActivationPolicy(.regular)
        if !self.mainWindowController.window.isVisible,
           UserDefaults.standard.string(forKey: "NSWindow Frame MainWindow") == nil {
            self.mainWindowController.window.center()
        }
        self.mainWindowController.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func restoreMainWindowFocus() {
        guard let window = mainWindowController?.window, window.isVisible else { return }
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
    }

    func hideDockIconIfNoWindowsAreVisible() {
        DispatchQueue.main.async {
            guard !NSApp.windows.contains(where: { $0.isVisible }) else { return }
            self.enterMenuBarMode()
        }
    }

    func enterMenuBarMode() {
        NSApp.setActivationPolicy(.accessory)
    }

    func applyStatusItemVisibility(hidden: Bool) {
        statusItem?.isVisible = !hidden
    }

    func applyDeveloperMode(enabled: Bool) {
        if enabled {
            MirageLogService.shared.start()
            developerLogWindowWasOpened = true
            if developerLogWindowController == nil {
                developerLogWindowController = DeveloperLogWindowController()
            }
            developerLogWindowController?.showWindow(nil)
        } else if developerLogWindowController?.window?.isVisible == true {
            developerLogWindowController?.close()
        }
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
        settingsWindowController?.window?.title = L("设置")
        developerLogWindowController?.refreshLocalization()
    }
}
