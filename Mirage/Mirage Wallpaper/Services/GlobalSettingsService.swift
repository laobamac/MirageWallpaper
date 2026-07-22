//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import Combine
import SwiftUI
import ServiceManagement
import IOKit.ps
import CoreAudio

enum GSQuality {
    case low, medium, high, ultra
}

enum GSPlayback: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case keepRunning, mute, pause, stop
}

enum GSAntiAliasingQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case none, msaa_x2, msaa_x4, msaa_x8
}

enum GSPostProcessingQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case disabled, enabled, ultra
}

enum GSTextureResolutionQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case highQuality, highPerformance, automatic
}

enum GSWallpaperLoadSource: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case disk, memory
}

enum GSAppearance: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case light, dark, followSystem
}

enum GSLocalization: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case en_US, zh_CN, followSystem
    case zh_TW
}

enum GSVideoFramework: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case avkit
}

enum GSProcessPiority: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case normal, belowNormal
}

enum GSLogLevel: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case error, verbose, none
}

enum GSSteamAPIEndpoint: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case official
    case mirror
}

enum MirageRegion {
    static var isMainlandChina: Bool {
        Locale.current.region?.identifier.uppercased() == "CN"
    }
}

struct GlobalSettings: Codable, Equatable {
    // MARK: Playback
    var otherApplicationFocused = GSPlayback.keepRunning
    var otherApplicationFullscreen = GSPlayback.keepRunning
    var otherApplicationPlayingAudio = GSPlayback.keepRunning
    var displayAsleep = GSPlayback.keepRunning
    var laptopOnBattery = GSPlayback.keepRunning
    
    // MARK: Quality
    var antiAliasing = GSAntiAliasingQuality.msaa_x2
    var postProcessing = GSPostProcessingQuality.disabled
    var textureResolution = GSTextureResolutionQuality.automatic
    // Optional keeps settings written by older Mirage versions decodable.
    var wallpaperLoadSource: GSWallpaperLoadSource? = .disk
    var reflections = false
    var fps: Double = 30
    
    // MARK: Automatic Setup
    var autoStart = false
    var safeMode = false
    // Optional solely for backwards-compatible decoding of settings written
    // before the software-update section existed.
    var automaticUpdatesEnabled: Bool? = true
    // Optional solely for backwards-compatible decoding of settings written
    // before the software-update section existed.
    var receivePrereleaseUpdates: Bool? = false

    var shouldAutomaticallyUpdate: Bool {
        automaticUpdatesEnabled ?? true
    }

    var shouldReceivePrereleaseUpdates: Bool {
        receivePrereleaseUpdates ?? false
    }
    
    // MARK: Basic Setup
    var language = GSLocalization.followSystem
    
    // MARK: macOS
    var adjustMenuBarTint = true
    
    // MARK: Appearance
    var appearance = GSAppearance.followSystem
    
    // MARK: Audio
    var audioOutput = true
    var reloadWhenChangingOutputDevice = true
    var masterVolume: Double = 1.0
    var globalMuted = false
    var enableSpectrum = true
    
    // MARK: Video
    var videoFramework = GSVideoFramework.avkit
    
    // MARK: Advanced
    var processPiority = GSProcessPiority.normal
    var pauseOnVRAMExhausted = false
    var restartAfterCrashing = false
    
    // MARK: Developer
    var logLevel = GSLogLevel.none
    var verboseLog = false

    // MARK: Misc
    var autoRefresh = true

    // MARK: Steam Workshop
    var steamAPIEndpoint = GSSteamAPIEndpoint.official
    var steamAPIKey = ""

    var normalizedSteamAPIKey: String {
        steamAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var hasValidCustomSteamAPIKey: Bool {
        normalizedSteamAPIKey.range(of: "^[A-Fa-f0-9]{32}$", options: .regularExpression) != nil
    }
}

class GlobalSettingsViewModel: ObservableObject {
    @Published var settings: GlobalSettings 
    {
        didSet {
            MirageLocalization.shared.apply(settings.language)
            validate()
        }
    }
    
