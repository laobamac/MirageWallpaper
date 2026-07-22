//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import CoreGraphics

struct WallpaperRuntimeState: Codable, Equatable {
    var volume: Float = 1.0
    var speed: Float = 1.0
    var muted: Bool = false
    var fillMode: FillMode = .cover
    var propertyOverrides: [String: WEPropertyValue] = [:]
}

class WallpaperViewModel: ObservableObject {
    let renderer = RendererController()

    private struct ScreenAssignment {
        let wallpaper: WEWallpaper
        let runtime: WallpaperRuntimeState
        let displayID: CGDirectDisplayID
    }

    private struct AppliedPlaybackState: Equatable {
        let paused: Bool
        let muted: Bool
        let volume: Float
        let speed: Float
    }

    private var lastAppliedPlaybackState: AppliedPlaybackState?
    private var screenAssignments: [Int: ScreenAssignment] = [:]
    private var stoppedByPlaybackPolicy = false
    private var runtimeSaveWorkItem: DispatchWorkItem?
    private var playbackCommandWorkItem: DispatchWorkItem?
    private var propertyCommandWorkItem: DispatchWorkItem?
    private var pendingPropertyCommands: [String: WEProjectProperty] = [:]

    static var invalidWallpaper: WEWallpaper {
        WEWallpaper(using: .invalid,
                    where: Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")
                        ?? URL(fileURLWithPath: "/dev/null"))
    }

    @Published var nextCurrentWallpaper: WEWallpaper = WallpaperViewModel.invalidWallpaper {
        willSet {
            if newValue.kind == .web, !isTrusted(newValue) {
                AppDelegate.shared.contentViewModel.warningUnsafeWallpaperModal(which: newValue)
            } else {
                self.currentWallpaper = newValue
            }
        }
    }

    @Published var currentWallpaper: WEWallpaper {
        didSet {
            if oldValue.isValid, oldValue.id != currentWallpaper.id {
                runtimeSaveWorkItem?.cancel()
                runtimeSaveWorkItem = nil
                persistRuntime(runtime, for: oldValue)
            }
            UserDefaults.standard.set(try? JSONEncoder().encode(currentWallpaper), forKey: "CurrentWallpaper")
            applyCurrent()
        }
    }

    @Published var runtime = WallpaperRuntimeState()

    // 避免运行时回填 UI 时重复下发指令。
    private var suppressPlaybackSideEffects = false

    var lastPlayRate: Float = 1.0
    @Published var playRate: Float = 1.0 {
        willSet { syncStatusPauseItem(isPaused: newValue == 0.0) }
        didSet {
            lastPlayRate = oldValue
            guard !suppressPlaybackSideEffects else { return }
            runtime.speed = playRate
            schedulePlaybackPolicyApplication()
            scheduleRuntimeSave()
        }
    }

    var lastPlayVolume: Float = 1.0
    @Published var playVolume: Float = 1.0 {
        willSet { syncStatusMuteItem(isMuted: newValue == 0.0) }
        didSet {
            lastPlayVolume = oldValue
            guard !suppressPlaybackSideEffects else { return }
            runtime.volume = playVolume
            schedulePlaybackPolicyApplication()
            scheduleRuntimeSave()
        }
    }

