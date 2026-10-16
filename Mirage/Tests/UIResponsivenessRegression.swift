//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import AVFoundation
import Combine
import CoreAudio
import ImageIO
import Observation
import UniformTypeIdentifiers
@testable import Mirage_Wallpaper

private struct RegressionFailure: Error, CustomStringConvertible {
    let description: String
}

private final class Locked<Value>: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Value
    init(_ value: Value) { self.value = value }
    func access<T>(_ body: (inout Value) throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body(&value)
    }
}

private final class PlaybackAudioHardwareProbe {
    typealias Monitor = PlaybackAudioMonitor
    struct State {
        var sources: [AudioObjectID: Monitor.ProcessActivity] = [:]
        var failList = false
        var failedProcesses: Set<AudioObjectID> = []
        var failListeners = false
        var listeners: [Monitor.Property: (DispatchQueue, AudioObjectPropertyListenerBlock)] = [:]
        var registrations = 0
        var queries = 0
        var transitions: [Bool] = []
        var logs: [String] = []
        var excludedPIDs: Set<pid_t> = [100]
    }
    let state = Locked(State())

    var backend: Monitor.Backend {
        Monitor.Backend(
            processIDs: { [self] in
                try state.access {
                    $0.queries += 1
                    if $0.failList { throw RegressionFailure(description: "process-list-unavailable") }
                    return Array($0.sources.keys)
                }
            },
            activity: { [self] object in
                try state.access {
                    if $0.failedProcesses.contains(object) {
                        throw RegressionFailure(description: "process-unavailable")
                    }
                    guard let source = $0.sources[object] else {
                        throw RegressionFailure(description: "process-exited")
                    }
                    return source
                }
            },
            addListener: { [self] property, queue, block in
                state.access {
                    if $0.failListeners { return kAudioHardwareUnspecifiedError }
                    $0.listeners[property] = (queue, block)
                    $0.registrations += 1
                    return noErr
                }
            },
            removeListener: { [self] property, _, _ in
                state.access { $0.listeners[property] = nil }
            })
    }

    func notify(_ object: AudioObjectID = AudioObjectID(kAudioObjectSystemObject),
                selector: AudioObjectPropertySelector = kAudioHardwarePropertyProcessObjectList) {
        let property = Monitor.Property(object: object, selector: selector)
        guard let (queue, block) = state.access({ $0.listeners[property] }) else { return }
        queue.async {
            var address = property.address
            withUnsafePointer(to: &address) { block(1, $0) }
        }
    }

    func monitor(initiallyActive: Bool = false) -> Monitor {
        Monitor(initiallyActive: initiallyActive, backend: backend,
                excludedPIDs: { [self] in state.access { $0.excludedPIDs } },
                log: { [self] message in state.access { $0.logs.append(message) } },
                onChange: { [self] active in state.access { $0.transitions.append(active) } })
    }
}

private final class ImageProtocol: URLProtocol, @unchecked Sendable {
    struct Metrics { var starts = 0; var cancellations = 0; var active = 0; var maximum = 0 }
    static let metrics = Locked(Metrics())
    static var imageData = Data()
    private let ended = Locked(false)

    override class func canInit(with request: URLRequest) -> Bool { request.url?.host == "mirage-image.test" }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        Self.metrics.access {
            $0.starts += 1
            $0.active += 1
            $0.maximum = max($0.maximum, $0.active)
        }
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.15) { [self] in
            guard ended.access({ if $0 { return false }; $0 = true; return true }) else { return }
            Self.metrics.access { $0.active -= 1 }
            let response = HTTPURLResponse(url: request.url!, statusCode: 200,
                                           httpVersion: "HTTP/1.1", headerFields: nil)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: Self.imageData)
            client?.urlProtocolDidFinishLoading(self)
        }
    }

    override func stopLoading() {
        guard ended.access({ if $0 { return false }; $0 = true; return true }) else { return }
        Self.metrics.access { $0.cancellations += 1; $0.active -= 1 }
    }
}

private final class PlaylistPlaybackProbe: PlaylistPlayback {
    let displayStatesChanges: CurrentValueSubject<[DisplayKey: DisplayWallpaperState], Never>
    let wallpaperChangeRequests = PassthroughSubject<DisplayKey, Never>()
    var paused = true
    var muted = true
    var stopped = false
    var requests: [(String, DisplayKey)] = []
    var restoredFocus = false
    private var generation = UUID()

    init(wallpaper: WEWallpaper, display: DisplayKey) {
        displayStatesChanges = CurrentValueSubject([
            display: DisplayWallpaperState(wallpaper: wallpaper, runtime: WallpaperRuntimeState())
        ])
    }

    func state(for key: DisplayKey) -> DisplayWallpaperState? { displayStatesChanges.value[key] }
    func isTrusted(_ wallpaper: WEWallpaper) -> Bool { false }

    func allowsPlaylistAdvance(on key: DisplayKey, manually: Bool, updateOnPause: Bool) -> Bool {
        !stopped && (manually || updateOnPause || !paused)
    }

    func assign(_ wallpaper: WEWallpaper, to key: DisplayKey, restoreFocus: Bool,
                preservingPlaybackState: Bool, completion: ((Bool) -> Void)?) {
        if !preservingPlaybackState { paused = false; muted = false }
        restoredFocus = restoredFocus || restoreFocus
        requests.append((wallpaper.id, key))
        generation = UUID()
        let request = generation
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.025) { [self] in
            guard generation == request else { completion?(false); return }
            var states = displayStatesChanges.value
            states[key] = DisplayWallpaperState(wallpaper: wallpaper, runtime: WallpaperRuntimeState())
            displayStatesChanges.send(states)
            completion?(true)
        }
    }

    func applyDirectly(_ wallpaper: WEWallpaper, to key: DisplayKey) {
        generation = UUID()
        wallpaperChangeRequests.send(key)
        var states = displayStatesChanges.value
        states[key] = DisplayWallpaperState(wallpaper: wallpaper, runtime: WallpaperRuntimeState())
        displayStatesChanges.send(states)
    }
}

@main
@MainActor
private struct UIResponsivenessRegression {
    static let root = Bundle.main.resourceURL!.appending(path: "Fixtures")

    static func require(_ condition: @autoclosure () -> Bool, _ message: String) throws {
        if !condition() { throw RegressionFailure(description: message) }
    }

    static func waitUntil(_ message: String, timeout: TimeInterval = 5,
                          _ condition: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() {
            if Date() >= deadline { throw RegressionFailure(description: "Timed out: " + message) }
            try await Task.sleep(for: .milliseconds(5))
        }
    }

    static func main() async {
        if URL(fileURLWithPath: CommandLine.arguments[0]).lastPathComponent == "blocked-worker" {
            while true { sleep(10) }
        }
        if WEConditionEvaluator.runWorkerIfRequested() { return }
        if CommandLine.arguments.contains("--control-stdin") { runRenderer(); return }
        do {
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            NSApplication.shared.setActivationPolicy(.accessory)
            NSApplication.shared.finishLaunching()
            if CommandLine.arguments.contains("--wallpaper-runtime") {
                ImageProtocol.imageData = try pngData(color: .green)
                try await testConfiguration()
                try await testLockScreenDeployment()
                try testWallpaperRuntimeSnapshots()
                print("WallpaperRuntimeRegression: all checks passed")
                return
            }
            if CommandLine.arguments.contains("--playback-policy") {
                try testAudioActivityState()
                try await testAudioMonitoring()
                if CommandLine.arguments.contains("--headless") {
                    print("SKIP: native CoreAudio device integration requires audio hardware")
                } else { try await testNativeAudioMonitoring() }
                try testPlaybackPolicyEvaluation()
                try testPlaybackPolicyInputs()
                try testFullscreenPlaybackPolicy()
                try await testPlaybackPolicySynchronization()
                print("PlaybackPolicyRegression: all checks passed")
                return
            }
            if let index = CommandLine.arguments.firstIndex(of: "--lock-preview-fixtures"), index + 1 < CommandLine.arguments.count {
                try await testRealLockPreviews(in: URL(fileURLWithPath: CommandLine.arguments[index + 1], isDirectory: true))
                return
            }
            if CommandLine.arguments.contains("--startup-playlist") {
                try testStartupAndPlaylistNavigation()
                try await testPlaylistControls()
                try await testPlaylistTransitions()
                print("StartupPlaylistRegression: all checks passed")
                return
            }
            try testStartupAndPlaylistNavigation()
            try await testPlaylistControls()
            try await testPlaylistTransitions()
            try await testWorkers()
            try await testInteractiveUpdates()
            try await testWallpaperSizes()
            try await testSubscriptionFiltering()
            try await testConditions()
            try testObservation()
            try await testImages()
            try await testConfiguration()
            try testWallpaperRuntimeSnapshots()
            try await testLockScreenDeployment()
            try await testRenderers()
            try await testLogs()
            try testAudioActivityState()
            try await testAudioMonitoring()
            if CommandLine.arguments.contains("--headless") {
                print("SKIP: native CoreAudio device integration requires audio hardware")
            } else { try await testNativeAudioMonitoring() }
            try testPlaybackPolicyEvaluation()
            try testPlaybackPolicyInputs()
            try testFullscreenPlaybackPolicy()
            try await testPlaybackPolicySynchronization()
            print("UIResponsivenessRegression: all checks passed")
        } catch {
            fputs("UIResponsivenessRegression: \(error)\n", stderr)
            exit(1)
        }
    }

    static func testAudioActivityState() throws {
        typealias Monitor = PlaybackAudioMonitor
        var state = Monitor.ActivityState()
        _ = state.update(false, at: 0)
        _ = state.update(true, at: 1)
        _ = state.update(false, at: 1.2)
        try require(!state.isActive, "A brief system sound interrupted playback")
        _ = state.update(true, at: 2)
        _ = state.update(false, at: 2.3)
        _ = state.update(true, at: 2.4)
        _ = state.update(true, at: 2.8)
        try require(!state.isActive, "Separate audio pulses accumulated into a playback interruption")
        _ = state.update(true, at: 2.91)
        try require(state.isActive, "Sustained external output did not activate the rule")
        _ = state.update(false, at: 3)
        _ = state.update(true, at: 4)
        try require(state.isActive, "A short gap between tracks resumed the wallpaper")
        _ = state.update(false, at: 5)
        _ = state.update(false, at: 6.49)
        try require(state.isActive, "The audio rule released before the recovery interval")
        _ = state.update(false, at: 6.5)
        try require(!state.isActive, "Stopped external audio left the wallpaper paused")

        state = Monitor.ActivityState(isActive: true)
        _ = state.update(nil, at: 10)
        _ = state.update(nil, at: 14.99)
        try require(state.isActive, "An unavailable audio sample immediately released the rule")
        _ = state.update(true, at: 15)
        _ = state.update(nil, at: 20)
        try require(state.isActive, "A recovered read did not reset the error grace period")
        _ = state.update(nil, at: 25)
        try require(!state.isActive, "Persistent audio errors permanently paused the wallpaper")
        _ = state.update(true, at: 26)
        _ = state.update(true, at: 26.5)
        try require(state.isActive, "Audio detection did not recover after a persistent read failure")
        state = Monitor.ActivityState()
        _ = state.update(true, at: 0)
        _ = state.update(nil, at: 0.4)
        _ = state.update(true, at: 0.6)
        try require(!state.isActive, "Unknown samples were counted toward confirmed activity")

        let speechPath = "/System/Library/PrivateFrameworks/CoreSpeech.framework/corespeechd"
        let speech = Monitor.ProcessActivity(pid: 610, bundleID: "com.apple.CoreSpeech",
                                             executablePath: speechPath, output: true, input: true)
        try require(speech.exclusionReason(excludedPIDs: []) == "speech-listener",
                    "The verified CoreSpeech listener was treated as external playback")
        var restarted = speech
        restarted.pid = 9123
        restarted.bundleID = ""
        try require(restarted.exclusionReason(excludedPIDs: []) == "speech-listener",
                    "Speech filtering depended on a transient PID or bundle identifier")
        var unrelated = speech
        unrelated.executablePath = "/Applications/Player.app/Contents/MacOS/corespeechd"
        try require(unrelated.exclusionReason(excludedPIDs: []) == nil,
                    "A process basename or framework bundle ID was enough to exclude an application")
        for path in ["/usr/sbin/systemsoundserverd", "/usr/libexec/audiomxd"] {
            var sound = Monitor.ProcessActivity(pid: 700, executablePath: path, output: true)
            try require(sound.exclusionReason(excludedPIDs: []) == "system-sound",
                        "A system sound service was treated as application playback")
            sound.pid = 9700
            try require(sound.exclusionReason(excludedPIDs: []) == "system-sound",
                        "System sound filtering stopped working after a process restart")
            sound.executablePath = "/Applications/Player.app/Contents/MacOS/" + URL(fileURLWithPath: path).lastPathComponent
            try require(sound.exclusionReason(excludedPIDs: []) == nil,
                        "System sound filtering matched an unrelated executable with the same name")
        }
        let audioServer = Monitor.ProcessActivity(pid: 800, executablePath: "/usr/sbin/coreaudiod", output: true)
        try require(audioServer.exclusionReason(excludedPIDs: []) == nil,
                    "System sound filtering excluded the general CoreAudio server")
        let call = Monitor.ProcessActivity(pid: 300, bundleID: "com.apple.avconferenced", output: true, input: true)
        try require(call.exclusionReason(excludedPIDs: []) == nil,
                    "A voice call was ignored because it also uses the microphone")
        try require(call.exclusionReason(excludedPIDs: [300]) == "mirage", "A renderer PID was not excluded")
        let web = Monitor.ProcessActivity(pid: 400, bundleID: "com.apple.WebKit.GPU", name: "WebWallpaper Graphics and Media", output: true)
        try require(web.exclusionReason(excludedPIDs: []) == "web-wallpaper", "A web wallpaper counted its own audio")
        var browser = web
        browser.name = "Safari Graphics and Media"
        try require(browser.exclusionReason(excludedPIDs: []) == nil, "Ordinary browser audio was ignored")
        print("PASS: audio source identity, confirmation, recovery, input/output calls and bounded read failures")
    }

