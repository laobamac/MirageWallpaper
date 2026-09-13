//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa

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
    /// What we put on each screen. Authoritative for pruning: reading it back
    /// from `desktopImageURL(for:)` races WallpaperAgent's own bookkeeping.
    private var installedByScreen: [CGDirectDisplayID: URL] = [:]
    private static let captureRetryDelays: [TimeInterval] = [1.0, 2.0, 4.0, 6.0, 8.0, 10.0]
    private var pendingTargets: Set<URL> = []

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

        if mode == .transient && !preserveForDynamicLockScreen {
            // A transient marker cannot legitimately survive its own process:
            // the previous run was killed before it could restore.
            NSLog("[Mirage] 检测到上次运行未正常还原桌面图片，正在还原")
            restore()
        }

        repairDanglingDesktopPointer()
        pruneUnreferencedOverridesAtLaunch()
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
        guard storedOverrideDisplayKeys().isEmpty else { return }
        let keep = Set(NSScreen.screens.compactMap {
            NSWorkspace.shared.desktopImageURL(for: $0)
        }.filter(isMirageGenerated).map { $0.resolvingSymlinksInPath() })
        ioQueue.async { [weak self] in
            self?.pruneAllExceptNow(keep)
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
        // A new UUID every time: WallpaperAgent caches by path, so rewriting the
        // bytes under a path it already displays does not repaint.
        let target = directory.appending(path: "override-\(UUID().uuidString).heic")
        pendingTargets.insert(target)
        AppDelegate.shared.wallpaperViewModel.renderer.snapshot(
            onDisplay: displayID, path: target.path
        ) { [weak self] ok in
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                guard self.captureRequests[displayID] == request else {
                    try? FileManager.default.removeItem(at: target)
                    self.pendingTargets.remove(target)
                    return
                }
                let current = AppDelegate.shared.wallpaperViewModel.renderer
                    .currentWallpaper(onDisplay: displayID)
                guard current?.id == request.wallpaperID else {
                    try? FileManager.default.removeItem(at: target)
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
                    self.install(target, forDisplay: displayID, request: request,
                                 attempt: attempt)
                    return
                }
                try? FileManager.default.removeItem(at: target)
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
                try? FileManager.default.removeItem(at: url)
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
            try? FileManager.default.removeItem(at: url)
        }
    }

    /// Points `screenIndex` at `url`, recording the user's own picture first and
    /// deleting every override file that is no longer displayed.
    private func install(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                         request: CaptureRequest, attempt: Int) {
        guard captureRequests[displayID] == request else {
            try? FileManager.default.removeItem(at: url)
            pendingTargets.remove(url)
            return
        }
        if preserveForDynamicLockScreen {
            installDynamicLockScreenFallback(
                url, forDisplay: displayID, request: request)
            return
        }
        guard let screen = screen(for: displayID) else {
            try? FileManager.default.removeItem(at: url)
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
        var displayKeys = storedOverrideDisplayKeys()
        displayKeys.insert(backupKey(for: displayID))
        saveOverrideDisplayKeys(displayKeys)
        guard setDesktopImage(url, for: screen) else {
            displayKeys.remove(backupKey(for: displayID))
            saveOverrideDisplayKeys(displayKeys)
            try? FileManager.default.removeItem(at: url)
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
        captureRequests[displayID] = nil
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
        captureRequests.removeAll()
        pendingTargets.removeAll()
        var backups = storedBackups()
        var outstanding = storedOverrideDisplayKeys()
        outstanding.formUnion(backups.keys)
        for displayID in installedByScreen.keys {
            outstanding.insert(backupKey(for: displayID))
        }
        for screen in NSScreen.screens {
            guard let displayID = (screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value else { continue }
            // Only touch screens Mirage actually took over; a screen showing
            // something else is the user's business.
            //
            // `installedByScreen` is checked FIRST and is authoritative for this
            // process. `desktopImageURL(for:)` on macOS 26 is only eventually
            // consistent — right after a write it still reports the previous
            // picture — so trusting it alone would skip the restore for an
            // override installed moments before the user quit. The read-back is
            // still needed for the crash-recovery case, where a previous process
            // installed the override and this one has no record of it (by then
            // the store has long settled).
            let key = backupKey(for: displayID)
            let installed = installedByScreen[displayID] != nil
            let current = NSWorkspace.shared.desktopImageURL(for: screen)
            let recorded = outstanding.contains(key)
            if !installed, let current, !isMirageGenerated(current) {
                outstanding.remove(key)
                backups.removeValue(forKey: key)
                continue
            }
            guard installed || recorded || current.map(isMirageGenerated) == true else { continue }
            outstanding.insert(key)
            if setDesktopImage(backups[key] ?? Self.systemFallbackPicture(), for: screen) {
                outstanding.remove(key)
                backups.removeValue(forKey: key)
                installedByScreen.removeValue(forKey: displayID)
            }
        }
        saveBackups(backups)
        saveOverrideDisplayKeys(outstanding)
        defaults.removeObject(forKey: Key.backup)
        if outstanding.isEmpty {
            mode = .none
            installedByScreen.removeAll()
            ioQueue.sync {}
        } else {
            mode = .transient
            ioQueue.sync {}
        }
        defaults.synchronize()
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

    /// Keeps only the files currently on screen — one per active display — so
    /// the directory cannot grow the way the old cache deliberately did.
    private func pruneAllExcept(_ keep: Set<URL>) {
        let activeDisplayKeys = Set(installedByScreen.keys.map(backupKey(for:)))
        guard storedOverrideDisplayKeys().isSubset(of: activeDisplayKeys) else { return }
        let protected = Set((Array(installedByScreen.values) + Array(pendingTargets)).map {
            $0.resolvingSymlinksInPath()
        })
        ioQueue.async { [weak self] in
            guard let self else { return }
            self.pruneAllExceptNow(keep.union(protected))
        }
    }

    private func pruneAllExceptNow(_ keep: Set<URL>) {
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil,
            options: .skipsHiddenFiles) else { return }
        for url in urls where !keep.contains(url.resolvingSymlinksInPath()) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    @discardableResult
    private func setDesktopImage(_ url: URL, for screen: NSScreen) -> Bool {
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
                self.pruneAllExcept(Set(self.installedByScreen.values.map {
                    $0.resolvingSymlinksInPath()
                }))
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
}
