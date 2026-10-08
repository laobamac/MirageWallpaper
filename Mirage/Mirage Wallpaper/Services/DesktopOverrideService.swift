//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import CryptoKit

/// Replaces the macOS desktop picture with a still frame of the live wallpaper,
/// so the menu bar, Dock and every other surface that samples the desktop for
/// its tint agrees with what is actually on screen.
///
/// Two user-visible behaviours, mirroring Wallpaper Engine's "override
/// wallpaper" option:
///
/// - ON  (`persistent`): the override survives quitting Mirage and is re-taken
///   whenever the wallpaper changes. The user's own picture is never restored
///   until they turn the option off.
/// - OFF (`transient`): the override exists only while Mirage runs, purely for
///   tint consistency, and the user's picture is restored on quit.
///
/// The mode is persisted *before* the desktop picture is touched, so a run that
/// dies without restoring (crash, SIGKILL, power loss) is detectable at the next
/// launch: a `transient` marker that outlived its process means the restore
/// never happened, and `recoverAtLaunch()` performs it.
final class DesktopOverrideService {

    static let shared = DesktopOverrideService()

    private enum Mode: String {
        /// The desktop picture is the user's own; nothing to undo.
        case none
        /// Overriding only for this run. MUST be restored on quit.
        case transient
        /// Overriding across launches, at the user's request.
        case persistent
    }

    private enum Key {
        static let mode = "DesktopOverrideMode"
        static let backup = "DesktopOverrideBackup"
        static let backups = "DesktopOverrideBackups"
        static let displays = "DesktopOverrideDisplays"
        static let installed = "DesktopOverrideInstalled"
        static let ownedCacheHashes = "DesktopOverrideOwnedCacheHashes"
        static let cacheBookmark = "DesktopOverrideCacheBookmark"
    }

    private struct CaptureRequest: Equatable {
        let id: UUID
        let wallpaperID: String
    }

    /// Not `.cachesDirectory`: the system may purge caches at any time, and a
    /// desktop picture whose file has vanished makes WallpaperAgent silently
    /// reset the slot to `default` — losing the user's wallpaper irrecoverably.
    private let directory: URL
    private let defaults = UserDefaults.standard
    private let ioQueue = DispatchQueue(label: "cn.laobamac.Mirage.desktopOverride")
    private var pendingCapture: [CGDirectDisplayID: DispatchWorkItem] = [:]
    private var captureRequests: [CGDirectDisplayID: CaptureRequest] = [:]
    private var installedByScreen: [CGDirectDisplayID: URL] = [:]
    private var externallyChanged = Set<CGDirectDisplayID>()
    /// Read-only detection; a different desktop image does not identify which app changed it.
    func externallyChangedDisplays() -> Set<CGDirectDisplayID> {
        for screen in NSScreen.screens {
            guard let displayID = (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value,
                  let installed = installedByScreen[displayID],
                  let current = NSWorkspace.shared.desktopImageURL(for: screen)?.resolvingSymlinksInPath() else { continue }
            let recent = recentInstalls[displayID]
            if !DesktopRestoreOwnership.shouldRestore(
                current: current, installed: installed, previous: recent?.previous,
                recentWriteAge: recent.map { ProcessInfo.processInfo.systemUptime - $0.uptime },
                recorded: true, isCurrentGenerated: isMirageGenerated(current)) {
                externallyChanged.insert(displayID)
            }
        }
        return externallyChanged
    }

    private var recentInstalls: [CGDirectDisplayID: (previous: URL?, uptime: TimeInterval)] = [:]
    private static let captureRetryDelays: [TimeInterval] = [1.0, 2.0, 4.0, 6.0, 8.0, 10.0]
    private var pendingTargets: Set<URL> = []
    private var pendingPrune: DispatchWorkItem?
    private var cacheAccessPromptShown = false
    private let ownedCacheHashesLock = NSLock()

    private init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/DesktopOverride")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        self.directory = base
    }

    private var mode: Mode {
        get { Mode(rawValue: defaults.string(forKey: Key.mode) ?? "") ?? .none }
        set {
            if newValue == .none {
                defaults.removeObject(forKey: Key.mode)
            } else {
                defaults.set(newValue.rawValue, forKey: Key.mode)
            }
            // The marker is the only crash evidence there is, so it must reach
            // disk before the desktop picture changes rather than at the next
            // periodic flush.
            defaults.synchronize()
        }
    }

    private var isEnabled: Bool {
        AppDelegate.shared.globalSettingsViewModel.settings.shouldOverrideWallpaper
    }