    static func testAudioMonitoring() async throws {
        typealias Monitor = PlaybackAudioMonitor
        let probe = PlaybackAudioHardwareProbe()
        probe.state.access {
            $0.sources = [
                10: Monitor.ProcessActivity(pid: 100, output: true),
                11: Monitor.ProcessActivity(pid: 610, bundleID: "com.apple.CoreSpeech",
                    executablePath: "/System/Library/PrivateFrameworks/CoreSpeech.framework/corespeechd", output: true, input: true),
                12: Monitor.ProcessActivity(pid: 300, bundleID: "org.example.recorder", input: true),
                15: Monitor.ProcessActivity(pid: 700, executablePath: "/usr/sbin/systemsoundserverd", output: true),
                16: Monitor.ProcessActivity(pid: 701, executablePath: "/usr/libexec/audiomxd", output: true)
            ]
        }
        let monitor = probe.monitor()
        monitor.start()
        defer { monitor.stop() }
        try await waitUntil("Audio listeners and initial sample") { probe.state.access { $0.listeners.count == 7 && !$0.logs.isEmpty } }
        try await Task.sleep(for: .milliseconds(750))
        try require(probe.state.access { $0.transitions.isEmpty }, "Speech, input-only or wallpaper audio triggered the rule")
        try require(probe.state.access { $0.logs.contains { $0.contains("speech-listener") } }, "Filtered audio sources were not diagnosed")
        try require(probe.state.access { $0.logs.contains { $0.contains("system-sound") } },
                    "System sound output lasting beyond the confirmation window was not filtered")

        probe.state.access { $0.sources[13] = Monitor.ProcessActivity(pid: 400, bundleID: "org.example.player", output: true) }
        probe.notify()
        try await waitUntil("External audio onset from process notification", timeout: 1.5) {
            probe.state.access { $0.transitions == [true] }
        }
        probe.state.access { $0.failedProcesses = [13] }
        probe.notify(13, selector: kAudioProcessPropertyIsRunningOutput)
        try await waitUntil("Read error diagnostic") { probe.state.access { $0.logs.last?.contains("raw=unknown") == true } }
        try require(probe.state.access { $0.transitions == [true] }, "A process read failure resumed the wallpaper")
        probe.state.access { $0.failedProcesses = []; $0.sources[13]?.output = false }
        probe.notify(13, selector: kAudioProcessPropertyIsRunningOutput)
        try await waitUntil("Stopped audio recovery", timeout: 2) { probe.state.access { $0.transitions == [true, false] } }
        let registrations = probe.state.access { $0.registrations }
        probe.notify(selector: kAudioHardwarePropertyServiceRestarted)
        try await waitUntil("Audio server restart listener recovery") { probe.state.access { $0.registrations > registrations + 2 } }
        probe.state.access { $0.sources[13] = nil }
        probe.notify()
        try await waitUntil("Exited process listener removal") {
            probe.state.access { !$0.listeners.keys.contains { $0.object == 13 } }
        }
        probe.state.access { $0.sources[14] = Monitor.ProcessActivity(pid: 500, output: true); $0.excludedPIDs.insert(500) }
        probe.notify()
        try await waitUntil("New renderer PID exclusion") { probe.state.access { $0.logs.last?.contains("pid=500") == true } }
        try require(probe.state.access { $0.transitions == [true, false] }, "A new renderer triggered the audio rule")
        monitor.stop()
        try await waitUntil("Listener cleanup on stop") { probe.state.access { $0.listeners.isEmpty } }
        let stoppedQueries = probe.state.access { $0.queries }
        try await Task.sleep(for: .milliseconds(550))
        try require(probe.state.access { $0.queries == stoppedQueries }, "Stopped monitoring retained scheduled samples")

        let fallback = PlaybackAudioHardwareProbe()
        fallback.state.access { $0.failListeners = true }
        let fallbackMonitor = fallback.monitor()
        fallbackMonitor.start()
        defer { fallbackMonitor.stop() }
        try await waitUntil("Initial polling sample") { fallback.state.access { $0.queries > 0 } }
        fallback.state.access { $0.sources[20] = Monitor.ProcessActivity(pid: 600, output: true) }
        try await waitUntil("Polling fallback after listener failure", timeout: 3.5) { fallback.state.access { $0.transitions == [true] } }
        fallback.state.access { $0.failList = true }
        try await waitUntil("Process list failure", timeout: 3) { fallback.state.access { $0.logs.last?.contains("raw=unknown") == true } }
        try require(fallback.state.access { $0.transitions == [true] }, "A process-list failure was treated as no audio")
        fallbackMonitor.stop()
        print("PASS: event-driven audio monitoring, process churn, restart recovery, polling fallback and cleanup")
    }