    @Published var selection = 0

    // Drives the settings panel that now floats over the main window as a sheet
    // instead of living in its own top-level NSWindow.
    @Published var isSettingsPresented = false

    @Published var isFirstLaunch = UserDefaults.standard.value(forKey: "IsFirstLaunch") as? Bool ?? true
    
    var didFinishLaunchingNotificationCancellable: Cancellable?
    var didCurrentWallpaperChangeCancellable: Cancellable?
    var didAddToLoginItemCancellable: Cancellable?
    var didChangeAdjustMenuBarTintCancellable: Cancellable?
    var playbackPolicySettingsCancellable: Cancellable?
    
    // In-memory snapshot of what is persisted, so the settings UI can tell
    // whether there are unsaved edits with a cheap value comparison instead of
    // decoding GlobalSettings JSON from UserDefaults on every footer render.
    @Published private(set) var savedSettings: GlobalSettings

    init() {
        var initial: GlobalSettings
        if let data = UserDefaults.standard.data(forKey: "GlobalSettings"),
           let settings = try? JSONDecoder().decode(GlobalSettings.self, from: data) {
            initial = settings
        } else {
            initial = GlobalSettings()
        }
        if !MirageRegion.isMainlandChina {
            initial.steamAPIEndpoint = .official
        }
        self.settings = initial
        self.savedSettings = initial
        MirageLocalization.shared.apply(self.settings.language)
        self.didFinishLaunchingNotificationCancellable =
        NotificationCenter.default.publisher(for: NSApplication.didFinishLaunchingNotification)
            .sink { [weak self] _ in self?.didFinishLaunchingNotification() }
    }
    
    deinit {
        didFinishLaunchingNotificationCancellable?.cancel()
        didCurrentWallpaperChangeCancellable?.cancel()
        didAddToLoginItemCancellable?.cancel()
        didChangeAdjustMenuBarTintCancellable?.cancel()
        playbackPolicySettingsCancellable?.cancel()
        playbackEvalTimer?.invalidate()
        settlingEvalWorkItems.forEach { $0.cancel() }
        for observer in workspacePlaybackObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        if let desktopClickMonitor { NSEvent.removeMonitor(desktopClickMonitor) }
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }
    
    func didFinishLaunchingNotification() {
        self.didCurrentWallpaperChangeCancellable =
        AppDelegate.shared.wallpaperViewModel.$currentWallpaper
            .sink { [weak self] in self?.didCurrentWallpaperChange($0) }
        
        self.didAddToLoginItemCancellable =
        self.$settings
            .removeDuplicates { $0.autoStart == $1.autoStart }
            .map { $0.autoStart }
            .sink { [weak self] in self?.didAddToLoginItem($0) }
        
        self.didChangeAdjustMenuBarTintCancellable =
        self.$settings
            .removeDuplicates { $0.adjustMenuBarTint == $1.adjustMenuBarTint }
            .map { $0.adjustMenuBarTint }
            .sink { [weak self] in self?.didChangeAdjustMenuBarTint($0) }

        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(displayDidSleep),
            name: NSWorkspace.screensDidSleepNotification, object: nil)
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(displayDidWake),
            name: NSWorkspace.screensDidWakeNotification, object: nil)