    private var preserveForDynamicLockScreen: Bool {
        guard UserDefaults.standard.bool(forKey: "Mirage.DynamicLockScreen.Enabled"),
              let container = FileManager.default.containerURL(
                forSecurityApplicationGroupIdentifier: "group.cn.laobamac.Mirage"),
              let data = try? Data(contentsOf: container.appendingPathComponent(
                "dynamic-lock-screen.json")),
              let configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data)
        else { return false }
        return configuration.enabled != false && !configuration.displays.isEmpty
    }

    // MARK: - Launch recovery

    /// Must run before anything that can restart WallpaperAgent (i.e. before the
    /// screen-saver install check), and before the first wallpaper is applied.
    func recoverAtLaunch() {
        // Order matters. The legacy placeholders are deleted only after every
        // desktop slot that points at one has been pointed somewhere else:
        // WallpaperAgent resets a slot whose file has vanished to `default`,
        // which loses the wallpaper for good.
        evictLegacyPlaceholderPointers()
        migrateLegacyPlaceholders()
        migrateLegacyBackup()
        registerExistingOverrideHashes()
        reconcileInstalledOverrides()

        if mode == .transient && !preserveForDynamicLockScreen {
            // A transient marker cannot legitimately survive its own process:
            // the previous run was killed before it could restore.
            NSLog("[Mirage] 检测到上次运行未正常还原桌面图片，正在还原")
            restore()
        }

        repairDanglingDesktopPointer()
        pruneUnreferencedOverridesAtLaunch()
        cleanSystemCacheIfEnabled()
    }

    /// The pre-2026-08 implementation set `staticWP_*.tiff` as the desktop
    /// picture and never recorded what it replaced, so the user's original
    /// choice is already unrecoverable on machines that ran it. The best
    /// available outcome is a valid system picture, which also gives the backup
    /// key something real to hold before this run takes over.
    private func evictLegacyPlaceholderPointers() {
        let fallback = restoreTarget()
        for screen in NSScreen.screens {
            guard let current = NSWorkspace.shared.desktopImageURL(for: screen),
                  isLegacyPlaceholder(current) else { continue }
            NSLog("[Mirage] 桌面图片仍指向旧版占位图，改为系统图片")
            setDesktopImage(fallback, for: screen)
        }
    }

    /// Deletes the 24 MB-per-file `staticWP_*.tiff` stills written by the
    /// pre-2026-08 placeholder implementation, which kept eight of them.
    private func migrateLegacyPlaceholders() {
        ioQueue.async {
            let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            guard let urls = try? FileManager.default.contentsOfDirectory(
                at: caches, includingPropertiesForKeys: nil,
                options: .skipsHiddenFiles) else { return }
            var removed = 0
            for url in urls where url.lastPathComponent.hasPrefix("staticWP_") {
                if (try? FileManager.default.removeItem(at: url)) != nil { removed += 1 }
            }
            if removed > 0 {
                NSLog("[Mirage] 已清理 \(removed) 个遗留的桌面占位图")
            }
        }
    }

    /// If a desktop slot still points at one of our files that no longer exists,
    /// WallpaperAgent will reset that slot to `default` the next time it
    /// re-resolves it. Point it somewhere real first.
    private func repairDanglingDesktopPointer() {
        for screen in NSScreen.screens {
            guard let current = NSWorkspace.shared.desktopImageURL(for: screen),
                  isMirageGenerated(current),
                  !FileManager.default.fileExists(atPath: current.path) else { continue }
            NSLog("[Mirage] 桌面图片指向已删除的覆盖文件，正在修复")
            let displayID = (screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
            let target = displayID.flatMap(backupURL(for:)) ?? restoreTarget()
            setDesktopImage(target, for: screen)
        }
    }

    private func pruneUnreferencedOverridesAtLaunch() {
        var keep = Set(storedInstalledOverrides().values.map { $0.resolvingSymlinksInPath() })
        keep.formUnion(NSScreen.screens.compactMap {
            NSWorkspace.shared.desktopImageURL(for: $0)
        }.filter(isGeneratedOverride).map { $0.resolvingSymlinksInPath() })
        let protected = keep
        ioQueue.async { [weak self] in
            self?.pruneAllExceptNow(protected)
        }
    }

    // MARK: - Applying

    /// Requests a fresh still for `screenIndex` and installs it as that screen's
    /// desktop picture. Coalesced per screen.
    func scheduleCapture(for screenIndex: Int, wallpaper: WEWallpaper) {
        guard let displayID = AppDelegate.shared.wallpaperViewModel.renderer
            .displayID(for: screenIndex) else { return }
        scheduleCapture(forDisplay: displayID, wallpaper: wallpaper)
    }

    func scheduleCapture(forDisplay displayID: CGDirectDisplayID, wallpaper: WEWallpaper) {
        if UserDefaults.standard.bool(forKey: "Mirage.DynamicLockScreen.Locked") { return }
        guard wallpaper.isValid, wallpaper.kind != .unsupported else { return }
        pendingCapture[displayID]?.cancel()
        if preserveForDynamicLockScreen && !isEnabled {
            captureRequests[displayID] = nil
            Task { @MainActor in
                DynamicLockScreenManager.shared.restoreSystemDesktopFallbacks()
            }
            return
        }
        let request = CaptureRequest(id: UUID(), wallpaperID: wallpaper.id)
        captureRequests[displayID] = request
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.captureRequests[displayID] == request else { return }
            self.pendingCapture[displayID] = nil
            self.capture(forDisplay: displayID, wallpaper: wallpaper, request: request)
        }
        pendingCapture[displayID] = work
        DispatchQueue.main.async(execute: work)
    }

    /// Re-takes the still for every screen that currently has a wallpaper.
    func scheduleCaptureForAllScreens() {
        if UserDefaults.standard.bool(forKey: "Mirage.DynamicLockScreen.Locked") { return }
        let viewModel = AppDelegate.shared.wallpaperViewModel
        for displayID in viewModel.renderer.activeDisplayIDs {
            guard let wallpaper = viewModel.renderer.currentWallpaper(onDisplay: displayID) else { continue }
            scheduleCapture(forDisplay: displayID, wallpaper: wallpaper)
        }
    }

    @MainActor
    func finalizeForApplicationTermination() {
        guard preserveForDynamicLockScreen else { return }
        guard isEnabled else {
            DynamicLockScreenManager.shared.restoreSystemDesktopFallbacks()
            DynamicLockScreenManager.shared.refreshExtension()
            return
        }
        scheduleCaptureForAllScreens()
        let deadline = Date().addingTimeInterval(2)
        while Date() < deadline && (!pendingCapture.isEmpty || !captureRequests.isEmpty) {
            RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.05))
        }
        DynamicLockScreenManager.shared.refreshExtension()
    }

    private func screen(for displayID: CGDirectDisplayID) -> NSScreen? {
        NSScreen.screens.first {
            ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?
                .uint32Value == displayID
        }
    }

    private func capture(forDisplay displayID: CGDirectDisplayID, wallpaper: WEWallpaper,
                         request: CaptureRequest, attempt: Int = 0) {
        guard captureRequests[displayID] == request else { return }
        // Read the user's picture before anything is written, not after: the
        // read-back is only eventually consistent, so once an override is in
        // flight it can no longer be trusted to reveal what was there before.
        guard let screen = screen(for: displayID) else { return }
        if !preserveForDynamicLockScreen {
            backUpUserPictureIfNeeded(on: screen, displayID: displayID)
        }
        let target = directory.appending(path: "override-\(UUID().uuidString).heic")
        pendingTargets.insert(target)
        AppDelegate.shared.wallpaperViewModel.renderer.snapshot(
            onDisplay: displayID, path: target.path
        ) { [weak self] ok in
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                guard self.captureRequests[displayID] == request else {
                    self.removeCaptureArtifacts(for: target)
                    self.pendingTargets.remove(target)
                    return
                }
                let current = AppDelegate.shared.wallpaperViewModel.renderer
                    .currentWallpaper(onDisplay: displayID)
                guard current?.id == request.wallpaperID else {
                    self.removeCaptureArtifacts(for: target)
                    self.pendingTargets.remove(target)
                    self.captureRequests[displayID] = nil
                    return
                }
                if ok, FileManager.default.fileExists(atPath: target.path) {
                    NSLog("[Mirage] 已捕获壁纸实时画面 (显示器=\(displayID))")
                    if self.preserveForDynamicLockScreen {
                        self.installDynamicLockScreenFallback(
                            target, forDisplay: displayID, request: request)
                        return
                    }
                    let canonical = self.canonicalizeSnapshot(at: target)
                    if self.installedURL(for: displayID)?.resolvingSymlinksInPath()
                        == canonical.resolvingSymlinksInPath() {
                        self.pendingTargets.remove(canonical)
                        self.captureRequests[displayID] = nil
                        self.schedulePrune()
                        self.cleanSystemCacheIfEnabled()
                        return
                    }
                    self.install(canonical, forDisplay: displayID, request: request,
                                 attempt: attempt)
                    return
                }
                self.removeCaptureArtifacts(for: target)
                let delay = Self.captureRetryDelays[
                    min(attempt, Self.captureRetryDelays.count - 1)]
                self.pendingTargets.remove(target)
                NSLog("[Mirage] 壁纸实时画面暂不可用，稍后重试 (显示器=\(displayID))")
                DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                    guard let self, self.captureRequests[displayID] == request else { return }
                    self.capture(
                        forDisplay: displayID, wallpaper: wallpaper,
                        request: request, attempt: attempt + 1)
                }
            }
        }
    }

    private func installDynamicLockScreenFallback(
        _ url: URL,
        forDisplay displayID: CGDirectDisplayID,
        request: CaptureRequest
    ) {
        Task { @MainActor [weak self] in
            guard let self, self.captureRequests[displayID] == request else {
                self?.removeCaptureArtifacts(for: url)
                return
            }
            if !self.preserveForDynamicLockScreen {
                self.install(url, forDisplay: displayID, request: request, attempt: 0)
                return
            }
            if self.isEnabled {
                do {
                    _ = try DynamicLockScreenManager.shared.updateDesktopFallback(
                        from: url, forDisplay: displayID)
                } catch {
                    NSLog("[Mirage] 更新动态锁屏桌面备用图失败: \(error.localizedDescription)")
                }
            } else {
                DynamicLockScreenManager.shared.restoreSystemDesktopFallbacks()
            }
            self.pendingTargets.remove(url)
            self.captureRequests[displayID] = nil
            self.removeCaptureArtifacts(for: url)
        }
    }

    /// Points `screenIndex` at `url`, recording the user's own picture first and
    /// deleting every override file that is no longer displayed.
    private func install(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                         request: CaptureRequest, attempt: Int) {
        guard captureRequests[displayID] == request else {
            removeCaptureArtifacts(for: url)
            pendingTargets.remove(url)
            return
        }
        if preserveForDynamicLockScreen {
            installDynamicLockScreenFallback(
                url, forDisplay: displayID, request: request)
            return
        }
        guard let screen = screen(for: displayID) else {
            removeCaptureArtifacts(for: url)
            pendingTargets.remove(url)
            captureRequests[displayID] = nil
            return
        }

        // Normally already recorded before the capture was requested; repeated
        // here so an install from any other path cannot skip it.
        backUpUserPictureIfNeeded(on: screen, displayID: displayID)
        // Persist the undo marker before the desktop changes, never after: a
        // crash in between must leave evidence that a restore is owed.
        //
        // Always re-derived from the live setting rather than left at whatever
        // an earlier install wrote. The two values are a matched pair — this
        // marker decides whether the quit path restores, and the setting decides
        // whether it should — so letting them drift apart produced a persisted
        // override still marked `transient`, which the next launch then undid.
        let wanted: Mode = isEnabled ? .persistent : .transient
        if mode != wanted {
            mode = wanted
        }
        let key = backupKey(for: displayID)
        var displayKeys = storedOverrideDisplayKeys()
        displayKeys.insert(key)
        saveOverrideDisplayKeys(displayKeys)
        var installedOverrides = storedInstalledOverrides()
        let previousInstalled = installedOverrides[key]
        installedOverrides[key] = url
        saveInstalledOverrides(installedOverrides)
        let previousPicture = NSWorkspace.shared.desktopImageURL(for: screen)
        guard setDesktopImage(url, for: screen) else {
            displayKeys.remove(key)
            saveOverrideDisplayKeys(displayKeys)
            if let previousInstalled {
                installedOverrides[key] = previousInstalled
            } else {
                installedOverrides.removeValue(forKey: key)
            }
            saveInstalledOverrides(installedOverrides)
            removeCaptureArtifacts(for: url)
            pendingTargets.remove(url)
            let delay = Self.captureRetryDelays[
                min(attempt, Self.captureRetryDelays.count - 1)]
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self, self.captureRequests[displayID] == request,
                      let wallpaper = AppDelegate.shared.wallpaperViewModel.renderer
                        .currentWallpaper(onDisplay: displayID),
                      wallpaper.id == request.wallpaperID else { return }
                self.capture(forDisplay: displayID, wallpaper: wallpaper,
                             request: request, attempt: attempt + 1)
            }
            return
        }
        pendingTargets.remove(url)
        installedByScreen[displayID] = url
        externallyChanged.remove(displayID)
        recentInstalls[displayID] = (previousPicture, ProcessInfo.processInfo.systemUptime)
        captureRequests[displayID] = nil
        schedulePrune()
        cleanSystemCacheIfEnabled()
        Task { @MainActor in
            DynamicLockScreenManager.shared.refreshDesktopFallback(forDisplay: displayID)
        }
        verifyDesktopImage(url, forDisplay: displayID, screen: screen)
    }

    /// Records the picture the user chose, so it can be put back. Only ever
    /// stores something that is not one of our own files — otherwise a crash
    /// followed by a relaunch would "back up" our override and lose the real
    /// wallpaper permanently.
    private func backUpUserPictureIfNeeded(on screen: NSScreen,
                                           displayID: CGDirectDisplayID) {
        guard backupURL(for: displayID) == nil,
              let current = NSWorkspace.shared.desktopImageURL(for: screen),
              !isMirageGenerated(current) else { return }
        var values = storedBackups()
        values[backupKey(for: displayID)] = current
        saveBackups(values)
        NSLog("[Mirage] 已备份原桌面图片: \(current.lastPathComponent) (显示器=\(displayID))")
    }

    // MARK: - Restoring

    /// Called on quit. A persistent override is meant to outlive the app, so
    /// only a transient one is undone.
    ///
    /// The live setting is the authority, not the stored marker: the marker
    /// exists for the *next* process (which cannot ask a running app anything),
    /// while this one can still read the real value. Deciding from the marker
    /// alone would restore an override the user asked to keep if the two ever
    /// disagreed.
    func restoreIfTransient() {
        guard !preserveForDynamicLockScreen else { return }
        guard !isEnabled else {
            // Leave the desktop alone, but make sure what the next launch finds
            // says "keep", so recovery does not undo a wanted override.
            if mode == .transient { mode = .persistent }
            return
        }
        guard mode != .none else { return }
        restore()
    }

    /// Puts every screen back to the user's own picture and removes our files.
    ///
    /// Synchronous throughout: the quit path calls this and returns straight
    /// into process exit, so anything deferred to a queue would never run.
    func restore() {
        pendingCapture.values.forEach { $0.cancel() }
        pendingCapture.removeAll()
        pendingPrune?.cancel()
        pendingPrune = nil
        captureRequests.removeAll()
        pendingTargets.removeAll()
        var backups = storedBackups()
        var installedOverrides = storedInstalledOverrides()
        var outstanding = storedOverrideDisplayKeys()
        outstanding.formUnion(backups.keys)
        outstanding.formUnion(installedOverrides.keys)
        for displayID in installedByScreen.keys {
            outstanding.insert(backupKey(for: displayID))
        }
        for screen in NSScreen.screens {
            guard let displayID = (screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value else { continue }
            // Only touch screens Mirage actually took over; a screen showing
            // something else is the user's business.
            //
            // Keep only a short allowance for a just-written picture that has
            // not appeared in the system's eventually consistent readback.
            // Later external choices belong to the user, even in this process.
            let key = backupKey(for: displayID)
            let installed = installedByScreen[displayID]
            let current = NSWorkspace.shared.desktopImageURL(for: screen)
            let recorded = outstanding.contains(key)
            let recent = recentInstalls[displayID]
            guard DesktopRestoreOwnership.shouldRestore(
                current: current, installed: installed, previous: recent?.previous,
                recentWriteAge: recent.map { ProcessInfo.processInfo.systemUptime - $0.uptime },
                recorded: recorded, isCurrentGenerated: current.map(isMirageGenerated) == true
            ) else {
                outstanding.remove(key)
                backups.removeValue(forKey: key)
                installedOverrides.removeValue(forKey: key)
                installedByScreen.removeValue(forKey: displayID)
                recentInstalls.removeValue(forKey: displayID)
                continue
            }
            outstanding.insert(key)
            if setDesktopImage(backups[key] ?? Self.systemFallbackPicture(), for: screen) {
                outstanding.remove(key)
                backups.removeValue(forKey: key)
                installedByScreen.removeValue(forKey: displayID)
                recentInstalls.removeValue(forKey: displayID)
                installedOverrides.removeValue(forKey: key)
            }
        }
        saveBackups(backups)
        saveInstalledOverrides(installedOverrides)
        saveOverrideDisplayKeys(outstanding)
        defaults.removeObject(forKey: Key.backup)
        if outstanding.isEmpty {
            mode = .none
            installedByScreen.removeAll()
            recentInstalls.removeAll()
            ioQueue.sync {}
        } else {
            mode = .transient
            ioQueue.sync {}
        }
        defaults.synchronize()
        let keep = Set(installedOverrides.values.map { $0.resolvingSymlinksInPath() })
        ioQueue.sync {
            pruneAllExceptNow(keep)
        }
        cleanSystemCacheIfEnabled(synchronously: true)
    }

    /// The user's backed-up picture, or a system one when the backup is missing
    /// or its file is gone. Never returns a path inside our own directory.
    private func restoreTarget() -> URL {
        if let backup = defaults.url(forKey: Key.backup),
           !isMirageGenerated(backup),
           FileManager.default.fileExists(atPath: backup.path) {
            return backup
        }
        return Self.systemFallbackPicture()
    }

    private func backupKey(for displayID: CGDirectDisplayID) -> String {
        DisplayRegistry.shared.key(forDisplay: displayID)?.rawValue ?? "display:\(displayID)"
    }

    private func storedBackups() -> [String: URL] {
        guard let data = defaults.data(forKey: Key.backups),
              let values = try? JSONDecoder().decode([String: URL].self, from: data) else {
            return [:]
        }
        return values.filter {
            !isMirageGenerated($0.value) &&
                FileManager.default.fileExists(atPath: $0.value.path)
        }
    }

    private func saveBackups(_ values: [String: URL]) {
        if values.isEmpty {
            defaults.removeObject(forKey: Key.backups)
        } else if let data = try? JSONEncoder().encode(values) {
            defaults.set(data, forKey: Key.backups)
        }
        defaults.synchronize()
    }

    private func backupURL(for displayID: CGDirectDisplayID) -> URL? {
        storedBackups()[backupKey(for: displayID)]
    }

    private func storedInstalledOverrides() -> [String: URL] {
        guard let data = defaults.data(forKey: Key.installed),
              let values = try? JSONDecoder().decode([String: URL].self, from: data) else {
            return [:]
        }
        return values
    }

    private func saveInstalledOverrides(_ values: [String: URL]) {
        if values.isEmpty {
            defaults.removeObject(forKey: Key.installed)
        } else if let data = try? JSONEncoder().encode(values) {
            defaults.set(data, forKey: Key.installed)
        }
        defaults.synchronize()
    }

    private func installedURL(for displayID: CGDirectDisplayID) -> URL? {
        installedByScreen[displayID] ?? storedInstalledOverrides()[backupKey(for: displayID)]
    }

    private func reconcileInstalledOverrides() {
        var values = storedInstalledOverrides().filter {
            isGeneratedOverride($0.value) && FileManager.default.fileExists(atPath: $0.value.path)
        }
        for screen in NSScreen.screens {
            guard let displayID = (screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value,
                  let current = NSWorkspace.shared.desktopImageURL(for: screen),
                  isGeneratedOverride(current),
                  FileManager.default.fileExists(atPath: current.path) else { continue }
            values[backupKey(for: displayID)] = current
            installedByScreen[displayID] = current
        }
        saveInstalledOverrides(values)
        var displayKeys = storedOverrideDisplayKeys()
        displayKeys.formIntersection(Set(values.keys).union(storedBackups().keys))
        displayKeys.formUnion(values.keys)
        saveOverrideDisplayKeys(displayKeys)
    }

    func dynamicLockScreenFallbackURL(forDisplay displayID: CGDirectDisplayID) -> URL? {
        if isEnabled,
           let installed = installedByScreen[displayID],
           FileManager.default.fileExists(atPath: installed.path) {
            return installed
        }
        if !isEnabled, let backup = backupURL(for: displayID) {
            return backup
        }
        if let screen = screen(for: displayID),
           let current = NSWorkspace.shared.desktopImageURL(for: screen) {
            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: current.path, isDirectory: &isDirectory),
               !isDirectory.boolValue,
               isEnabled || !isMirageGenerated(current) {
                return current
            }
        }
        let fallback = Self.systemFallbackPicture()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: fallback.path, isDirectory: &isDirectory),
              !isDirectory.boolValue else { return nil }
        return fallback
    }

    func dynamicLockScreenSystemFallbackURL(forDisplay displayID: CGDirectDisplayID) -> URL? {
        if let backup = backupURL(for: displayID) {
            return backup
        }
        if let screen = screen(for: displayID),
           let current = NSWorkspace.shared.desktopImageURL(for: screen),
           !isMirageGenerated(current) {
            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: current.path, isDirectory: &isDirectory),
               !isDirectory.boolValue {
                return current
            }
        }
        let fallback = Self.systemFallbackPicture()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: fallback.path, isDirectory: &isDirectory),
              !isDirectory.boolValue else { return nil }
        return fallback
    }

    private func storedOverrideDisplayKeys() -> Set<String> {
        Set(defaults.stringArray(forKey: Key.displays) ?? [])
    }

    private func saveOverrideDisplayKeys(_ values: Set<String>) {
        if values.isEmpty {
            defaults.removeObject(forKey: Key.displays)
        } else {
            defaults.set(values.sorted(), forKey: Key.displays)
        }
        defaults.synchronize()
    }

    private func migrateLegacyBackup() {
        guard let legacy = defaults.url(forKey: Key.backup),
              !isMirageGenerated(legacy),
              FileManager.default.fileExists(atPath: legacy.path),
              let mainID = DisplayRegistry.shared.mainKey.flatMap({
                  DisplayRegistry.shared.displayID(for: $0)
              }) else { return }
        var values = storedBackups()
        if values[backupKey(for: mainID)] == nil {
            values[backupKey(for: mainID)] = legacy
            saveBackups(values)
        }
        defaults.removeObject(forKey: Key.backup)
        defaults.synchronize()
    }

    /// Picked by enumeration rather than a hardcoded name: the bundled set is
    /// renamed with every macOS release (and macOS 26 has no `Solid Colors/`).
    private static func systemFallbackPicture() -> URL {
        let directory = URL(filePath: "/System/Library/Desktop Pictures")
        let candidates = (try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil,
            options: .skipsHiddenFiles)) ?? []
        let heic = candidates
            .filter { $0.pathExtension.lowercased() == "heic" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        return heic.first ?? directory
    }

    // MARK: - Files

    private func isGeneratedOverride(_ url: URL) -> Bool {
        url.resolvingSymlinksInPath().path
            .hasPrefix(directory.resolvingSymlinksInPath().path + "/")
    }

    /// A still written by the pre-2026-08 placeholder implementation.
    private func isLegacyPlaceholder(_ url: URL) -> Bool {
        url.lastPathComponent.hasPrefix("staticWP_")
    }

    private func isDynamicLockScreenFallback(_ url: URL) -> Bool {
        guard let container = FileManager.default.containerURL(
            forSecurityApplicationGroupIdentifier: "group.cn.laobamac.Mirage")
        else { return false }
        let directory = container.appendingPathComponent(
            "DynamicLockScreen/DesktopFallbacks", isDirectory: true)
        return url.resolvingSymlinksInPath().path
            .hasPrefix(directory.resolvingSymlinksInPath().path + "/")
    }

    /// Any file Mirage generated, past or present. Used everywhere a value must
    /// not be mistaken for the user's own picture — above all the backup key,
    /// which is what a restore ultimately points the desktop back at.
    private func isMirageGenerated(_ url: URL) -> Bool {
        isGeneratedOverride(url) || isLegacyPlaceholder(url)
            || isDynamicLockScreenFallback(url)
    }

    private func canonicalizeSnapshot(at url: URL) -> URL {
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { return url }
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let canonical = directory.appending(path: "override-\(digest).heic")
        pendingTargets.remove(url)
        if canonical != url {
            if FileManager.default.fileExists(atPath: canonical.path) {
                removeCaptureArtifacts(for: url)
            } else {
                do {
                    try FileManager.default.moveItem(at: url, to: canonical)
                    removeCaptureCompanions(for: url)
                } catch {
                    pendingTargets.insert(url)
                    return url
                }
            }
        }
        pendingTargets.insert(canonical)
        return canonical
    }

    private func removeCaptureCompanions(for url: URL) {
        try? FileManager.default.removeItem(at: url.appendingPathExtension("json"))
        guard let entries = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil) else { return }
        let prefix = ".\(url.lastPathComponent)-"
        for entry in entries where entry.lastPathComponent.hasPrefix(prefix) {
            try? FileManager.default.removeItem(at: entry)
        }
    }

    private func removeCaptureArtifacts(for url: URL) {
        try? FileManager.default.removeItem(at: url)
        removeCaptureCompanions(for: url)
    }

    private func schedulePrune() {
        pendingPrune?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.pendingPrune = nil
            self.pruneAllExcept([])
        }
        pendingPrune = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2, execute: work)
    }

    private func pruneAllExcept(_ keep: Set<URL>) {
        let persisted = storedInstalledOverrides().values
        let protected = Set((Array(persisted) + Array(installedByScreen.values)
            + Array(pendingTargets)).map {
            $0.resolvingSymlinksInPath()
        })
        ioQueue.async { [weak self] in
            guard let self else { return }
            self.pruneAllExceptNow(keep.union(protected))
        }
    }

    private func pruneAllExceptNow(_ keep: Set<URL>) {
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil) else { return }
        for url in urls where !keep.contains(url.resolvingSymlinksInPath()) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private var systemCacheDirectory: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/Containers/com.apple.wallpaper.agent/Data/Library/Caches/com.apple.wallpaper.caches/extension-com.apple.wallpaper.extension.image")
    }

    private var supportsSystemCacheCleaning: Bool {
        ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 26
    }

    private func cacheHash(for url: URL) -> String {
        SHA256.hash(data: Data(url.path.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    private func storedOwnedCacheHashes() -> Set<String> {
        ownedCacheHashesLock.lock()
        defer { ownedCacheHashesLock.unlock() }
        return Set(defaults.stringArray(forKey: Key.ownedCacheHashes) ?? [])
    }

    private func updateOwnedCacheHashes(_ update: (inout Set<String>) -> Void) {
        ownedCacheHashesLock.lock()
        defer { ownedCacheHashesLock.unlock() }
        var values = Set(defaults.stringArray(forKey: Key.ownedCacheHashes) ?? [])
        let previous = values
        update(&values)
        guard values != previous else { return }
        if values.isEmpty {
            defaults.removeObject(forKey: Key.ownedCacheHashes)
        } else {
            defaults.set(values.sorted(), forKey: Key.ownedCacheHashes)
        }
        defaults.synchronize()
    }

    private func registerOwnedCacheURL(_ url: URL) {
        guard isGeneratedOverride(url) else { return }
        let hash = cacheHash(for: url)
        updateOwnedCacheHashes { values in
            values.insert(hash)
        }
    }

    private func registerExistingOverrideHashes() {
        var discovered: Set<String> = []
        if let urls = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil) {
            for url in urls where url.pathExtension.lowercased() == "heic" {
                discovered.insert(cacheHash(for: url))
            }
        }
        for url in storedInstalledOverrides().values where isGeneratedOverride(url) {
            discovered.insert(cacheHash(for: url))
        }
        updateOwnedCacheHashes { values in
            values.formUnion(discovered)
        }
    }

    private func cacheDirectoryAccess() -> (URL, Bool)? {
        let expected = systemCacheDirectory.standardizedFileURL
        if let data = defaults.data(forKey: Key.cacheBookmark) {
            var stale = false
            if let url = try? URL(
                resolvingBookmarkData: data,
                options: .withSecurityScope,
                relativeTo: nil,
                bookmarkDataIsStale: &stale
            ), url.standardizedFileURL == expected {
                let accessed = url.startAccessingSecurityScopedResource()
                if stale,
                   let refreshed = try? url.bookmarkData(
                    options: .withSecurityScope,
                    includingResourceValuesForKeys: nil,
                    relativeTo: nil) {
                    defaults.set(refreshed, forKey: Key.cacheBookmark)
                }
                return (url, accessed)
            }
        }
        guard FileManager.default.isReadableFile(atPath: expected.path),
              FileManager.default.isWritableFile(atPath: expected.path) else { return nil }
        return (expected, false)
    }

    private func currentCacheHashes() -> Set<String> {
        var urls = Set(storedInstalledOverrides().values)
        urls.formUnion(installedByScreen.values)
        urls.formUnion(NSScreen.screens.compactMap {
            NSWorkspace.shared.desktopImageURL(for: $0)
        }.filter(isGeneratedOverride))
        return Set(urls.map(cacheHash(for:)))
    }

    private func cleanSystemCache(
        force: Bool,
        synchronously: Bool = false,
        report: Bool = false
    ) {
        guard supportsSystemCacheCleaning else {
            if report { presentCleanupResult(count: 0, bytes: 0) }
            return
        }
        if !force && !AppDelegate.shared.globalSettingsViewModel.settings
            .shouldAutomaticallyCleanWallpaperCache { return }
        let staleHashes = storedOwnedCacheHashes().subtracting(currentCacheHashes())
        if staleHashes.isEmpty {
            if report { presentCleanupResult(count: 0, bytes: 0) }
            return
        }
        guard let (cacheDirectory, accessed) = cacheDirectoryAccess() else {
            if report {
                DispatchQueue.main.async {
                    let alert = NSAlert()
                    alert.messageText = NSLocalizedString(
                        "无法保存壁纸缓存目录授权", comment: "")
                    alert.informativeText = NSLocalizedString(
                        "Mirage 无法保存对该目录的访问权限。", comment: "")
                    alert.addButton(withTitle: NSLocalizedString("确定", comment: ""))
                    alert.runModal()
                }
            } else if !staleHashes.isEmpty {
                promptForCacheAccess()
            }
            return
        }
        let operation = { [weak self] in
            guard let self else { return }
            var removedCount = 0
            var removedBytes: Int64 = 0
            var matchedHashes: Set<String> = []
            var failedHashes: Set<String> = []
            if let urls = try? FileManager.default.contentsOfDirectory(
                at: cacheDirectory,
                includingPropertiesForKeys: [.fileSizeKey],
                options: .skipsHiddenFiles) {
                let hexadecimal = CharacterSet(charactersIn: "0123456789abcdef")
                for url in urls where url.pathExtension.lowercased() == "bmp" {
                    let name = url.deletingPathExtension().lastPathComponent
                    guard let separator = name.firstIndex(of: "-") else { continue }
                    let prefix = String(name[..<separator]).lowercased()
                    guard prefix.count == 64,
                          prefix.unicodeScalars.allSatisfy({ hexadecimal.contains($0) }),
                          staleHashes.contains(prefix) else { continue }
                    matchedHashes.insert(prefix)
                    let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize)
                        .map(Int64.init) ?? 0
                    do {
                        try FileManager.default.removeItem(at: url)
                        removedCount += 1
                        removedBytes += size
                    } catch {
                        failedHashes.insert(prefix)
                        NSLog("[Mirage] 清理 macOS 壁纸缓存失败: \(error.localizedDescription)")
                    }
                }
            }
            let completedHashes = matchedHashes.subtracting(failedHashes)
            if !completedHashes.isEmpty {
                self.updateOwnedCacheHashes { values in
                    values.subtract(completedHashes)
                }
            }
            if accessed {
                cacheDirectory.stopAccessingSecurityScopedResource()
            }
            if removedCount > 0 {
                NSLog("[Mirage] 已清理 \(removedCount) 个 macOS 壁纸缓存文件")
            }
            if report {
                self.presentCleanupResult(count: removedCount, bytes: removedBytes)
            }
        }
        if synchronously {
            ioQueue.sync(execute: operation)
        } else {
            ioQueue.async(execute: operation)
        }
    }

    private func presentCleanupResult(count: Int, bytes: Int64) {
        DispatchQueue.main.async {
            let size = ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
            let alert = NSAlert()
            alert.messageText = NSLocalizedString("壁纸缓存清理完成", comment: "")
            alert.informativeText = String(
                format: NSLocalizedString("已删除 %d 个缓存文件，释放 %@。", comment: ""),
                count,
                size)
            alert.addButton(withTitle: NSLocalizedString("确定", comment: ""))
            alert.runModal()
        }
    }

    private func promptForCacheAccess() {
        guard !cacheAccessPromptShown else { return }
        cacheAccessPromptShown = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            let alert = NSAlert()
            alert.messageText = NSLocalizedString("需要访问壁纸缓存", comment: "")
            alert.informativeText = NSLocalizedString(
                "macOS 26 不会自动清理壁纸缓存。Mirage 需要你授权后，才能删除自己产生且已不再使用的缓存。",
                comment: "")
            alert.addButton(withTitle: NSLocalizedString("授权访问", comment: ""))
            alert.addButton(withTitle: NSLocalizedString("稍后", comment: ""))
            alert.addButton(withTitle: NSLocalizedString("关闭自动清理", comment: ""))
            let response = alert.runModal()
            if response == .alertFirstButtonReturn {
                self.requestSystemCacheAccessAndClean()
            } else if response == .alertThirdButtonReturn {
                let viewModel = AppDelegate.shared.globalSettingsViewModel
                viewModel.settings.automaticWallpaperCacheCleaning = false
                viewModel.save()
            }
        }
    }

    private func cleanSystemCacheIfEnabled(synchronously: Bool = false) {
        cleanSystemCache(force: false, synchronously: synchronously)
    }

    func requestSystemCacheAccessAndClean() {
        guard supportsSystemCacheCleaning else {
            presentCleanupResult(count: 0, bytes: 0)
            return
        }
        if storedOwnedCacheHashes().subtracting(currentCacheHashes()).isEmpty {
            presentCleanupResult(count: 0, bytes: 0)
            return
        }
        if let (directory, accessed) = cacheDirectoryAccess() {
            if accessed {
                directory.stopAccessingSecurityScopedResource()
            }
            cleanSystemCache(force: true, report: true)
            return
        }
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = false
        panel.directoryURL = systemCacheDirectory.deletingLastPathComponent()
        panel.message = NSLocalizedString("请选择 Wallpaper Agent 的图像缓存目录。", comment: "")
        panel.prompt = NSLocalizedString("授权", comment: "")
        panel.begin { [weak self] response in
            guard response == .OK, let self, let selected = panel.url else { return }
            guard selected.standardizedFileURL == self.systemCacheDirectory.standardizedFileURL else {
                let alert = NSAlert()
                alert.messageText = NSLocalizedString("选择的目录不正确", comment: "")
                alert.informativeText = String(
                    format: NSLocalizedString("请选择以下目录：\n%@", comment: ""),
                    self.systemCacheDirectory.path)
                alert.addButton(withTitle: NSLocalizedString("确定", comment: ""))
                alert.runModal()
                return
            }
            guard let bookmark = try? selected.bookmarkData(
                options: .withSecurityScope,
                includingResourceValuesForKeys: nil,
                relativeTo: nil) else {
                let alert = NSAlert()
                alert.messageText = NSLocalizedString("无法保存壁纸缓存目录授权", comment: "")
                alert.informativeText = NSLocalizedString(
                    "Mirage 无法保存对该目录的访问权限。", comment: "")
                alert.addButton(withTitle: NSLocalizedString("确定", comment: ""))
                alert.runModal()
                return
            }
            self.defaults.set(bookmark, forKey: Key.cacheBookmark)
            self.defaults.synchronize()
            self.cleanSystemCache(force: true, report: true)
        }
    }

    @discardableResult
    private func setDesktopImage(_ url: URL, for screen: NSScreen) -> Bool {
        registerOwnedCacheURL(url)
        do {
            try NSWorkspace.shared.setDesktopImageURL(url, for: screen)
            return true
        } catch {
            NSLog("[Mirage] 设置桌面图片失败: \(error.localizedDescription)")
            return false
        }
    }

    private func verifyDesktopImage(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                                    screen: NSScreen, attempt: Int = 0) {
        let delays: [TimeInterval] = [0.08, 0.25, 0.6]
        guard attempt < delays.count else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + delays[attempt]) { [weak self] in
            guard let self, self.installedByScreen[displayID] == url else { return }
            let current = NSWorkspace.shared.desktopImageURL(for: screen)?.resolvingSymlinksInPath()
            if current == url.resolvingSymlinksInPath() {
                self.recentInstalls.removeValue(forKey: displayID)
                self.pruneAllExcept(Set(self.installedByScreen.values.map {
                    $0.resolvingSymlinksInPath()
                }))
                return
            }
            let recent = self.recentInstalls[displayID]
            guard DesktopRestoreOwnership.shouldRestore(
                current: current, installed: url, previous: recent?.previous,
                recentWriteAge: recent.map { ProcessInfo.processInfo.systemUptime - $0.uptime },
                recorded: true, isCurrentGenerated: current.map(self.isMirageGenerated) == true
            ) else {
                self.externallyChanged.insert(displayID)
                self.installedByScreen.removeValue(forKey: displayID)
                self.recentInstalls.removeValue(forKey: displayID)
                NSLog("[DesktopOverride] External desktop change on display %u; stopped retrying", displayID)
                return
            }
            self.setDesktopImage(url, for: screen)
            self.verifyDesktopImage(url, forDisplay: displayID, screen: screen,
                                    attempt: attempt + 1)
        }
    }

    // MARK: - Settings changes

    /// ON  → keep overriding forever (and take a still right now).
    /// OFF → keep overriding for the rest of this run for tint consistency,
    ///       but owe a restore on quit.
    func didChangeEnabled(_ enabled: Bool) {
        if mode != .none {
            mode = enabled ? .persistent : .transient
        }
        if preserveForDynamicLockScreen && !enabled {
            Task { @MainActor in
                DynamicLockScreenManager.shared.restoreSystemDesktopFallbacks()
            }
        }
        guard enabled else { return }
        scheduleCaptureForAllScreens()
    }

    func didChangeCacheCleaningEnabled(_ enabled: Bool) {
        guard enabled else { return }
        cleanSystemCacheIfEnabled()
    }
}
