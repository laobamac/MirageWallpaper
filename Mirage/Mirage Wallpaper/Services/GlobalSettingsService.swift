//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import Combine
import SwiftUI
import Observation
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
    var pauseWhenWindowCoverageExceeds: Bool? = false
    var windowCoverageThreshold: Double? = 90

    var shouldPauseWhenWindowCoverageExceeds: Bool {
        pauseWhenWindowCoverageExceeds ?? false
    }

    var normalizedWindowCoverageThreshold: Double {
        min(100, max(1, windowCoverageThreshold ?? 90))
    }

    var hasWindowPlaybackRules: Bool {
        otherApplicationFocused != .keepRunning ||
            otherApplicationFullscreen != .keepRunning ||
            shouldPauseWhenWindowCoverageExceeds
    }

    var hasPlaybackRules: Bool {
        hasWindowPlaybackRules || otherApplicationPlayingAudio != .keepRunning ||
            displayAsleep != .keepRunning || laptopOnBattery != .keepRunning
    }
    
    // MARK: Quality
    var antiAliasing = GSAntiAliasingQuality.msaa_x2
    var postProcessing = GSPostProcessingQuality.disabled
    var textureResolution = GSTextureResolutionQuality.automatic
    var metalFXEnabled: Bool? = false
    // Optional keeps settings written by older Mirage versions decodable.
    var wallpaperLoadSource: GSWallpaperLoadSource? = .disk
    var animatedPreviewPlayback: GSAnimatedPreviewPlayback? = .hover
    var reflections = false
    var fps: Double = 30

    var animatedPreviewPlaybackMode: GSAnimatedPreviewPlayback {
        animatedPreviewPlayback ?? .hover
    }

    var shouldEnableMetalFX: Bool {
        metalFXEnabled ?? false
    }
    
    // MARK: Automatic Setup
    var autoStart = false
    var startupPage: String?

    var startupSection: MainSection {
        get { startupPage.flatMap(MainSection.init(rawValue:)) ?? .installed }
        set { startupPage = newValue.rawValue }
    }

    var hideMenuBarIcon: Bool? = false
    var monochromeMenuBarIcon: Bool? = false
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
    var automaticWallpaperCacheCleaning: Bool? = true

    var shouldOverrideWallpaper: Bool {
        overrideWallpaper ?? false
    }

    var shouldAutomaticallyCleanWallpaperCache: Bool {
        automaticWallpaperCacheCleaning ?? true
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
    var enableHDRVideo: Bool? = false

    var shouldEnableHDRVideo: Bool {
        enableHDRVideo ?? false
    }
    
    // MARK: Advanced
    var processPiority = GSProcessPiority.normal
    var pauseOnVRAMExhausted = false
    var restartAfterCrashing = false
    
    // MARK: Developer
    var developerMode: Bool? = false

    var shouldHideMenuBarIcon: Bool {
        hideMenuBarIcon ?? false
    }

    var shouldUseMonochromeMenuBarIcon: Bool {
        monochromeMenuBarIcon ?? false
    }

    var isDeveloperModeEnabled: Bool {
        developerMode ?? false
    }

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

@Observable
class GlobalSettingsViewModel {
    private static let loginItemIdentifier = "cn.laobamac.Mirage.LoginItem"

    private static var loginItemService: SMAppService {
        SMAppService.loginItem(identifier: loginItemIdentifier)
    }

    var settings: GlobalSettings
    {
        didSet {
            guard settings != oldValue else { return }
            if settings.language != oldValue.language {
                MirageLocalization.shared.apply(settings.language)
            }
            if settings.animatedPreviewPlaybackMode != animatedPreviewPlaybackMode {
                animatedPreviewPlaybackMode = settings.animatedPreviewPlaybackMode
            }
            if settings.hasValidCustomSteamAPIKey != hasValidCustomSteamAPIKey {
                hasValidCustomSteamAPIKey = settings.hasValidCustomSteamAPIKey
            }
            validate()
            settingsChanges.send(settings)
        }
    }
    
    private let settingsChanges = CurrentValueSubject<GlobalSettings, Never>(GlobalSettings())
    private var appliedAppearance: GSAppearance?
    private var monitoredWallpaperIDs: [DisplayKey: String] = [:]
    private(set) var animatedPreviewPlaybackMode: GSAnimatedPreviewPlayback = .hover
    private(set) var hasValidCustomSteamAPIKey = false

    var selection = 0

    var isSettingsPresented = false

    var isFirstLaunch = UserDefaults.standard.value(forKey: "IsFirstLaunch") as? Bool ?? true
    
    var didFinishLaunchingNotificationCancellable: Cancellable?
    var didCurrentWallpaperChangeCancellable: Cancellable?
    var didAddToLoginItemCancellable: Cancellable?
    var didChangeStatusItemVisibilityCancellable: Cancellable?
    var didChangeStatusItemIconCancellable: Cancellable?
    var didChangeDeveloperModeCancellable: Cancellable?
    var didChangeOverrideWallpaperCancellable: Cancellable?
    var didChangeWallpaperCacheCleaningCancellable: Cancellable?
    var playbackPolicySettingsCancellable: Cancellable?
    
    // In-memory snapshot of what is persisted, so the settings UI can tell
    // whether there are unsaved edits with a cheap value comparison instead of
    // decoding GlobalSettings JSON from UserDefaults on every footer render.
    private(set) var savedSettings: GlobalSettings
    private(set) var loginItemStatus: SMAppService.Status = .notRegistered
    private(set) var loginItemError: String?
    private var isValidatingSettings = false
    private var isUpdatingLoginItem = false

    init() {
        let loginItemMigrationError = Self.migrateMainAppLoginItem()
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
        initial.windowCoverageThreshold = initial.normalizedWindowCoverageThreshold
        if initial.shouldPauseWhenWindowCoverageExceeds {
            initial.otherApplicationFocused = .keepRunning
        }
        let loginStatus = Self.loginItemService.status
        switch loginStatus {
        case .enabled, .requiresApproval:
            initial.autoStart = true
        case .notRegistered, .notFound:
            initial.autoStart = false
        @unknown default:
            initial.autoStart = false
        }
        self.settings = initial
        self.savedSettings = initial
        self.loginItemStatus = loginStatus
        self.loginItemError = loginItemMigrationError
        animatedPreviewPlaybackMode = initial.animatedPreviewPlaybackMode
        hasValidCustomSteamAPIKey = initial.hasValidCustomSteamAPIKey
        settingsChanges.send(initial)
        MirageLocalization.shared.apply(self.settings.language)
        self.didFinishLaunchingNotificationCancellable =
        NotificationCenter.default.publisher(for: NSApplication.didFinishLaunchingNotification)
            .sink { [weak self] _ in self?.didFinishLaunchingNotification() }
    }
    
    deinit {
        didFinishLaunchingNotificationCancellable?.cancel()
        didCurrentWallpaperChangeCancellable?.cancel()
        didAddToLoginItemCancellable?.cancel()
        didChangeStatusItemVisibilityCancellable?.cancel()
        didChangeStatusItemIconCancellable?.cancel()
        didChangeDeveloperModeCancellable?.cancel()
        didChangeOverrideWallpaperCancellable?.cancel()
        didChangeWallpaperCacheCleaningCancellable?.cancel()
        playbackPolicySettingsCancellable?.cancel()
        playbackEvalTimer?.invalidate()
        playbackAudioMonitor?.stop()
        settlingEvalWorkItems.forEach { $0.cancel() }
        playbackRecoveryWorkItems.forEach { $0.cancel() }
        if let powerSourceRunLoopSource { CFRunLoopSourceInvalidate(powerSourceRunLoopSource) }
        for observer in workspacePlaybackObservers + playbackLifecycleObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        if let desktopClickMonitor { NSEvent.removeMonitor(desktopClickMonitor) }
        NSWorkspace.shared.notificationCenter.removeObserver(self)
        NotificationCenter.default.removeObserver(self)
    }
    
    func didFinishLaunchingNotification() {
        self.didCurrentWallpaperChangeCancellable =
        AppDelegate.shared.wallpaperViewModel.displayStatesChanges
            .sink { [weak self] in self?.didDisplayStatesChange($0) }
        
        self.didAddToLoginItemCancellable =
        self.settingsChanges
            .removeDuplicates { $0.autoStart == $1.autoStart }
            .map { $0.autoStart }
            .sink { [weak self] in self?.didAddToLoginItem($0) }

        self.didChangeStatusItemVisibilityCancellable =
        self.settingsChanges
            .removeDuplicates { $0.shouldHideMenuBarIcon == $1.shouldHideMenuBarIcon }
            .map { $0.shouldHideMenuBarIcon }
            .sink { AppDelegate.shared.applyStatusItemVisibility(hidden: $0) }

        self.didChangeStatusItemIconCancellable =
        self.settingsChanges
            .removeDuplicates {
                $0.shouldUseMonochromeMenuBarIcon == $1.shouldUseMonochromeMenuBarIcon
            }
            .map { $0.shouldUseMonochromeMenuBarIcon }
            .sink { AppDelegate.shared.applyStatusItemIcon(monochrome: $0) }

        self.didChangeDeveloperModeCancellable =
        self.settingsChanges
            .removeDuplicates { $0.isDeveloperModeEnabled == $1.isDeveloperModeEnabled }
            .map { $0.isDeveloperModeEnabled }
            .sink { AppDelegate.shared.applyDeveloperMode(enabled: $0) }
        
        self.didChangeOverrideWallpaperCancellable =
        self.settingsChanges
            .removeDuplicates { $0.shouldOverrideWallpaper == $1.shouldOverrideWallpaper }
            .map { $0.shouldOverrideWallpaper }
            .sink { DesktopOverrideService.shared.didChangeEnabled($0) }

        self.didChangeWallpaperCacheCleaningCancellable =
        self.settingsChanges
            .removeDuplicates {
                $0.shouldAutomaticallyCleanWallpaperCache
                    == $1.shouldAutomaticallyCleanWallpaperCache
            }
            .map { $0.shouldAutomaticallyCleanWallpaperCache }
            .sink { DesktopOverrideService.shared.didChangeCacheCleaningEnabled($0) }

        let lifecycleNotifications: [(Notification.Name, PlaybackLifecycleEvent)] = [
            (NSWorkspace.willSleepNotification, .systemSleep),
            (NSWorkspace.didWakeNotification, .systemWake),
            (NSWorkspace.screensDidSleepNotification, .displaySleep),
            (NSWorkspace.screensDidWakeNotification, .displayWake)
        ]
        for (name, event) in lifecycleNotifications {
            let observer = NSWorkspace.shared.notificationCenter.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak self] _ in self?.handlePlaybackLifecycleEvent(event) }
            playbackLifecycleObservers.append(observer)
        }
        NotificationCenter.default.addObserver(
            self, selector: #selector(playbackDisplaysDidChange(_:)),
            name: DisplayRegistry.didChangeNotification, object: DisplayRegistry.shared)

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
        playbackPolicySettingsCancellable = settingsChanges
            .map {
                PlaybackPolicySettingsKey(
                    focused: $0.otherApplicationFocused,
                    fullscreen: $0.otherApplicationFullscreen,
                    audio: $0.otherApplicationPlayingAudio,
                    displayAsleep: $0.displayAsleep,
                    battery: $0.laptopOnBattery,
                    coverageEnabled: $0.shouldPauseWhenWindowCoverageExceeds,
                    coverageThreshold: $0.normalizedWindowCoverageThreshold
                )
            }
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] _ in self?.configurePlaybackMonitoring() }

        self.configurePlaybackMonitoring()
    }

    private var playbackEvalTimer: Timer?
    private var playbackAudioMonitor: PlaybackAudioMonitor?
    private var audioMonitoringGeneration: UInt64 = 0
    private var otherAudioPlaying = false
    private var settlingEvalWorkItems: [DispatchWorkItem] = []
    private var playbackRecoveryWorkItems: [DispatchWorkItem] = []
    private var workspacePlaybackObservers: [NSObjectProtocol] = []
    private var playbackLifecycleObservers: [NSObjectProtocol] = []
    private var powerSourceRunLoopSource: CFRunLoopSource?
    private var desktopClickMonitor: Any?
    // A click on bare desktop starts the reveal-desktop animation, during which
    // windows are still covering the screen and geometry detection would wrongly
    // report the desktop as hidden. We briefly trust the click as a reveal hint
    // to bridge that animation, then hand back to the geometry truth.
    private var lastDesktopRevealHintAt: [CGDirectDisplayID: Date] = [:]
    private static let desktopRevealGrace: TimeInterval = 1.2
    private(set) var effectivePlaybackActions: [DisplayKey: GSPlayback] = [:]

    func effectivePlaybackAction(for key: DisplayKey) -> GSPlayback {
        effectivePlaybackActions[key] ?? .keepRunning
    }

    /// Runs the window-geometry / power / audio probes off the main thread.
    private let policyQueue = DispatchQueue(label: "com.mirage.playback-policy", qos: .utility)
    private var playbackEvaluation = PlaybackEvaluationState()
    private var lastPolicyReadFailure: PolicyReadFailure?
    private var lastFullscreenDiagnostics: [CGDirectDisplayID: String] = [:]

    struct PlaybackEvaluationState {
        struct Completion {
            let shouldApply: Bool
            let force: Bool
            let shouldEvaluateAgain: Bool
        }

        private(set) var generation: UInt64 = 0
        private(set) var isSleeping = false
        private var inFlight = false
        private var pending = false
        private var forcePending = false

        mutating func invalidate(force: Bool = false) {
            generation &+= 1
            pending = false
            forcePending = forcePending || force
        }

        mutating func suspend() {
            isSleeping = true
            invalidate(force: true)
        }

        mutating func resume() {
            isSleeping = false
            invalidate(force: true)
        }

        mutating func begin() -> UInt64? {
            guard !isSleeping else { return nil }
            guard !inFlight else {
                pending = true
                return nil
            }
            inFlight = true
            return generation
        }

        mutating func finish(generation: UInt64, hasResult: Bool) -> Completion {
            inFlight = false
            let shouldApply = hasResult && !isSleeping && generation == self.generation
            let force = shouldApply && forcePending
            if shouldApply { forcePending = false }
            let shouldEvaluateAgain = pending && !isSleeping
            pending = false
            return Completion(shouldApply: shouldApply, force: force,
                              shouldEvaluateAgain: shouldEvaluateAgain)
        }
    }

    enum PlaybackLifecycleEvent: String {
        case systemSleep, systemWake, displaySleep, displayWake, displaysChanged
    }

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
        let timer = Timer(timeInterval: interval, repeats: true) {
            [weak self] _ in self?.evaluatePlaybackState()
        }
        timer.tolerance = interval * 0.1
        playbackEvalTimer = timer
        RunLoop.main.add(timer, forMode: .common)
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
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.configurePlaybackMonitoring() }
            return
        }
        playbackEvaluation.invalidate(force: true)
        playbackEvalTimer?.invalidate()
        playbackEvalTimer = nil
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        playbackRecoveryWorkItems.forEach { $0.cancel() }
        playbackRecoveryWorkItems.removeAll()
        if let powerSourceRunLoopSource { CFRunLoopSourceInvalidate(powerSourceRunLoopSource) }
        powerSourceRunLoopSource = nil
        for observer in workspacePlaybackObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        workspacePlaybackObservers.removeAll()
        if let desktopClickMonitor {
            NSEvent.removeMonitor(desktopClickMonitor)
            self.desktopClickMonitor = nil
        }

        configureAudioMonitoring()

        guard settings.hasPlaybackRules,
              AppDelegate.shared.wallpaperViewModel.hasAnyWallpaper else {
            effectivePlaybackActions.removeAll()
            if !playbackEvaluation.isSleeping {
                AppDelegate.shared.wallpaperViewModel.applyPlaybackPolicy(.keepRunning, force: true)
            }
            return
        }

        if settings.hasWindowPlaybackRules {
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
                guard let displayID = self.displayClickedOnBareDesktop(
                    at: NSEvent.mouseLocation) else { return }
                // Record the hint so the grace window bridges the reveal
                // animation, then let the settling re-evaluations confirm the
                // state from real window geometry.
                self.lastDesktopRevealHintAt[displayID] = Date()
                self.scheduleSettlingEvaluations()
            }
        }

        // Focus/fullscreen rules also poll: revealing the desktop (click-wallpaper,
        // F11, hot corners, Mission Control) and re-covering it emit no reliable
        // notification, so periodic geometry checks keep playback correct.
        startPlaybackPolling()
        configurePowerSourceMonitoring()
        evaluatePlaybackState()
    }

    private func startPlaybackPolling() {
        guard !playbackEvaluation.isSleeping, settings.hasPlaybackRules,
              AppDelegate.shared.wallpaperViewModel.hasAnyWallpaper else { return }
        basePollInterval = settings.hasWindowPlaybackRules ? 1.0 : 2.0
        stableEvaluationCount = 0
        schedulePlaybackTimer(interval: basePollInterval)
    }

    private func configureAudioMonitoring() {
        let needsAudio = settings.otherApplicationPlayingAudio != .keepRunning &&
            AppDelegate.shared.wallpaperViewModel.hasAnyWallpaper
        guard needsAudio, !playbackEvaluation.isSleeping else {
            audioMonitoringGeneration &+= 1
            playbackAudioMonitor?.stop()
            playbackAudioMonitor = nil
            if !needsAudio { otherAudioPlaying = false }
            return
        }
        guard playbackAudioMonitor == nil else { return }
        audioMonitoringGeneration &+= 1
        let generation = audioMonitoringGeneration
        let renderer = AppDelegate.shared.wallpaperViewModel.renderer
        let selfPID = ProcessInfo.processInfo.processIdentifier
        let monitor = PlaybackAudioMonitor(
            initiallyActive: otherAudioPlaying,
            excludedPIDs: { [weak renderer] in
                (renderer?.processIdentifiers ?? []).union([selfPID])
            },
            onChange: { [weak self] active in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.audioMonitoringGeneration == generation,
                          self.otherAudioPlaying != active else { return }
                    self.otherAudioPlaying = active
                    self.playbackEvaluation.invalidate()
                    self.evaluatePlaybackState()
                }
            })
        playbackAudioMonitor = monitor
        monitor.start()
    }

    private func configurePowerSourceMonitoring() {
        guard powerSourceRunLoopSource == nil, settings.laptopOnBattery != .keepRunning,
              AppDelegate.shared.wallpaperViewModel.hasAnyWallpaper else { return }
        let context = Unmanaged.passUnretained(self).toOpaque()
        guard let source = IOPSNotificationCreateRunLoopSource({ context in
            guard let context else { return }
            Unmanaged<GlobalSettingsViewModel>.fromOpaque(context)
                .takeUnretainedValue().powerSourceDidChange()
        }, context)?.takeRetainedValue() else {
            MirageLogService.shared.append("powerSource monitor unavailable", source: "playback-policy")
            return
        }
        powerSourceRunLoopSource = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
    }

    private func powerSourceDidChange() {
        playbackEvaluation.invalidate()
        stableEvaluationCount = 0
        restorePollingInterval()
        scheduleSettlingEvaluations()
    }

    @objc private func playbackDisplaysDidChange(_ notification: Notification) {
        guard DisplayRegistry.Change.from(notification)?.topologyChanged == true else { return }
        handlePlaybackLifecycleEvent(.displaysChanged)
    }

    func handlePlaybackLifecycleEvent(_ event: PlaybackLifecycleEvent) {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.handlePlaybackLifecycleEvent(event) }
            return
        }
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        playbackRecoveryWorkItems.forEach { $0.cancel() }
        playbackRecoveryWorkItems.removeAll()
        lastDesktopRevealHintAt.removeAll()
        switch event {
        case .systemSleep:
            playbackEvaluation.suspend()
        case .systemWake, .displayWake:
            playbackEvaluation.resume()
            DisplayRegistry.shared.invalidate()
        case .displaySleep:
            playbackEvaluation.invalidate(force: true)
        case .displaysChanged:
            playbackEvaluation.invalidate(force: true)
        }
        playbackEvalTimer?.invalidate()
        playbackEvalTimer = nil
        configureAudioMonitoring()
        MirageLogService.shared.append(
            "event=\(event.rawValue) generation=\(playbackEvaluation.generation)",
            source: "playback-policy")
        guard !playbackEvaluation.isSleeping else { return }
        startPlaybackPolling()
        configurePowerSourceMonitoring()
        evaluatePlaybackState()
        let generation = playbackEvaluation.generation
        for delay in [0.7, 2.0] {
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.playbackEvaluation.generation == generation else { return }
                DisplayRegistry.shared.invalidate()
                self.evaluatePlaybackState()
            }
            playbackRecoveryWorkItems.append(work)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        }
    }

    // Thermal and low-power transitions change the frame budget, not the
    // playback decision, so they skip the full evaluation and just re-apply.
    @objc private func powerStateDidChange()   { reapplyPowerBudget() }
    @objc private func thermalStateDidChange() { reapplyPowerBudget() }

    private func reapplyPowerBudget() {
        // Both notifications may arrive on an arbitrary thread.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            AppDelegate.shared.wallpaperViewModel
                .applyPlaybackPolicies(self.effectivePlaybackActions)
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
        guard !playbackEvaluation.isSleeping else { return }
        settlingEvalWorkItems.forEach { $0.cancel() }
        settlingEvalWorkItems.removeAll()
        let generation = playbackEvaluation.generation
        for delay in [0.05, 0.7] {
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.playbackEvaluation.generation == generation else { return }
                self.evaluatePlaybackState()
            }
            settlingEvalWorkItems.append(work)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        }
    }
    
    func didAddToLoginItem(_ added: Bool) {
        guard !isUpdatingLoginItem else { return }
        isUpdatingLoginItem = true
        defer { isUpdatingLoginItem = false }
        let appService = Self.loginItemService
        let legacyService = SMAppService.mainApp
        loginItemError = nil
        do {
            if added {
                try Self.removeLegacyLaunchAgentIfPresent()
                switch appService.status {
                case .notRegistered, .notFound:
                    try appService.register()
                case .enabled, .requiresApproval:
                    break
                @unknown default:
                    break
                }
                try Self.unregisterIfNeeded(legacyService)
            } else {
                try Self.unregisterIfNeeded(legacyService)
                try Self.unregisterIfNeeded(appService)
            }
        } catch {
            let nsError = error as NSError
            if nsError.code == kSMErrorInvalidSignature {
                loginItemError = L("当前 Mirage 签名无效，无法注册登录项。")
            } else if nsError.code == kSMErrorLaunchDeniedByUser {
                loginItemError = L("登录项已被系统拒绝，请在系统设置中批准。")
            } else {
                loginItemError = L("无法更新登录项：%@", nsError.localizedDescription)
            }
            NSLog("[Mirage] Failed to update login item: %@ (%@:%ld)", nsError.localizedDescription, nsError.domain, nsError.code)
        }
        refreshLoginItemStatus(persist: true)
    }

    func refreshLoginItemStatus(persist: Bool = false) {
        let status = Self.loginItemService.status
        if loginItemStatus != status {
            loginItemStatus = status
        }
        let enabled: Bool
        switch status {
        case .enabled, .requiresApproval:
            enabled = true
        case .notRegistered, .notFound:
            enabled = false
        @unknown default:
            enabled = false
        }
        if settings.autoStart != enabled {
            let wasUpdating = isUpdatingLoginItem
            isUpdatingLoginItem = true
            settings.autoStart = enabled
            isUpdatingLoginItem = wasUpdating
            if persist { save() }
        }
    }

    func openLoginItemSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    private static func migrateMainAppLoginItem() -> String? {
        let helper = loginItemService
        let legacy = SMAppService.mainApp
        let legacyEnabled: Bool
        switch legacy.status {
        case .enabled, .requiresApproval:
            legacyEnabled = true
        case .notRegistered, .notFound:
            legacyEnabled = false
        @unknown default:
            legacyEnabled = false
        }
        guard legacyEnabled else { return nil }
        do {
            switch helper.status {
            case .notRegistered, .notFound:
                try helper.register()
            case .enabled, .requiresApproval:
                break
            @unknown default:
                break
            }
            try legacy.unregister()
            return nil
        } catch {
            let nsError = error as NSError
            return L("无法迁移登录项：%@", nsError.localizedDescription)
        }
    }

    private static func unregisterIfNeeded(_ service: SMAppService) throws {
        switch service.status {
        case .enabled, .requiresApproval:
            try service.unregister()
        case .notRegistered, .notFound:
            break
        @unknown default:
            break
        }
    }

    private static func removeLegacyLaunchAgentIfPresent() throws {
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/LaunchAgents/cn.laobamac.Mirage.plist")
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = ["bootout", "gui/\(getuid())/cn.laobamac.Mirage"]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        if (try? process.run()) != nil {
            process.waitUntilExit()
        }
        try FileManager.default.removeItem(at: url)
        NSLog("[Mirage] Removed legacy LaunchAgent login item")
    }

    func didDisplayStatesChange(_ states: [DisplayKey: DisplayWallpaperState]) {
        let identities = states.mapValues { $0.wallpaper.id }
        guard identities != monitoredWallpaperIDs else { return }
        monitoredWallpaperIDs = identities
        if playbackPolicySettingsCancellable != nil {
            DispatchQueue.main.async { [weak self] in self?.configurePlaybackMonitoring() }
        }
    }
    
    func reset() {
        var restored = (try? JSONDecoder()
            .decode(GlobalSettings.self,
                from: UserDefaults.standard.data(forKey: "GlobalSettings")
            ?? Data()))
        ?? GlobalSettings()
        restored.animatedPreviewPlayback = restored.animatedPreviewPlayback ?? .hover
        if settings != restored {
            settings = restored
        }
        if savedSettings != restored {
            savedSettings = restored
        }
    }

    func save() {
        guard let data = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(data, forKey: "GlobalSettings")
        let loadFromMemory = (settings.wallpaperLoadSource ?? .disk) == .memory
        ScreenSaverManager.shared.updateGlobalSettings(settings, languageIdentifier: MirageLocalization.shared.locale.identifier)
        Task { @MainActor in
            DynamicLockScreenManager.shared.updateLoadFromMemory(loadFromMemory)
            ScreenSaverDynamicLockScreenManager.shared.updateLoadFromMemory(loadFromMemory)
        }
        if savedSettings != settings {
            savedSettings = settings
        }
    }
    
    func setQuality(_ quality: GSQuality) {
        switch quality {
        case .low:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .disabled
            self.settings.textureResolution = .highPerformance
            self.settings.metalFXEnabled = false
            self.settings.fps = 10
            self.settings.reflections = false
        case .medium:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .automatic
            self.settings.metalFXEnabled = true
            self.settings.fps = 15
            self.settings.reflections = true
        case .high:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .automatic
            self.settings.metalFXEnabled = true
            self.settings.fps = 25
            self.settings.reflections = true
        case .ultra:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .ultra
            self.settings.textureResolution = .highQuality
            self.settings.metalFXEnabled = true
            self.settings.fps = 30
            self.settings.reflections = true
        }
    }
    
    private func validate() {
        guard !isValidatingSettings else { return }
        isValidatingSettings = true
        defer { isValidatingSettings = false }
        let threshold = settings.normalizedWindowCoverageThreshold
        if settings.windowCoverageThreshold != threshold {
            settings.windowCoverageThreshold = threshold
        }
        if settings.shouldPauseWhenWindowCoverageExceeds,
           settings.otherApplicationFocused != .keepRunning {
            settings.otherApplicationFocused = .keepRunning
        }
        guard appliedAppearance != settings.appearance else { return }
        appliedAppearance = settings.appearance
        switch settings.appearance {
        case .light:
            NSApp.appearance = NSAppearance(named: .aqua)
        case .dark:
            NSApp.appearance = NSAppearance(named: .darkAqua)
        case .followSystem:
            NSApp.appearance = nil
        }
    }

    func setFocusedPlaybackRule(_ rule: GSPlayback) {
        if rule != .keepRunning {
            settings.pauseWhenWindowCoverageExceeds = false
        }
        settings.otherApplicationFocused = rule
    }

    func setWindowCoveragePauseEnabled(_ enabled: Bool) {
        if enabled {
            settings.otherApplicationFocused = .keepRunning
        }
        settings.pauseWhenWindowCoverageExceeds = enabled
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
    struct PolicyInputs {
        var onDisplayAsleep = GSPlayback.keepRunning
        var onBattery = GSPlayback.keepRunning
        var onFocused = GSPlayback.keepRunning
        var onFullscreen = GSPlayback.keepRunning
        var onAudio = GSPlayback.keepRunning
        var otherAudioPlaying = false
        var pauseOnCoverage = false
        var coverageThreshold: CGFloat = 0.9

        var revealGraceDisplays: Set<CGDirectDisplayID> = []

        var frontPID: pid_t?
        var frontBundleID: String?
        var frontIsRegular = false

        var selfPID: pid_t = 0
        var rendererPIDs: Set<pid_t> = []
        /// PIDs whose `activationPolicy` is `.regular`, resolved up front because
        /// `NSWorkspace.runningApplications` is AppKit state.
        var regularPIDs: Set<pid_t> = []
        var wallpaperDisplays: [CGDirectDisplayID: CGRect] = [:]
        var fullscreenSafeAreas: [CGDirectDisplayID: CGRect] = [:]
        var diagnoseFullscreen = false
    }

    private struct PlaybackPolicySettingsKey: Equatable {
        var focused: GSPlayback
        var fullscreen: GSPlayback
        var audio: GSPlayback
        var displayAsleep: GSPlayback
        var battery: GSPlayback
        var coverageEnabled: Bool
        var coverageThreshold: Double
    }

    /// One parsed entry of the on-screen window list. The raw CFDictionary form
    /// is bridged once per evaluation and then reused by every geometry test.
    struct WindowEntry {
        var layer: Int
        var pid: pid_t
        var bounds: CGRect
        var alpha: Double
    }

    private struct FullscreenMatch {
        let window: WindowEntry
        var companion: WindowEntry? = nil
    }

    struct PolicyProbes {
        var onBattery: () -> Bool? = GlobalSettingsViewModel.isOnBattery
        var displayAsleep: (CGDirectDisplayID) -> Bool = { CGDisplayIsAsleep($0) != 0 }
        var windows: () -> [WindowEntry]? = GlobalSettingsViewModel.captureWindowList
    }

    struct PolicyResult {
        let actions: [CGDirectDisplayID: GSPlayback]
        let onBattery: Bool?
        var fullscreenDiagnostics: [CGDirectDisplayID: String] = [:]
    }

    enum PolicyReadFailure: String, Error {
        case displaysUnavailable, powerSourceUnavailable, windowListUnavailable
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
        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        guard wallpaperViewModel.hasAnyWallpaper,
              let generation = playbackEvaluation.begin() else { return }
        let inputs = collectPolicyInputs(for: wallpaperViewModel)
        let started = ProcessInfo.processInfo.systemUptime
        policyQueue.async { [weak self] in
            let result = Self.computePlaybackActions(inputs)
            DispatchQueue.main.async {
                guard let self else { return }
                let hasResult: Bool
                switch result {
                case .success: hasResult = true
                case .failure: hasResult = false
                }
                let completion = self.playbackEvaluation.finish(
                    generation: generation, hasResult: hasResult)
                if completion.shouldApply, case .success(let value) = result {
                    self.lastPolicyReadFailure = nil
                    self.applyPolicyResult(value, force: completion.force,
                                           elapsed: ProcessInfo.processInfo.systemUptime - started)
                } else if generation == self.playbackEvaluation.generation,
                          case .failure(let failure) = result,
                          failure != self.lastPolicyReadFailure {
                    self.lastPolicyReadFailure = failure
                    MirageLogService.shared.append(
                        "generation=\(generation) deferred=\(failure.rawValue)", source: "playback-policy")
                }
                if completion.shouldEvaluateAgain { self.evaluatePlaybackState() }
            }
        }
    }

    /// Main-thread half: read AppKit state into plain values. Cheap by design.
    func collectPolicyInputs(for wallpaperViewModel: WallpaperViewModel) -> PolicyInputs {
        var inputs = PolicyInputs()
        inputs.onDisplayAsleep = settings.displayAsleep
        inputs.onBattery = settings.laptopOnBattery
        inputs.onFocused = settings.otherApplicationFocused
        inputs.onFullscreen = settings.otherApplicationFullscreen
        inputs.diagnoseFullscreen = settings.isDeveloperModeEnabled && inputs.onFullscreen != .keepRunning
        inputs.onAudio = settings.otherApplicationPlayingAudio
        inputs.otherAudioPlaying = otherAudioPlaying
        inputs.pauseOnCoverage = settings.shouldPauseWhenWindowCoverageExceeds
        inputs.coverageThreshold = CGFloat(settings.normalizedWindowCoverageThreshold / 100)

        inputs.selfPID = ProcessInfo.processInfo.processIdentifier

        let needsWindowGeometry = settings.hasWindowPlaybackRules
        for info in DisplayRegistry.shared.connected where wallpaperViewModel.displayStates[info.key] != nil {
            inputs.wallpaperDisplays[info.displayID] = needsWindowGeometry ? CGDisplayBounds(info.displayID) : .zero
        }
        if inputs.onFullscreen != .keepRunning {
            for screen in NSScreen.screens {
                guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
                      let bounds = inputs.wallpaperDisplays[number.uint32Value] else { continue }
                inputs.fullscreenSafeAreas[number.uint32Value] = Self.fullscreenSafeArea(
                    display: bounds, screenFrame: screen.frame, insets: screen.safeAreaInsets)
            }
        }
        let needsRendererPIDs = needsWindowGeometry ||
            settings.otherApplicationPlayingAudio != .keepRunning
        if needsRendererPIDs {
            inputs.rendererPIDs = wallpaperViewModel.renderer.processIdentifiers
        }
        guard needsWindowGeometry else { return inputs }

        let now = Date()
        lastDesktopRevealHintAt = lastDesktopRevealHintAt.filter {
            now.timeIntervalSince($0.value) < Self.desktopRevealGrace
        }
        inputs.revealGraceDisplays = Set(lastDesktopRevealHintAt.keys)

        let front = NSWorkspace.shared.frontmostApplication
        inputs.frontPID = front?.processIdentifier
        inputs.frontBundleID = front?.bundleIdentifier
        inputs.frontIsRegular = front?.activationPolicy == .regular
        for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular {
            inputs.regularPIDs.insert(app.processIdentifier)
        }
        return inputs
    }

    /// Background half: window geometry, power source and audio probes. Static so
    /// it provably touches no main-thread-owned state.
    static func computePlaybackActions(
        _ inputs: PolicyInputs, probes: PolicyProbes = PolicyProbes()
    ) -> Result<PolicyResult, PolicyReadFailure> {
        guard !inputs.wallpaperDisplays.isEmpty else { return .failure(.displaysUnavailable) }
        var globalActions: [GSPlayback] = []
        var onBattery: Bool?
        if inputs.onBattery != .keepRunning {
            guard let sampled = probes.onBattery() else { return .failure(.powerSourceUnavailable) }
            onBattery = sampled
            if sampled { globalActions.append(inputs.onBattery) }
        }
        if inputs.onAudio != .keepRunning, inputs.otherAudioPlaying {
            globalActions.append(inputs.onAudio)
        }
        let needsWindows = inputs.onFocused != .keepRunning ||
            inputs.onFullscreen != .keepRunning || inputs.pauseOnCoverage
        let windows: [WindowEntry]
        if needsWindows {
            guard inputs.wallpaperDisplays.values.allSatisfy({ $0.width > 0 && $0.height > 0 }) else {
                return .failure(.displaysUnavailable)
            }
            guard let sampled = probes.windows() else { return .failure(.windowListUnavailable) }
            windows = sampled
        } else {
            windows = []
        }
        let isSelf = inputs.frontPID == inputs.selfPID
        let isDesktopFinder = inputs.frontBundleID == "com.apple.finder" &&
            !appHasVisibleWindows(windows, pid: inputs.frontPID)
        var result: [CGDirectDisplayID: GSPlayback] = [:]
        var fullscreenDiagnostics: [CGDirectDisplayID: String] = [:]
        for (displayID, bounds) in inputs.wallpaperDisplays {
            var actions = globalActions
            if inputs.onDisplayAsleep != .keepRunning,
               probes.displayAsleep(displayID) {
                actions.append(inputs.onDisplayAsleep)
            }
            if inputs.pauseOnCoverage,
               windowCoverageExceedsThreshold(windows, display: bounds, inputs: inputs) {
                actions.append(.pause)
            }
            let revealing = inputs.revealGraceDisplays.contains(displayID)
            let desktopExposed = needsWindows && isDesktopExposed(windows, display: bounds, inputs: inputs)
            var fullscreen: FullscreenMatch?
            if !revealing, !desktopExposed {
                if inputs.onFullscreen != .keepRunning,
                   let window = fullscreenWindow(windows, display: bounds,
                                                 safeArea: inputs.fullscreenSafeAreas[displayID], inputs: inputs) {
                    fullscreen = window
                    actions.append(inputs.onFullscreen)
                } else if inputs.onFocused != .keepRunning,
                          let frontPID = inputs.frontPID, inputs.frontIsRegular,
                          !isSelf, !isDesktopFinder,
                          appHasVisibleWindow(windows, pid: frontPID, display: bounds) {
                    actions.append(inputs.onFocused)
                }
            }
            if inputs.diagnoseFullscreen {
                fullscreenDiagnostics[displayID] = fullscreenDiagnostic(
                    windows, display: bounds, safeArea: inputs.fullscreenSafeAreas[displayID],
                    inputs: inputs, match: fullscreen, revealing: revealing, desktopExposed: desktopExposed)
            }
            result[displayID] = strongestAction(actions)
        }
        return .success(PolicyResult(actions: result, onBattery: onBattery,
                                     fullscreenDiagnostics: fullscreenDiagnostics))
    }

    /// Main-thread tail: publish the decision and retune the polling cadence.
    private func applyPolicyResult(_ result: PolicyResult, force: Bool, elapsed: TimeInterval) {
        let registry = DisplayRegistry.shared
        let actions = Dictionary(uniqueKeysWithValues: result.actions.compactMap { displayID, action in
            registry.info(forDisplay: displayID).map { ($0.key, action) }
        })
        let changed = actions != effectivePlaybackActions
        if !changed && !force {
            stableEvaluationCount += 1
            if stableEvaluationCount >= Self.stableEvaluationsBeforeBackoff {
                stableEvaluationCount = 0
                backOffPollingInterval()
            }
        } else {
            stableEvaluationCount = 0
            restorePollingInterval()
        }
        effectivePlaybackActions = actions
        AppDelegate.shared.wallpaperViewModel.applyPlaybackPolicies(actions, force: force)
        for (displayID, diagnostic) in result.fullscreenDiagnostics.sorted(by: { $0.key < $1.key })
        where diagnostic != lastFullscreenDiagnostics[displayID] {
            MirageLogService.shared.append("display=\(displayID) \(diagnostic)", source: "playback-fullscreen")
        }
        lastFullscreenDiagnostics = result.fullscreenDiagnostics
        if changed || force {
            let decisions = result.actions.sorted { $0.key < $1.key }
                .map { "\($0.key):\($0.value.rawValue)" }.joined(separator: ",")
            let battery = result.onBattery.map { String($0) } ?? "unused"
            MirageLogService.shared.append(
                "generation=\(playbackEvaluation.generation) battery=\(battery) actions=[\(decisions)] force=\(force) elapsedMs=\(Int(elapsed * 1000))",
                source: "playback-policy")
        }
    }

    /// Bridge the on-screen window list once. Front-to-back order is preserved,
    /// which `clickLandedOnDesktop` relies on.
    private static func captureWindowList() -> [WindowEntry]? {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else { return nil }
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

    private static func isOnBattery() -> Bool? {
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let source = IOPSGetProvidingPowerSourceType(blob)?.takeUnretainedValue() as String? else { return nil }
        switch source {
        case kIOPMBatteryPowerKey: return true
        case kIOPMACPowerKey, kIOPMUPSPowerKey: return false
        default: return nil
        }
    }

    static func fullscreenSafeArea(display: CGRect, screenFrame: CGRect, insets: NSEdgeInsets) -> CGRect? {
        let dimensions = [display.width, display.height, screenFrame.width, screenFrame.height]
        let margins = [insets.top, insets.left, insets.bottom, insets.right]
        guard dimensions.allSatisfy({ $0.isFinite && $0 > 0 }),
              display.minX.isFinite, display.minY.isFinite,
              margins.allSatisfy({ $0.isFinite && $0 >= 0 }), margins.contains(where: { $0 > 0 }),
              insets.left + insets.right < screenFrame.width,
              insets.top + insets.bottom < screenFrame.height else { return nil }
        let scaleX = display.width / screenFrame.width
        let scaleY = display.height / screenFrame.height
        return CGRect(x: display.minX + insets.left * scaleX,
                      y: display.minY + insets.top * scaleY,
                      width: display.width - (insets.left + insets.right) * scaleX,
                      height: display.height - (insets.top + insets.bottom) * scaleY)
    }

    private static func isFullscreenCandidate(_ window: WindowEntry, inputs: PolicyInputs) -> Bool {
        window.layer == 0 && window.alpha > 0.05 &&
            window.pid != inputs.selfPID && !inputs.rendererPIDs.contains(window.pid) &&
            inputs.regularPIDs.contains(window.pid) &&
            window.bounds.width >= 120 && window.bounds.height >= 80
    }

    private static func fullscreenWindow(_ windows: [WindowEntry], display: CGRect,
                                         safeArea: CGRect?, inputs: PolicyInputs) -> FullscreenMatch? {
        let candidates = windows.filter { isFullscreenCandidate($0, inputs: inputs) }
        for window in candidates {
            let intersection = window.bounds.intersection(display)
            guard !intersection.isNull else { continue }
            let displayArea = display.width * display.height
            let windowArea = window.bounds.width * window.bounds.height
            let intersectionArea = intersection.width * intersection.height
            let tolerance = max(4, min(display.width, display.height) * 0.005)
            let edgesMatch = abs(window.bounds.minX - display.minX) <= tolerance &&
                abs(window.bounds.minY - display.minY) <= tolerance &&
                abs(window.bounds.maxX - display.maxX) <= tolerance &&
                abs(window.bounds.maxY - display.maxY) <= tolerance
            if edgesMatch || (intersectionArea / max(displayArea, 1) >= 0.985 &&
                              intersectionArea / max(windowArea, 1) >= 0.90) {
                return FullscreenMatch(window: window)
            }
            if let safeArea, safeArea.width > 0, safeArea.height > 0, display.contains(safeArea),
               abs(window.bounds.minX - safeArea.minX) <= 1,
               abs(window.bounds.minY - safeArea.minY) <= 1,
               abs(window.bounds.maxX - safeArea.maxX) <= 1,
               abs(window.bounds.maxY - safeArea.maxY) <= 1 {
                return FullscreenMatch(window: window)
            }
        }
        var targets = [display]
        if let safeArea, safeArea.width > 0, safeArea.height > 0, display.contains(safeArea) {
            targets.append(safeArea)
        }
        for target in targets {
            let aligned = candidates.filter {
                abs($0.bounds.minX - target.minX) <= 1 &&
                    abs($0.bounds.maxX - target.maxX) <= 1
            }
            for upper in aligned where abs(upper.bounds.minY - target.minY) <= 1 {
                for lower in aligned where lower.pid == upper.pid {
                    guard abs(lower.bounds.maxY - target.maxY) <= 1,
                          lower.bounds.height > target.height / 2,
                          upper.bounds.height < lower.bounds.height,
                          lower.bounds.minY > upper.bounds.minY + 1,
                          upper.bounds.maxY < lower.bounds.maxY - 1,
                          upper.bounds.maxY - lower.bounds.minY > 1 else { continue }
                    return FullscreenMatch(window: lower, companion: upper)
                }
            }
        }
        return nil
    }

    private static func fullscreenDiagnostic(_ windows: [WindowEntry], display: CGRect, safeArea: CGRect?,
                                             inputs: PolicyInputs, match: FullscreenMatch?, revealing: Bool,
                                             desktopExposed: Bool) -> String {
        func rectangle(_ bounds: CGRect) -> String {
            "\(bounds.minX),\(bounds.minY),\(bounds.width),\(bounds.height)"
        }
        let candidates = windows.lazy.filter {
            $0.bounds.width >= 120 && $0.bounds.height >= 80 && !$0.bounds.intersection(display).isNull
        }.prefix(6).map {
            "pid=\($0.pid),layer=\($0.layer),alpha=\($0.alpha),eligible=\(isFullscreenCandidate($0, inputs: inputs)),bounds=\(rectangle($0.bounds))"
        }.joined(separator: ";")
        let safe = safeArea.map(rectangle) ?? "none"
        let matched = match.map { "\($0.window.pid):\(rectangle($0.window.bounds))" } ?? "none"
        let companion = match?.companion.map { "\($0.pid):\(rectangle($0.bounds))" } ?? "none"
        return "bounds=\(rectangle(display)) safe=\(safe) front=\(inputs.frontPID ?? 0) reveal=\(revealing) exposed=\(desktopExposed) match=\(matched) companion=\(companion) windows=[\(candidates)]"
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

    private static func appHasVisibleWindow(_ windows: [WindowEntry], pid: pid_t,
                                            display: CGRect) -> Bool {
        windows.contains {
            $0.pid == pid && $0.layer == 0 && $0.alpha > 0.05 &&
                !$0.bounds.intersection(display).isNull
        }
    }

    private static func windowCoverageExceedsThreshold(_ windows: [WindowEntry],
                                                        display: CGRect,
                                                        inputs: PolicyInputs) -> Bool {
        let candidates = windows.filter {
            $0.layer == 0 &&
            $0.pid != inputs.selfPID &&
            !inputs.rendererPIDs.contains($0.pid) &&
            inputs.regularPIDs.contains($0.pid) &&
            $0.alpha > 0.05 &&
            $0.bounds.width >= 120 &&
            $0.bounds.height >= 80
        }
        let area = display.width * display.height
        guard area > 0 else { return false }
        let rectangles = candidates.compactMap { window -> CGRect? in
            let clipped = window.bounds.intersection(display).standardized
            guard !clipped.isNull, clipped.width > 0, clipped.height > 0 else { return nil }
            return clipped
        }
        return rectangleUnionArea(rectangles) / area >= inputs.coverageThreshold
    }

    private static func rectangleUnionArea(_ rectangles: [CGRect]) -> CGFloat {
        let xCoordinates = Array(Set(rectangles.flatMap { [$0.minX, $0.maxX] })).sorted()
        guard xCoordinates.count > 1 else { return 0 }
        var area: CGFloat = 0
        for index in 0..<(xCoordinates.count - 1) {
            let left = xCoordinates[index]
            let right = xCoordinates[index + 1]
            guard right > left else { continue }
            let intervals = rectangles.compactMap { rectangle -> ClosedRange<CGFloat>? in
                guard rectangle.minX < right, rectangle.maxX > left else { return nil }
                return rectangle.minY...rectangle.maxY
            }.sorted { $0.lowerBound < $1.lowerBound }
            guard var current = intervals.first else { continue }
            var height: CGFloat = 0
            for interval in intervals.dropFirst() {
                if interval.lowerBound <= current.upperBound {
                    current = current.lowerBound...max(current.upperBound, interval.upperBound)
                } else {
                    height += current.upperBound - current.lowerBound
                    current = interval
                }
            }
            height += current.upperBound - current.lowerBound
            area += (right - left) * height
        }
        return area
    }

    /// Hit-test the click point against the on-screen window list to decide
    /// whether the user clicked bare desktop (a reveal-desktop gesture) rather
    /// than any on-screen UI. Evaluated at mouse-down, before a reveal animation
    /// moves windows, so it does not depend on transient window geometry.
    private func displayClickedOnBareDesktop(at screenPoint: NSPoint) -> CGDirectDisplayID? {
        // NSEvent.mouseLocation is in AppKit coordinates (origin bottom-left of
        // the main screen). CGWindowList bounds are in CoreGraphics coordinates
        // (origin top-left). Flip Y using the primary display height.
        guard let primary = NSScreen.screens.first(where: { $0.frame.origin == .zero }) ?? NSScreen.main else {
            return nil
        }
        let cgPoint = CGPoint(x: screenPoint.x, y: primary.frame.height - screenPoint.y)
        guard let displayID = DisplayRegistry.shared.connected.first(where: {
            CGDisplayBounds($0.displayID).contains(cgPoint)
        })?.displayID else { return nil }

        guard let windows = Self.captureWindowList() else { return nil }
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
                return nil
            }
        }
        return displayID
    }

    private static func isDesktopExposed(_ windows: [WindowEntry], display: CGRect,
                                         inputs: PolicyInputs) -> Bool {
        for window in windows {
            guard window.layer == 0,
                  window.pid != inputs.selfPID,
                  !inputs.rendererPIDs.contains(window.pid),
                  inputs.regularPIDs.contains(window.pid),
                  window.bounds.width >= 120, window.bounds.height >= 80,
                  window.alpha > 0.05 else { continue }

            let bounds = window.bounds
            let windowArea = bounds.width * bounds.height
            let visibleArea = bounds.intersection(display).standardized
            guard !visibleArea.isNull else { continue }
            let intersectionArea = visibleArea.width * visibleArea.height
            let screenArea = display.width * display.height
            if intersectionArea >= 30_000,
               (intersectionArea / max(windowArea, 1) >= 0.25 ||
                intersectionArea / max(screenArea, 1) >= 0.02) {
                return false
            }
        }
        return true
    }
}