        self.validate()
        playbackPolicySettingsCancellable = $settings
            .map {
                [$0.otherApplicationFocused, $0.otherApplicationFullscreen,
                 $0.otherApplicationPlayingAudio, $0.displayAsleep, $0.laptopOnBattery]
            }
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] _ in self?.configurePlaybackMonitoring() }

        self.configurePlaybackMonitoring()
    }

    private var displayAsleep = false
    private var playbackEvalTimer: Timer?
    private var settlingEvalWorkItems: [DispatchWorkItem] = []
    private var workspacePlaybackObservers: [NSObjectProtocol] = []
    private var desktopClickMonitor: Any?
    // A click on bare desktop starts the reveal-desktop animation, during which
    // windows are still covering the screen and geometry detection would wrongly
    // report the desktop as hidden. We briefly trust the click as a reveal hint
    // to bridge that animation, then hand back to the geometry truth.
    private var lastDesktopRevealHintAt: Date = .distantPast
    private static let desktopRevealGrace: TimeInterval = 1.2
    private(set) var effectivePlaybackAction = GSPlayback.keepRunning

    private func configurePlaybackMonitoring() {
        playbackEvalTimer?.invalidate()
        playbackEvalTimer = nil
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        for observer in workspacePlaybackObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        workspacePlaybackObservers.removeAll()
        if let desktopClickMonitor {
            NSEvent.removeMonitor(desktopClickMonitor)
            self.desktopClickMonitor = nil
        }

        let focusRulesEnabled = settings.otherApplicationFocused != .keepRunning ||
            settings.otherApplicationFullscreen != .keepRunning
        let anyRuleEnabled = focusRulesEnabled ||
            settings.otherApplicationPlayingAudio != .keepRunning ||
            settings.displayAsleep != .keepRunning ||
            settings.laptopOnBattery != .keepRunning

        guard anyRuleEnabled,
              AppDelegate.shared.wallpaperViewModel.currentWallpaper.isValid else {
            effectivePlaybackAction = .keepRunning
            AppDelegate.shared.wallpaperViewModel.applyPlaybackPolicy(.keepRunning)
            return
        }

        if focusRulesEnabled {
            let playbackNotifications: [Notification.Name] = [
                NSWorkspace.activeSpaceDidChangeNotification,
                NSWorkspace.didActivateApplicationNotification,
                NSWorkspace.didDeactivateApplicationNotification,
                NSWorkspace.didHideApplicationNotification,
                NSWorkspace.didUnhideApplicationNotification,
                NSWorkspace.didLaunchApplicationNotification,
                NSWorkspace.didTerminateApplicationNotification
            ]
            for name in playbackNotifications {
                let observer = NSWorkspace.shared.notificationCenter.addObserver(
                    forName: name, object: nil, queue: .main
                ) { [weak self] _ in
                    if name == NSWorkspace.didActivateApplicationNotification {
                        self?.activateApplicationDidChange()
                    } else {
                        self?.scheduleSettlingEvaluations()
                    }
                }
                workspacePlaybackObservers.append(observer)
            }
            desktopClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseDown) {
                [weak self] _ in
                guard let self else { return }
                // A click on bare desktop begins a reveal. Record the hint so the
                // grace window bridges the reveal animation, then let the settling
                // re-evaluations confirm the state from real window geometry.
                if self.clickLandedOnDesktop(at: NSEvent.mouseLocation) {
                    self.lastDesktopRevealHintAt = Date()
                }
                self.scheduleSettlingEvaluations()
            }
        }

        // Focus/fullscreen rules also poll: revealing the desktop (click-wallpaper,
        // F11, hot corners, Mission Control) and re-covering it emit no reliable
        // notification, so periodic geometry checks keep playback correct.
        let pollingRuleEnabled = focusRulesEnabled ||
            settings.otherApplicationPlayingAudio != .keepRunning ||
            settings.laptopOnBattery != .keepRunning
        if pollingRuleEnabled {
            let interval: TimeInterval = focusRulesEnabled ? 1.0 : 2.0
            playbackEvalTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) {
                [weak self] _ in self?.evaluatePlaybackState()
            }
        }
        evaluatePlaybackState()
    }

    @objc private func displayDidSleep() { displayAsleep = true; evaluatePlaybackState() }
    @objc private func displayDidWake()  { displayAsleep = false; evaluatePlaybackState() }

    // Revealing or re-covering the desktop animates windows over ~0.3–0.5s, and
    // macOS posts no notification when that animation ends. A single debounced
    // evaluation therefore samples window geometry mid-flight and sticks with a
    // stale result. Instead fire a short burst of re-evaluations that straddle
    // the animation so playback settles on the real, post-animation geometry.
    private func scheduleSettlingEvaluations() {
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        for delay in [0.05, 0.35, 0.7, 1.1] {
            let work = DispatchWorkItem { [weak self] in self?.evaluatePlaybackState() }
            settlingEvalWorkItems.append(work)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        }
    }
    
    func didAddToLoginItem(_ added: Bool) {
        let appService = SMAppService.mainApp
        do {
            switch (added, appService.status) {
            case (true, .notRegistered):
                try appService.register()
            case (false, .enabled), (false, .requiresApproval):
                try appService.unregister()
            case (true, .notFound):
                NSLog("[Mirage] Login item is unavailable")
                settings.autoStart = false
            default:
                break
            }
        } catch {
            let nsError = error as NSError
            NSLog("[Mirage] Failed to update login item: %@ (%@:%ld)", nsError.localizedDescription, nsError.domain, nsError.code)
            let isRegistered = appService.status == .enabled || appService.status == .requiresApproval
            if settings.autoStart != isRegistered {
                settings.autoStart = isRegistered
            }
        }
    }
    
    func didChangeAdjustMenuBarTint(_ newValue: Bool) {
        if newValue != true {
            // NSScreen.main can be nil while displays are asleep or being
            // reconfigured — exactly when this handler runs — so guard it.
            if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper"),
               let screen = NSScreen.main {
                try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: screen)
            }
        } else {
            AppDelegate.shared.setPlaceholderWallpaper(
                with: AppDelegate.shared.wallpaperViewModel.currentWallpaper)
        }
    }
    
    func didCurrentWallpaperChange(_ newValue: WEWallpaper) {
        AppDelegate.shared.setPlaceholderWallpaper(with: newValue)
        if playbackPolicySettingsCancellable != nil {
            DispatchQueue.main.async { [weak self] in self?.configurePlaybackMonitoring() }
        }
    }
    
    func reset() {
        settings = (try? JSONDecoder()
            .decode(GlobalSettings.self,
                from: UserDefaults.standard.data(forKey: "GlobalSettings")
            ?? Data()))
        ?? GlobalSettings()
        savedSettings = settings
    }

    func save() {
        guard let data = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(data, forKey: "GlobalSettings")
        savedSettings = settings
    }
    
    func setQuality(_ quality: GSQuality) {
        switch quality {
        case .low:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .disabled
            self.settings.textureResolution = .highPerformance
            self.settings.fps = 10
            self.settings.reflections = false
        case .medium:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .automatic
            self.settings.fps = 15
            self.settings.reflections = true
        case .high:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .automatic
            self.settings.fps = 25
            self.settings.reflections = true
        case .ultra:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .ultra
            self.settings.textureResolution = .highQuality
            self.settings.fps = 30
            self.settings.reflections = true
        }
    }
    
    private func validate() {
        switch settings.appearance {
        case .light:
            NSApp.appearance = NSAppearance(named: .aqua)
        case .dark:
            NSApp.appearance = NSAppearance(named: .darkAqua)
        case .followSystem:
            NSApp.appearance = nil
        }
    }
    
    func activateApplicationDidChange() {
        // Activating another app can settle window geometry over a few frames
        // (a reveal collapsing, a window coming forward). Re-evaluate as it
        // settles instead of trusting a single mid-animation sample.
        scheduleSettlingEvaluations()
    }

    func evaluatePlaybackState() {
        var actions: [GSPlayback] = []

        if settings.displayAsleep != .keepRunning, displayAsleep {
            actions.append(settings.displayAsleep)
        }

        if settings.laptopOnBattery != .keepRunning, isOnBattery() {
            actions.append(settings.laptopOnBattery)
        }

        if settings.otherApplicationFocused != .keepRunning ||
            settings.otherApplicationFullscreen != .keepRunning {
            let front = NSWorkspace.shared.frontmostApplication
            let isSelf = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier
            let withinRevealGrace = Date().timeIntervalSince(lastDesktopRevealHintAt) < Self.desktopRevealGrace
            let desktopViewed = withinRevealGrace || isDesktopExposed()
            let isDesktopFinder = front?.bundleIdentifier == "com.apple.finder"
                && !appHasVisibleWindows(pid: front?.processIdentifier)

            if let front, front.activationPolicy == .regular, !isSelf,
               !isDesktopFinder, !desktopViewed {
                if appIsFullscreen(pid: front.processIdentifier) {
                    actions.append(settings.otherApplicationFullscreen)
                } else {
                    actions.append(settings.otherApplicationFocused)
                }
            }
        }

        if settings.otherApplicationPlayingAudio != .keepRunning,
           isOtherAppPlayingAudio() {
            actions.append(settings.otherApplicationPlayingAudio)
        }

        effectivePlaybackAction = strongestAction(actions)
        AppDelegate.shared.wallpaperViewModel.applyPlaybackPolicy(effectivePlaybackAction)
    }

    private func strongestAction(_ actions: [GSPlayback]) -> GSPlayback {
        func rank(_ a: GSPlayback) -> Int {
            switch a { case .keepRunning: return 0; case .mute: return 1; case .pause: return 2; case .stop: return 3 }
        }
        return actions.max(by: { rank($0) < rank($1) }) ?? .keepRunning
    }

    private func isOnBattery() -> Bool {
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let list = IOPSCopyPowerSourcesList(blob)?.takeRetainedValue() as? [CFTypeRef] else { return false }
        for ps in list {
            guard let desc = IOPSGetPowerSourceDescription(blob, ps)?.takeUnretainedValue() as? [String: Any],
                  let state = desc[kIOPSPowerSourceStateKey] as? String else { continue }
            if state == kIOPSBatteryPowerValue { return true }
        }
        return false
    }

    private func isOtherAppPlayingAudio() -> Bool {
        let system = AudioObjectID(kAudioObjectSystemObject)
        var size: UInt32 = 0
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyProcessObjectList,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyDataSize(system, &addr, 0, nil, &size) == noErr else { return false }

        let count = Int(size) / MemoryLayout<AudioObjectID>.size
        guard count > 0 else { return false }
        var processes = [AudioObjectID](repeating: kAudioObjectUnknown, count: count)
        guard AudioObjectGetPropertyData(system, &addr, 0, nil, &size, &processes) == noErr else { return false }

        var excludedPIDs = AppDelegate.shared.wallpaperViewModel.renderer.processIdentifiers
        excludedPIDs.insert(ProcessInfo.processInfo.processIdentifier)

        for process in processes {
            guard audioProcessIsRunningOutput(process),
                  let pid = audioProcessPID(process),
                  !excludedPIDs.contains(pid) else { continue }
            return true
        }
        return false
    }

    private func audioProcessIsRunningOutput(_ process: AudioObjectID) -> Bool {
        var running: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyIsRunningOutput,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(process, &addr, 0, nil, &size, &running) == noErr else { return false }
        return running != 0
    }

    private func audioProcessPID(_ process: AudioObjectID) -> pid_t? {
        var pid: pid_t = 0
        var size = UInt32(MemoryLayout<pid_t>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyPID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(process, &addr, 0, nil, &size, &pid) == noErr else { return nil }
        return pid
    }

    private func appIsFullscreen(pid: pid_t) -> Bool {
        guard let main = NSScreen.main else { return false }
        let infoList = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
        for info in infoList {
            guard let ownerPID = info[kCGWindowOwnerPID as String] as? pid_t, ownerPID == pid,
                  let boundsDict = info[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsDict as CFDictionary) else { continue }
            if bounds.width >= main.frame.width - 1 && bounds.height >= main.frame.height - 1 {
                return true
            }
        }
        return false
    }

    private func appHasVisibleWindows(pid: pid_t?) -> Bool {
        guard let pid else { return false }
        let infoList = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
        for info in infoList {
            guard let ownerPID = info[kCGWindowOwnerPID as String] as? pid_t, ownerPID == pid,
                  let boundsDict = info[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsDict as CFDictionary) else { continue }
            if bounds.width > 0 && bounds.height > 0 {
                return true
            }
        }
        return false
    }

    /// Hit-test the click point against the on-screen window list to decide
    /// whether the user clicked bare desktop (a reveal-desktop gesture) rather
    /// than any on-screen UI. Evaluated at mouse-down, before a reveal animation
    /// moves windows, so it does not depend on transient window geometry.
    private func clickLandedOnDesktop(at screenPoint: NSPoint) -> Bool {
        // NSEvent.mouseLocation is in AppKit coordinates (origin bottom-left of
        // the main screen). CGWindowList bounds are in CoreGraphics coordinates
        // (origin top-left). Flip Y using the primary display height.
        guard let primary = NSScreen.screens.first(where: { $0.frame.origin == .zero }) ?? NSScreen.main else {
            return false
        }
        let cgPoint = CGPoint(x: screenPoint.x, y: primary.frame.height - screenPoint.y)

        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
        let rendererPIDs = AppDelegate.shared.wallpaperViewModel.renderer.processIdentifiers
        let selfPID = ProcessInfo.processInfo.processIdentifier

        // The click lands on the desktop only if no on-screen window sits under
        // the cursor. We deliberately include chrome such as the Dock, the menu
        // bar and Control Center (positive window layers, non-regular owners) so
        // that clicking them is never mistaken for a reveal gesture. Windows are
        // returned front-to-back; the first one containing the point wins.
        for info in windows {
            guard let layer = info[kCGWindowLayer as String] as? Int, layer >= 0,
                  let pid = info[kCGWindowOwnerPID as String] as? pid_t,
                  pid != selfPID, !rendererPIDs.contains(pid),
                  let boundsDictionary = info[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsDictionary as CFDictionary) else { continue }

            let alpha = (info[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1
            guard alpha > 0.05 else { continue }
            if bounds.contains(cgPoint) {
                return false
            }
        }
        return true
    }

    private func isDesktopExposed() -> Bool {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
        let rendererPIDs = AppDelegate.shared.wallpaperViewModel.renderer.processIdentifiers
        let selfPID = ProcessInfo.processInfo.processIdentifier
        let applications = Dictionary(uniqueKeysWithValues: NSWorkspace.shared.runningApplications.map {
            ($0.processIdentifier, $0)
        })

        var displayCount: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &displayCount)
        var displayIDs = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
        CGGetActiveDisplayList(displayCount, &displayIDs, &displayCount)
        let displays = displayIDs.prefix(Int(displayCount)).map(CGDisplayBounds)

        for info in windows {
            guard let layer = info[kCGWindowLayer as String] as? Int, layer == 0,
                  let pid = info[kCGWindowOwnerPID as String] as? pid_t,
                  pid != selfPID, !rendererPIDs.contains(pid),
                  let app = applications[pid], app.activationPolicy == .regular,
                  let boundsDictionary = info[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsDictionary as CFDictionary),
                  bounds.width >= 120, bounds.height >= 80 else { continue }

            let alpha = (info[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1
            guard alpha > 0.05 else { continue }
            let windowArea = bounds.width * bounds.height
            for display in displays {
                let visibleArea = bounds.intersection(display).standardized
                guard !visibleArea.isNull else { continue }
                let intersectionArea = visibleArea.width * visibleArea.height
                let screenArea = display.width * display.height
                if intersectionArea >= 30_000,
                   (intersectionArea / max(windowArea, 1) >= 0.25 || intersectionArea / max(screenArea, 1) >= 0.02) {
                    return false
                }
            }
        }
        return true
    }
}