    static func testNativeAudioMonitoring() async throws {
        typealias Monitor = PlaybackAudioMonitor
        let native = Monitor.Backend()
        let ownPID = ProcessInfo.processInfo.processIdentifier
        let transitions = Locked([Bool]())
        var backend = native
        backend.activity = { object in
            let activity = try native.activity(object)
            return activity.pid == ownPID ? activity : Monitor.ProcessActivity()
        }
        let monitor = Monitor(backend: backend, excludedPIDs: { [] }, log: { _ in },
                              onChange: { active in transitions.access { $0.append(active) } })
        monitor.start()
        defer { monitor.stop() }
        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
        let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 4096)!
        buffer.frameLength = buffer.frameCapacity
        for channel in 0..<Int(format.channelCount) {
            buffer.floatChannelData![channel].update(repeating: 0, count: Int(buffer.frameLength))
        }
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        engine.mainMixerNode.outputVolume = 0
        player.scheduleBuffer(buffer, at: nil, options: .loops, completionHandler: nil)
        try engine.start()
        defer { engine.stop() }
        player.play()
        try await waitUntil("Native CoreAudio output detection", timeout: 4) { transitions.access { $0 == [true] } }
        player.stop()
        engine.stop()
        try await waitUntil("Native CoreAudio output release", timeout: 4) { transitions.access { $0 == [true, false] } }
        print("PASS: native CoreAudio process enumeration, output transitions and listener callbacks")
    }

    static func testPlaybackPolicyEvaluation() throws {
        var state = GlobalSettingsViewModel.PlaybackEvaluationState()
        let beforeWake = state.begin()!
        state.resume()
        for _ in 0..<100 {
            try require(state.begin() == nil, "Wake notifications started overlapping evaluations")
        }
        let stale = state.finish(generation: beforeWake, hasResult: true)
        try require(!stale.shouldApply && !stale.force && stale.shouldEvaluateAgain,
                    "A pre-wake result was applied or the new evaluation was lost")
        let unavailable = state.finish(generation: state.begin()!, hasResult: false)
        try require(!unavailable.shouldApply, "An unavailable power sample was applied")
        let recovered = state.finish(generation: state.begin()!, hasResult: true)
        try require(recovered.shouldApply && recovered.force,
                    "A failed sample consumed the required wake synchronization")
        let ordinary = state.finish(generation: state.begin()!, hasResult: true)
        try require(ordinary.shouldApply && !ordinary.force, "Normal polling kept forcing playback commands")

        let beforeSleep = state.begin()!
        state.suspend()
        state.invalidate(force: true)
        try require(state.begin() == nil, "An evaluation started while the system was asleep")
        let asleep = state.finish(generation: beforeSleep, hasResult: true)
        try require(!asleep.shouldApply && !asleep.shouldEvaluateAgain, "Sleep allowed stale playback commands")
        state.resume()
        let firstWake = state.begin()!
        state.resume()
        state.invalidate()
        try require(state.begin() == nil, "An overlapping power change bypassed evaluation coalescing")
        let superseded = state.finish(generation: firstWake, hasResult: true)
        try require(!superseded.shouldApply && superseded.shouldEvaluateAgain,
                    "A second wake or power change accepted an obsolete result")
        let final = state.finish(generation: state.begin()!, hasResult: true)
        try require(final.shouldApply && final.force,
                    "Repeated wake and power notifications lost the forced synchronization")
        print("PASS: sleep boundaries, stale results, coalesced wake events and synchronization after unavailable samples")
    }

    static func testPlaybackPolicyInputs() throws {
        guard let display = DisplayRegistry.shared.connected.first else {
            throw RegressionFailure(description: "A connected display is required for playback policy regression")
        }
        let wallpaper = try wallpaper("playback-policy-inputs")
        let model = WallpaperViewModel(initialStates: [
            display.key: DisplayWallpaperState(wallpaper: wallpaper, runtime: WallpaperRuntimeState())
        ])
        let settingsModel = AppDelegate.shared.globalSettingsViewModel
        let saved = settingsModel.settings
        defer { settingsModel.settings = saved }
        var windowQueries = 0
        var probes = GlobalSettingsViewModel.PolicyProbes(
            onBattery: { true }, displayAsleep: { _ in true },
            windows: { windowQueries += 1; return [] })
        for rule in 0..<3 {
            var settings = GlobalSettings()
            let expected: GSPlayback
            switch rule {
            case 0: settings.laptopOnBattery = .pause; expected = .pause
            case 1: settings.displayAsleep = .stop; expected = .stop
            default: settings.otherApplicationPlayingAudio = .mute; expected = .mute
            }
            settingsModel.settings = settings
            var inputs = settingsModel.collectPolicyInputs(for: model)
            inputs.otherAudioPlaying = true
            try require(inputs.wallpaperDisplays[display.displayID] != nil,
                        "An independent battery, sleep or audio rule lost its display")
            let result = try GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes).get()
            try require(result.actions[display.displayID] == expected, "An independent playback rule was ignored")
        }
        try require(windowQueries == 0, "Global playback rules unnecessarily queried window geometry")

        var inputs = GlobalSettingsViewModel.PolicyInputs()
        inputs.wallpaperDisplays = [1001: CGRect(x: 0, y: 0, width: 1920, height: 1080),
                                    1002: CGRect(x: 1920, y: 0, width: 1920, height: 1080)]
        inputs.onBattery = .pause
        inputs.onAudio = .mute
        inputs.otherAudioPlaying = true
        inputs.onDisplayAsleep = .stop
        probes.displayAsleep = { $0 == 1001 }
        let battery = try GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes).get()
        try require(battery.actions == [1001: .stop, 1002: .pause], "Rule priority or per-display sleep was lost")
        probes.onBattery = { false }
        let ac = try GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes).get()
        try require(ac.actions == [1001: .stop, 1002: .mute], "AC power incorrectly released another active rule")
        probes.onBattery = { nil }
        guard case .failure(.powerSourceUnavailable) = GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes) else {
            throw RegressionFailure(description: "A failed power read was treated as AC power")
        }
        probes.onBattery = { true }
        let retried = try GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes).get()
        try require(retried.actions == battery.actions, "A recovered power sample did not restore battery policy")

        inputs.onBattery = .keepRunning
        inputs.onAudio = .keepRunning
        inputs.onDisplayAsleep = .keepRunning
        inputs.onFocused = .pause
        probes.windows = { nil }
        guard case .failure(.windowListUnavailable) = GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes) else {
            throw RegressionFailure(description: "A failed window query was treated as an exposed desktop")
        }
        probes.windows = { [] }
        let emptyWindows = try GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes).get()
        try require(emptyWindows.actions == [1001: .keepRunning, 1002: .keepRunning],
                    "A valid empty window list was rejected")
        inputs.wallpaperDisplays.removeAll()
        guard case .failure(.displaysUnavailable) = GlobalSettingsViewModel.computePlaybackActions(inputs, probes: probes) else {
            throw RegressionFailure(description: "An unavailable display topology released playback rules")
        }
        print("PASS: independent battery, display sleep and audio rules, per-display priorities and failed system queries")
    }

    static func testFullscreenPlaybackPolicy() throws {
        typealias Policy = GlobalSettingsViewModel
        let display = CGRect(x: 0, y: 0, width: 1512, height: 982)
        let safe = CGRect(x: 0, y: 37, width: 1512, height: 945)
        let insets = NSEdgeInsets(top: 37, left: 0, bottom: 0, right: 0)
        for origin in [CGPoint.zero, CGPoint(x: -1512, y: -982), CGPoint(x: 1920, y: 1080)] {
            let bounds = display.offsetBy(dx: origin.x, dy: origin.y)
            let screenFrame = CGRect(x: origin.x, y: 1080 - bounds.maxY, width: 1512, height: 982)
            let area = Policy.fullscreenSafeArea(display: bounds, screenFrame: screenFrame, insets: insets)
            try require(area == safe.offsetBy(dx: origin.x, dy: origin.y),
                        "Safe area used the wrong display origin or inverted the top inset")
        }
        let scaled = Policy.fullscreenSafeArea(
            display: CGRect(x: -3024, y: -1964, width: 3024, height: 1964), screenFrame: display,
            insets: NSEdgeInsets(top: 37, left: 5, bottom: 4, right: 3))
        try require(scaled == CGRect(x: -3014, y: -1890, width: 3008, height: 1882),
                    "Safe area did not convert all four insets into display coordinates")
        for invalid in [NSEdgeInsets(), NSEdgeInsets(top: -1, left: 0, bottom: 0, right: 0),
                        NSEdgeInsets(top: .nan, left: 0, bottom: 0, right: 0),
                        NSEdgeInsets(top: 982, left: 0, bottom: 0, right: 0),
                        NSEdgeInsets(top: 37, left: 1512, bottom: 0, right: 0)] {
            try require(Policy.fullscreenSafeArea(display: display, screenFrame: display, insets: invalid) == nil,
                        "An absent or invalid safe area created a fullscreen fallback")
        }
        try require(Policy.fullscreenSafeArea(display: .zero, screenFrame: display, insets: insets) == nil,
                    "An unavailable display produced a safe area")
        try require(Policy.fullscreenSafeArea(display: display, screenFrame: .zero, insets: insets) == nil,
                    "An unavailable AppKit frame produced a safe area")

        var inputs = Policy.PolicyInputs()
        inputs.onFullscreen = .pause
        inputs.selfPID = 1
        inputs.frontPID = 2
        inputs.frontIsRegular = true
        inputs.regularPIDs = [1, 2, 3, 4]
        inputs.rendererPIDs = [4]
        inputs.wallpaperDisplays = [1001: display]
        inputs.fullscreenSafeAreas = [1001: safe]
        var queries = 0
        func evaluate(_ windows: [Policy.WindowEntry], _ sample: Policy.PolicyInputs) throws -> Policy.PolicyResult {
            var probes = Policy.PolicyProbes(
                onBattery: { true }, displayAsleep: { _ in false })
            probes.windows = { queries += 1; return windows }
            let before = queries
            let result = try Policy.computePlaybackActions(sample, probes: probes).get()
            try require(queries == before + 1, "Fullscreen evaluation queried windows more than once")
            return result
        }
        func window(_ bounds: CGRect, pid: pid_t = 2, layer: Int = 0, alpha: Double = 1) -> Policy.WindowEntry {
            Policy.WindowEntry(layer: layer, pid: pid, bounds: bounds, alpha: alpha)
        }
        for bounds in [display, safe, safe.insetBy(dx: 0.5, dy: 0.5)] {
            for action in [GSPlayback.mute, .pause, .stop] {
                inputs.onFullscreen = action
                let result = try evaluate([window(bounds)], inputs)
                try require(result.actions[1001] == action, "Fullscreen did not apply the configured action")
                try require(result.fullscreenDiagnostics.isEmpty, "Fullscreen diagnostics ran outside developer mode")
            }
        }
        inputs.onFullscreen = .pause
        for bounds in [CGRect(x: 0, y: 37, width: 1512, height: 870),
                       CGRect(x: 0, y: 37, width: 756, height: 945),
                       CGRect(x: 0, y: 37, width: 1512, height: 933),
                       CGRect(x: 100, y: 37, width: 1512, height: 945),
                       CGRect(x: -1512, y: 0, width: 3024, height: 982)] {
            let result = try evaluate([window(bounds)], inputs)
            try require(result.actions[1001] == .keepRunning, "A partial or spanning window triggered fullscreen")
        }
        let tiled = try evaluate([window(CGRect(x: 0, y: 37, width: 756, height: 945)),
                                  window(CGRect(x: 756, y: 37, width: 756, height: 945), pid: 3)], inputs)
        try require(tiled.actions[1001] == .keepRunning, "Tiled windows were combined into a fullscreen window")
        for excluded in [window(safe, pid: 1), window(safe, pid: 4), window(safe, pid: 5),
                         window(safe, layer: 1), window(safe, layer: -1), window(safe, alpha: 0.05)] {
            let result = try evaluate([excluded], inputs)
            try require(result.actions[1001] == .keepRunning, "An excluded window triggered fullscreen")
        }
        var ordinaryDisplay = inputs
        ordinaryDisplay.fullscreenSafeAreas = [:]
        let noNotch = try evaluate([window(safe)], ordinaryDisplay)
        try require(noNotch.actions[1001] == .keepRunning, "Safe area matching was enabled on an ordinary screen")
        let uncut = try evaluate([window(display)], ordinaryDisplay)
        try require(uncut.actions[1001] == .pause, "Ordinary fullscreen detection regressed")
        ordinaryDisplay.wallpaperDisplays = [1001: safe]
        let compatibility = try evaluate([window(safe)], ordinaryDisplay)
        try require(compatibility.actions[1001] == .pause, "An adjusted active display area was inset twice")

        let second = CGRect(x: -1920, y: -1080, width: 1920, height: 1080)
        inputs.wallpaperDisplays[1002] = second
        let secondWindow = window(second.insetBy(dx: 100, dy: 100), pid: 3)
        for front in [pid_t(1), 2, 3, 5] {
            inputs.frontPID = front
            inputs.frontIsRegular = front != 5
            let result = try evaluate([secondWindow, window(safe)], inputs)
            try require(result.actions == [1001: .pause, 1002: .keepRunning],
                        "Changing focus on another display released a visible fullscreen window")
        }
        inputs.frontPID = nil
        let noFocus = try evaluate([window(safe)], inputs)
        try require(noFocus.actions == [1001: .pause, 1002: .keepRunning],
                    "Fullscreen detection depended on a frontmost application")
        let both = try evaluate([window(safe), window(second, pid: 3)], inputs)
        try require(both.actions == [1001: .pause, 1002: .pause], "Only one fullscreen display received its rule")
        inputs.fullscreenSafeAreas[1002] = second.insetBy(dx: 0, dy: 37)
        let independent = try evaluate([window(safe)], inputs)
        try require(independent.actions[1002] == .keepRunning, "A safe area from another display triggered fullscreen")

        inputs.frontPID = 3
        inputs.frontIsRegular = true
        inputs.onFocused = .stop
        inputs.onFullscreen = .mute
        let priority = try evaluate([secondWindow, window(safe)], inputs)
        try require(priority.actions == [1001: .mute, 1002: .stop], "Fullscreen and focus priorities changed")
        inputs.frontPID = 2
        let sameAppPriority = try evaluate([window(safe)], inputs)
        try require(sameAppPriority.actions[1001] == .mute, "The focus rule overrode the fullscreen rule")
        inputs.pauseOnCoverage = true
        let coverage = try evaluate([window(safe)], inputs)
        try require(coverage.actions[1001] == .pause, "Fullscreen bypassed the independent coverage rule")
        inputs.pauseOnCoverage = false
        inputs.onBattery = .stop
        let battery = try evaluate([window(safe)], inputs)
        try require(battery.actions == [1001: .stop, 1002: .stop], "Fullscreen bypassed a stronger global rule")
        inputs.onBattery = .keepRunning
        inputs.onFocused = .keepRunning
        inputs.onFullscreen = .pause
        inputs.revealGraceDisplays = [1001]
        let reveal = try evaluate([window(safe), window(second, pid: 3)], inputs)
        try require(reveal.actions == [1001: .keepRunning, 1002: .pause], "Desktop reveal affected the wrong display")
        inputs.revealGraceDisplays = []
        let hidden = try evaluate([window(safe.offsetBy(dx: 10_000, dy: 0))], inputs)
        try require(hidden.actions == [1001: .keepRunning, 1002: .keepRunning], "An off-screen window kept playback paused")
        let exited = try evaluate([secondWindow], inputs)
        try require(exited.actions == [1001: .keepRunning, 1002: .keepRunning], "Leaving fullscreen did not release its rule")
        let empty = try evaluate([], inputs)
        try require(empty.actions == exited.actions, "An exposed desktop kept the fullscreen rule active")
        inputs.onFullscreen = .keepRunning
        inputs.onFocused = .mute
        let disabled = try evaluate([window(safe)], inputs)
        try require(disabled.actions[1001] == .mute, "Disabling fullscreen suppressed the focus rule")

        inputs.onFullscreen = .pause
        inputs.diagnoseFullscreen = true
        let diagnosed = try evaluate([window(safe)], inputs)
        try require(diagnosed.fullscreenDiagnostics[1001]?.contains("match=2:") == true &&
                    diagnosed.fullscreenDiagnostics[1001]?.contains("safe=0.0,37.0,1512.0,945.0") == true,
                    "Fullscreen diagnostics omitted the matched window or safe area")
        print("PASS: fullscreen safe areas, coordinate conversion, exclusions, per-display focus, priorities and desktop recovery")
    }

    static func testPlaybackPolicySynchronization() async throws {
        guard let display = DisplayRegistry.shared.connected.first else {
            throw RegressionFailure(description: "A connected display is required for playback synchronization regression")
        }
        let directory = Bundle.main.resourceURL!.appending(path: "Renderers")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var createdBinaries: [URL] = []
        for name in ["VideoWallpaper", "SceneWallpaper", "WebWallpaper"] {
            let binary = directory.appending(path: name)
            if !FileManager.default.fileExists(atPath: binary.path) {
                try FileManager.default.createSymbolicLink(at: binary, withDestinationURL: Bundle.main.executableURL!)
                createdBinaries.append(binary)
            }
        }
        let commandLog = root.appending(path: "playback-policy-commands.jsonl")
        try Data().write(to: commandLog)
        let environmentKey = "MIRAGE_PLAYBACK_POLICY_COMMAND_LOG"
        let previousEnvironment = ProcessInfo.processInfo.environment[environmentKey]
        setenv(environmentKey, commandLog.path, 1)
        let delegate = AppDelegate.shared
        let settingsModel = delegate.globalSettingsViewModel
        let savedSettings = settingsModel.settings
        let savedModel = delegate.wallpaperViewModel
        var settings = GlobalSettings()
        settings.enableSpectrum = false
        settingsModel.settings = settings
        defer {
            settingsModel.handlePlaybackLifecycleEvent(.systemSleep)
            delegate.wallpaperViewModel = savedModel
            settingsModel.settings = savedSettings
            settingsModel.handlePlaybackLifecycleEvent(.systemWake)
            if let previousEnvironment { setenv(environmentKey, previousEnvironment, 1) }
            else { unsetenv(environmentKey) }
            for binary in createdBinaries { try? FileManager.default.removeItem(at: binary) }
        }
        func commands(pid: pid_t) -> [[String: Any]] {
            let text = (try? String(contentsOf: commandLog, encoding: .utf8)) ?? ""
            return text.split(separator: "\n").compactMap { line in
                guard let value = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                      (value["pid"] as? NSNumber)?.int32Value == pid else { return nil }
                return value
            }
        }
        for (kind, file) in [("video", "video.mp4"), ("scene", "scene.json"), ("web", "index.html")] {
            let fixture = root.appending(path: "playback-policy-\(kind)")
            try FileManager.default.createDirectory(at: fixture, withIntermediateDirectories: true)
            try JSONSerialization.data(withJSONObject: ["title": kind, "type": kind, "file": file])
                .write(to: fixture.appending(path: "project.json"))
            try Data("{}".utf8).write(to: fixture.appending(path: file))
            let wallpaper = WEWallpaper.load(from: fixture)
            let model = WallpaperViewModel(initialStates: [
                display.key: DisplayWallpaperState(wallpaper: wallpaper, runtime: WallpaperRuntimeState())
            ])
            model.renderer.isWallpaperTrusted = { _ in true }
            delegate.wallpaperViewModel = model
            defer { model.renderer.stopAllAndWait() }
            model.restoreAllDisplays()
            try await waitUntil("\(kind) playback renderer") { model.renderer.isRendering(onDisplay: display.displayID) }
            guard let pid = model.renderer.processIdentifiers.first else {
                throw RegressionFailure(description: "Playback renderer has no process identifier")
            }
            let commandName = kind == "web" ? "playbackState" : "power"
            func playbackCommands() -> [[String: Any]] {
                commands(pid: pid).filter { $0["cmd"] as? String == commandName }
            }
            try await waitUntil("\(kind) initial playback directive") { !playbackCommands().isEmpty }
            model.renderer.setFps(119, on: display.index)
            try await waitUntil("\(kind) command barrier") {
                commands(pid: pid).contains { $0["cmd"] as? String == "fps" && $0["value"] as? Int == 119 }
            }
            let beforeWake = playbackCommands().count
            settingsModel.handlePlaybackLifecycleEvent(.systemSleep)
            settingsModel.handlePlaybackLifecycleEvent(.systemWake)
            settingsModel.handlePlaybackLifecycleEvent(.displayWake)
            try await waitUntil("\(kind) unchanged state synchronized after wake") {
                playbackCommands().count > beforeWake && playbackCommands().last?["state"] as? String == "run"
            }
            try require(commands(pid: pid).last(where: { $0["cmd"] as? String == "fps" })?["value"] as? Int == Int(settings.fps),
                        "Wake did not replace stale renderer playback options")
            model.pauseAll()
            model.muteAll()
            try await waitUntil("\(kind) manual pause") { playbackCommands().last?["state"] as? String == "pause" }
            let beforePausedWake = playbackCommands().count
            settingsModel.handlePlaybackLifecycleEvent(.systemSleep)
            settingsModel.handlePlaybackLifecycleEvent(.displayWake)
            try await waitUntil("\(kind) manual pause survives wake") {
                playbackCommands().count > beforePausedWake && playbackCommands().last?["state"] as? String == "pause"
            }
            if kind == "web" {
                try require(playbackCommands().last?["muted"] as? Bool == true, "Wake cleared manual web mute")
            } else {
                try require(commands(pid: pid).last(where: { $0["cmd"] as? String == "muted" })?["value"] as? Bool == true,
                            "Wake cleared manual mute")
            }
            settingsModel.handlePlaybackLifecycleEvent(.systemSleep)
            model.applyPlaybackPolicy(.stop)
            try await waitUntil("\(kind) policy stop") { !model.renderer.hasCoverageOrWork(onDisplay: display.displayID) }
            let restarted = await render(model.renderer, wallpaper, display: display.displayID)
            try require(restarted, "Could not simulate a renderer outliving its cached stop policy")
            model.applyPlaybackPolicy(.stop, force: true)
            try await waitUntil("\(kind) forced stop") { !model.renderer.hasCoverageOrWork(onDisplay: display.displayID) }
            model.applyPlaybackPolicy(.keepRunning, force: true)
            try await waitUntil("\(kind) restore after stop") { model.renderer.isRendering(onDisplay: display.displayID) }
            model.renderer.stopAllAndWait()
            print("PASS: \(kind) wake command replay, manual pause/mute preservation, forced stop and restore")
        }
    }

    static func testWorkers() async throws {
        let started = Locked(false)
        let processed = Locked([Int]())
        let gate = DispatchSemaphore(value: 0)
        let worker = LatestValueWorker<Int, Int>(label: "mirage.regression.latest") { value in
            processed.access { $0.append(value) }
            if value == 0 { started.access { $0 = true }; gate.wait() }
            return value
        }
        var delivered: [Int] = []
        worker.submit(0) { delivered.append($0) }
        try await waitUntil("first computation") { started.access { $0 } }
        for value in 1...1_000 { worker.submit(value) { delivered.append($0) } }
        gate.signal()
        try await waitUntil("latest computation") { delivered == [1_000] }
        try require(processed.access { $0 } == [0, 1_000], "Obsolete computations were not coalesced")
        worker.submit(2_000) { delivered.append($0) }
        worker.cancel()
        try await Task.sleep(for: .milliseconds(50))
        try require(delivered == [1_000], "Cancelled result reached the UI")

        let writer = CoalescingWorkQueue(label: "mirage.regression.persistence")
        let writing = Locked(false)
        let writes = Locked([String: Int]())
        let saveCount = Locked(0)
        let writeGate = DispatchSemaphore(value: 0)
        writer.submit(key: "barrier") { writing.access { $0 = true }; writeGate.wait() }
        try await waitUntil("persistence barrier") { writing.access { $0 } }
        for value in 1...1_000 {
            writer.submit(key: "display-a") { writes.access { $0["a"] = value }; saveCount.access { $0 += 1 } }
            writer.submit(key: "display-b") { writes.access { $0["b"] = value }; saveCount.access { $0 += 1 } }
        }
        writeGate.signal()
        writer.flush()
        try require(writes.access { $0 } == ["a": 1_000, "b": 1_000], "Flush lost a display's latest save")
        try require(saveCount.access { $0 } == 2, "Obsolete saves were not coalesced")
        print("PASS: latest-value computation, cancellation, independent saves and flush")
    }

    static func evaluate(_ evaluator: WEConditionEvaluator, identity: String = "conditions",
                         conditions: [String], values: [String: Any] = [:]) async -> [String: Bool] {
        await withCheckedContinuation { continuation in
            evaluator.evaluate(identity: identity, conditions: conditions, values: values) {
                continuation.resume(returning: $0)
            }
        }
    }

    static func testInteractiveUpdates() async throws {
        let throttler = LatestValueThrottler<String>()
        var left: [Int] = []
        var right: [Int] = []
        for value in 0..<48 {
            throttler.submit(key: "left") { left.append(value) }
            throttler.submit(key: "right") { right.append(value * 2) }
            try await Task.sleep(for: .milliseconds(4))
        }
        try require(left.count > 3 && right.count > 3, "Continuous input postponed all renderer updates until dragging stopped")
        throttler.flush(key: "left")
        throttler.flush(key: "right")
        try require(left.last == 47 && right.last == 94, "Ending a drag lost its final value or mixed displays")
        let count = left.count
        throttler.submit(key: "left") { left.append(-1) }
        throttler.cancel(key: "left")
        try await Task.sleep(for: .milliseconds(40))
        try require(left.count == count, "A cancelled interaction still sent a command")

        let key = DisplayKey(rawValue: "interaction-a")
        let otherKey = DisplayKey(rawValue: "interaction-b")
        var first = try wallpaper("interaction-first")
        first.project.general = WEProjectGeneral(properties: WEProjectProperties(items: [
            "left": WEProjectProperty(type: "slider", value: .number(0)),
            "right": WEProjectProperty(type: "slider", value: .number(10))
        ]))
        let state = DisplayWallpaperState(wallpaper: first, runtime: WallpaperRuntimeState())
        let model = WallpaperViewModel(initialStates: [key: state, otherKey: state])
        let rows = model.propertyModel.rows
        let leftValue = rows.first { $0.id == "left" }!.state
        let rightValue = rows.first { $0.id == "right" }!.state
        let leftChanges = Locked(0)
        let rightChanges = Locked(0)
        let structureChanges = Locked(0)
        let volumeChanges = Locked(0)
        withObservationTracking { _ = leftValue.value } onChange: { leftChanges.access { $0 += 1 } }
        withObservationTracking { _ = rightValue.value } onChange: { rightChanges.access { $0 += 1 } }
        withObservationTracking { _ = model.propertyModel.rows } onChange: { structureChanges.access { $0 += 1 } }
        withObservationTracking { _ = model.playVolume } onChange: { volumeChanges.access { $0 += 1 } }
        var runtime = model.runtime
        runtime.propertyOverrides["left"] = .number(5)
        model.runtime = runtime
        try require(leftChanges.access { $0 } == 1 && rightChanges.access { $0 } == 0 &&
                    structureChanges.access { $0 } == 0 && volumeChanges.access { $0 } == 0,
                    "Changing one property invalidated unrelated controls or the property list")
        try require(model.state(for: otherKey)?.runtime.propertyOverrides.isEmpty == true,
                    "A property update changed another display")
        let second = try wallpaper("interaction-second")
        let third = try wallpaper("interaction-third")
        var prepared: [String] = []
        model.prepareWallpaper(second, for: key) { prepared.append($0.id) }
        try require(model.previewWallpaper.id == second.id && model.currentWallpaper.id == first.id && model.isApplyingSelection,
                    "Selecting a wallpaper waited for the renderer or prematurely committed playback")
        model.prepareWallpaper(third, for: key) { prepared.append($0.id) }
        try await waitUntil("latest preview preparation") { prepared == [third.id] }
        try require(model.previewWallpaper.id == third.id && model.currentWallpaper.id == first.id,
                    "An obsolete preparation overwrote the current selection")
        model.cancelPendingPreview(wallpaperID: third.id)
        try require(model.previewWallpaper.id == first.id && !model.isApplyingSelection,
                    "Cancelling a selection failed to restore the committed preview")
        var invalidFinished = false
        let invalid = WEWallpaper(using: .invalid, where: root.appending(path: "missing-selection"))
        model.prepareWallpaper(invalid, for: key) { _ in invalidFinished = true }
        try await waitUntil("invalid selection rollback") { invalidFinished }
        try require(model.previewWallpaper.id == first.id && !model.isApplyingSelection,
                    "An invalid selection left the preview in a loading state")
        let unsupportedDirectory = root.appending(path: "unsupported-selection")
        try FileManager.default.createDirectory(at: unsupportedDirectory, withIntermediateDirectories: true)
        let unsupportedProject = WEProject(file: "app.exe", preview: "", title: "Unsupported", type: "application")
        try JSONEncoder().encode(unsupportedProject).write(to: unsupportedDirectory.appending(path: "project.json"))
        var unsupportedFinished = false
        model.prepareWallpaper(WEWallpaper.load(from: unsupportedDirectory), for: key) { _ in unsupportedFinished = true }
        try await waitUntil("unsupported selection rollback") { unsupportedFinished }
        try require(model.previewWallpaper.id == first.id && !model.isApplyingSelection,
                    "An unsupported wallpaper left the preview in a loading state")
        model.prepareWallpaper(second, for: key) { _ in }
        model.prepareWallpaper(second, for: otherKey) { _ in }
        model.cancelPendingPreview(wallpaperID: second.id, for: otherKey)
        try require(model.previewWallpaper.id == second.id && model.isApplyingSelection,
                    "Cancelling another display cleared this display's selection")
        model.cancelPendingPreview(wallpaperID: second.id, for: key)
        print("PASS: continuous interaction delivery, final values, cancellation, per-property observation and immediate selection rollback")
    }

    static func testWallpaperSizes() async throws {
        let calls = Locked(0)
        let ranOnMain = Locked(false)
        let cache = WallpaperSizeCache { _, cancelled in
            calls.access { $0 += 1 }
            if Thread.isMainThread { ranOnMain.access { $0 = true } }
            for _ in 0..<60 {
                if cancelled() { return nil }
                usleep(1_000)
            }
            return 4_096
        }
        let directory = root.appending(path: "size-shared")
        let cancelledDelivered = Locked(false)
        let values = Locked([Int]())
        let token = cache.load(at: directory) { _ in cancelledDelivered.access { $0 = true } }
        cache.load(at: directory) { value in if let value { values.access { $0.append(value) } } }
        cache.cancel(token)
        let synchronous = await withCheckedContinuation { continuation in
            DispatchQueue.global().async { continuation.resume(returning: cache.size(at: directory)) }
        }
        try await waitUntil("shared size result") { values.access { $0 } == [4_096] }
        try require(calls.access { $0 } == 1 && synchronous == 4_096 && !ranOnMain.access({ $0 }) &&
                    !cancelledDelivered.access({ $0 }), "Directory size work was duplicated, blocked main, or ignored consumer cancellation")
        try require(cache.size(at: directory) == 4_096 && calls.access { $0 } == 1, "Directory size cache was not reused")

        let began = Locked(false)
        let cancelled = Locked(false)
        let cancellable = WallpaperSizeCache { _, isCancelled in
            began.access { $0 = true }
            while !isCancelled() { usleep(1_000) }
            cancelled.access { $0 = true }
            return nil
        }
        let cancelling = cancellable.load(at: directory) { _ in }
        try await waitUntil("cancellable directory enumeration") { began.access { $0 } }
        cancellable.cancel(cancelling)
        try await waitUntil("cancelled directory enumeration") { cancelled.access { $0 } }

        let oldStarted = Locked(false)
        let gate = DispatchSemaphore(value: 0)
        let generation = Locked(0)
        let generations = WallpaperSizeCache { _, _ in
            let current = generation.access { value in value += 1; return value }
            if current == 1 { oldStarted.access { $0 = true }; gate.wait() }
            return current
        }
        generations.load(at: directory) { _ in }
        try await waitUntil("old size generation") { oldStarted.access { $0 } }
        generations.invalidate()
        let fresh = Locked<Int?>(nil)
        generations.load(at: directory) { result in fresh.access { $0 = result } }
        try await waitUntil("new size generation") { fresh.access { $0 } == 2 }
        gate.signal()
        try await Task.sleep(for: .milliseconds(30))
        try require(generations.cachedSize(at: directory) == 2, "An obsolete enumeration replaced a newer cached size")

        let displayCache = WallpaperSizeCache { url, _ in url.lastPathComponent == "one" ? 1 : 2 }
        let display = WallpaperSizeModel(cache: displayCache)
        display.load(root.appending(path: "one"))
        display.load(root.appending(path: "two"))
        try await waitUntil("current size label") { display.bytes == 2 }
        try await Task.sleep(for: .milliseconds(30))
        try require(display.bytes == 2, "An old size callback changed the current label")
        print("PASS: shared directory work, background execution, cooperative cancellation, cache invalidation and stale label rejection")
    }

    static func testSubscriptionFiltering() async throws {
        let items = (0..<600).map { index in
            WorkshopItem(publishedFileId: String(index), title: index % 2 == 0 ? "Alpha" : "Beta",
                itemDescription: "Subscription fixture", previewImageURL: nil, tags: [], subscriptions: 0,
                favorited: 0, views: 0, fileSize: 1, timeCreated: Date(), timeUpdated: Date(),
                creatorSteamId: "", wallpaperType: "video")
        }
        let model = WorkshopViewModel(subscriptionCatalog: items)
        try await waitUntil("subscription catalog") { !model.isFilteringSubscriptions && model.subscriptionTotal == 600 }
        model.subscriptionSearchText = " Alpha "
        model.refreshSubscriptionFilters()
        try await waitUntil("subscription search") { !model.isFilteringSubscriptions && model.subscriptionTotal == 300 }
        try require(model.subscriptionItems.allSatisfy { $0.title == "Alpha" }, "Subscription filtering returned unmatched items")
        model.goToSubscriptionPage(2)
        try require(model.subscriptionCurrentPage == 2 && !model.isFilteringSubscriptions,
                    "Paging reran subscription filtering")
        model.subscriptionSearchText = "Beta"
        model.refreshSubscriptionFilters()
        try await Task.sleep(for: .milliseconds(1))
        model.subscriptionSearchText = "Alpha"
        model.refreshSubscriptionFilters()
        try await waitUntil("restored cached filter") { !model.isFilteringSubscriptions }
        try await Task.sleep(for: .milliseconds(80))
        try require(model.subscriptionItems.allSatisfy { $0.title == "Alpha" } && model.subscriptionCurrentPage == 1,
                    "A stale background filter overwrote the restored search")
        model.subscriptionSearchText = "does-not-exist"
        model.refreshSubscriptionFilters()
        try await waitUntil("empty subscription filter") { !model.isFilteringSubscriptions && model.subscriptionTotal == 0 }
        try require(model.subscriptionItems.isEmpty && model.subscriptionCurrentPage == 1, "Empty filtering left an invalid page")
        print("PASS: background subscription filtering, cached pagination, rapid query reversal and empty-result clamping")
    }

    static func testConditions() async throws {
        let evaluator = WEConditionEvaluator(evaluationTimeout: 0.15, startupTimeout: 1.5)
        defer { evaluator.cancel() }
        let basic = await evaluate(evaluator,
            conditions: ["flag.value", "amount.value > 2", "false", "undefined", "missing.value"],
            values: ["flag": ["value": true], "amount": ["value": 3]])
        try require(basic["flag.value"] == true && basic["amount.value > 2"] == true &&
                    basic["false"] == false && basic["undefined"] == true && basic["missing.value"] == true,
                    "Condition semantics changed")
        let typed: [String: Bool] = await withCheckedContinuation { continuation in
            evaluator.evaluate(identity: "typed", conditions: ["flag.value === false", "amount.value === 7", "label.value === 'hello'"],
                properties: ["flag": WEProjectProperty(type: "bool", value: .string("1")),
                             "amount": WEProjectProperty(type: "slider", value: .string("2")),
                             "label": WEProjectProperty(type: "textinput", value: .string("hello"))],
                overrides: ["flag": .bool(false), "amount": .number(7)]) { continuation.resume(returning: $0) }
        }
        try require(typed.count == 3 && typed.values.allSatisfy { $0 },
                    "Background condition preparation changed property types or override precedence")
        let ticks = Locked(0)
        let timer = Timer.scheduledTimer(withTimeInterval: 0.005, repeats: true) { _ in ticks.access { $0 += 1 } }
        let start = Date()
        let expired = await evaluate(evaluator, conditions: ["(() => { while (true) {} })()"])
        timer.invalidate()
        try require(expired.isEmpty && Date().timeIntervalSince(start) < 2,
                    "Infinite condition was not terminated")
        try require(ticks.access { $0 } >= 5, "Condition evaluation blocked the main run loop")
        for _ in 0..<4 {
            evaluator.evaluate(identity: "conditions", conditions: ["(() => { while (true) {} })()"], values: [:]) { _ in }
            try await Task.sleep(for: .milliseconds(35))
            evaluator.cancel()
            try await Task.sleep(for: .milliseconds(35))
        }
        let recovered = await evaluate(evaluator, conditions: ["false"])
        try require(recovered["false"] == false, "Hiding a property panel exhausted its failure budget")

        let blockedURL = Bundle.main.resourceURL!.appending(path: "blocked-worker")
        try FileManager.default.createSymbolicLink(at: blockedURL, withDestinationURL: Bundle.main.executableURL!)
        let blocked = WEConditionEvaluator(executableURL: blockedURL, evaluationTimeout: 0.2, startupTimeout: 0.2)
        let blockedStart = Date()
        let result = await evaluate(blocked, conditions: ["false"],
                                    values: ["large": ["value": String(repeating: "x", count: 1_000_000)]])
        blocked.cancel()
        try require(result.isEmpty && Date().timeIntervalSince(blockedStart) < 2,
                    "A worker that never reads stdin blocked its timeout")
        print("PASS: condition values, exceptions, timeout, main-loop heartbeat and cancellation recovery")
    }

    static func testObservation() throws {
        let item = WorkshopItem(publishedFileId: "1", title: "Regression", itemDescription: "",
            previewImageURL: nil, tags: [], subscriptions: 0, favorited: 0, views: 0, fileSize: 1,
            timeCreated: Date(), timeUpdated: Date(), creatorSteamId: "", wallpaperType: "video")
        let progress = DownloadProgress(receivedBytes: 1, totalBytes: 100, bytesPerSecond: 1, etaSeconds: nil)
        let task = DownloadTask(workshopItem: item, attemptID: nil, state: .downloading(progress),
                                startedAt: nil, completedAt: nil, purpose: .wallpaper)
        let summaryChanges = Locked(0)
        let progressChanges = Locked(0)
        withObservationTracking {
            _ = task.isActive; _ = task.isCompleted; _ = task.isClearable
        } onChange: { summaryChanges.access { $0 += 1 } }
        withObservationTracking { _ = task.state } onChange: { progressChanges.access { $0 += 1 } }
        task.state = .downloading(.init(receivedBytes: 50, totalBytes: 100, bytesPerSecond: 10, etaSeconds: 5))
        try require(summaryChanges.access { $0 } == 0 && progressChanges.access { $0 } == 1,
                    "Download progress invalidated the page summary")
        task.state = .completed
        try require(summaryChanges.access { $0 } == 1 && task.isCompleted && task.isClearable && !task.isActive,
                    "Download completion did not update its summary")
        MirageLocalization.shared.apply(MirageLocalization.shared.language)
        let localeUpdates = Locked(0)
        let subscription = MirageLocalization.shared.$locale.dropFirst().sink { _ in localeUpdates.access { $0 += 1 } }
        MirageLocalization.shared.apply(MirageLocalization.shared.language)
        try require(localeUpdates.access { $0 } == 0, "Unchanged language was republished")
        subscription.cancel()
        print("PASS: download observation scope and unchanged localization")
    }

    static func testStartupAndPlaylistNavigation() throws {
        var settings = GlobalSettings()
        settings.fps = 47
        settings.masterVolume = 0.37
        let encoded = try JSONEncoder().encode(settings)
        let restored = try JSONDecoder().decode(GlobalSettings.self, from: encoded)
        try require(restored.startupSection == .installed && restored.fps == 47 && restored.masterVolume == 0.37,
                    "A legacy settings payload lost its existing preferences")
        for section in MainSection.allCases {
            settings.startupSection = section
            let decoded = try JSONDecoder().decode(GlobalSettings.self, from: JSONEncoder().encode(settings))
            try require(decoded.startupSection == section && decoded.fps == 47,
                        "Startup page did not survive serialization")
        }
        settings.startupPage = "future-page"
        let unknown = try JSONDecoder().decode(GlobalSettings.self, from: JSONEncoder().encode(settings))
        try require(unknown.startupSection == .installed && unknown.masterVolume == 0.37,
                    "An unknown startup page reset unrelated settings")
        let navigation = MainNavigationModel(selection: .subscriptions)
        navigation.selection = .workshop
        try require(navigation.selection == .workshop, "Startup selection prevented subsequent navigation")

        func playlist(_ ids: [String], order: PlaylistOrder = .sorted) -> Playlist {
            var playlist = Playlist(items: ids.map { PlaylistItem(wallpaperID: $0, addedAt: Date()) })
            playlist.settings.order = order
            return playlist
        }
        var ordered = playlist(["a", "b", "missing", "c"])
        var cursor = PlaylistNavigation()
        cursor.synchronize(with: ordered)
        cursor.observe("a")
        let available: Set<String> = ["a", "b", "c"]
        func candidate(_ direction: PlaylistDirection, current: String?, pending: PlaylistNavigation.Target? = nil,
                       availableIDs: Set<String> = ["a", "b", "c"]) -> PlaylistNavigation.Target? {
            cursor.candidates(for: direction, in: ordered, availableIDs: availableIDs,
                              currentID: current, pending: pending).first
        }
        try require(candidate(.next, current: "a")?.wallpaperID == "b" &&
                    candidate(.previous, current: "a")?.wallpaperID == "c" &&
                    candidate(.next, current: "b")?.wallpaperID == "c",
                    "Ordered navigation failed to wrap or skip missing entries")
        try require(candidate(.next, current: "outside")?.wallpaperID == "a" &&
                    candidate(.previous, current: nil)?.wallpaperID == "c" &&
                    candidate(.previous, current: "missing")?.wallpaperID == "b",
                    "An absent or unplayable current wallpaper broke list positioning")
        try require(candidate(.next, current: "a", availableIDs: ["a"]) == nil &&
                    candidate(.previous, current: "a", availableIDs: []) == nil,
                    "Navigation reloaded the only current wallpaper or an empty list")
        let pending = candidate(.next, current: "a")!
        try require(candidate(.next, current: "a", pending: pending)?.wallpaperID == "c" &&
                    candidate(.previous, current: "a", pending: pending)?.wallpaperID == "a" && cursor.history == ["a"],
                    "Rapid commands used stale playback state or recorded unplayed targets")

        ordered.settings.order = .random
        cursor.synchronize(with: ordered)
        cursor.observe("a")
        try require(candidate(.previous, current: "a") == nil, "Random playback invented a previous wallpaper")
        cursor.commit(.init(wallpaperID: "b"))
        cursor.commit(.init(wallpaperID: "c"))
        let previous = candidate(.previous, current: "c")!
        try require(previous.wallpaperID == "b" &&
                    candidate(.previous, current: "c", pending: previous)?.wallpaperID == "a",
                    "Repeated backwards commands did not follow playback history")
        cursor.commit(previous)
        let forward = candidate(.next, current: "b")!
        try require(forward.wallpaperID == "c" && forward.historyIndex != nil,
                    "Forward navigation discarded the remaining random history")
        cursor.commit(forward)
        let randomChoices = cursor.candidates(for: .next, in: ordered, availableIDs: available, currentID: "c")
        try require(Set(randomChoices.map(\.wallpaperID)) == ["a", "b"] &&
                    randomChoices.allSatisfy { $0.historyIndex == nil },
                    "Random navigation repeated the current wallpaper")
        ordered.items.removeAll { $0.wallpaperID == "b" }
        cursor.synchronize(with: ordered)
        try require(candidate(.previous, current: "c")?.wallpaperID == "a", "Deleted history entries were retained")
        ordered = playlist(["a", "c"], order: .random)
        cursor.synchronize(with: ordered)
        try require(cursor.history.isEmpty, "Loading another playlist retained the previous history")
        for index in 0..<150 { cursor.observe(index.isMultiple(of: 2) ? "a" : "c") }
        try require(cursor.history.count == 100 && cursor.historyIndex == 99, "Playback history grew without a bound")
        cursor.observe("outside")
        try require(cursor.history.isEmpty, "An external wallpaper left an invalid history cursor")
        print("PASS: startup settings compatibility, page round trips, ordered navigation and bounded random history")
    }

    static func testPlaylistControls() async throws {
        guard let display = DisplayRegistry.shared.connected.first else {
            throw RegressionFailure(description: "Playlist integration tests require a connected display")
        }
        let first = try wallpaper("playlist-a")
        let second = try wallpaper("playlist-b")
        let third = try wallpaper("playlist-c")
        var library = [first, second, third]
        let playback = PlaylistPlaybackProbe(wallpaper: first, display: display.key)
        let manager = PlaylistManager(storageURL: root.appending(path: "playlists.json"))
        manager.ensureScreen(display.index)
        manager.clear(screen: display.index)
        for wallpaper in library { manager.add(wallpaper, to: display.index) }
        manager.updateSettings(on: display.index) { $0.timing = .never; $0.transition = .disabled }
        manager.startRotators(wallpaperViewModel: playback)
        defer { manager.stopAllRotators() }
        func advance(_ direction: PlaylistDirection) {
            manager.advance(direction, on: display.key, library: library)
        }
        func currentID() -> String? { playback.state(for: display.key)?.wallpaper.id }
        try require(manager.canAdvance(.next, on: display.key, library: library), "A populated playlist was disabled")
        advance(.next)
        try await waitUntil("manual next while paused") { currentID() == second.id }
        advance(.previous)
        try await waitUntil("manual previous") { currentID() == first.id }
        try require(playback.paused && playback.muted && !playback.restoredFocus,
                    "Manual switching cleared playback overrides or requested window focus")
        let beforeRapid = playback.requests.count
        for _ in 0..<7 { advance(.next) }
        try await waitUntil("coalesced manual navigation") { currentID() == second.id }
        try require(playback.requests.count == beforeRapid + 1,
                    "Rapid commands launched redundant intermediate wallpapers")
        playback.stopped = true
        try require(!manager.canAdvance(.next, on: display.key, library: library), "Stop policy allowed navigation")
        playback.stopped = false
        try require(!manager.canAdvance(.next, on: DisplayKey(rawValue: "disconnected"), library: library),
                    "An obsolete menu target was treated as a connected display")
        manager.updateSettings(on: display.index) { $0.videoSequence = true }
        advance(.next)
        try await waitUntil("manual video-sequence override") { currentID() == third.id }
        NotificationCenter.default.post(name: .rendererVideoDidEnd, object: nil, userInfo: ["screen": display.index])

        let missing = try wallpaper("playlist-missing")
        let webDirectory = root.appending(path: "playlist-web")
        try FileManager.default.createDirectory(at: webDirectory, withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: ["title": "Web", "type": "web", "file": "index.html"])
            .write(to: webDirectory.appending(path: "project.json"))
        try Data("<html></html>".utf8).write(to: webDirectory.appending(path: "index.html"))
        let web = WEWallpaper.load(from: webDirectory)
        for wallpaper in [missing, web] { manager.add(wallpaper, to: display.index) }
        library += [missing, web]
        try FileManager.default.removeItem(at: missing.entryURL)
        advance(.next)
        try await waitUntil("missing and untrusted playlist entries") { currentID() == first.id }
        try require(!playback.requests.contains { $0.0 == missing.id || $0.0 == web.id },
                    "An invalid entry or untrusted web wallpaper reached playback")
        try require(playback.requests.allSatisfy { $0.1 == display.key }, "Navigation changed the wrong display")

        manager.remove(itemID: missing.id, from: display.index)
        manager.remove(itemID: web.id, from: display.index)
        library = [first, second, third]
        manager.updateSettings(on: display.index) { $0.order = .random }
        try require(!manager.canAdvance(.previous, on: display.key, library: library), "A fresh random history had a previous item")
        advance(.next)
        try await waitUntil("first random item") { currentID() != first.id }
        let randomFirst = currentID()!
        advance(.next)
        try await waitUntil("second random item") { currentID() != randomFirst }
        let randomSecond = currentID()!
        advance(.previous)
        try await waitUntil("random history backwards") { currentID() == randomFirst }
        advance(.next)
        try await waitUntil("random history forwards") { currentID() == randomSecond }

        playback.paused = false
        let weekdays = try (0..<7).map { try wallpaper("weekday-\($0)") }
        var weekdayPlaylist = Playlist(items: weekdays.map { PlaylistItem(wallpaperID: $0.id, addedAt: Date()) })
        weekdayPlaylist.settings.timing = .dayOfWeek
        weekdayPlaylist.settings.transition = .disabled
        library = weekdays
        manager.load(saved: weekdayPlaylist, into: display.index)
        let today = Calendar.current.component(.weekday, from: Date()) - 1
        try await waitUntil("weekday anchor") { currentID() == weekdays[today].id }
        advance(.next)
        try await waitUntil("manual weekday selection") { currentID() == weekdays[(today + 1) % 7].id }
        try await Task.sleep(for: .milliseconds(100))
        try require(currentID() == weekdays[(today + 1) % 7].id,
                    "Rescheduling immediately restored the weekday anchor")

        manager.updateSettings(on: display.index) { $0.timing = .never; $0.transition = .enabled; $0.transitionSeconds = 0.2 }
        advance(.next)
        playback.applyDirectly(weekdays[0], to: display.key)
        try await Task.sleep(for: .milliseconds(250))
        try require(currentID() == weekdays[0].id, "A pending playlist operation overwrote a direct selection")
        let beforeStop = playback.requests.count
        advance(.next)
        manager.stopAllRotators()
        try await Task.sleep(for: .milliseconds(250))
        try require(playback.requests.count == beforeStop &&
                    !manager.canAdvance(.next, on: display.key, library: library),
                    "A stopped rotator accepted or completed an obsolete request")
        print("PASS: playlist dispatch, rapid commands, pause/mute preservation, invalid entries, random history and weekday override")
    }

    static func testPlaylistTransitions() async throws {
        let overlay = PlaylistTransitionOverlay.shared
        var applied: [String] = []
        overlay.present(on: 0, duration: 0, kind: .disabled) { applied.append("cancelled") }
        overlay.cancel(on: 0)
        try await Task.sleep(for: .milliseconds(30))
        try require(applied.isEmpty, "A cancelled queued transition still applied its wallpaper")
        overlay.present(on: 0, duration: 0.2, kind: .enabled) { applied.append("old") }
        try await Task.sleep(for: .milliseconds(30))
        overlay.present(on: 0, duration: 0.2, kind: .enabled) { applied.append("latest") }
        try await waitUntil("latest transition") { applied.contains("latest") }
        try await Task.sleep(for: .milliseconds(250))
        try require(applied == ["latest"], "An obsolete transition applied after its replacement")
        overlay.cancel(on: 0)
        print("PASS: queued transition cancellation and superseded animation callbacks")
    }

    static func pngData(color: NSColor = .systemTeal, width: Int = 128, height: Int = 128) throws -> Data {
        let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        context.setFillColor(color.cgColor)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let data = NSMutableData()
        let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, context.makeImage()!, nil)
        try require(CGImageDestinationFinalize(destination), "PNG fixture creation failed")
        return data as Data
    }

    static func image(_ loader: WorkshopImageLoader, source: WorkshopImageLoader.Source) async -> WorkshopImageLoader.Result? {
        await withCheckedContinuation { continuation in
            loader.load(source: source, variant: .init(pixels: 64, animated: false), priority: 1) {
                continuation.resume(returning: $0)
            }
        }
    }

    static func testImages() async throws {
        ImageProtocol.imageData = try pngData()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [ImageProtocol.self]
        configuration.urlCache = nil
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        let loader = WorkshopImageLoader(session: session, cacheDirectory: root.appending(path: "ImageCache"))
        let source = WorkshopImageLoader.Source(url: URL(string: "https://mirage-image.test/shared")!)
        var images: [WorkshopImageLoader.Result] = []
        for _ in 0..<2 {
            loader.load(source: source, variant: .init(pixels: 64, animated: false), priority: 1) {
                if let result = $0 { images.append(result) }
            }
        }
        try await waitUntil("coalesced images") { images.count == 2 }
        try require(images[0] === images[1] && ImageProtocol.metrics.access { $0.starts } == 1,
                    "One resource created duplicate in-flight downloads or decodes")
        try require(images[0].image.size.width <= 64, "Thumbnail was not downsampled")

        let cancelledSource = WorkshopImageLoader.Source(url: URL(string: "https://mirage-image.test/cancel")!)
        var cancelledDelivered = false
        let token = loader.load(source: cancelledSource, variant: .init(pixels: 64, animated: false), priority: 1) { _ in
            cancelledDelivered = true
        }
        try await waitUntil("cancellable transfer") { ImageProtocol.metrics.access { $0.starts } == 2 }
        loader.cancel(token)
        try await waitUntil("URLSession cancellation") { ImageProtocol.metrics.access { $0.cancellations } == 1 }
        let retry = await image(loader, source: cancelledSource)
        try require(retry != nil && !cancelledDelivered, "Cancelled transfer delivered a stale image or prevented retry")

        var completed = 0
        for index in 0..<12 {
            loader.load(source: .init(url: URL(string: "https://mirage-image.test/parallel-\(index)")!),
                        variant: .init(pixels: 64, animated: false), priority: 1) { result in
                if result != nil { completed += 1 }
            }
        }
        try await waitUntil("bounded image transfers") { completed == 12 }
        try require(ImageProtocol.metrics.access { $0.maximum } <= 4, "Image loading exceeded its concurrency bound")
        let corrupt = root.appending(path: "retry.png")
        try Data("invalid image".utf8).write(to: corrupt)
        let corruptResult = await image(loader, source: .init(url: corrupt))
        try require(corruptResult == nil, "Invalid image unexpectedly decoded")
        try ImageProtocol.imageData.write(to: corrupt)
        let corrected = await image(loader, source: .init(url: corrupt))
        try require(corrected != nil, "Invalid cached bytes prevented a corrected image from loading")
        let escaped = await image(loader, source: .init(url: corrupt, directory: root.appending(path: "inside"),
                                                       relativePath: "../retry.png"))
        try require(escaped == nil, "Preview loading bypassed path containment")
        print("PASS: image request sharing, downsampling, real transfer cancellation, retry and concurrency limit")
    }

    static func wallpaper(_ name: String) throws -> WEWallpaper {
        let directory = root.appending(path: name)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let object = ["title": name, "type": "video", "file": "video.mp4", "preview": "preview.png"]
        try JSONSerialization.data(withJSONObject: object).write(to: directory.appending(path: "project.json"))
        try Data().write(to: directory.appending(path: "video.mp4"))
        let result = WEWallpaper.load(from: directory)
        try require(result.presentationIsValid, "Wallpaper fixture is invalid")
        return result
    }

    static func testConfiguration() async throws {
        let first = try wallpaper("config-a")
        let second = try wallpaper("config-b")
        var changedPresentation = first
        changedPresentation.renderDirectory = second.renderDirectory
        try require(!first.hasSamePresentation(as: changedPresentation), "Resolved-directory change was ignored by the UI snapshot")
        let manager = ScreenSaverManager(configurationDirectory: root.appending(path: "Configuration"))
        let positions = ["a": WallpaperPosition(x: 0.1), "b": WallpaperPosition(x: 0.9)]
        let context = ScreenSaverManager.ConfigurationContext(positions: positions, selectedPosition: positions["b"]!,
            fps: 45, enableHDRVideo: false, loadFromMemory: false, language: "en-US")
        var runtime = WallpaperRuntimeState()
        runtime.position = positions["b"]!
        try manager.configure(with: first, runtime: runtime, properties: [:], fps: 45, context: context)
        try manager.configure(with: second, runtime: runtime, properties: [:], fps: 45, context: context)
        manager.updateRuntimeIfConfigured(wallpaper: first, runtime: runtime, properties: [:], context: context)
        var settings = GlobalSettings()
        settings.fps = 55
        settings.enableHDRVideo = true
        settings.wallpaperLoadSource = .memory
        manager.updateGlobalSettings(settings, languageIdentifier: "zh-Hans")
        manager.updateRuntimeIfConfigured(wallpaper: second, runtime: runtime, properties: [:], context: context)
        manager.flushConfigurationUpdates()
        let data = try Data(contentsOf: manager.configurationURL)
        let object = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        try require(object["wallpaperID"] as? String == second.id, "An old save replaced the newly configured wallpaper")
        try require(object["loadFromMemory"] as? Bool == true, "A stale runtime save overwrote a newer global configuration update")
        try require(object["fps"] as? Int == 55 && object["enableHDRVideo"] as? Bool == true &&
                    object["language"] as? String == "zh-Hans", "New global screen saver settings were not preserved")
        let saved = object["positionsByDisplay"] as? [String: [String: Double]]
        try require(saved?["a"]?["x"] == 0.1 && saved?["b"]?["x"] == 0.9,
                    "Per-display positions were not preserved")
        print("PASS: screen saver snapshots, stale save rejection and independent display positions")
    }

    static func testWallpaperRuntimeSnapshots() throws {
        let fm = FileManager.default
        let directory = root.appending(path: "runtime-scene")
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let asset = root.appending(path: "runtime-texture.png")
        let image = try pngData(color: .green)
        try image.write(to: asset)
        let properties: [String: WEProjectProperty] = [
            "enabled": .init(type: "bool", value: .bool(false)),
            "amount": .init(type: "slider", value: .number(0.375)),
            "number": .init(type: "combo", value: .number(1)),
            "zero": .init(type: "combo", value: .number(0)),
            "flag": .init(type: "combo", value: .bool(true)),
            "word": .init(type: "combo", value: .string("1")),
            "text": .init(type: "textinput", value: .string("文字 🌙\nsecond line")),
            "color": .init(type: "color", value: .string("0.2 0.4 0.8")),
            "texture": .init(type: "scenetexture", value: .string(asset.path))
        ]
        let descriptors = try JSONSerialization.jsonObject(with: JSONEncoder().encode(properties))
        try JSONSerialization.data(withJSONObject: ["title": "Runtime", "type": "scene", "file": "scene.json",
            "general": ["properties": descriptors]]).write(to: directory.appending(path: "project.json"))
        try Data("{}".utf8).write(to: directory.appending(path: "scene.json"))
        let wallpaper = WEWallpaper.load(from: directory)
        var runtime = WallpaperRuntimeState()
        runtime.speed = 1.75
        runtime.fillMode = .contain
        runtime.position = WallpaperPosition(x: 0.1, y: 0.2)
        let a = WallpaperRenderSnapshot(runtime: runtime, properties: properties,
                                        scriptStorage: ["origin": "[12,34]"])
        let decoded = try JSONDecoder().decode(WallpaperRenderSnapshot.self, from: JSONEncoder().encode(a))
        try require(decoded == a, "Runtime snapshot did not round-trip")
        try require(a.rawProperties["number"] == .number(1) && a.rawProperties["zero"] == .number(0) &&
                    a.rawProperties["flag"] == .bool(true) && a.rawProperties["word"] == .string("1"),
                    "Combo primitive types were changed")
        try require(a.properties(for: wallpaper)["text"]?.value == properties["text"]?.value,
                    "Unicode preview property was changed")
        var b = a
        b.rawProperties["enabled"] = .bool(true)
        b.position = WallpaperPosition(x: 0.9, y: 0.8)
        b.speed = 0.5
        b.scriptStorage = ["origin": "[56,78]"]
        let manager = ScreenSaverManager(configurationDirectory: root.appending(path: "RuntimeConfiguration"))
        var context = ScreenSaverManager.ConfigurationContext(positions: ["a": a.position, "b": b.position],
            selectedPosition: a.position, fps: 45, enableHDRVideo: false, loadFromMemory: false, language: "en")
        context.sourceDisplayKey = "a"
        context.runtimeByDisplay = ["a": a, "b": b]
        try manager.configure(with: wallpaper, runtime: runtime, properties: properties, fps: 45, context: context)
        try manager.configure(with: wallpaper, runtime: runtime, properties: properties, fps: 45,
                              forDynamicLockScreen: true, context: context)
        let savedA = try Data(contentsOf: manager.configurationURL)
        context.sourceDisplayKey = "b"
        b.rawProperties["amount"] = .number(0.8)
        context.runtimeByDisplay = ["b": b]
        manager.updateRuntimeIfConfigured(wallpaper: wallpaper, runtime: runtime,
                                           properties: b.properties(for: wallpaper), context: context)
        for url in [manager.configurationURL, manager.dynamicLockScreenConfigurationURL] {
            let object = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
            let perDisplay = object["runtimeByDisplay"] as! [String: [String: Any]]
            try require((object["speed"] as? NSNumber)?.floatValue == a.speed,
                        "Editing another display replaced the fallback runtime")
            try require(perDisplay["a"]?["scriptStorage"] as? [String: String] == a.scriptStorage &&
                        perDisplay["b"]?["scriptStorage"] as? [String: String] == b.scriptStorage,
                        "Saver script state crossed display boundaries")
            let values = perDisplay["b"]?["rawProperties"] as? [String: Any]
            try require(values?["amount"] as? Double == 0.8, "Saver did not synchronize changed properties")
        }
        let current = try Data(contentsOf: manager.configurationURL)
        manager.updateRuntimeIfConfigured(wallpaper: wallpaper, runtime: runtime,
                                           properties: b.properties(for: wallpaper), context: context)
        let duplicate = try Data(contentsOf: manager.configurationURL)
        try require(current == duplicate && current != savedA, "Saver updates were not stable or did not change")
        context.capturedAt = 0
        manager.updateRuntimeIfConfigured(wallpaper: wallpaper, runtime: runtime, properties: [:], context: context)
        let afterStale = try Data(contentsOf: manager.configurationURL)
        try require(afterStale == current, "A stale saver update replaced a newer configuration")

        let container = root.appending(path: "RuntimeLockDeployment")
        let configURL = container.appending(path: "dynamic-lock-screen.json")
        let displays = [
            DynamicLockScreenManager.DisplaySnapshot(displayID: 701, fallbackSource: nil,
                systemFallbackSource: nil, position: a.position, displayKey: "a", runtime: a),
            DynamicLockScreenManager.DisplaySnapshot(displayID: 702, fallbackSource: nil,
                systemFallbackSource: nil, position: b.position, displayKey: "b", runtime: b)
        ]
        let prepared = try DynamicLockScreenManager.prepareConfiguration(wallpaper, runtime: runtime,
            properties: properties, fps: 45, displays: displays, loadFromMemory: false, container: container)
        try DynamicLockScreenManager.commitConfiguration(prepared, configurationURL: configURL)
        let initialData = try Data(contentsOf: configURL)
        let initial = try JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: initialData)
        try require(initial.displays["display-701"]?.speed == a.speed && initial.displays["display-702"]?.speed == b.speed,
                    "Lock deployment lost per-display speed")
        let unchanged = try DynamicLockScreenManager.updateRuntime(a, wallpaper: wallpaper,
            displayKey: "a", displayID: 701, configurationURL: configURL)
        try require(unchanged == nil, "Identical lock runtime triggered a rewrite")
        var changed = b
        changed.rawProperties["text"] = .string("updated")
        changed.scriptStorage = [:]
        let updated = try DynamicLockScreenManager.updateRuntime(changed, wallpaper: wallpaper,
            displayKey: "b", displayID: 702, configurationURL: configURL)
        try require(updated?.displays["display-702"]?.rawProperties["text"] == .string("updated") &&
                    updated?.displays["display-702"]?.scriptStorage == [:], "Lock runtime/reset was not synchronized")
        try require(updated?.displays["display-701"]?.runtimeRevision == initial.displays["display-701"]?.runtimeRevision,
                    "Editing display B invalidated display A")
        let stale = try DynamicLockScreenManager.updateRuntime(b, wallpaper: wallpaper,
            displayKey: "b", displayID: 702, configurationURL: configURL, requestedAt: 0)
        try require(stale == nil, "Stale lock runtime was accepted")
        let sourceAfter = try Data(contentsOf: asset)
        try require(sourceAfter == image, "Deployment changed the original texture")
        guard case .object(let texture) = updated?.displays["display-702"]?.rawProperties["texture"],
              case .string(let path) = texture["value"] else {
            throw RegressionFailure(description: "Deployed texture descriptor is missing")
        }
        let deployedImage = try Data(contentsOf: URL(fileURLWithPath: path))
        try require(path.hasPrefix(prepared.root.path) && deployedImage == image,
                    "Updated lock texture is outside the deployment or damaged")
        print("PASS: typed runtime snapshots, per-display saver/lock synchronization, state isolation, speed, deduplication and stale update rejection")
    }

    static func testLockScreenDeployment() async throws {
        let fm = FileManager.default
        let video = try wallpaper("lock-video")
        let sceneDirectory = root.appending(path: "lock-scene")
        try fm.createDirectory(at: sceneDirectory, withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: [
            "title": "Lock Scene", "type": "scene", "file": "scene.json", "preview": "preview.png"
        ]).write(to: sceneDirectory.appending(path: "project.json"))
        try Data("{}".utf8).write(to: sceneDirectory.appending(path: "scene.json"))
        let scene = WEWallpaper.load(from: sceneDirectory)
        let asset = root.appending(path: "lock-property.png")
        try ImageProtocol.imageData.write(to: asset)
        try pngData(color: .red).write(to: scene.previewURL)
        let otherFrame = root.appending(path: "lock-rendered-frame.png")
        try pngData(color: .blue, width: 160, height: 90).write(to: otherFrame)
        let properties = ["texture": WEProjectProperty(type: "scenetexture", value: .string(asset.path))]
        let container = root.appending(path: "LockDeployment")
        let configurationURL = container.appending(path: "dynamic-lock-screen.json")
        let displays = [
            DynamicLockScreenManager.DisplaySnapshot(displayID: 9001, fallbackSource: asset,
                systemFallbackSource: asset, position: WallpaperPosition(x: 0.1), renderedPreviewSource: asset),
            DynamicLockScreenManager.DisplaySnapshot(displayID: 9002, fallbackSource: asset,
                systemFallbackSource: nil, position: WallpaperPosition(x: 0.9), renderedPreviewSource: otherFrame)
        ]
        let prepared = try await Task.detached {
            try DynamicLockScreenManager.prepareConfiguration(scene, runtime: WallpaperRuntimeState(),
                properties: properties, fps: 120, displays: displays, loadFromMemory: true, container: container)
        }.value
        let pending = try await Task.detached {
            try DynamicLockScreenManager.prepareConfiguration(video, runtime: WallpaperRuntimeState(),
                properties: [:], fps: 30, displays: displays, loadFromMemory: false, container: container)
        }.value
        try require(!fm.fileExists(atPath: configurationURL.path), "Preparation published an uncommitted lock configuration")
        try require(fm.fileExists(atPath: prepared.stagingRoot.path) && !fm.fileExists(atPath: prepared.root.path),
                    "Preparation did not isolate staged deployment files")
        try DynamicLockScreenManager.commitConfiguration(prepared, configurationURL: configurationURL)
        try require(!fm.fileExists(atPath: prepared.stagingRoot.path) && fm.fileExists(atPath: pending.stagingRoot.path),
                    "Committing one deployment damaged another pending deployment")
        let savedData = try Data(contentsOf: configurationURL)
        let saved = try JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: savedData)
        try require(saved.displays.count == 2 && saved.enabled == true, "Lock configuration lost its displays")
        try require(saved.displays["display-9001"]?.position?.x == 0.1 &&
                    saved.displays["display-9002"]?.position?.x == 0.9, "Lock deployment lost display positions")
        for display in saved.displays.values {
            try require(display.wallpaperID == scene.id && display.fps == 60 && display.loadFromMemory == true,
                        "Lock configuration did not preserve its settings snapshot")
            let paths = [display.renderDirectory, display.entryPath, display.previewPath,
                         display.renderedPreviewPath, display.desktopFallbackPath, display.systemFallbackPath].compactMap { $0 }
            for path in paths {
                try require(path.hasPrefix(prepared.root.path + "/") && fm.fileExists(atPath: path),
                            "A deployed lock asset still points into staging or is missing: \(path)")
            }
            guard let previewPath = display.renderedPreviewPath,
                  let preview = NSBitmapImageRep(data: try Data(contentsOf: URL(fileURLWithPath: previewPath))) else {
                throw RegressionFailure(description: "The deployed rendered preview cannot be decoded")
            }
            let expectedWidth = display.displayID == 9001 ? 128 : 160
            let expectedHeight = display.displayID == 9001 ? 128 : 90
            try require(preview.pixelsWide == expectedWidth && preview.pixelsHigh == expectedHeight,
                        "A display received the project preview or another display's frame")
            try require(display.previewPath == previewPath, "Lock snapshots do not use the rendered frame")
            let color = preview.colorAt(x: 0, y: 0)!.usingColorSpace(.deviceRGB)!
            try require(color.redComponent < 0.5, "The wallpaper's red project preview was deployed")
            guard case .object(let property) = display.rawProperties["texture"],
                  case .string(let path) = property["value"] else {
                throw RegressionFailure(description: "Lock scene property was not serialized")
            }
            try require(path.hasPrefix(prepared.root.path + "/") && fm.fileExists(atPath: path),
                        "Scene property asset path was not relocated with the deployment")
        }
        let blockedParent = container.appending(path: "blocked-parent")
        try Data().write(to: blockedParent)
        var rejected = false
        do {
            try DynamicLockScreenManager.commitConfiguration(pending,
                configurationURL: blockedParent.appending(path: "configuration.json"))
        } catch { rejected = true }
        try require(rejected && !fm.fileExists(atPath: pending.root.path),
                    "A failed configuration commit left its deployed files behind")
        let afterFailure = try Data(contentsOf: configurationURL)
        try require(afterFailure == savedData && fm.fileExists(atPath: prepared.root.path),
                    "A failed deployment damaged the active lock configuration")
        let missing = try wallpaper("lock-missing")
        try fm.removeItem(at: missing.entryURL)
        rejected = false
        do {
            _ = try DynamicLockScreenManager.prepareConfiguration(missing, runtime: WallpaperRuntimeState(),
                properties: [:], fps: 30, displays: displays, loadFromMemory: false, container: container)
        } catch { rejected = true }
        let remaining = try fm.contentsOfDirectory(atPath: container.appending(path: "DynamicLockScreen/Staging").path)
        try require(rejected && remaining.isEmpty, "A failed preparation retained staging files")

        let corruptFrame = root.appending(path: "corrupt-rendered-frame.png")
        try Data("invalid frame".utf8).write(to: corruptFrame)
        let unavailableFrames: [URL?] = [nil, corruptFrame, root.appending(path: "missing-frame.png"), root]
        let unavailableDisplays = unavailableFrames.enumerated().map { index, source in
            DynamicLockScreenManager.DisplaySnapshot(displayID: UInt32(index + 9001), fallbackSource: nil,
                systemFallbackSource: nil, position: .center, renderedPreviewSource: source)
        }
        let withoutFrames = try await Task.detached {
            try DynamicLockScreenManager.prepareConfiguration(scene, runtime: WallpaperRuntimeState(),
                properties: [:], fps: 30, displays: unavailableDisplays, loadFromMemory: false, container: container)
        }.value
        let fallbackConfiguration = try JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: withoutFrames.data)
        try require(fallbackConfiguration.displays.values.allSatisfy {
            guard let path = $0.renderedPreviewPath else { return false }
            return path == $0.previewPath && path.hasPrefix(withoutFrames.root.path + "/")
                && !fm.fileExists(atPath: path)
        }, "An unavailable rendered frame fell back to the wallpaper's project preview")
        try DynamicLockScreenManager.commitConfiguration(withoutFrames, configurationURL: configurationURL)
        let pendingPreviewData = try Data(contentsOf: configurationURL)
        let blueFrame = try pngData(color: .blue, width: 160, height: 90)
        let redFrame = try pngData(color: .red, width: 160, height: 90)
        let firstPreview = unavailableDisplays[0]
        try require(DynamicLockScreenManager.publishPreviews([(firstPreview, blueFrame)],
            deployment: withoutFrames.root, configurationURL: configurationURL,
            wallpaperID: scene.id, fillMode: .cover), "A background preview was not published after deployment completed")
        let previewURL = URL(fileURLWithPath: fallbackConfiguration.displays["display-9001"]!.renderedPreviewPath!)
        let publishedFrame = try Data(contentsOf: previewURL)
        let afterPreview = try Data(contentsOf: configurationURL)
        try require(publishedFrame == blueFrame && afterPreview == pendingPreviewData,
                    "Publishing a preview overwrote the lock configuration")
        var changed = fallbackConfiguration
        changed.displays["display-9001"]?.position = WallpaperPosition(x: 0.9)
        try JSONEncoder().encode(changed).write(to: configurationURL, options: .atomic)
        try require(!DynamicLockScreenManager.publishPreviews([(firstPreview, redFrame)],
            deployment: withoutFrames.root, configurationURL: configurationURL,
            wallpaperID: scene.id, fillMode: .cover), "A stale preview overwrote a newer crop setting")
        changed = fallbackConfiguration
        changed.enabled = false
        try JSONEncoder().encode(changed).write(to: configurationURL, options: .atomic)
        try require(!DynamicLockScreenManager.publishPreviews([(firstPreview, redFrame)],
            deployment: withoutFrames.root, configurationURL: configurationURL,
            wallpaperID: scene.id, fillMode: .cover), "A disabled lock screen accepted a pending preview")
        let replacement = try await Task.detached {
            try DynamicLockScreenManager.prepareConfiguration(scene, runtime: WallpaperRuntimeState(),
                properties: [:], fps: 30, displays: unavailableDisplays, loadFromMemory: false, container: container)
        }.value
        try DynamicLockScreenManager.commitConfiguration(replacement, configurationURL: configurationURL)
        try require(!DynamicLockScreenManager.publishPreviews([(firstPreview, redFrame)],
            deployment: withoutFrames.root, configurationURL: configurationURL,
            wallpaperID: scene.id, fillMode: .cover), "An old capture was published after reconfiguring the same wallpaper")
        let preservedFrame = try Data(contentsOf: previewURL)
        try require(preservedFrame == blueFrame, "A rejected capture changed a published frame")
        print("PASS: deployment before preview completion, background publication and stale capture rejection")

        let saver = ScreenSaverManager(configurationDirectory: root.appending(path: "LockSaverConfiguration"))
        let context = ScreenSaverManager.ConfigurationContext(
            positions: ["a": WallpaperPosition(x: 0.2)], selectedPosition: WallpaperPosition(x: 0.2),
            fps: 50, enableHDRVideo: true, loadFromMemory: true, language: "zh-Hans")
        let data = try await Task.detached {
            try ScreenSaverManager.prepareConfiguration(with: scene, runtime: WallpaperRuntimeState(),
                properties: properties, context: context)
        }.value
        try require(!fm.fileExists(atPath: saver.dynamicLockScreenConfigurationURL.path),
                    "Screen saver preparation published an uncommitted configuration")
        try saver.configure(with: data, forDynamicLockScreen: true)
        let saverData = try Data(contentsOf: saver.dynamicLockScreenConfigurationURL)
        let object = try JSONSerialization.jsonObject(with: saverData) as! [String: Any]
        try require(object["wallpaperID"] as? String == scene.id && object["fps"] as? Int == 50 &&
                    object["enableHDRVideo"] as? Bool == true && object["loadFromMemory"] as? Bool == true,
                    "Background screen saver preparation lost its settings snapshot")
        print("PASS: background lock deployment, staging isolation, relocated assets and failure rollback")
    }

    static func runRenderer() {
        let fails = CommandLine.arguments.dropFirst().first?.contains("fail-activate") == true
        var activated = false
        var snapshotRequests = 0
        let commandLog = ProcessInfo.processInfo.environment["MIRAGE_PLAYBACK_POLICY_COMMAND_LOG"]
            .flatMap { try? FileHandle(forWritingTo: URL(fileURLWithPath: $0)) }
        defer { try? commandLog?.close() }
        func emit(_ event: String, _ fields: [String: Any] = [:]) {
            var object = fields
            object["event"] = event
            var data = try! JSONSerialization.data(withJSONObject: object)
            data.append(10)
            try? FileHandle.standardOutput.write(contentsOf: data)
        }
        usleep(50_000)
        emit(CommandLine.arguments.contains("--no-spectrum") ? "first-frame-presented" : "prepared")
        while let line = readLine(), let data = line.data(using: .utf8) {
            guard let command = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
            if let commandLog {
                var entry = command
                entry["pid"] = ProcessInfo.processInfo.processIdentifier
                if var encoded = try? JSONSerialization.data(withJSONObject: entry) {
                    encoded.append(10)
                    _ = try? commandLog.seekToEnd()
                    try? commandLog.write(contentsOf: encoded)
                }
            }
            switch command["cmd"] as? String {
            case "activate":
                activated = true
                emit(fails ? "activation-failed" : "activated")
                if !fails { emit("position-availability", ["x": true, "y": false]) }
            case "snapshot":
                guard let path = command["path"] as? String, let token = command["token"] as? String else { continue }
                snapshotRequests += 1
                if path.contains("retry-preview"), snapshotRequests < 3 {
                    emit("snapshot-done", ["token": token, "ok": false])
                    continue
                }
                if path.contains("stalled-preview") {
                    try? Data().write(to: URL(fileURLWithPath: path + ".requested"))
                    continue
                }
                var details: [String: Any] = ["arguments": CommandLine.arguments, "activated": activated,
                                               "snapshotRequests": snapshotRequests]
                if let index = CommandLine.arguments.firstIndex(of: "--user-properties"),
                   index + 1 < CommandLine.arguments.count,
                   let data = try? Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[index + 1])),
                   let properties = try? JSONSerialization.jsonObject(with: data) {
                    details["properties"] = properties
                }
                let saved = (try? pngData(color: .blue).write(to: URL(fileURLWithPath: path))) != nil
                if let data = try? JSONSerialization.data(withJSONObject: details) {
                    try? data.write(to: URL(fileURLWithPath: path + ".json"))
                }
                emit("snapshot-done", ["token": token, "ok": saved && !activated])
            case "deactivate": emit("deactivated")
            case "quit": return
            default: break
            }
        }
    }

    static func render(_ controller: RendererController, _ wallpaper: WEWallpaper,
                       display: CGDirectDisplayID) async -> Bool {
        var options = RenderOptions()
        options.enableSpectrum = false
        options.assignmentID = UUID()
        return await withCheckedContinuation { continuation in
            controller.render(wallpaper, onDisplay: display, options: options) { continuation.resume(returning: $0) }
        }
    }

    static func testRealLockPreviews(in directory: URL) async throws {
        guard let screen = NSScreen.main,
              let displayID = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
            throw RegressionFailure(description: "A connected display is required")
        }
        let controller = RendererController()
        defer { controller.stopAllAndWait() }
        for kind in ["video", "scene"] {
            let wallpaper = WEWallpaper.load(from: directory.appending(path: kind))
            try require(wallpaper.isValid, "Real \(kind) fixture is not valid")
            for (index, position) in [0.0, 0.5, 1.0].enumerated() {
                var options = RenderOptions()
                options.position = WallpaperPosition(x: position)
                options.enableSpectrum = false
                let output = directory.appending(path: "\(kind)-\(index).heic")
                let success = await withCheckedContinuation { continuation in
                    controller.snapshot(wallpaper: wallpaper, onDisplay: displayID.uint32Value,
                                        options: options, path: output.path) { continuation.resume(returning: $0) }
                }
                try require(success, "Real \(kind) preview failed at position \(position)")
                guard let image = NSBitmapImageRep(data: try Data(contentsOf: output)),
                      let color = image.colorAt(x: image.pixelsWide / 2, y: image.pixelsHigh / 2)?.usingColorSpace(.sRGB) else {
                    throw RegressionFailure(description: "Real preview cannot be decoded")
                }
                let channels = [color.redComponent, color.greenComponent, color.blueComponent]
                try require(channels[index] > 0.7 && channels[index] - max(channels[(index + 1) % 3], channels[(index + 2) % 3]) > 0.5,
                            "Real \(kind) preview crop is incorrect: \(channels)")
                try require(abs(Double(image.pixelsWide) / Double(image.pixelsHigh) - screen.frame.width / screen.frame.height) < 0.02,
                            "Real preview lost the display aspect ratio")
                try require(controller.activeDisplayIDs.isEmpty, "Preview rendering became desktop playback")
                try await waitUntil("temporary preview renderer exit") { controller.processIdentifiers.isEmpty }
                print("PASS: real \(kind) preview, position \(position), \(image.pixelsWide)x\(image.pixelsHigh), RGB \(channels)")
            }
        }
    }

    static func testRenderers() async throws {
        let directory = Bundle.main.resourceURL!.appending(path: "Renderers")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: directory.appending(path: "VideoWallpaper"),
                                                   withDestinationURL: Bundle.main.executableURL!)
        let first = try wallpaper("renderer-a")
        let second = try wallpaper("renderer-b")
        let failure = try wallpaper("fail-activate")
        let controller = RendererController()
        defer { controller.stopAllAndWait() }
        let firstResult = await render(controller, first, display: 9001)
        try require(firstResult, "Initial renderer did not activate")
        try await waitUntil("position capabilities") {
            controller.positionAvailability(onDisplay: 9001, wallpaperID: first.id).known
        }
        let failed = await render(controller, failure, display: 9001)
        try require(!failed && controller.currentWallpaper(onDisplay: 9001)?.id == first.id,
                    "Activation failure did not restore the previous wallpaper")
        let otherDisplay = await render(controller, first, display: 9002)
        try require(otherDisplay, "Second display did not activate")
        try FileManager.default.createSymbolicLink(at: directory.appending(path: "SceneWallpaper"),
                                                   withDestinationURL: Bundle.main.executableURL!)
        let previewScene = WEWallpaper.load(from: root.appending(path: "lock-scene"))
        var previewOptions = RenderOptions()
        previewOptions.position = WallpaperPosition(x: 0.2, y: 0.8)
        previewOptions.fillMode = .contain
        previewOptions.userProperties = ["tint": WEProjectProperty(type: "color", value: .string("0 1 0"))]
        for wallpaper in [second, previewScene] {
            let target = root.appending(path: "rendered-\(wallpaper.kind.rawValue).heic")
            let captured = await withCheckedContinuation { continuation in
                controller.snapshot(wallpaper: wallpaper, onDisplay: 9003, options: previewOptions, path: target.path) {
                    continuation.resume(returning: $0)
                }
            }
            try require(captured && NSImage(contentsOf: target)?.isValid == true, "Hidden rendering did not return a readable frame")
            let details = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: target.path + ".json"))) as! [String: Any]
            let arguments = details["arguments"] as! [String]
            try require(details["activated"] as? Bool == false && arguments.contains("--muted"),
                        "Generating a preview showed or unmuted the temporary renderer")
            for (option, value) in [("--display-id", "9003"), ("--fill", "contain"), ("--position-x", "0.2"), ("--position-y", "0.8")] {
                let index = arguments.firstIndex(of: option)!
                try require(arguments[index + 1] == value, "A preview lost its display or crop settings")
            }
            if wallpaper.kind == .scene {
                let properties = details["properties"] as? [String: [String: String]]
                try require(properties?["tint"]?["value"] == "0 1 0", "The scene preview lost its configured properties")
            }
            try require(controller.currentWallpaper(onDisplay: 9001)?.id == first.id &&
                        controller.currentWallpaper(onDisplay: 9002)?.id == first.id &&
                        !controller.hasCoverageOrWork(onDisplay: 9003), "Preview generation changed desktop playback")
        }
        let retryPreview = root.appending(path: "retry-preview.heic")
        let retried = await withCheckedContinuation { continuation in
            controller.snapshot(wallpaper: second, onDisplay: 9003, options: RenderOptions(), path: retryPreview.path) {
                continuation.resume(returning: $0)
            }
        }
        let retryDetails = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: retryPreview.path + ".json"))) as! [String: Any]
        try require(retried && retryDetails["snapshotRequests"] as? Int == 3, "A frame that was not ready was not retried")
        var completions = 0
        var latestSucceeded = false
        var options = RenderOptions()
        options.enableSpectrum = false
        let start = Date()
        for index in 0..<100 {
            controller.render(index == 99 ? second : first, onDisplay: 9001, options: options) { success in
                completions += 1
                if index == 99 { latestSucceeded = success }
            }
        }
        try require(Date().timeIntervalSince(start) < 0.1, "Submitting renderer requests blocked the UI")
        try require(controller.hasCoverageOrWork(onDisplay: 9001), "Queued requests were absent from coverage state")
        try await waitUntil("rapid renderer switching", timeout: 10) { completions == 100 }
        try require(latestSucceeded && controller.currentWallpaper(onDisplay: 9001)?.id == second.id &&
                    controller.currentWallpaper(onDisplay: 9002)?.id == first.id,
                    "Rapid switching lost the newest request or changed another display")
        controller.stop(displayID: 9001)
        try await waitUntil("per-display stop") { !controller.hasCoverageOrWork(onDisplay: 9001) }
        try require(controller.isRendering(onDisplay: 9002), "Stopping one display stopped another")
        controller.render(second, onDisplay: 9001, options: options)
        controller.stopAll()
        try await waitUntil("stop supersedes queued launch") {
            !controller.hasCoverageOrWork(onDisplay: 9001) && !controller.hasCoverageOrWork(onDisplay: 9002) &&
                controller.processIdentifiers.isEmpty
        }
        for cycle in 0..<5 {
            let beforeLock = await render(controller, first, display: 9001)
            let secondDisplay = await render(controller, second, display: 9002)
            try require(beforeLock && secondDisplay, "Renderers failed before lock cycle \(cycle)")
            let previousPIDs = controller.processIdentifiers
            var pendingCompletions = 0
            controller.render(second, onDisplay: 9001, options: options) { _ in
                pendingCompletions += 1
            }
            controller.suspendAllAndWait()
            controller.suspendAllAndWait()
            try require(controller.processIdentifiers.isEmpty,
                        "Lock suspension retained renderer processes")
            try require(previousPIDs.allSatisfy { Darwin.kill($0, 0) == -1 && errno == ESRCH },
                        "Lock suspension left a renderer alive")
            let lockedResult = await render(controller, first, display: 9001)
            try require(!lockedResult, "Rendering was accepted while locked")
            try require(controller.resumeAfterSuspension(), "Unlock failed to resume the controller")
            let afterUnlock = await render(controller, second, display: 9001)
            let otherAfterUnlock = await render(controller, first, display: 9002)
            try require(afterUnlock && otherAfterUnlock,
                        "Wallpaper switching failed after unlock cycle \(cycle)")
            try await waitUntil("cancelled lock transition completion") { pendingCompletions == 1 }
            try require(controller.currentWallpaper(onDisplay: 9001)?.id == second.id &&
                        controller.currentWallpaper(onDisplay: 9002)?.id == first.id,
                        "An obsolete lock transition overwrote the restored wallpapers")
        }
        let cancelledPreview = root.appending(path: "stalled-preview-cancel.heic")
        try await waitUntil("desktop renderer cleanup before preview cancellation") { controller.processIdentifiers.count == 2 }
        let desktopPIDs = controller.processIdentifiers
        var cancelledPreviewCompletions: [Bool] = []
        let cancelledToken = controller.snapshot(wallpaper: second, onDisplay: 9003,
            options: RenderOptions(), path: cancelledPreview.path) { cancelledPreviewCompletions.append($0) }
        try await waitUntil("cancellable preview") { FileManager.default.fileExists(atPath: cancelledPreview.path + ".requested") }
        controller.cancelPreview(cancelledToken)
        controller.cancelPreview(cancelledToken)
        try await waitUntil("cancelled preview cleanup") {
            cancelledPreviewCompletions.count == 1 && controller.processIdentifiers == desktopPIDs
        }
        try require(cancelledPreviewCompletions == [false] && controller.isRendering(onDisplay: 9001)
            && controller.isRendering(onDisplay: 9002), "Cancelling a preview changed desktop playback or completed more than once")
        let stalled = root.appending(path: "stalled-preview.heic")
        var previewCompletions: [Bool] = []
        controller.snapshot(wallpaper: second, onDisplay: 9003, options: RenderOptions(), path: stalled.path) {
            previewCompletions.append($0)
        }
        try await waitUntil("pending preview") { FileManager.default.fileExists(atPath: stalled.path + ".requested") }
        let previewPIDs = controller.processIdentifiers
        controller.stopAllAndWait()
        try await waitUntil("preview shutdown acknowledgement") { previewCompletions.count == 1 }
        try require(previewCompletions == [false] && previewPIDs.allSatisfy { Darwin.kill($0, 0) == -1 && errno == ESRCH },
                    "Shutdown left a preview renderer or an unresolved preview request")
        try require(!controller.resumeAfterSuspension(), "App termination became resumable")
        let afterTermination = await render(controller, first, display: 9001)
        try require(!afterTermination, "Rendering was accepted after app termination")
        print("PASS: repeated lock suspension, in-flight cancellation, unlock switching and terminal shutdown")
        print("PASS: renderer activation, failure rollback, 100 rapid requests, display isolation and stop ordering")
        print("PASS: hidden video and scene previews, crop and property propagation, desktop isolation and preview shutdown")
    }

    static func testLogs() async throws {
        let service = MirageLogService(logDirectory: root.appending(path: "Logs"), capturesStandardStreams: false)
        service.start()
        var publications = 0
        let subscription = service.$visibleText.dropFirst().sink { _ in publications += 1 }
        for index in 0..<4_000 { service.append("line-\(index) password=example " + String(repeating: "z", count: 80)) }
        let export = root.appending(path: "log-export.txt")
        service.export(to: export)
        try await waitUntil("complete log export") { FileManager.default.fileExists(atPath: export.path) }
        try require(publications == 0 && service.visibleText.isEmpty, "Hidden log window still published text")
        let contents = try String(contentsOf: export, encoding: .utf8)
        try require(contents.contains("line-0 ") && contents.contains("line-3999 ") &&
                    contents.contains("password=<redacted>") && !contents.contains("password=example"),
                    "Log export lost buffered lines or redaction")
        service.setDisplaying(true)
        try await waitUntil("visible log tail") { service.visibleText.contains("line-3999") }
        try require(service.visibleText.utf16.count <= 200_000, "Log display exceeded its bound")
        let previous = service.visibleText
        service.setDisplaying(false)
        service.append("hidden-after-close")
        try await Task.sleep(for: .milliseconds(300))
        try require(service.visibleText == previous, "Closed log view received a delayed update")
        let controller = DeveloperLogWindowController(service: service)
        controller.showWindow(nil)
        try await waitUntil("native log presentation") { service.visibleText.contains("hidden-after-close") }
        if let view = controller.window?.contentView {
            view.layoutSubtreeIfNeeded()
            if let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: bitmap)
                try bitmap.representation(using: .png, properties: [:])?.write(to: root.appending(path: "log-window.png"))
            }
        }
        controller.window?.orderOut(nil)
        service.setDisplaying(false)
        subscription.cancel()
        print("PASS: hidden log buffering, bounded publication, full export, redaction and native log view")
    }
}