final class PlaybackAudioMonitor {
    struct ActivityState {
        private(set) var isActive: Bool
        private var candidate: Bool?
        private var candidateSince: TimeInterval = 0
        private var unavailableSince: TimeInterval?

        init(isActive: Bool = false) {
            self.isActive = isActive
        }

        mutating func update(_ sample: Bool?, at now: TimeInterval) -> TimeInterval? {
            guard let sample else {
                candidate = nil
                if unavailableSince == nil { unavailableSince = now }
                if now - unavailableSince! >= 5 {
                    isActive = false
                    return nil
                }
                return 0.25
            }
            unavailableSince = nil
            guard sample != isActive else {
                candidate = nil
                return nil
            }
            if candidate != sample {
                candidate = sample
                candidateSince = now
            }
            let remaining = (sample ? 0.5 : 1.5) - (now - candidateSince)
            if remaining <= 0 {
                isActive = sample
                candidate = nil
                return nil
            }
            return remaining
        }
    }

    struct ProcessActivity {
        var pid: pid_t = 0
        var bundleID = ""
        var name: String?
        var executablePath: String?
        var output = false
        var input: Bool?

        func exclusionReason(excludedPIDs: Set<pid_t>) -> String? {
            if excludedPIDs.contains(pid) { return "mirage" }
            if executablePath == "/usr/sbin/systemsoundserverd" ||
                executablePath == "/usr/libexec/audiomxd" ||
                executablePath == "/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow" ||
                executablePath == "/System/Library/CoreServices/ControlCenter.app/Contents/MacOS/ControlCenter" ||
                (executablePath == nil && (bundleID == "com.apple.loginwindow" || bundleID == "com.apple.controlcenter")) {
                return "system-sound"
            }
            if executablePath == "/System/Library/PrivateFrameworks/CoreSpeech.framework/corespeechd" ||
                executablePath == "/System/Library/PrivateFrameworks/CoreSpeech.framework/corespeechd_system" {
                return "speech-listener"
            }
            if bundleID == "com.apple.WebKit.GPU", let name,
               name == "WebWallpaper" || name.hasPrefix("WebWallpaper ") {
                return "web-wallpaper"
            }
            return nil
        }
    }

