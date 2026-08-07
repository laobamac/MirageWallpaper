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

enum GSAnimatedPreviewPlayback: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case hover, visible
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
    var animatedPreviewPlayback: GSAnimatedPreviewPlayback? = .hover
    var reflections = false
    var fps: Double = 30

    var animatedPreviewPlaybackMode: GSAnimatedPreviewPlayback {
        animatedPreviewPlayback ?? .hover
    }
    
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
    // Optional solely for backwards-compatible decoding of settings written
    // before the desktop-override section existed.
    var overrideWallpaper: Bool? = false

    var shouldOverrideWallpaper: Bool {
        overrideWallpaper ?? false
    }

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
    var didChangeOverrideWallpaperCancellable: Cancellable?
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
        initial.animatedPreviewPlayback = initial.animatedPreviewPlayback ?? .hover
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
        didChangeOverrideWallpaperCancellable?.cancel()
        playbackPolicySettingsCancellable?.cancel()
        playbackEvalTimer?.invalidate()
        settlingEvalWorkItems.forEach { $0.cancel() }
        for observer in workspacePlaybackObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        if let desktopClickMonitor { NSEvent.removeMonitor(desktopClickMonitor) }
        NSWorkspace.shared.notificationCenter.removeObserver(self)
        DistributedNotificationCenter.default().removeObserver(self)
        NotificationCenter.default.removeObserver(self)
    }
    
    func didFinishLaunchingNotification() {
        self.didCurrentWallpaperChangeCancellable =
        AppDelegate.shared.wallpaperViewModel.$displayStates
            .sink { [weak self] in self?.didDisplayStatesChange($0) }
        
        self.didAddToLoginItemCancellable =
        self.$settings
            .removeDuplicates { $0.autoStart == $1.autoStart }
            .map { $0.autoStart }
            .sink { [weak self] in self?.didAddToLoginItem($0) }
        
        self.didChangeOverrideWallpaperCancellable =
        self.$settings
            .removeDuplicates { $0.shouldOverrideWallpaper == $1.shouldOverrideWallpaper }
            .map { $0.shouldOverrideWallpaper }
            .sink { DesktopOverrideService.shared.didChangeEnabled($0) }

        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(displayDidSleep),
            name: NSWorkspace.screensDidSleepNotification, object: nil)
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(displayDidWake),
            name: NSWorkspace.screensDidWakeNotification, object: nil)

        // Screen lock is not a workspace notification; it only arrives on the
        // distributed centre. A locked screen hides the wallpaper just as
        // completely as a sleeping one, so it feeds the same user-facing rule.
        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(screenDidLock),
            name: NSNotification.Name("com.apple.screenIsLocked"), object: nil)
        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(screenDidUnlock),
            name: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil)

        // Low Power Mode and thermal pressure are global signals: the user has
        // either asked the machine to conserve, or the machine is already
        // struggling. Both retune playback without needing a dedicated setting.
        NotificationCenter.default.addObserver(
            self, selector: #selector(powerStateDidChange),
            name: .NSProcessInfoPowerStateDidChange, object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(thermalStateDidChange),
            name: ProcessInfo.thermalStateDidChangeNotification, object: nil)

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
    private var screenLocked = false
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

    /// Runs the window-geometry / power / audio probes off the main thread.
    private let policyQueue = DispatchQueue(label: "com.mirage.playback-policy", qos: .utility)
    private var evaluationInFlight = false
    private var evaluationPending = false
    /// Bumped whenever the rule set is reconfigured, so results computed against
    /// a stale rule set are discarded instead of applied.
    private var policyGeneration: UInt64 = 0

    // Polling exists only as a backstop for transitions macOS does not announce
    // (desktop reveal, Mission Control, F11). Once the decision stops changing
    // there is nothing left to catch, so the timer backs off rather than paying
    // for a window-server round trip every second indefinitely. Any notification
    // — app activation, space change, desktop click — still evaluates at once,
    // and the first differing result snaps the interval back to the base rate.
    private var basePollInterval: TimeInterval = 1.0
    private var currentPollInterval: TimeInterval = 1.0
    private var stableEvaluationCount = 0
    private static let stableEvaluationsBeforeBackoff = 5
    private static let maxPollInterval: TimeInterval = 5.0

    private func schedulePlaybackTimer(interval: TimeInterval) {
        playbackEvalTimer?.invalidate()
        currentPollInterval = interval
        playbackEvalTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) {
            [weak self] _ in self?.evaluatePlaybackState()
        }
    }

    private func backOffPollingInterval() {
        guard playbackEvalTimer != nil else { return }
        let next = min(currentPollInterval * 2, Self.maxPollInterval)
        guard next > currentPollInterval else { return }
        schedulePlaybackTimer(interval: next)
    }

    private func restorePollingInterval() {
        guard playbackEvalTimer != nil, currentPollInterval > basePollInterval else { return }
        schedulePlaybackTimer(interval: basePollInterval)
    }

    private func configurePlaybackMonitoring() {
        // Invalidate any evaluation still in flight: it was computed against the
        // previous rule set, and letting it land would overwrite the decision
        // this reconfiguration is about to make.
        policyGeneration &+= 1
        evaluationInFlight = false
        evaluationPending = false
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
              AppDelegate.shared.wallpaperViewModel.hasAnyWallpaper else {
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
                // Only a click on bare desktop can begin a reveal. A click that
                // lands on a window changes nothing this policy observes, and if
                // it activates another app the activation notification already
                // schedules the re-evaluation. Short-circuiting here drops the
                // settling burst that every click anywhere on screen used to fire.
                guard self.clickLandedOnDesktop(at: NSEvent.mouseLocation) else { return }
                // Record the hint so the grace window bridges the reveal
                // animation, then let the settling re-evaluations confirm the
                // state from real window geometry.
                self.lastDesktopRevealHintAt = Date()
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
            basePollInterval = focusRulesEnabled ? 1.0 : 2.0
            stableEvaluationCount = 0
            schedulePlaybackTimer(interval: basePollInterval)
        }
        evaluatePlaybackState()
    }

    @objc private func displayDidSleep() { displayAsleep = true; evaluatePlaybackState() }
    @objc private func displayDidWake()  { displayAsleep = false; evaluatePlaybackState() }
    @objc private func screenDidLock()   { screenLocked = true; evaluatePlaybackState() }
    @objc private func screenDidUnlock() { screenLocked = false; evaluatePlaybackState() }

    // Thermal and low-power transitions change the frame budget, not the
    // playback decision, so they skip the full evaluation and just re-apply.
    @objc private func powerStateDidChange()   { reapplyPowerBudget() }
    @objc private func thermalStateDidChange() { reapplyPowerBudget() }

    private func reapplyPowerBudget() {
        // Both notifications may arrive on an arbitrary thread.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            AppDelegate.shared.wallpaperViewModel
                .applyPlaybackPolicy(self.effectivePlaybackAction)
        }
    }

    /// Frame rate the renderers should target, reduced under thermal or
    /// low-power pressure.
    ///
    /// This is deliberately advisory-only: it lowers the frame rate but never
    /// pauses. Stopping playback stays entirely under the user's own rules —
    /// the machine running warm is not consent to blank someone's desktop.
    func throttledFps(base: Int) -> Int {
        var cap = base
        if ProcessInfo.processInfo.isLowPowerModeEnabled {
            cap = min(cap, Self.lowPowerFpsCap)
        }
        switch ProcessInfo.processInfo.thermalState {
        case .serious:  cap = min(cap, Self.seriousThermalFpsCap)
        case .critical: cap = min(cap, Self.criticalThermalFpsCap)
        case .nominal, .fair: break
        @unknown default: break
        }
        return max(1, cap)
    }

    private static let lowPowerFpsCap = 15
    private static let seriousThermalFpsCap = 15
    private static let criticalThermalFpsCap = 10

    // Revealing or re-covering the desktop animates windows over ~0.3–0.5s, and
    // macOS posts no notification when that animation ends. A single debounced
    // evaluation therefore samples window geometry mid-flight and sticks with a
    // stale result. Instead fire re-evaluations that straddle the animation so
    // playback settles on the real, post-animation geometry: one immediately for
    // responsiveness, one after the animation can no longer be in flight. The
    // two intermediate samples the burst used to take only ever observed
    // mid-animation geometry that the final sample then overwrote.
    private func scheduleSettlingEvaluations() {
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        for delay in [0.05, 0.7] {
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
    
    func didDisplayStatesChange(_ states: [DisplayKey: DisplayWallpaperState]) {
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
        settings.animatedPreviewPlayback = settings.animatedPreviewPlayback ?? .hover
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

    /// Everything the policy decision needs, sampled from AppKit on the main
    /// thread. Kept to plain values so the expensive part of the evaluation can
    /// run on `policyQueue` without touching main-thread-only state.
    private struct PolicyInputs {
        var onDisplayAsleep = GSPlayback.keepRunning
        var onBattery = GSPlayback.keepRunning
        var onFocused = GSPlayback.keepRunning
        var onFullscreen = GSPlayback.keepRunning
        var onAudio = GSPlayback.keepRunning

        var displayAsleep = false
        var withinRevealGrace = false

        var frontPID: pid_t?
        var frontBundleID: String?
        var frontIsRegular = false

        var selfPID: pid_t = 0
        var rendererPIDs: Set<pid_t> = []
        /// PIDs whose `activationPolicy` is `.regular`, resolved up front because
        /// `NSWorkspace.runningApplications` is AppKit state.
        var regularPIDs: Set<pid_t> = []
        var mainScreenFrame: CGRect?
    }

    /// One parsed entry of the on-screen window list. The raw CFDictionary form
    /// is bridged once per evaluation and then reused by every geometry test.
    private struct WindowEntry {
        var layer: Int
        var pid: pid_t
        var bounds: CGRect
        var alpha: Double
    }

    // Playback evaluation used to run entirely on the main thread, once a second,
    // and could issue three separate `CGWindowListCopyWindowInfo` calls per tick
    // on top of IOKit and CoreAudio probes. Now only the cheap AppKit reads stay
    // on the main thread; the window-server round trip happens once per
    // evaluation on `policyQueue`, and the result is applied back on main.
    // Overlapping requests coalesce so a burst of notifications cannot pile up.
    func evaluatePlaybackState() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.evaluatePlaybackState() }
            return
        }
        if evaluationInFlight {
            evaluationPending = true
            return
        }
        let inputs = collectPolicyInputs()
        let generation = policyGeneration
        evaluationInFlight = true
        policyQueue.async { [weak self] in
            let action = Self.computePlaybackAction(inputs)
            DispatchQueue.main.async {
                guard let self, self.policyGeneration == generation else { return }
                self.evaluationInFlight = false
                self.applyPolicyResult(action)
                if self.evaluationPending {
                    self.evaluationPending = false
                    self.evaluatePlaybackState()
                }
            }
        }
    }

    /// Main-thread half: read AppKit state into plain values. Cheap by design.
    private func collectPolicyInputs() -> PolicyInputs {
        var inputs = PolicyInputs()
        inputs.onDisplayAsleep = settings.displayAsleep
        inputs.onBattery = settings.laptopOnBattery
        inputs.onFocused = settings.otherApplicationFocused
        inputs.onFullscreen = settings.otherApplicationFullscreen
        inputs.onAudio = settings.otherApplicationPlayingAudio

        // A locked screen shows the wallpaper no more than a sleeping one does,
        // so both feed the single user-facing "display asleep" rule rather than
        // introducing a setting the user never asked for.
        inputs.displayAsleep = displayAsleep || screenLocked
        inputs.withinRevealGrace =
            Date().timeIntervalSince(lastDesktopRevealHintAt) < Self.desktopRevealGrace
        inputs.selfPID = ProcessInfo.processInfo.processIdentifier

        let needsWindowGeometry = settings.otherApplicationFocused != .keepRunning ||
            settings.otherApplicationFullscreen != .keepRunning
        let needsRendererPIDs = needsWindowGeometry ||
            settings.otherApplicationPlayingAudio != .keepRunning
        if needsRendererPIDs {
            inputs.rendererPIDs = AppDelegate.shared.wallpaperViewModel.renderer.processIdentifiers
        }
        guard needsWindowGeometry else { return inputs }

        let front = NSWorkspace.shared.frontmostApplication
        inputs.frontPID = front?.processIdentifier
        inputs.frontBundleID = front?.bundleIdentifier
        inputs.frontIsRegular = front?.activationPolicy == .regular
        inputs.mainScreenFrame = NSScreen.main?.frame
        for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular {
            inputs.regularPIDs.insert(app.processIdentifier)
        }
        return inputs
    }

    /// Background half: window geometry, power source and audio probes. Static so
    /// it provably touches no main-thread-owned state.
    private static func computePlaybackAction(_ inputs: PolicyInputs) -> GSPlayback {
        var actions: [GSPlayback] = []

        if inputs.onDisplayAsleep != .keepRunning, inputs.displayAsleep {
            actions.append(inputs.onDisplayAsleep)
        }

        if inputs.onBattery != .keepRunning, isOnBattery() {
            actions.append(inputs.onBattery)
        }

        if inputs.onFocused != .keepRunning || inputs.onFullscreen != .keepRunning {
            // A single window-list snapshot serves all three geometry tests
            // below, and is only captured if one of them is actually reached.
            var cachedWindows: [WindowEntry]?
            func windows() -> [WindowEntry] {
                if let cachedWindows { return cachedWindows }
                let captured = captureWindowList()
                cachedWindows = captured
                return captured
            }

            let isSelf = inputs.frontPID == inputs.selfPID
            let desktopViewed = inputs.withinRevealGrace ||
                isDesktopExposed(windows(), inputs: inputs)
            let isDesktopFinder = inputs.frontBundleID == "com.apple.finder"
                && !appHasVisibleWindows(windows(), pid: inputs.frontPID)

            if let frontPID = inputs.frontPID, inputs.frontIsRegular, !isSelf,
               !isDesktopFinder, !desktopViewed {
                if appIsFullscreen(windows(), pid: frontPID,
                                   mainScreenFrame: inputs.mainScreenFrame) {
                    actions.append(inputs.onFullscreen)
                } else {
                    actions.append(inputs.onFocused)
                }
            }
        }

        if inputs.onAudio != .keepRunning,
           isOtherAppPlayingAudio(selfPID: inputs.selfPID, rendererPIDs: inputs.rendererPIDs) {
            actions.append(inputs.onAudio)
        }

        return strongestAction(actions)
    }

    /// Main-thread tail: publish the decision and retune the polling cadence.
    private func applyPolicyResult(_ action: GSPlayback) {
        if action == effectivePlaybackAction {
            stableEvaluationCount += 1
            if stableEvaluationCount >= Self.stableEvaluationsBeforeBackoff {
                stableEvaluationCount = 0
                backOffPollingInterval()
            }
        } else {
            stableEvaluationCount = 0
            restorePollingInterval()
        }
        effectivePlaybackAction = action
        AppDelegate.shared.wallpaperViewModel.applyPlaybackPolicy(action)
    }

    /// Bridge the on-screen window list once. Front-to-back order is preserved,
    /// which `clickLandedOnDesktop` relies on.
    private static func captureWindowList() -> [WindowEntry] {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
        return raw.compactMap { info in
            guard let layer = info[kCGWindowLayer as String] as? Int,
                  let pid = info[kCGWindowOwnerPID as String] as? pid_t,
                  let boundsDictionary = info[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsDictionary as CFDictionary)
            else { return nil }
            let alpha = (info[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1
            return WindowEntry(layer: layer, pid: pid, bounds: bounds, alpha: alpha)
        }
    }

    private static func strongestAction(_ actions: [GSPlayback]) -> GSPlayback {
        func rank(_ a: GSPlayback) -> Int {
            switch a { case .keepRunning: return 0; case .mute: return 1; case .pause: return 2; case .stop: return 3 }
        }
        return actions.max(by: { rank($0) < rank($1) }) ?? .keepRunning
    }

    private static func isOnBattery() -> Bool {
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let list = IOPSCopyPowerSourcesList(blob)?.takeRetainedValue() as? [CFTypeRef] else { return false }
        for ps in list {
            guard let desc = IOPSGetPowerSourceDescription(blob, ps)?.takeUnretainedValue() as? [String: Any],
                  let state = desc[kIOPSPowerSourceStateKey] as? String else { continue }
            if state == kIOPSBatteryPowerValue { return true }
        }
        return false
    }

    private static func isOtherAppPlayingAudio(selfPID: pid_t,
                                               rendererPIDs: Set<pid_t>) -> Bool {
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

        var excludedPIDs = rendererPIDs
        excludedPIDs.insert(selfPID)

        for process in processes {
            guard audioProcessIsRunningOutput(process),
                  let pid = audioProcessPID(process),
                  !excludedPIDs.contains(pid) else { continue }
            return true
        }
        return false
    }

    private static func audioProcessIsRunningOutput(_ process: AudioObjectID) -> Bool {
        var running: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyIsRunningOutput,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(process, &addr, 0, nil, &size, &running) == noErr else { return false }
        return running != 0
    }

    private static func audioProcessPID(_ process: AudioObjectID) -> pid_t? {
        var pid: pid_t = 0
        var size = UInt32(MemoryLayout<pid_t>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyPID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(process, &addr, 0, nil, &size, &pid) == noErr else { return nil }
        return pid
    }

    private static func appIsFullscreen(_ windows: [WindowEntry], pid: pid_t,
                                        mainScreenFrame: CGRect?) -> Bool {
        guard let main = mainScreenFrame else { return false }
        for window in windows where window.pid == pid {
            if window.bounds.width >= main.width - 1 && window.bounds.height >= main.height - 1 {
                return true
            }
        }
        return false
    }

    private static func appHasVisibleWindows(_ windows: [WindowEntry], pid: pid_t?) -> Bool {
        guard let pid else { return false }
        for window in windows where window.pid == pid {
            if window.bounds.width > 0 && window.bounds.height > 0 {
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

        let windows = Self.captureWindowList()
        let rendererPIDs = AppDelegate.shared.wallpaperViewModel.renderer.processIdentifiers
        let selfPID = ProcessInfo.processInfo.processIdentifier

        // The click lands on the desktop only if no on-screen window sits under
        // the cursor. We deliberately include chrome such as the Dock, the menu
        // bar and Control Center (positive window layers, non-regular owners) so
        // that clicking them is never mistaken for a reveal gesture. Windows are
        // returned front-to-back; the first one containing the point wins.
        for window in windows {
            guard window.layer >= 0,
                  window.pid != selfPID, !rendererPIDs.contains(window.pid),
                  window.alpha > 0.05 else { continue }
            if window.bounds.contains(cgPoint) {
                return false
            }
        }
        return true
    }

    private static func isDesktopExposed(_ windows: [WindowEntry],
                                         inputs: PolicyInputs) -> Bool {
        var displayCount: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &displayCount)
        var displayIDs = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
        CGGetActiveDisplayList(displayCount, &displayIDs, &displayCount)
        let displays = displayIDs.prefix(Int(displayCount)).map(CGDisplayBounds)

        for window in windows {
            guard window.layer == 0,
                  window.pid != inputs.selfPID,
                  !inputs.rendererPIDs.contains(window.pid),
                  inputs.regularPIDs.contains(window.pid),
                  window.bounds.width >= 120, window.bounds.height >= 80,
                  window.alpha > 0.05 else { continue }

            let bounds = window.bounds
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