    init() {
        if let json = UserDefaults.standard.data(forKey: "CurrentWallpaper"),
           let wallpaper = try? JSONDecoder().decode(WEWallpaper.self, from: json),
           FileManager.default.fileExists(atPath: wallpaper.wallpaperDirectory.path) {
            currentWallpaper = WEWallpaper.load(from: wallpaper.wallpaperDirectory)
        } else {
            currentWallpaper = WallpaperViewModel.invalidWallpaper
        }
        NotificationCenter.default.addObserver(
            self, selector: #selector(displayTopologyChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    // MARK: 全局设置桥接

    private var masterVolume: Float {
        Float(AppDelegate.shared.globalSettingsViewModel.settings.masterVolume)
    }
    private var globalMuted: Bool {
        AppDelegate.shared.globalSettingsViewModel.settings.globalMuted
    }
    private var globalFps: Int {
        Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
    }
    private var enableSpectrum: Bool {
        AppDelegate.shared.globalSettingsViewModel.settings.enableSpectrum
    }
    private var currentPlaybackPolicy: GSPlayback {
        AppDelegate.shared.globalSettingsViewModel.effectivePlaybackAction
    }

    // MARK: 信任（网页壁纸安全确认）

    func isTrusted(_ w: WEWallpaper) -> Bool {
        let list = UserDefaults.standard.stringArray(forKey: "TrustedWallpapers") ?? []
        return list.contains(w.id)
    }

    func trust(_ w: WEWallpaper) {
        var list = UserDefaults.standard.stringArray(forKey: "TrustedWallpapers") ?? []
        if !list.contains(w.id) { list.append(w.id) }
        UserDefaults.standard.set(list, forKey: "TrustedWallpapers")
    }

    func trustAndApply(_ w: WEWallpaper) {
        trust(w)
        currentWallpaper = w
    }

    // MARK: 运行时状态持久化

    private func runtimeKey(for w: WEWallpaper) -> String { "Runtime_\(w.id)" }

    func loadRuntime(for w: WEWallpaper) -> WallpaperRuntimeState {
        if let data = UserDefaults.standard.data(forKey: runtimeKey(for: w)),
           let saved = try? JSONDecoder().decode(WallpaperRuntimeState.self, from: data) {
            let normalized = normalizedRuntime(saved, for: w)
            if normalized != saved, let migrated = try? JSONEncoder().encode(normalized) {
                UserDefaults.standard.set(migrated, forKey: runtimeKey(for: w))
            }
            return normalized
        }
        return WallpaperRuntimeState()
    }

    func saveRuntime() {
        guard currentWallpaper.isValid else { return }
        let normalized = normalizedRuntime(runtime, for: currentWallpaper)
        if normalized != runtime { runtime = normalized }
        screenAssignments[0] = ScreenAssignment(
            wallpaper: currentWallpaper, runtime: normalized, displayID: displayID(for: 0))
        persistRuntime(normalized, for: currentWallpaper)
    }

    private func persistRuntime(_ state: WallpaperRuntimeState, for wallpaper: WEWallpaper) {
        let normalized = normalizedRuntime(state, for: wallpaper)
        guard let data = try? JSONEncoder().encode(normalized) else { return }
        UserDefaults.standard.set(data, forKey: runtimeKey(for: wallpaper))
        if ScreenSaverManager.shared.configuredWallpaperID() == wallpaper.id {
            try? ScreenSaverManager.shared.configure(
                with: wallpaper,
                runtime: normalized,
                properties: effectiveProperties(for: wallpaper, runtime: normalized),
                fps: Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
            )
        }
    }

    private func scheduleRuntimeSave() {
        runtimeSaveWorkItem?.cancel()
        let wallpaper = currentWallpaper
        let state = runtime
        let work = DispatchWorkItem { [weak self] in
            self?.persistRuntime(state, for: wallpaper)
        }
        runtimeSaveWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
    }

    private func schedulePlaybackPolicyApplication() {
        playbackCommandWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.applyPlaybackPolicy(self.currentPlaybackPolicy)
        }
        playbackCommandWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 60.0, execute: work)
    }

    // MARK: 属性合并

    func effectiveProperties(for w: WEWallpaper) -> [String: WEProjectProperty] {
        effectiveProperties(for: w, runtime: runtime)
    }

    func effectiveProperties(for w: WEWallpaper,
                             runtime runtimeState: WallpaperRuntimeState) -> [String: WEProjectProperty] {
        var result = w.project.general?.properties?.items ?? [:]
        for (key, override) in runtimeState.propertyOverrides {
            if var prop = result[key] {
                prop.value = prop.normalizedComboValue(override)
                result[key] = prop
            }
        }
        // A workshop preset may store file/directory values relative to its
        // own overlay (for example "files/background.jpg"). Resolve those
        // for both scene and web dependencies: many legacy web wallpapers
        // prepend file:/// themselves and therefore cannot use a bare path
        // relative to the dependency's entry page.
        if !w.assetOverlayDirectories.isEmpty {
            let baseProperties = loadBaseProperties(for: w)
            let presetKeys = Set(w.project.preset?.keys.map { $0 } ?? [])
            for (key, var property) in result where property.propertyType == .file ||
                property.propertyType == .scenetexture || property.propertyType == .directory {
                let path = property.value.stringValue
                guard !path.isEmpty else { continue }
                if isWindowsAbsolutePath(path) || ((path as NSString).isAbsolutePath &&
                    !FileManager.default.fileExists(atPath: path)) {
                    if presetKeys.contains(key), let fallback = baseProperties[key]?.value {
                        property.value = fallback
                        result[key] = property
                    }
                } else if !(path as NSString).isAbsolutePath,
                          let resolved = resolvedPresetAsset(path, in: w.assetOverlayDirectories) {
                    property.value = .string(resolved.path)
                    result[key] = property
                } else if !(path as NSString).isAbsolutePath,
                          presetKeys.contains(key), path.hasPrefix("files/"),
                          let fallback = baseProperties[key]?.value {
                    property.value = fallback
                    result[key] = property
                }
            }
        }
        return result
    }

