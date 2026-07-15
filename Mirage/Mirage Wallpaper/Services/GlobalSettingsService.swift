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

enum GSAppearance: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case light, dark, followSystem
}

enum GSLocalization: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case en_US, zh_CN, followSystem
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
    var reflections = false
    var fps: Double = 30
    
    // MARK: Automatic Setup
    var autoStart = false
    var safeMode = false
    
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
        didSet { validate() }
    }
    
    @Published var selection = 0
    
    @Published var isFirstLaunch = UserDefaults.standard.value(forKey: "IsFirstLaunch") as? Bool ?? true
    
    var didFinishLaunchingNotificationCancellable: Cancellable?
    var didActivateApplicationNotificationCancellable: Cancellable?
    var didCurrentWallpaperChangeCancellable: Cancellable?
    var didAddToLoginItemCancellable: Cancellable?
    var didChangeAdjustMenuBarTintCancellable: Cancellable?
    
    init() {
        if let data = UserDefaults.standard.data(forKey: "GlobalSettings"),
           let settings = try? JSONDecoder().decode(GlobalSettings.self, from: data) {
            self.settings = settings
        } else {
            self.settings = GlobalSettings()
        }
        if !MirageRegion.isMainlandChina {
            self.settings.steamAPIEndpoint = .official
        }
        self.didFinishLaunchingNotificationCancellable =
        NotificationCenter.default.publisher(for: NSApplication.didFinishLaunchingNotification)
            .sink { [weak self] _ in self?.didFinishLaunchingNotification() }
    }
    
    deinit {
        didActivateApplicationNotificationCancellable?.cancel()
        didFinishLaunchingNotificationCancellable?.cancel()
        didCurrentWallpaperChangeCancellable?.cancel()
        didAddToLoginItemCancellable?.cancel()
        didChangeAdjustMenuBarTintCancellable?.cancel()
    }
    
    func didFinishLaunchingNotification() {
        self.didActivateApplicationNotificationCancellable =
        NotificationCenter.default.publisher(for: NSWorkspace.didActivateApplicationNotification)
            .sink { [weak self] _ in self?.activateApplicationDidChange() }
        
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

        self.playbackEvalTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.evaluatePlaybackState()
        }

        self.validate()
        self.evaluatePlaybackState()
    }

    private var displayAsleep = false
    private var playbackEvalTimer: Timer?
    private(set) var effectivePlaybackAction = GSPlayback.keepRunning

    @objc private func displayDidSleep() { displayAsleep = true; evaluatePlaybackState() }
    @objc private func displayDidWake()  { displayAsleep = false; evaluatePlaybackState() }
    
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
            if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper") {
                try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: .main!)
            }
        } else {
            do {
                let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appending(path: "staticWP_\(AppDelegate.shared.wallpaperViewModel.currentWallpaper.wallpaperDirectory.hashValue).tiff")
                try NSWorkspace.shared.setDesktopImageURL(url, for: .main!)
            } catch {
                print(error)
            }
        }
    }
    
    func didCurrentWallpaperChange(_ newValue: WEWallpaper) {
        AppDelegate.shared.setPlaceholderWallpaper(with: newValue)
    }
    
    func reset() {
        settings = (try? JSONDecoder()
            .decode(GlobalSettings.self,
                from: UserDefaults.standard.data(forKey: "GlobalSettings")
            ?? Data()))
        ?? GlobalSettings()
    }
    
    func save() {
        let data = try! JSONEncoder().encode(settings)
        UserDefaults.standard.set(data, forKey: "GlobalSettings")
    }
    
    func setQuality(_ quality: GSQuality) {
        switch quality {
        case .low:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .disabled
            self.settings.textureResolution = .highQuality
            self.settings.fps = 10
            self.settings.reflections = false
        case .medium:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .highQuality
            self.settings.fps = 15
            self.settings.reflections = true
        case .high:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .highQuality
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
        evaluatePlaybackState()
    }

    func evaluatePlaybackState() {
        var actions: [GSPlayback] = []

        if displayAsleep { actions.append(settings.displayAsleep) }

        if isOnBattery() { actions.append(settings.laptopOnBattery) }

        let front = NSWorkspace.shared.frontmostApplication
        let isSelf = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier
        let isDesktopFinder = front?.bundleIdentifier == "com.apple.finder"
            && !appHasVisibleWindows(pid: front?.processIdentifier)

        if let front, front.activationPolicy == .regular, !isSelf, !isDesktopFinder {
            if appIsFullscreen(pid: front.processIdentifier) {
                actions.append(settings.otherApplicationFullscreen)
            } else {
                actions.append(settings.otherApplicationFocused)
            }
        }

        if isOtherAppPlayingAudio() {
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
}
