//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Combine
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
    func access<T>(_ body: (inout Value) -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body(&value)
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
            try await testConditions()
            try testObservation()
            try await testImages()
            try await testConfiguration()
            try await testLockScreenDeployment()
            try await testRenderers()
            try await testLogs()
            print("UIResponsivenessRegression: all checks passed")
        } catch {
            fputs("UIResponsivenessRegression: \(error)\n", stderr)
            exit(1)
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

    static func testConditions() async throws {
        let evaluator = WEConditionEvaluator(evaluationTimeout: 0.15, startupTimeout: 1.5)
        defer { evaluator.cancel() }
        let basic = await evaluate(evaluator,
            conditions: ["flag.value", "amount.value > 2", "false", "undefined", "missing.value"],
            values: ["flag": ["value": true], "amount": ["value": 3]])
        try require(basic["flag.value"] == true && basic["amount.value > 2"] == true &&
                    basic["false"] == false && basic["undefined"] == true && basic["missing.value"] == true,
                    "Condition semantics changed")
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

    static func pngData() throws -> Data {
        let context = CGContext(data: nil, width: 128, height: 128, bitsPerComponent: 8, bytesPerRow: 512,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        context.setFillColor(NSColor.systemTeal.cgColor)
        context.fill(CGRect(x: 0, y: 0, width: 128, height: 128))
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
        try ImageProtocol.imageData.write(to: scene.previewURL)
        let properties = ["texture": WEProjectProperty(type: "scenetexture", value: .string(asset.path))]
        let container = root.appending(path: "LockDeployment")
        let configurationURL = container.appending(path: "dynamic-lock-screen.json")
        let displays = [
            DynamicLockScreenManager.DisplaySnapshot(displayID: 9001, fallbackSource: asset,
                systemFallbackSource: asset, position: WallpaperPosition(x: 0.1)),
            DynamicLockScreenManager.DisplaySnapshot(displayID: 9002, fallbackSource: asset,
                systemFallbackSource: nil, position: WallpaperPosition(x: 0.9))
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
                         display.desktopFallbackPath, display.systemFallbackPath].compactMap { $0 }
            for path in paths {
                try require(path.hasPrefix(prepared.root.path + "/") && fm.fileExists(atPath: path),
                            "A deployed lock asset still points into staging or is missing: \(path)")
            }
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
        func emit(_ event: String, _ fields: [String: Any] = [:]) {
            var object = fields
            object["event"] = event
            var data = try! JSONSerialization.data(withJSONObject: object)
            data.append(10)
            try? FileHandle.standardOutput.write(contentsOf: data)
        }
        usleep(50_000)
        emit("prepared")
        while let line = readLine(), let data = line.data(using: .utf8) {
            guard let command = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
            switch command["cmd"] as? String {
            case "activate":
                emit(fails ? "activation-failed" : "activated")
                if !fails { emit("position-availability", ["x": true, "y": false]) }
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
        controller.stopAllAndWait()
        try require(!controller.resumeAfterSuspension(), "App termination became resumable")
        let afterTermination = await render(controller, first, display: 9001)
        try require(!afterTermination, "Rendering was accepted after app termination")
        print("PASS: repeated lock suspension, in-flight cancellation, unlock switching and terminal shutdown")
        print("PASS: renderer activation, failure rollback, 100 rapid requests, display isolation and stop ordering")
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