    private func normalizedRuntime(_ source: WallpaperRuntimeState,
                                   for wallpaper: WEWallpaper) -> WallpaperRuntimeState {
        var result = source
        let properties = wallpaper.project.general?.properties?.items ?? [:]
        for (key, value) in result.propertyOverrides {
            guard let property = properties[key] else { continue }
            result.propertyOverrides[key] = property.normalizedComboValue(value)
        }
        return result
    }

    private func loadBaseProperties(for wallpaper: WEWallpaper) -> [String: WEProjectProperty] {
        let url = wallpaper.renderDirectory.appending(path: "project.json")
        guard let data = try? Data(contentsOf: url),
              let project = try? JSONDecoder().decode(WEProject.self, from: data) else { return [:] }
        return project.general?.properties?.items ?? [:]
    }

    private func isWindowsAbsolutePath(_ path: String) -> Bool {
        path.range(of: "^[A-Za-z]:[\\\\/]", options: .regularExpression) != nil
    }

    private func resolvedPresetAsset(_ relativePath: String, in directories: [URL]) -> URL? {
        for directory in directories {
            let root = directory.standardizedFileURL.resolvingSymlinksInPath()
            let candidate = root.appending(path: relativePath).standardizedFileURL.resolvingSymlinksInPath()
            let isInside = candidate.path == root.path || candidate.path.hasPrefix(root.path + "/")
            if isInside, FileManager.default.fileExists(atPath: candidate.path) {
                return candidate
            }
        }
        return nil
    }

    private func makeRenderOptions(for w: WEWallpaper,
                                   runtime runtimeState: WallpaperRuntimeState? = nil) -> RenderOptions {
        let state = runtimeState ?? runtime
        let settings = AppDelegate.shared.globalSettingsViewModel.settings
        var opts = RenderOptions()
        opts.fps = globalFps
        opts.enableSpectrum = enableSpectrum
        opts.muted = state.muted || globalMuted || state.volume == 0 || currentPlaybackPolicy == .mute
        opts.volume = state.volume * masterVolume
        opts.speed = state.speed
        opts.fillMode = state.fillMode
        opts.userProperties = effectiveProperties(for: w, runtime: state)
        opts.loadFromMemory = (settings.wallpaperLoadSource ?? .disk) == .memory
        switch settings.textureResolution {
        case .highQuality: opts.renderScale = 1.0
        case .automatic: opts.renderScale = 0.75
        case .highPerformance: opts.renderScale = 0.5
        }
        switch settings.antiAliasing {
        case .none: opts.msaaSamples = 1
        case .msaa_x2: opts.msaaSamples = 2
        case .msaa_x4: opts.msaaSamples = 4
        case .msaa_x8: opts.msaaSamples = 8
        }
        return opts
    }

