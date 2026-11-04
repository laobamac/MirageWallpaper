//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import CryptoKit
import ImageIO

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
        static let backups = "DesktopOverrideBackups"
        static let displays = "DesktopOverrideDisplays"
        static let installed = "DesktopOverrideInstalled"
        static let pendingInstalled = "DesktopOverridePendingInstalled"
        static let verifiedBackups = "DesktopOverrideVerifiedBackups"
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
    private var deferredCaptures: [CGDirectDisplayID: WEWallpaper] = [:]
    private var installedByScreen: [CGDirectDisplayID: URL] = [:]
    private static let captureRetryDelays: [TimeInterval] = [1.0, 2.0, 4.0, 6.0, 8.0, 10.0]
    private var pendingTargets: Set<URL> = [] {
        didSet {
            fileLifecycleLock.lock()
            protectedCaptureTargets = pendingTargets
            fileLifecycleLock.unlock()
        }
    }
    private var protectedCaptureTargets: Set<URL> = []
    private var unpublishedTargets: Set<URL> = []
    private var pendingPrune: DispatchWorkItem?
    private var cacheAccessPromptShown = false
    private let ownedCacheHashesLock = NSLock()
    private let fileLifecycleLock = NSRecursiveLock()
    private var isTerminating = false
    private var isRestoring = false
    private var restoreGeneration = UUID()
    private var shouldPruneLegacy = true

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
        registerExistingOverrideHashes()
        reconcileInstalledOverrides()
        repairDanglingDesktopPointer()
        if mode == .transient && !preserveForDynamicLockScreen {
            restore()
        } else {
            pruneAllExcept([])
            cleanSystemCacheIfEnabled()
        }
    }

    private func repairDanglingDesktopPointer() {
        guard let store = DesktopWallpaperStore.read() else { return }
        for screen in NSScreen.screens {
            guard let displayID = displayID(for: screen),
                  let uuid = displayUUID(for: displayID),
                  let current = store.picture(for: uuid)?.url,
                  isMirageGenerated(current),
                  !FileManager.default.fileExists(atPath: current.path) else { continue }
            let key = backupKey(for: displayID)
            if let backup = storedBackupRecords()[key], isUsablePicture(backup.url) {
                setDesktopImage(backup.url, for: screen, options: backup.options.workspaceOptions)
            } else if let installed = storedInstalledOverrides()[key], isUsablePicture(installed) {
                var pending = storedPendingOverrides()
                pending[key] = installed
                savePendingOverrides(pending)
                setDesktopImage(installed, for: screen)
            }
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
        if isTerminating || isRestoring || UserDefaults.standard.bool(forKey: "Mirage.DynamicLockScreen.Locked") { return }
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
        if isTerminating || isRestoring || UserDefaults.standard.bool(forKey: "Mirage.DynamicLockScreen.Locked") { return }
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
        guard captureRequests[displayID] == request, !isTerminating, !isRestoring else { return }
        guard let screen = screen(for: displayID) else {
            captureRequests[displayID] = nil
            return
        }
        if !preserveForDynamicLockScreen {
            if let pending = storedPendingOverrides()[backupKey(for: displayID)] {
                deferredCaptures[displayID] = wallpaper
                captureRequests[displayID] = nil
                verifyDesktopImage(pending, forDisplay: displayID)
                return
            }
            guard backUpUserPictureIfNeeded(on: screen, displayID: displayID) else {
                captureRequests[displayID] = nil
                return
            }
        }
        let target = directory.appending(path: "override-\(UUID().uuidString).heic")
        pendingTargets.insert(target)
        unpublishedTargets.insert(target)
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
                        == canonical.resolvingSymlinksInPath(),
                       let uuid = self.displayUUID(for: displayID),
                       DesktopWallpaperStore.read()?.matches(canonical, for: uuid) == true {
                        self.pendingTargets.remove(canonical)
                        self.captureRequests[displayID] = nil
                        self.schedulePrune()
                        self.cleanSystemCacheIfEnabled()
                        return
                    }
                    self.install(canonical, forDisplay: displayID, request: request)
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
                self.install(url, forDisplay: displayID, request: request)
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

    private func install(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                         request: CaptureRequest) {
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
        guard captureRequests[displayID] == request, !isTerminating, !isRestoring else {
            pendingTargets.remove(url)
            removeCaptureArtifacts(for: url)
            return
        }
        if preserveForDynamicLockScreen {
            installDynamicLockScreenFallback(url, forDisplay: displayID, request: request)
            return
        }
        guard let screen = screen(for: displayID),
              storedPendingOverrides()[backupKey(for: displayID)] == nil,
              backUpUserPictureIfNeeded(on: screen, displayID: displayID) else {
            pendingTargets.remove(url)
            removeCaptureArtifacts(for: url)
            captureRequests[displayID] = nil
            return
        }
        mode = isEnabled ? .persistent : .transient
        let key = backupKey(for: displayID)
        var displayKeys = storedOverrideDisplayKeys()
        displayKeys.insert(key)
        saveOverrideDisplayKeys(displayKeys)
        var pending = storedPendingOverrides()
        pending[key] = url
        savePendingOverrides(pending)
        setDesktopImage(url, for: screen)
        pendingTargets.remove(url)
        captureRequests[displayID] = nil
        verifyDesktopImage(url, forDisplay: displayID)
    }

    private func displayID(for screen: NSScreen) -> CGDirectDisplayID? {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
    }

    private func displayUUID(for displayID: CGDirectDisplayID) -> String? {
        guard let uuid = CGDisplayCreateUUIDFromDisplayID(displayID)?.takeRetainedValue() else { return nil }
        return CFUUIDCreateString(nil, uuid) as String
    }

    private func isUsablePicture(_ url: URL) -> Bool {
        guard url.isFileURL,
              let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isReadableKey]),
              values.isRegularFile == true, values.isReadable == true,
              let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return false }
        return CGImageSourceGetCount(source) > 0
    }

    @discardableResult
    private func backUpUserPictureIfNeeded(on screen: NSScreen,
                                           displayID: CGDirectDisplayID) -> Bool {
        guard let uuid = displayUUID(for: displayID),
              let store = DesktopWallpaperStore.read(), store.referencedURLs != nil,
              let picture = store.picture(for: uuid) else { return false }
        let key = backupKey(for: displayID)
        if isMirageGenerated(picture.url) {
            return storedOverrideDisplayKeys().contains(key)
                && storedBackupRecords()[key].map { isUsablePicture($0.url) } == true
        }
        guard isUsablePicture(picture.url) else { return false }
        let options: DesktopWallpaperOptions?
        if let storedOptions = picture.options {
            options = storedOptions
        } else if NSWorkspace.shared.desktopImageURL(for: screen)?.resolvingSymlinksInPath() == picture.url,
           let workspaceOptions = NSWorkspace.shared.desktopImageOptions(for: screen) {
            options = DesktopWallpaperOptions(workspaceOptions)
        } else {
            options = nil
        }
        guard let options else { return false }
        var records = storedBackupRecords()
        records[key] = DesktopWallpaperBackup(url: picture.url, options: options,
                                              encodedOptions: picture.encodedOptions)
        saveBackupRecords(records)
        return true
    }

    func prepareForApplicationTermination(completion: @escaping () -> Void) {
        isTerminating = true
        cancelCaptures()
        restoreIfTransient(completion: completion)
    }

    func restoreIfTransient(completion: (() -> Void)? = nil) {
        guard !preserveForDynamicLockScreen, !isEnabled, mode != .none else {
            if isEnabled && mode == .transient { mode = .persistent }
            if let completion { DispatchQueue.main.async(execute: completion) }
            return
        }
        restore(completion: completion)
    }

    private func cancelCaptures() {
        pendingCapture.values.forEach { $0.cancel() }
        pendingCapture.removeAll()
        pendingPrune?.cancel()
        pendingPrune = nil
        captureRequests.removeAll()
        deferredCaptures.removeAll()
    }

    private func restore(completion: (() -> Void)? = nil) {
        cancelCaptures()
        isRestoring = true
        let generation = UUID()
        restoreGeneration = generation
        let initialStore = DesktopWallpaperStore.read()
        let records = storedBackupRecords()
        var targets: [CGDirectDisplayID: DesktopWallpaperBackup] = [:]
        var outstanding = storedOverrideDisplayKeys()
        outstanding.formUnion(storedInstalledOverrides().keys)
        outstanding.formUnion(storedPendingOverrides().keys)
        saveOverrideDisplayKeys(outstanding)
        for screen in NSScreen.screens {
            guard let displayID = displayID(for: screen),
                  let uuid = displayUUID(for: displayID) else { continue }
            let key = backupKey(for: displayID)
            guard outstanding.contains(key),
                  let current = initialStore?.picture(for: uuid) else { continue }
            let pending = storedPendingOverrides()[key]
            if !isMirageGenerated(current.url), pending == nil {
                finishRestoring(key: key, displayID: displayID)
                continue
            }
            guard let backup = records[key], isUsablePicture(backup.url) else { continue }
            if !isMirageGenerated(current.url), current.url != backup.url.resolvingSymlinksInPath() {
                continue
            }
            targets[displayID] = backup
            setDesktopImage(backup.url, for: screen, options: backup.options.workspaceOptions)
        }
        verifyRestoration(targets, initialStore: initialStore, generation: generation,
                          deadline: Date().addingTimeInterval(2), completion: completion)
    }

    private func verifyRestoration(_ targets: [CGDirectDisplayID: DesktopWallpaperBackup],
                                   initialStore: DesktopWallpaperStore?, generation: UUID,
                                   deadline: Date, completion: (() -> Void)?) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            guard let self, self.restoreGeneration == generation else { return }
            var remaining = targets
            if let store = DesktopWallpaperStore.read(), let initialStore {
                for (displayID, backup) in targets {
                    guard let uuid = self.displayUUID(for: displayID),
                          store.matches(backup, for: uuid),
                          !store.hasSameDesktop(as: initialStore, for: uuid) else { continue }
                    self.finishRestoring(key: self.backupKey(for: displayID), displayID: displayID)
                    remaining.removeValue(forKey: displayID)
                }
            }
            if !remaining.isEmpty && Date() < deadline {
                self.verifyRestoration(remaining, initialStore: initialStore, generation: generation,
                                       deadline: deadline, completion: completion)
                return
            }
            self.isRestoring = false
            self.mode = self.storedOverrideDisplayKeys().isEmpty ? .none : .transient
            self.pruneAllExcept([], synchronously: self.isTerminating)
            self.cleanSystemCacheIfEnabled(synchronously: self.isTerminating)
            if let completion { completion() }
            if !self.isTerminating { self.scheduleCaptureForAllScreens() }
        }
    }

    private func finishRestoring(key: String, displayID: CGDirectDisplayID) {
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
        var installed = storedInstalledOverrides()
        installed.removeValue(forKey: key)
        saveInstalledOverrides(installed)
        var pending = storedPendingOverrides()
        pending.removeValue(forKey: key)
        savePendingOverrides(pending)
        var outstanding = storedOverrideDisplayKeys()
        outstanding.remove(key)
        saveOverrideDisplayKeys(outstanding)
        installedByScreen.removeValue(forKey: displayID)
    }

    private func storedBackupRecords() -> [String: DesktopWallpaperBackup] {
        guard let data = defaults.data(forKey: Key.verifiedBackups),
              let records = try? JSONDecoder().decode([String: DesktopWallpaperBackup].self, from: data)
        else { return [:] }
        return records.filter { !isMirageGenerated($0.value.url) }
    }

    private func saveBackupRecords(_ records: [String: DesktopWallpaperBackup]) {
        guard let data = try? JSONEncoder().encode(records) else { return }
        defaults.set(data, forKey: Key.verifiedBackups)
        saveBackups(records.mapValues(\.url))
    }

    private func backupKey(for displayID: CGDirectDisplayID) -> String {
        DisplayRegistry.shared.key(forDisplay: displayID)?.rawValue ?? "display:\(displayID)"
    }

    private func storedBackups() -> [String: URL] {
        storedBackupRecords().mapValues(\.url).filter { isUsablePicture($0.value) }
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
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
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

    private func storedPendingOverrides() -> [String: URL] {
        guard let data = defaults.data(forKey: Key.pendingInstalled),
              let values = try? JSONDecoder().decode([String: URL].self, from: data) else { return [:] }
        return values
    }

    private func savePendingOverrides(_ values: [String: URL]) {
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
        if values.isEmpty {
            defaults.removeObject(forKey: Key.pendingInstalled)
        } else if let data = try? JSONEncoder().encode(values) {
            defaults.set(data, forKey: Key.pendingInstalled)
        }
        defaults.synchronize()
    }

    private func reconcileInstalledOverrides() {
        guard let store = DesktopWallpaperStore.read() else { return }
        var values = storedInstalledOverrides()
        for screen in NSScreen.screens {
            guard let displayID = displayID(for: screen),
                  let uuid = displayUUID(for: displayID),
                  let current = store.picture(for: uuid)?.url,
                  isGeneratedOverride(current), isUsablePicture(current) else { continue }
            values[backupKey(for: displayID)] = current
            installedByScreen[displayID] = current
        }
        saveInstalledOverrides(values)
        var keys = storedOverrideDisplayKeys()
        keys.formUnion(values.keys)
        keys.formUnion(storedPendingOverrides().keys)
        saveOverrideDisplayKeys(keys)
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
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
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
                    unpublishedTargets.remove(url)
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
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
        if unpublishedTargets.remove(url) == nil {
            guard let protected = protectedOverrideURLs(includeCaptures: false),
                  !protected.contains(url.resolvingSymlinksInPath()) else { return }
        }
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

    private func protectedOverrideURLs(includeCaptures: Bool = true) -> Set<URL>? {
        guard let references = DesktopWallpaperStore.read()?.referencedURLs else { return nil }
        var protected = references
        protected.formUnion(storedInstalledOverrides().values.map { $0.resolvingSymlinksInPath() })
        protected.formUnion(storedPendingOverrides().values.map { $0.resolvingSymlinksInPath() })
        protected.formUnion(installedByScreen.values.map { $0.resolvingSymlinksInPath() })
        protected.formUnion(NSScreen.screens.compactMap {
            NSWorkspace.shared.desktopImageURL(for: $0)?.resolvingSymlinksInPath()
        })
        if includeCaptures {
            protected.formUnion(pendingTargets.map { $0.resolvingSymlinksInPath() })
        }
        return protected
    }

    private func pruneAllExcept(_ keep: Set<URL>, synchronously: Bool = false) {
        let workspaceURLs = NSScreen.screens.compactMap {
            NSWorkspace.shared.desktopImageURL(for: $0)?.resolvingSymlinksInPath()
        }
        let operation = { [weak self] in
            guard let self else { return }
            self.fileLifecycleLock.lock()
            defer { self.fileLifecycleLock.unlock() }
            guard let references = DesktopWallpaperStore.read()?.referencedURLs else { return }
            let allProtected = references.union(keep).union(workspaceURLs)
                .union(self.storedInstalledOverrides().values.map { $0.resolvingSymlinksInPath() })
                .union(self.storedPendingOverrides().values.map { $0.resolvingSymlinksInPath() })
                .union(self.protectedCaptureTargets.map { $0.resolvingSymlinksInPath() })
            guard let urls = try? FileManager.default.contentsOfDirectory(
                at: self.directory, includingPropertiesForKeys: nil) else { return }
            for url in urls {
                let name = url.lastPathComponent
                guard name.hasPrefix("override-") || name.hasPrefix(".override-") else { continue }
                guard !allProtected.contains(url.resolvingSymlinksInPath()),
                      !allProtected.contains(where: {
                          name == $0.lastPathComponent + ".json" || name.hasPrefix("." + $0.lastPathComponent + "-")
                      }) else { continue }
                try? FileManager.default.removeItem(at: url)
            }
            guard self.shouldPruneLegacy else { return }
            self.shouldPruneLegacy = false
            let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            if let legacy = try? FileManager.default.contentsOfDirectory(
                at: caches, includingPropertiesForKeys: nil, options: .skipsHiddenFiles) {
                for url in legacy where self.isLegacyPlaceholder(url)
                    && !allProtected.contains(url.resolvingSymlinksInPath()) {
                    try? FileManager.default.removeItem(at: url)
                }
            }
        }
        if synchronously { ioQueue.sync(execute: operation) }
        else { ioQueue.async(execute: operation) }
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
        guard let protected = protectedOverrideURLs() else { return storedOwnedCacheHashes() }
        return Set(protected.filter(isGeneratedOverride).map(cacheHash(for:)))
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
            defer {
                if accessed { cacheDirectory.stopAccessingSecurityScopedResource() }
            }
            self.fileLifecycleLock.lock()
            defer { self.fileLifecycleLock.unlock() }
            guard let references = DesktopWallpaperStore.read()?.referencedURLs else { return }
            let protected = references.union(self.storedInstalledOverrides().values)
                .union(self.storedPendingOverrides().values)
            let staleHashes = staleHashes.subtracting(protected.map(self.cacheHash(for:)))
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
    private func setDesktopImage(_ url: URL, for screen: NSScreen,
                                 options: [NSWorkspace.DesktopImageOptionKey: Any] = [:]) -> Bool {
        fileLifecycleLock.lock()
        defer { fileLifecycleLock.unlock() }
        guard isUsablePicture(url) else { return false }
        unpublishedTargets.remove(url)
        registerOwnedCacheURL(url)
        do {
            try NSWorkspace.shared.setDesktopImageURL(url, for: screen, options: options)
            return true
        } catch {
            NSLog("[Mirage] 设置桌面图片失败: \(error.localizedDescription)")
            return false
        }
    }

    private func verifyDesktopImage(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                                    attempt: Int = 0) {
        let delays: [TimeInterval] = [0.15, 0.3, 0.6, 1.0, 2.0]
        guard attempt < delays.count else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + delays[attempt]) { [weak self] in
            guard let self, !self.isTerminating, !self.isRestoring,
                  self.storedPendingOverrides()[self.backupKey(for: displayID)] == url,
                  let uuid = self.displayUUID(for: displayID) else { return }
            if DesktopWallpaperStore.read()?.matches(url, for: uuid) == true {
                self.fileLifecycleLock.lock()
                var installed = self.storedInstalledOverrides()
                installed[self.backupKey(for: displayID)] = url
                self.saveInstalledOverrides(installed)
                self.installedByScreen[displayID] = url
                var pending = self.storedPendingOverrides()
                pending.removeValue(forKey: self.backupKey(for: displayID))
                self.savePendingOverrides(pending)
                self.fileLifecycleLock.unlock()
                self.schedulePrune()
                self.cleanSystemCacheIfEnabled()
                if let wallpaper = self.deferredCaptures.removeValue(forKey: displayID) {
                    self.scheduleCapture(forDisplay: displayID, wallpaper: wallpaper)
                }
                Task { @MainActor in
                    DynamicLockScreenManager.shared.refreshDesktopFallback(forDisplay: displayID)
                }
                return
            }
            self.verifyDesktopImage(url, forDisplay: displayID, attempt: attempt + 1)
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
