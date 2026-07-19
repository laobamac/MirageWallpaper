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
        MirageLocalization.shared.apply(self.settings.language)
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
        playbackEvalTimer?.invalidate()
        playbackEvalWorkItem?.cancel()
        for observer in workspacePlaybackObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
        if let desktopClickMonitor { NSEvent.removeMonitor(desktopClickMonitor) }
    }
    
    func didFinishLaunchingNotification() {
        // NSWorkspace posts activation notifications on its own notification
        // center, not the default one, so subscribe there or the handler never
        // fires and the reveal latch never clears on app switches.
        self.didActivateApplicationNotificationCancellable =
        NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didActivateApplicationNotification)
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
                self?.schedulePlaybackEvaluation()
            }
            workspacePlaybackObservers.append(observer)
        }
        desktopClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseDown) { [weak self] _ in
            guard let self else { return }
            // A click on bare desktop toggles the reveal: the first one reveals
            // the desktop (windows slide away), the next one collapses it back.
            // Both clicks hit-test as "on desktop" because the windows are off
            // screen while revealed, so we must toggle rather than assign.
            // A click that lands on any on-screen UI (an app window, the Dock,
            // the menu bar) is never a reveal gesture and clears the latch.
            if self.clickLandedOnDesktop(at: NSEvent.mouseLocation) {
                self.desktopRevealed.toggle()
            } else {
                self.desktopRevealed = false
            }
            self.schedulePlaybackEvaluation()
        }

        self.playbackEvalTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.evaluatePlaybackState()
        }

        self.validate()
        self.evaluatePlaybackState()
    }

    private var displayAsleep = false
    private var playbackEvalTimer: Timer?
    private var playbackEvalWorkItem: DispatchWorkItem?
    private var workspacePlaybackObservers: [NSObjectProtocol] = []
    private var desktopClickMonitor: Any?
    private var desktopRevealed = false
    private(set) var effectivePlaybackAction = GSPlayback.keepRunning

    @objc private func displayDidSleep() { displayAsleep = true; evaluatePlaybackState() }
    @objc private func displayDidWake()  { displayAsleep = false; evaluatePlaybackState() }

    private func schedulePlaybackEvaluation() {
        playbackEvalWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.evaluatePlaybackState() }
        playbackEvalWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: work)
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
        // A reveal-desktop gesture never changes the frontmost app, so any
        // genuine activation of another regular app means the desktop is no
        // longer being viewed. Clear the latch so focus rules apply again.
        let front = NSWorkspace.shared.frontmostApplication
        let isSelf = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier
        if let front, front.activationPolicy == .regular, !isSelf {
            desktopRevealed = false
        }
        evaluatePlaybackState()
    }

    func evaluatePlaybackState() {
        var actions: [GSPlayback] = []

        if displayAsleep { actions.append(settings.displayAsleep) }

        if isOnBattery() { actions.append(settings.laptopOnBattery) }

        let front = NSWorkspace.shared.frontmostApplication
        let isSelf = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier
        // The desktop is being viewed if the user latched a reveal gesture by
        // clicking empty desktop, or if window geometry currently shows no app
        // window covering a screen. The latch survives the reveal animation,
        // which the geometry heuristic alone cannot reliably observe.
        let desktopViewed = desktopRevealed || isDesktopExposed()
        let isDesktopFinder = front?.bundleIdentifier == "com.apple.finder"
            && !appHasVisibleWindows(pid: front?.processIdentifier)

        if let front, front.activationPolicy == .regular, !isSelf, !isDesktopFinder, !desktopViewed {
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