    struct Property: Hashable {
        var object: AudioObjectID
        var selector: AudioObjectPropertySelector

        var address: AudioObjectPropertyAddress {
            AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
                                       mElement: kAudioObjectPropertyElementMain)
        }
    }

    struct ReadFailure: Error, CustomStringConvertible {
        var property: Property
        var status: OSStatus

        var description: String {
            "object=\(property.object) selector=\(property.selector) status=\(status)"
        }
    }

    struct Backend {
        var processIDs: () throws -> [AudioObjectID] = PlaybackAudioMonitor.readProcessIDs
        var activity: (AudioObjectID) throws -> ProcessActivity = PlaybackAudioMonitor.readActivity
        var addListener: (Property, DispatchQueue, @escaping AudioObjectPropertyListenerBlock) -> OSStatus = {
            property, queue, block in
            var address = property.address
            return AudioObjectAddPropertyListenerBlock(property.object, &address, queue, block)
        }
        var removeListener: (Property, DispatchQueue, @escaping AudioObjectPropertyListenerBlock) -> Void = {
            property, queue, block in
            var address = property.address
            AudioObjectRemovePropertyListenerBlock(property.object, &address, queue, block)
        }
    }

    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.playback-audio", qos: .utility)
    private let backend: Backend
    private let excludedPIDs: () -> Set<pid_t>
    private let onChange: (Bool) -> Void
    private let log: (String) -> Void
    private var state: ActivityState
    private var running = false
    private var timer: DispatchSourceTimer?
    private var work: DispatchWorkItem?
    private var revision: UInt64 = 0
    private var listeners: [Property: AudioObjectPropertyListenerBlock] = [:]
    private var listenerFailures: [Property: OSStatus] = [:]
    private var lastDiagnostic: String?

    init(initiallyActive: Bool = false, backend: Backend = Backend(),
         excludedPIDs: @escaping () -> Set<pid_t>,
         log: @escaping (String) -> Void = { MirageLogService.shared.append($0, source: "playback-audio") },
         onChange: @escaping (Bool) -> Void) {
        state = ActivityState(isActive: initiallyActive)
        self.backend = backend
        self.excludedPIDs = excludedPIDs
        self.log = log
        self.onChange = onChange
    }

    deinit {
        work?.cancel()
        timer?.cancel()
        for (property, block) in listeners { backend.removeListener(property, queue, block) }
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + 2, repeating: 2, leeway: .milliseconds(200))
            timer.setEventHandler { [weak self] in self?.scheduleSample(after: 0) }
            self.timer = timer
            timer.resume()
            sample()
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            revision &+= 1
            work?.cancel()
            work = nil
            timer?.cancel()
            timer = nil
            removeListeners()
        }
    }

    private func removeListeners() {
        for (property, block) in listeners { backend.removeListener(property, queue, block) }
        listeners.removeAll()
        listenerFailures.removeAll()
    }

    private func observe(_ property: Property) {
        guard listeners[property] == nil else { return }
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            if property.selector == kAudioHardwarePropertyServiceRestarted {
                self.removeListeners()
            }
            self.scheduleSample(after: 0)
        }
        let status = backend.addListener(property, queue, block)
        if status == noErr {
            listeners[property] = block
            listenerFailures[property] = nil
        } else if listenerFailures[property] != status {
            listenerFailures[property] = status
            log("listener unavailable \(ReadFailure(property: property, status: status))")
        }
    }

    private func scheduleSample(after delay: TimeInterval) {
        guard running else { return }
        revision &+= 1
        let expected = revision
        work?.cancel()
        let item = DispatchWorkItem { [weak self] in
            guard let self, self.running, self.revision == expected else { return }
            self.work = nil
            self.sample()
        }
        work = item
        queue.asyncAfter(deadline: .now() + delay, execute: item)
    }

    private func sample() {
        guard running else { return }
        let system = AudioObjectID(kAudioObjectSystemObject)
        observe(Property(object: system, selector: kAudioHardwarePropertyProcessObjectList))
        observe(Property(object: system, selector: kAudioHardwarePropertyServiceRestarted))
        var active = false
        var failed = false
        var diagnostics: [String] = []
        do {
            let ids = try backend.processIDs()
            let properties = Set(ids.map { Property(object: $0, selector: kAudioProcessPropertyIsRunningOutput) })
            for property in Array(listeners.keys)
            where property.object != system && !properties.contains(property) {
                if let block = listeners.removeValue(forKey: property) {
                    backend.removeListener(property, queue, block)
                }
            }
            listenerFailures = listenerFailures.filter { $0.key.object == system || properties.contains($0.key) }
            let excluded = excludedPIDs()
            for object in ids.sorted() {
                observe(Property(object: object, selector: kAudioProcessPropertyIsRunningOutput))
                do {
                    let activity = try backend.activity(object)
                    guard activity.output else { continue }
                    let reason = activity.exclusionReason(excludedPIDs: excluded)
                    if reason == nil { active = true }
                    let input = activity.input.map { $0 ? "1" : "0" } ?? "unknown"
                    diagnostics.append("object=\(object) pid=\(activity.pid) bundle=\(activity.bundleID) path=\(activity.executablePath ?? "unknown") input=\(input) output=1 reason=\(reason ?? "external")")
                } catch {
                    failed = true
                    diagnostics.append("read failed \(error)")
                }
            }
        } catch {
            failed = true
            diagnostics.append("process list failed \(error)")
        }
        let value: Bool? = active ? true : (failed ? nil : false)
        let previous = state.isActive
        let next = state.update(value, at: ProcessInfo.processInfo.systemUptime)
        let raw = value.map { $0 ? "active" : "inactive" } ?? "unknown"
        let diagnostic = "raw=\(raw) confirmed=\(state.isActive) sources=[\(diagnostics.joined(separator: "; "))]"
        if diagnostic != lastDiagnostic {
            log(diagnostic)
            lastDiagnostic = diagnostic
        }
        if previous != state.isActive { onChange(state.isActive) }
        if let next { scheduleSample(after: next) }
    }

    private static func read<T>(_ property: Property, initial: T) throws -> T {
        var address = property.address
        var value = initial
        var size = UInt32(MemoryLayout<T>.size)
        let status = withUnsafeMutablePointer(to: &value) {
            AudioObjectGetPropertyData(property.object, &address, 0, nil, &size, $0)
        }
        guard status == noErr, size == MemoryLayout<T>.size else {
            throw ReadFailure(property: property, status: status == noErr ? kAudioHardwareBadPropertySizeError : status)
        }
        return value
    }

    private static func readProcessIDs() throws -> [AudioObjectID] {
        let property = Property(object: AudioObjectID(kAudioObjectSystemObject),
                                selector: kAudioHardwarePropertyProcessObjectList)
        var address = property.address
        var size: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(property.object, &address, 0, nil, &size)
        guard status == noErr else { throw ReadFailure(property: property, status: status) }
        guard size > 0 else { return [] }
        guard size % UInt32(MemoryLayout<AudioObjectID>.size) == 0 else {
            throw ReadFailure(property: property, status: kAudioHardwareBadPropertySizeError)
        }
        var ids = [AudioObjectID](repeating: kAudioObjectUnknown,
                                 count: Int(size) / MemoryLayout<AudioObjectID>.size)
        let capacity = size
        status = ids.withUnsafeMutableBytes {
            AudioObjectGetPropertyData(property.object, &address, 0, nil, &size, $0.baseAddress!)
        }
        guard status == noErr, size <= capacity, size % UInt32(MemoryLayout<AudioObjectID>.size) == 0 else {
            throw ReadFailure(property: property, status: status == noErr ? kAudioHardwareBadPropertySizeError : status)
        }
        return Array(ids.prefix(Int(size) / MemoryLayout<AudioObjectID>.size))
    }

    private static func readActivity(_ object: AudioObjectID) throws -> ProcessActivity {
        let output = try read(Property(object: object, selector: kAudioProcessPropertyIsRunningOutput), initial: UInt32(0))
        guard output != 0 else { return ProcessActivity() }
        let pid = try read(Property(object: object, selector: kAudioProcessPropertyPID), initial: pid_t(0))
        guard pid > 0 else {
            throw ReadFailure(property: Property(object: object, selector: kAudioProcessPropertyPID),
                              status: kAudioHardwareBadObjectError)
        }
        let bundle = try read(Property(object: object, selector: kAudioProcessPropertyBundleID),
                              initial: Optional<Unmanaged<CFString>>.none)
        let bundleID = bundle?.takeRetainedValue() as String? ?? ""
        var path = [CChar](repeating: 0, count: 4 * Int(MAXPATHLEN))
        let executablePath = proc_pidpath(pid, &path, UInt32(path.count)) > 0 ? String(cString: path) : nil
        let input = try? read(Property(object: object, selector: kAudioProcessPropertyIsRunningInput), initial: UInt32(0))
        return ProcessActivity(pid: pid, bundleID: bundleID,
                               name: bundleID == "com.apple.WebKit.GPU" ? NSRunningApplication(processIdentifier: pid)?.localizedName : nil,
                               executablePath: executablePath, output: true, input: input.map { $0 != 0 })
    }
}
