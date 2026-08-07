//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import ImageIO
import UniformTypeIdentifiers

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
    /// Debounce: a playlist rotating quickly would otherwise ask every renderer
    /// for a snapshot on every hop.
    private static let captureDebounce: TimeInterval = 1.5
    /// A renderer that is still starting up cannot produce a frame yet, and a
    /// scene's first frame can be seconds away. Back off rather than immediately
    /// settling for the packaged preview. The delays are cumulative: this budget
    /// covers roughly half a minute, comfortably past a scene cold start.
    private static let captureAttempts = 7
    private static let captureRetryDelays: [TimeInterval] = [1.0, 2.0, 4.0, 6.0, 8.0, 10.0]

    private static let desktopRefreshCommitCheckDelays: [TimeInterval] = [0.25, 0.5, 1.0, 2.0]
    private static let desktopRefreshSettleDelay: TimeInterval = 0.75
    private static let desktopRefreshCleanupDelay: TimeInterval = 2.0

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

        if mode == .transient {
            // A transient marker cannot legitimately survive its own process:
            // the previous run was killed before it could restore.
            NSLog("[Mirage] 检测到上次运行未正常还原桌面图片，正在还原")
            restore()
        }

        repairDanglingDesktopPointer()
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
            setDesktopImage(restoreTarget(), for: screen)
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
        guard wallpaper.isValid, wallpaper.kind != .unsupported else { return }
        pendingCapture[displayID]?.cancel()
        let request = CaptureRequest(id: UUID(), wallpaperID: wallpaper.id)
        captureRequests[displayID] = request
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.captureRequests[displayID] == request else { return }
            self.pendingCapture[displayID] = nil
            self.capture(forDisplay: displayID, wallpaper: wallpaper, request: request)
        }
        pendingCapture[displayID] = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.captureDebounce, execute: work)
    }

    /// Re-takes the still for every screen that currently has a wallpaper.
    func scheduleCaptureForAllScreens() {
        let viewModel = AppDelegate.shared.wallpaperViewModel
        for displayID in viewModel.renderer.activeDisplayIDs {
            guard let wallpaper = viewModel.renderer.currentWallpaper(onDisplay: displayID) else { continue }
            scheduleCapture(forDisplay: displayID, wallpaper: wallpaper)
        }
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
        backUpUserPictureIfNeeded(on: screen)
        // A new UUID every time: WallpaperAgent caches by path, so rewriting the
        // bytes under a path it already displays does not repaint.
        let target = directory.appending(path: "override-\(UUID().uuidString).heic")
        AppDelegate.shared.wallpaperViewModel.renderer.snapshot(
            onDisplay: displayID, path: target.path
        ) { [weak self] ok in
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                guard self.captureRequests[displayID] == request else {
                    try? FileManager.default.removeItem(at: target)
                    return
                }
                let current = AppDelegate.shared.wallpaperViewModel.renderer
                    .currentWallpaper(onDisplay: displayID)
                guard current?.id == request.wallpaperID else {
                    try? FileManager.default.removeItem(at: target)
                    self.captureRequests[displayID] = nil
                    return
                }
                if ok, FileManager.default.fileExists(atPath: target.path) {
                    NSLog("[Mirage] 已捕获壁纸实时画面 (显示器=\(displayID))")
                    self.install(target, forDisplay: displayID, request: request)
                    return
                }
                try? FileManager.default.removeItem(at: target)
                guard attempt + 1 < Self.captureAttempts else {
                    NSLog("[Mirage] 壁纸截图失败，改用预览图 (显示器=\(displayID))")
                    self.installFallbackPreview(
                        of: wallpaper, forDisplay: displayID, request: request)
                    return
                }
                let delay = Self.captureRetryDelays[
                    min(attempt, Self.captureRetryDelays.count - 1)]
                DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                    guard let self, self.captureRequests[displayID] == request else { return }
                    self.capture(
                        forDisplay: displayID, wallpaper: wallpaper,
                        request: request, attempt: attempt + 1)
                }
            }
        }
    }

    private func installFallbackPreview(of wallpaper: WEWallpaper,
                                        forDisplay displayID: CGDirectDisplayID,
                                        request: CaptureRequest) {
        guard captureRequests[displayID] == request else { return }
        guard !wallpaper.project.preview.isEmpty else { return }
        let source = wallpaper.previewURL
        guard FileManager.default.fileExists(atPath: source.path) else { return }
        let target = directory.appending(path: "override-\(UUID().uuidString).heic")
        ioQueue.async { [weak self] in
            guard let self,
                  let image = NSImage(contentsOf: source),
                  let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil),
                  self.encode(cgImage, to: target) else { return }
            DispatchQueue.main.async {
                guard self.captureRequests[displayID] == request else {
                    try? FileManager.default.removeItem(at: target)
                    return
                }
                self.install(target, forDisplay: displayID, request: request)
            }
        }
    }

    /// Points `screenIndex` at `url`, recording the user's own picture first and
    /// deleting every override file that is no longer displayed.
    private func install(_ url: URL, forDisplay displayID: CGDirectDisplayID,
                         request: CaptureRequest) {
        guard captureRequests[displayID] == request else {
            try? FileManager.default.removeItem(at: url)
            return
        }
        guard let screen = screen(for: displayID) else {
            try? FileManager.default.removeItem(at: url)
            captureRequests[displayID] = nil
            return
        }

        // Normally already recorded before the capture was requested; repeated
        // here so an install from any other path cannot skip it.
        backUpUserPictureIfNeeded(on: screen)
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
        setDesktopImage(url, for: screen)
        installedByScreen[displayID] = url
        captureRequests[displayID] = nil
        scheduleDesktopRefresh(forDisplay: displayID, screen: screen, url: url)
        pruneAllExcept(Set(installedByScreen.values.map { $0.resolvingSymlinksInPath() }))
    }

    private func scheduleDesktopRefresh(forDisplay displayID: CGDirectDisplayID,
                                        screen: NSScreen, url: URL,
                                        attempt: Int = 0) {
        let delay = Self.desktopRefreshCommitCheckDelays[
            min(attempt, Self.desktopRefreshCommitCheckDelays.count - 1)]
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self,
                  self.captureRequests[displayID] == nil,
                  self.installedByScreen[displayID]?.resolvingSymlinksInPath() ==
                      url.resolvingSymlinksInPath() else { return }

            let committed = NSWorkspace.shared.desktopImageURL(for: screen)?
                .resolvingSymlinksInPath() == url.resolvingSymlinksInPath()
            if !committed, attempt + 1 < Self.desktopRefreshCommitCheckDelays.count {
                self.scheduleDesktopRefresh(
                    forDisplay: displayID, screen: screen,
                    url: url, attempt: attempt + 1)
                return
            }

            let refreshURL = self.directory.appending(
                path: "override-\(UUID().uuidString).heic")
            self.ioQueue.asyncAfter(
                deadline: .now() + Self.desktopRefreshSettleDelay
            ) { [weak self] in
                guard let self else { return }
                guard (try? FileManager.default.copyItem(
                    at: url, to: refreshURL)) != nil else { return }

                DispatchQueue.main.async { [weak self] in
                    guard let self else {
                        try? FileManager.default.removeItem(at: refreshURL)
                        return
                    }
                    guard self.captureRequests[displayID] == nil,
                          self.installedByScreen[displayID]?.resolvingSymlinksInPath() ==
                              url.resolvingSymlinksInPath() else {
                        try? FileManager.default.removeItem(at: refreshURL)
                        return
                    }

                    self.setDesktopImage(refreshURL, for: screen)
                    self.installedByScreen[displayID] = refreshURL
                    DispatchQueue.main.asyncAfter(
                        deadline: .now() + Self.desktopRefreshCleanupDelay
                    ) { [weak self] in
                        guard let self,
                              self.installedByScreen[displayID]?.resolvingSymlinksInPath() ==
                                  refreshURL.resolvingSymlinksInPath() else { return }
                        var keep = Set(self.installedByScreen.values.map {
                            $0.resolvingSymlinksInPath()
                        })
                        if NSWorkspace.shared.desktopImageURL(for: screen)?
                            .resolvingSymlinksInPath() != refreshURL.resolvingSymlinksInPath() {
                            keep.insert(url.resolvingSymlinksInPath())
                        }
                        self.pruneAllExcept(keep)
                    }
                }
            }
        }
    }

    /// Records the picture the user chose, so it can be put back. Only ever
    /// stores something that is not one of our own files — otherwise a crash
    /// followed by a relaunch would "back up" our override and lose the real
    /// wallpaper permanently.
    private func backUpUserPictureIfNeeded(on screen: NSScreen) {
        guard defaults.url(forKey: Key.backup) == nil,
              let current = NSWorkspace.shared.desktopImageURL(for: screen),
              !isMirageGenerated(current) else { return }
        defaults.set(current, forKey: Key.backup)
        defaults.synchronize()
        NSLog("[Mirage] 已备份原桌面图片: \(current.lastPathComponent)")
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
        let target = restoreTarget()
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
            let ours = installedByScreen[displayID] != nil
            if !ours, let current = NSWorkspace.shared.desktopImageURL(for: screen),
               !isMirageGenerated(current) {
                continue
            }
            setDesktopImage(target, for: screen)
        }
        mode = .none
        defaults.removeObject(forKey: Key.backup)
        defaults.synchronize()
        installedByScreen.removeAll()
        pruneAllExceptNow([])
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

    /// Any file Mirage generated, past or present. Used everywhere a value must
    /// not be mistaken for the user's own picture — above all the backup key,
    /// which is what a restore ultimately points the desktop back at.
    private func isMirageGenerated(_ url: URL) -> Bool {
        isGeneratedOverride(url) || isLegacyPlaceholder(url)
    }

    /// Keeps only the files currently on screen — one per active display — so
    /// the directory cannot grow the way the old cache deliberately did.
    private func pruneAllExcept(_ keep: Set<URL>) {
        ioQueue.async { [weak self] in self?.pruneAllExceptNow(keep) }
    }

    private func pruneAllExceptNow(_ keep: Set<URL>) {
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil,
            options: .skipsHiddenFiles) else { return }
        for url in urls where !keep.contains(url.resolvingSymlinksInPath()) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private func setDesktopImage(_ url: URL, for screen: NSScreen) {
        do {
            try NSWorkspace.shared.setDesktopImageURL(url, for: screen)
        } catch {
            NSLog("[Mirage] 设置桌面图片失败: \(error.localizedDescription)")
        }
    }

    /// HEIC keeps a 5K still around a few hundred KB. Older Intel Macs have no
    /// HEVC encoder, so fall back to JPEG rather than writing nothing.
    private func encode(_ image: CGImage, to url: URL) -> Bool {
        if writeImage(image, to: url, type: UTType.heic.identifier) { return true }
        return writeImage(image, to: url, type: UTType.jpeg.identifier)
    }

    private func writeImage(_ image: CGImage, to url: URL, type: String) -> Bool {
        guard let destination = CGImageDestinationCreateWithURL(
            url as CFURL, type as CFString, 1, nil) else { return false }
        CGImageDestinationAddImage(destination, image, [
            kCGImageDestinationLossyCompressionQuality: 0.9,
        ] as CFDictionary)
        return CGImageDestinationFinalize(destination)
    }

    // MARK: - Settings changes

    /// ON  → keep overriding forever (and take a still right now).
    /// OFF → keep overriding for the rest of this run for tint consistency,
    ///       but owe a restore on quit.
    func didChangeEnabled(_ enabled: Bool) {
        if mode != .none {
            mode = enabled ? .persistent : .transient
        }
        guard enabled else { return }
        scheduleCaptureForAllScreens()
    }
}