    private func displayID(for screenIndex: Int) -> CGDirectDisplayID {
        guard NSScreen.screens.indices.contains(screenIndex) else { return 0 }
        return (NSScreen.screens[screenIndex].deviceDescription[
            NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
    }

    @objc private func displayTopologyChanged() {
        AppDelegate.shared.setPlaceholderWallpaper(with: currentWallpaper)
        let oldAssignments = screenAssignments.values
        var remapped: [Int: ScreenAssignment] = [:]
        for assignment in oldAssignments {
            guard let newIndex = NSScreen.screens.firstIndex(where: {
                ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")]
                    as? NSNumber)?.uint32Value == assignment.displayID
            }) else { continue }
            remapped[newIndex] = assignment
        }
        screenAssignments = remapped
        renderer.stopAll()
        guard !stoppedByPlaybackPolicy else { return }
        for (screen, assignment) in remapped.sorted(by: { $0.key < $1.key }) {
            renderer.render(assignment.wallpaper, on: screen,
                            options: makeRenderOptions(for: assignment.wallpaper,
                                                       runtime: assignment.runtime))
        }
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
    }

    // MARK: 应用壁纸

    private func applyCurrent() {
        propertyCommandWorkItem?.cancel()
        pendingPropertyCommands.removeAll(keepingCapacity: true)
        let w = currentWallpaper
        guard w.isValid, w.kind != .unsupported else {
            renderer.stopAll()
            screenAssignments.removeAll()
            return
        }
        runtime = loadRuntime(for: w)
        suppressPlaybackSideEffects = true
        playVolume = runtime.volume
        playRate = runtime.speed
        suppressPlaybackSideEffects = false
        let opts = makeRenderOptions(for: w)
        screenAssignments[0] = ScreenAssignment(
            wallpaper: w, runtime: runtime, displayID: displayID(for: 0))
        if currentPlaybackPolicy != .stop {
            renderer.render(w, on: 0, options: opts)
        }
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
        AppDelegate.shared.setPlaceholderWallpaper(with: w)
    }

    func reapplyCurrent() {
        propertyCommandWorkItem?.cancel()
        pendingPropertyCommands.removeAll(keepingCapacity: true)
        let w = currentWallpaper
        guard w.isValid else { return }
        runtime = loadRuntime(for: w)
        suppressPlaybackSideEffects = true
        playVolume = runtime.volume
        playRate = runtime.speed
        suppressPlaybackSideEffects = false
        screenAssignments[0] = ScreenAssignment(
            wallpaper: w, runtime: runtime, displayID: displayID(for: 0))
        if currentPlaybackPolicy != .stop {
            renderer.render(w, on: 0, options: makeRenderOptions(for: w))
        }
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
    }

    func applyToAllScreens() {
        let w = currentWallpaper
        guard w.isValid, w.kind != .unsupported else { return }
        let count = NSScreen.screens.count
        currentWallpaper = w
        for screen in 1..<max(count, 1) {
            var opts = makeRenderOptions(for: w)
            opts.volume = runtime.volume * Float(masterVolume)
            opts.muted = runtime.muted || globalMuted
            opts.speed = runtime.speed
            opts.fillMode = runtime.fillMode
            screenAssignments[screen] = ScreenAssignment(
                wallpaper: w, runtime: runtime, displayID: displayID(for: screen))
            if currentPlaybackPolicy != .stop {
                renderer.render(w, on: screen, options: opts)
            }
        }
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
    }

    func stopWallpaper() {
        renderer.stopAll()
        screenAssignments.removeAll()
        stoppedByPlaybackPolicy = false
        currentWallpaper = WallpaperViewModel.invalidWallpaper
    }

    func applyOnScreen(_ w: WEWallpaper, screen: Int) {
        guard w.isValid, w.kind != .unsupported else { return }
        if screen == 0 {
            currentWallpaper = w
        } else {
            let saved = loadRuntime(for: w)
            var opts = makeRenderOptions(for: w, runtime: saved)
            opts.volume = saved.volume * Float(masterVolume)
            opts.muted = saved.muted || globalMuted
            opts.speed = saved.speed
            opts.fillMode = saved.fillMode
            screenAssignments[screen] = ScreenAssignment(
                wallpaper: w, runtime: saved, displayID: displayID(for: screen))
            if currentPlaybackPolicy != .stop {
                renderer.render(w, on: screen, options: opts)
            }
            applyPlaybackPolicy(currentPlaybackPolicy, runtime: saved, on: screen)
        }
    }

    // MARK: 属性实时下发

    func setProperty(key: String, value: WEPropertyValue) {
        guard var prop = currentWallpaper.project.general?.properties?.items[key] else { return }
        let normalizedValue = prop.normalizedComboValue(value)
        prop.value = normalizedValue
        runtime.propertyOverrides[key] = normalizedValue
        scheduleRuntimeSave()

        switch currentWallpaper.kind {
        case .web, .scene:
            // 场景与网页渲染器都支持实时属性通道：颜色 / 透明度 / 开关(可见性) /
            // 下拉(shader combo & 脚本属性) / 文本 / 字号 均即时生效，无需重启进程。
            pendingPropertyCommands[key] = prop
            propertyCommandWorkItem?.cancel()
            let wallpaperID = currentWallpaper.id
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.currentWallpaper.id == wallpaperID else { return }
                let commands = self.pendingPropertyCommands
                self.pendingPropertyCommands.removeAll(keepingCapacity: true)
                for (key, property) in commands {
                    self.renderer.setProperty(key: key, property: property)
                }
            }
            propertyCommandWorkItem = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 60.0, execute: work)
        case .video, .unsupported:
            break
        }
    }

    func setFillMode(_ mode: FillMode) {
        runtime.fillMode = mode
        renderer.setFillMode(mode)
        scheduleRuntimeSave()
    }

    func resetProperties() {
        runtime = WallpaperRuntimeState()
        saveRuntime()
        suppressPlaybackSideEffects = true
        playVolume = runtime.volume
        playRate = runtime.speed
        suppressPlaybackSideEffects = false
        reapplyCurrent()
    }

    func reapplyVolume() {
        applyPlaybackPolicy(currentPlaybackPolicy)
    }

    func applyPlaybackPolicy(_ action: GSPlayback, force: Bool = false) {
        if action == .stop {
            if !stoppedByPlaybackPolicy {
                renderer.stopAll()
                stoppedByPlaybackPolicy = true
            }
            lastAppliedPlaybackState = nil
            return
        }

        if stoppedByPlaybackPolicy {
            stoppedByPlaybackPolicy = false
            for (screen, assignment) in screenAssignments.sorted(by: { $0.key < $1.key }) {
                renderer.render(assignment.wallpaper, on: screen,
                                options: makeRenderOptions(for: assignment.wallpaper,
                                                           runtime: assignment.runtime))
            }
            lastAppliedPlaybackState = nil
        }

        let state = AppliedPlaybackState(
            paused: playRate == 0 || action == .pause,
            muted: runtime.muted || globalMuted || runtime.volume == 0 || action == .mute,
            volume: runtime.volume * masterVolume,
            speed: playRate
        )
        guard force || state != lastAppliedPlaybackState else { return }

        if state.paused {
            renderer.pause()
            renderer.setVolume(state.volume)
            renderer.setMuted(state.muted)
        } else {
            renderer.setVolume(state.volume)
            renderer.setMuted(state.muted)
            renderer.setSpeed(state.speed)
            renderer.resume()
        }
        lastAppliedPlaybackState = state
    }

    private func applyPlaybackPolicy(_ action: GSPlayback, runtime: WallpaperRuntimeState, on screen: Int) {
        if action == .stop {
            renderer.stop(on: screen)
            return
        }
        let paused = runtime.speed == 0 || action == .pause
        let muted = runtime.muted || globalMuted || runtime.volume == 0 || action == .mute
        let volume = runtime.volume * masterVolume

        if paused {
            renderer.pause(on: screen)
            renderer.setVolume(volume, on: screen)
            renderer.setMuted(muted, on: screen)
        } else {
            renderer.setVolume(volume, on: screen)
            renderer.setMuted(muted, on: screen)
            renderer.setSpeed(runtime.speed, on: screen)
            renderer.resume(on: screen)
        }
    }

    // MARK: 状态栏菜单项文字同步（保留原 UI 行为）

    private func syncStatusPauseItem(isPaused: Bool) {
        guard let menu = AppDelegate.shared.statusItem?.menu else { return }
        for item in menu.items {
            if isPaused, item.title == "暂停" {
                item.title = "继续"
                item.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: nil)
                item.action = #selector(AppDelegate.resume)
                item.target = AppDelegate.shared
            } else if !isPaused, item.title == "继续" {
                item.title = "暂停"
                item.image = NSImage(systemSymbolName: "pause.fill", accessibilityDescription: nil)
                item.action = #selector(AppDelegate.pause)
                item.target = AppDelegate.shared
            }
        }
    }

    private func syncStatusMuteItem(isMuted: Bool) {
        guard let menu = AppDelegate.shared.statusItem?.menu else { return }
        for (i, item) in menu.items.enumerated() {
            if isMuted, item.title == "静音" {
                menu.items[i] = .init(title: "取消静音", systemImage: "speaker.fill",
                                      action: #selector(AppDelegate.unmute), keyEquivalent: "")
            } else if !isMuted, item.title == "取消静音" {
                menu.items[i] = .init(title: "静音", systemImage: "speaker.slash.fill",
                                      action: #selector(AppDelegate.mute), keyEquivalent: "")
            }
        }
    }
}
