//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Darwin
import Foundation

enum FillMode: String, CaseIterable, Codable, Identifiable {
    case cover, contain, stretch
    var id: Self { self }
    var displayName: String {
        switch self {
        case .cover: return L("填充")
        case .contain: return L("适应")
        case .stretch: return L("拉伸")
        }
    }
}

/// Final playback state handed to a renderer. The renderer never learns *why*
/// it is in this state — that judgement belongs to the app, which is the only
/// process with a global view of window layering, displays and power sources.
enum MiragePowerState: String, Equatable {
    /// Render normally at the configured frame rate.
    case run
    /// Keep rendering, but at the reduced frame rate carried alongside.
    case throttle
    /// Stop producing frames entirely.
    case pause
}

final class RendererProcess {
    let process: Process
    let stdinPipe: Pipe
    let stdoutPipe: Pipe?
    let stderrPipe: Pipe
    let wallpaper: WEWallpaper
    let displayID: CGDirectDisplayID
    let generation: UInt64
    var launchOptions: RenderOptions
    var assignmentID: UUID?
    var desiredVolume: Float
    var desiredMuted: Bool
    var desiredPaused = false
    /// Last power directive sent, replayed when a candidate is activated so a
    /// renderer that came up mid-throttle does not silently run at full rate.
    var desiredPowerState = MiragePowerState.run
    var desiredPowerFps: Int?
    var desiredFps: Int
    var desiredSpeed: Float
    var desiredFillMode: FillMode
    var desiredHDRVideo: Bool
    var desiredUserProperties: [String: WEProjectProperty]
    /// Scene renderers receive their initial properties through the launch-time
    /// JSON file. Runtime snapshots must not replay that same set of values:
    /// some complex scenes rebuild Vulkan resources on every property update.
    var usesLaunchUserProperties = false
    let spectrumEnabled: Bool
    var spectrumDemanded: Bool
    var transitionCompletions: [(Bool) -> Void] = []

    private let stateLock = NSLock()
    private let cleanupLock = NSLock()
    private let spectrumStateLock = NSLock()
    private let spectrumQueue = DispatchQueue(label: "cn.laobamac.Mirage.renderer.spectrum")
    private var terminated = false
    private var cleanupComplete = false
    private var pendingSpectrum: [Float]?
    private var spectrumWriterActive = false
    private var stdoutBuffer = Data()

    // MARK: Non-blocking outbound pipe
    //
    // The renderer's stdin has a fixed-size kernel buffer. A renderer that is
    // slow to drain it (shader compilation, Vulkan init, a wedged scene) used
    // to block whichever thread happened to call write() — including the main
    // thread, which beachballed the whole app. The write end is therefore put
    // into O_NONBLOCK and every command goes through this queue, drained on a
    // dedicated serial queue by a write source.
    private let outboundLock = NSLock()
    private let ioQueue = DispatchQueue(label: "cn.laobamac.Mirage.renderer.io")
    private var outboundControl: [Data] = []
    private var outboundSpectrumLine: Data?
    private var outboundChunk: Data?
    private var outboundOffset = 0
    private var writeSource: DispatchSourceWrite?
    private var writerResumed = false
    private var outboundClosed = false
    private var closeWhenDrained = false
    /// Control commands are never dropped on purpose. This bound only stops a
    /// permanently wedged renderer from growing the queue without limit.
    private static let maxPendingControlCommands = 512

    var isTerminated: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return terminated
    }

    /// Temporary files associated with this renderer process, cleaned up after exit.
    var tempFiles: [URL] = []

    init(process: Process, stdinPipe: Pipe, stdoutPipe: Pipe?, stderrPipe: Pipe,
         wallpaper: WEWallpaper, displayID: CGDirectDisplayID, generation: UInt64,
         options: RenderOptions,
         spectrumEnabled: Bool) {
        self.process = process
        self.stdinPipe = stdinPipe
        self.stdoutPipe = stdoutPipe
        self.stderrPipe = stderrPipe
        self.wallpaper = wallpaper
        self.displayID = displayID
        self.generation = generation
        self.launchOptions = options
        self.assignmentID = options.assignmentID
        self.desiredVolume = options.volume
        self.desiredMuted = options.muted
        self.desiredPaused = options.powerState == .pause
        self.desiredPowerState = options.powerState
        self.desiredPowerFps = options.powerFps
        self.desiredFps = options.fps
        self.desiredSpeed = options.speed
        self.desiredFillMode = options.fillMode
        self.desiredHDRVideo = options.enableHDRVideo
        self.desiredUserProperties = options.userProperties
        self.spectrumEnabled = spectrumEnabled
        self.spectrumDemanded = false
        setUpWriter()
    }

    deinit {
        // A suspended dispatch source must never be released, so the writer is
        // always torn down explicitly — including for handles that are dropped
        // without ever having been stopped.
        closeWriter()
    }

    func send(_ command: [String: Any]) {
        enqueue(command, allowAfterStop: false)
    }

    func sendSpectrum(_ spectrum: [Float]) {
        spectrumStateLock.lock()
        pendingSpectrum = spectrum
        let shouldStart = !spectrumWriterActive
        spectrumWriterActive = true
        spectrumStateLock.unlock()
        guard shouldStart else { return }
        spectrumQueue.async { [weak self] in
            self?.drainSpectrum()
        }
    }

    private func drainSpectrum() {
        while true {
            spectrumStateLock.lock()
            guard let spectrum = pendingSpectrum else {
                spectrumWriterActive = false
                spectrumStateLock.unlock()
                return
            }
            pendingSpectrum = nil
            spectrumStateLock.unlock()
            enqueue(["cmd": "audioSpectrum", "data": spectrum],
                    allowAfterStop: false, kind: .spectrum)
        }
    }

    // MARK: Outbound queue

    private enum OutboundKind {
        case control
        case spectrum
    }

    /// Serializes `command` and hands it to the writer. Never blocks: the
    /// caller is only charged the JSON encoding plus two uncontended locks.
    private func enqueue(_ command: [String: Any], allowAfterStop: Bool,
                         kind: OutboundKind = .control) {
        guard let data = try? JSONSerialization.data(withJSONObject: command, options: []) else {
            NSLog("[Mirage] 无法序列化控制指令: \(command)")
            return
        }
        var line = data
        line.append(0x0A)
        stateLock.lock()
        let canWrite = (allowAfterStop || !terminated) && process.isRunning
        stateLock.unlock()
        guard canWrite else { return }

        var overflowed = false
        outboundLock.lock()
        if outboundClosed || (closeWhenDrained && kind == .spectrum) {
            outboundLock.unlock()
            return
        }
        switch kind {
        case .spectrum:
            // Spectrum frames are telemetry. A renderer that stalls must never
            // accumulate a backlog of them, so a still-pending frame is
            // replaced rather than queued behind.
            outboundSpectrumLine = line
        case .control:
            if outboundControl.count >= Self.maxPendingControlCommands {
                outboundControl.removeFirst()
                overflowed = true
            }
            outboundControl.append(line)
        }
        if !writerResumed, let source = writeSource {
            writerResumed = true
            source.resume()
        }
        outboundLock.unlock()
        if overflowed {
            NSLog("[Mirage] 控制指令队列已满，丢弃最旧指令 (显示器=\(displayID))")
        }
    }

    private func setUpWriter() {
        let fd = stdinPipe.fileHandleForWriting.fileDescriptor
        guard fd >= 0 else { return }
        // Non-blocking from here on, so no caller of send() can ever be parked
        // inside write() by a renderer that stopped reading its stdin.
        let flags = fcntl(fd, F_GETFL, 0)
        if flags >= 0 {
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        }
        let source = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: ioQueue)
        source.setEventHandler { [weak self] in
            self?.drainOutbound()
        }
        // The source references the descriptor for as long as it is alive, so
        // the pipe is closed only once cancellation has completed — never from
        // under a live event handler.
        source.setCancelHandler { [pipe = stdinPipe] in
            try? pipe.fileHandleForWriting.close()
        }
        writeSource = source
    }

    /// Runs on `ioQueue` whenever the pipe has room. The descriptor is
    /// O_NONBLOCK, so a partial write simply leaves the remainder queued for
    /// the next writable event instead of parking the thread.
    private func drainOutbound() {
        let fd = stdinPipe.fileHandleForWriting.fileDescriptor
        while true {
            outboundLock.lock()
            if outboundClosed {
                outboundLock.unlock()
                return
            }
            if outboundChunk == nil {
                // Control commands always win over telemetry.
                if !outboundControl.isEmpty {
                    outboundChunk = outboundControl.removeFirst()
                } else if let spectrum = outboundSpectrumLine {
                    outboundSpectrumLine = nil
                    outboundChunk = spectrum
                }
                outboundOffset = 0
            }
            guard let chunk = outboundChunk else {
                let shouldClose = closeWhenDrained
                if !shouldClose, writerResumed {
                    // Park the source: a write source fires continuously while
                    // its descriptor is writable. Suspending under the same
                    // lock that guards `writerResumed` keeps suspend/resume
                    // balanced against a concurrent enqueue.
                    writerResumed = false
                    writeSource?.suspend()
                }
                outboundLock.unlock()
                if shouldClose { closeWriter() }
                return
            }
            let offset = outboundOffset
            outboundLock.unlock()

            var result = -1
            var failure: Int32 = 0
            chunk.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                guard let base = raw.baseAddress else {
                    result = 0
                    return
                }
                result = Darwin.write(fd, base + offset, chunk.count - offset)
                if result < 0 { failure = errno }
            }

            if result < 0 {
                if failure == EINTR { continue }
                if failure == EAGAIN || failure == EWOULDBLOCK {
                    // Not writable right now. Leave the source resumed; it
                    // fires again as soon as the renderer drains its stdin.
                    return
                }
                if failure != EPIPE {
                    NSLog("[Mirage] 控制管道写入失败 (显示器=\(displayID)): errno=\(failure)")
                }
                // EPIPE / EBADF: the renderer is gone. Drop the queue instead
                // of spinning on a dead descriptor.
                closeWriter()
                return
            }
            guard result > 0 else {
                // A pipe never accepts zero bytes of a non-empty write. Bail
                // out rather than spin: the source is level-triggered and
                // would fire again immediately.
                NSLog("[Mirage] 控制管道写入 0 字节，关闭通道 (显示器=\(displayID))")
                closeWriter()
                return
            }

            outboundLock.lock()
            outboundOffset += result
            if let pending = outboundChunk, outboundOffset >= pending.count {
                outboundChunk = nil
                outboundOffset = 0
            }
            outboundLock.unlock()
        }
    }

    /// Requests EOF on the renderer's stdin, but only once everything already
    /// queued (notably the graceful `quit`) has actually reached the pipe.
    private func closeWriterWhenDrained() {
        outboundLock.lock()
        if outboundClosed {
            outboundLock.unlock()
            return
        }
        closeWhenDrained = true
        outboundSpectrumLine = nil
        let hasPending = outboundChunk != nil || !outboundControl.isEmpty
        if let source = writeSource, hasPending {
            if !writerResumed {
                writerResumed = true
                source.resume()
            }
            outboundLock.unlock()
            return
        }
        outboundLock.unlock()
        closeWriter()
    }

    /// Tears the writer down exactly once. A suspended dispatch source can be
    /// neither released nor cancelled, so it is always resumed one last time
    /// before cancellation — that keeps suspend/resume balanced.
    private func closeWriter() {
        outboundLock.lock()
        if outboundClosed {
            outboundLock.unlock()
            return
        }
        outboundClosed = true
        outboundControl.removeAll()
        outboundSpectrumLine = nil
        outboundChunk = nil
        outboundOffset = 0
        let source = writeSource
        writeSource = nil
        if let source, !writerResumed {
            writerResumed = true
            source.resume()
        }
        outboundLock.unlock()
        if let source {
            source.cancel()
        } else {
            try? stdinPipe.fileHandleForWriting.close()
        }
    }

    func stop(alreadyDeactivated: Bool = false) {
        stateLock.lock()
        guard !terminated else {
            stateLock.unlock()
            return
        }
        terminated = true
        stateLock.unlock()
        spectrumStateLock.lock()
        pendingSpectrum = nil
        spectrumStateLock.unlock()

        if !alreadyDeactivated {
            enqueue(["cmd": "deactivate"], allowAfterStop: true)
        }
        // The graceful quit must be written after marking the handle stopped,
        // so no later live-control command can race it. Use the private bypass
        // instead of send(), whose normal guard intentionally rejects it.
        enqueue(["cmd": "quit"], allowAfterStop: true)
        // stdin can no longer be closed right here: the quit line may still be
        // sitting in the outbound queue. Close it once the queue drains, so
        // the renderer still sees EOF.
        closeWriterWhenDrained()
        let proc = process
        // Best-effort escalation for runtime switches. It is deliberately
        // asynchronous here; app termination uses the synchronous variant on
        // RendererController because deferred work never runs once
        // applicationWillTerminate returns.
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.5) {
            if proc.isRunning { proc.terminate() }
            DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
                if proc.isRunning { kill(proc.processIdentifier, SIGKILL) }
            }
        }
    }

    /// SIGTERM. Safe to call on an already dead renderer.
    func forceTerminate() {
        guard process.isRunning else { return }
        process.terminate()
    }

    func forceKill() {
        guard process.isRunning else { return }
        let pid = process.processIdentifier
        guard pid > 0 else { return }
        kill(pid, SIGKILL)
        NSLog("[Mirage] 渲染器未响应退出请求，已强制结束 (显示器=\(displayID))")
    }

    func startReadingOutput(onEvent: @escaping ([String: Any]) -> Void) {
        stdoutPipe?.fileHandleForReading.readabilityHandler = { [weak self] file in
            let data = file.availableData
            if data.isEmpty {
                file.readabilityHandler = nil
                return
            }
            self?.consumeStdout(data, onEvent: onEvent)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] file in
            let data = file.availableData
            if data.isEmpty {
                file.readabilityHandler = nil
                return
            }
            if let text = String(data: data, encoding: .utf8), !text.isEmpty {
                guard let self else { return }
                MirageLogService.shared.append(text, source: "renderer.\(self.wallpaper.kind.rawValue).\(self.displayID)")
            }
        }
    }

    private func consumeStdout(_ data: Data, onEvent: ([String: Any]) -> Void) {
        stateLock.lock()
        stdoutBuffer.append(data)
        var lines: [Data] = []
        while let newline = stdoutBuffer.firstIndex(of: 0x0A) {
            lines.append(Data(stdoutBuffer[..<newline]))
            stdoutBuffer.removeSubrange(...newline)
        }
        stateLock.unlock()

        for line in lines where !line.isEmpty {
            guard let object = try? JSONSerialization.jsonObject(with: line),
                  let event = object as? [String: Any] else { continue }
            onEvent(event)
        }
    }

    func cleanupAfterExit() {
        cleanupLock.lock()
        guard !cleanupComplete else {
            cleanupLock.unlock()
            return
        }
        cleanupComplete = true
        let files = tempFiles
        tempFiles.removeAll()
        cleanupLock.unlock()

        closeWriter()
        stdoutPipe?.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        for url in files {
            try? FileManager.default.removeItem(at: url)
        }
    }
}

struct RenderOptions: Equatable {
    var fps: Int = 30
    var volume: Float = 1.0
    var muted: Bool = false
    var speed: Float = 1.0
    var fillMode: FillMode = .cover
    var enableSpectrum: Bool = true
    var renderScale: Double = 1.0
    var enableMetalFX: Bool = false
    var msaaSamples: Int = 1
    var loadFromMemory: Bool = false
    var enableHDRVideo: Bool = false
    var userProperties: [String: WEProjectProperty] = [:]
    var powerState: MiragePowerState = .run
    var powerFps: Int?
    /// Identifies the UI assignment that owns these playback options. A newer
    /// proposal can reuse a process, so ownership is intentionally mutable on
    /// `RendererProcess` even though the process generation itself is fixed.
    var assignmentID: UUID?
}

// Subprocess control: the renderer receives JSON-line commands via stdin.
final class RendererController {
    private enum TransitionPhase: String {
        case preparing
        case waitingForVisibilityBlockers
        case deactivatingActive
        case activatingCandidate
        case hidingCandidateForRollback
        case terminatingCandidate
        case restoringStandby
        case hidingStandbyAfterRestoreFailure
        case terminatingStandby
    }

    private final class ReplacementTransition {
        let candidate: RendererProcess
        let generation: UInt64
        var phase: TransitionPhase = .preparing
        var prepared = false
        var candidateKnownHidden = true
        var standby: RendererProcess?
        var preparationEpoch: UInt64 = 0

        init(candidate: RendererProcess, generation: UInt64) {
            self.candidate = candidate
            self.generation = generation
        }
    }

    private final class RenderRequest {
        let wallpaper: WEWallpaper
        let displayID: CGDirectDisplayID
        var options: RenderOptions
        let reuseActive: Bool
        let binary: URL
        let requestToken: UUID
        var completions: [(Bool) -> Void]

        init(wallpaper: WEWallpaper, displayID: CGDirectDisplayID,
             options: RenderOptions, reuseActive: Bool, binary: URL,
             requestToken: UUID, completion: ((Bool) -> Void)?) {
            self.wallpaper = wallpaper
            self.displayID = displayID
            self.options = options
            self.reuseActive = reuseActive
            self.binary = binary
            self.requestToken = requestToken
            self.completions = completion.map { [$0] } ?? []
        }
    }

    private struct LaunchingRequest {
        let requestToken: UUID
        /// A request token survives pending handoff, while a lease identifies
        /// exactly one invocation of `renderRequest`. This prevents the defer of
        /// the queuing invocation from clearing the later pending launch.
        let leaseID: UUID
        var options: RenderOptions
    }

    private var running: [CGDirectDisplayID: RendererProcess] = [:]
    private var candidates: [CGDirectDisplayID: RendererProcess] = [:]
    private var transitions: [CGDirectDisplayID: ReplacementTransition] = [:]
    private var pendingRequests: [CGDirectDisplayID: RenderRequest] = [:]
    private var retiring: [ObjectIdentifier: RendererProcess] = [:]
    /// Processes removed by an explicit stop whose desktop window has not yet
    /// been confirmed hidden. A replacement may prepare behind these handles,
    /// but it cannot activate until each blocker emits a hidden lifecycle event
    /// or its process exits.
    private var visibilityBlockers: [ObjectIdentifier: RendererProcess] = [:]
    private var launchingRequests: [CGDirectDisplayID: LaunchingRequest] = [:]
    /// Active/standby processes can become non-running before Foundation queues
    /// their termination handler onto `queue`. Keep their former visible role
    /// recognizable until that handler supplies the actual exit status.
    private var awaitingVisibleExits: [ObjectIdentifier: RendererProcess] = [:]
    private var coverageSettlementWaiters:
        [CGDirectDisplayID: [UUID: () -> Void]] = [:]
    private var latestRequestTokens: [CGDirectDisplayID: UUID] = [:]
    private var generations: [CGDirectDisplayID: UInt64] = [:]
    private struct DeferredActiveExit {
        let status: Int32
        let wasTerminated: Bool
    }
    private var deferredActiveExits: [CGDirectDisplayID: DeferredActiveExit] = [:]
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.renderer")
    // Scene startup has two internal 30-second guards. Web is bounded as well,
    // while video gets a longer, activity-reset deadline for format conversion.
    private static let preparationTimeout: TimeInterval = 75
    private static let videoPreparationTimeout: TimeInterval = 300
    private static let deactivationTimeout: TimeInterval = 3
    private static let activationTimeout: TimeInterval = 8
    private static let hideTimeout: TimeInterval = 3
    private static let restoreTimeout: TimeInterval = 8

    var onProcessExit: ((Int, Bool) -> Void)?

    /// A video wallpaper reported that it cannot be played at all. Carries the
    /// renderer's own message; the wallpaper stays up showing black until it is
    /// replaced, so this is the only chance to tell the user why.
    var onVideoError: ((Int, WEWallpaper, String) -> Void)?

    /// Progress of rewriting a video AVFoundation cannot decode into H.264.
    /// `done` marks the final call, after which playback starts or `onVideoError`
    /// fires.
    var onVideoTranscodeProgress: ((Int, WEWallpaper, Double, Bool) -> Void)?

    /// Safety backstop for web wallpapers. The user-facing confirmation lives
    /// in the UI layer, but too many call sites assign the current wallpaper
    /// directly (playlist rotation, per-screen apply, launch restore) to rely
    /// on it. This closure is consulted at the one place a web renderer is
    /// actually spawned. It is injected rather than resolved through the view
    /// model so the controller keeps no reference back to its owner.
    var isWallpaperTrusted: ((WEWallpaper) -> Bool)?

    /// In-flight `snapshot` requests, keyed by the token echoed back in the
    /// renderer's `snapshot-done` event.
    private var snapshotPending: [String: (Bool) -> Void] = [:]
    /// A renderer that never answers must not leak its completion handler, and
    /// the desktop-override caller must not wait forever for its still.
    private static let snapshotTimeout: TimeInterval = 8

    init() {
        SystemAudioSpectrumService.shared.onSpectrum = { [weak self] spectrum in
            self?.setAudioSpectrum(spectrum)
        }
    }

    func displayID(for screenIndex: Int) -> CGDirectDisplayID? {
        guard NSScreen.screens.indices.contains(screenIndex),
              let number = NSScreen.screens[screenIndex].deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return nil }
        return number.uint32Value
    }

    func screenIndex(for displayID: CGDirectDisplayID) -> Int? {
        NSScreen.screens.firstIndex {
            ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?
                .uint32Value == displayID
        }
    }

    /// Asks the renderer on `screenIndex` to write a still of its own current
    /// frame to `path`. The renderer owns the pixels — no screen-recording
    /// permission is involved. `completion` runs on the main queue exactly once.
    func snapshot(on screenIndex: Int, path: String,
                  completion: @escaping (Bool) -> Void) {
        guard let displayID = displayID(for: screenIndex) else {
            completion(false)
            return
        }
        snapshot(onDisplay: displayID, path: path, completion: completion)
    }

    func snapshot(onDisplay displayID: CGDirectDisplayID, path: String,
                  completion: @escaping (Bool) -> Void) {
        let token = UUID().uuidString
        queue.async { [weak self] in
            guard let self else {
                DispatchQueue.main.async { completion(false) }
                return
            }
            guard let handle = self.running[displayID] else {
                DispatchQueue.main.async { completion(false) }
                return
            }
            self.snapshotPending[token] = completion
            handle.send(["cmd": "snapshot", "path": path, "token": token])
            self.queue.asyncAfter(deadline: .now() + Self.snapshotTimeout) { [weak self] in
                guard let self, let pending = self.snapshotPending.removeValue(forKey: token)
                else { return }
                NSLog("[Mirage] 壁纸截图超时 (显示器=\(displayID))")
                DispatchQueue.main.async { pending(false) }
            }
        }
    }

    /// Must be called on `queue`.
    private func completeSnapshot(token: String, ok: Bool) {
        guard let pending = snapshotPending.removeValue(forKey: token) else { return }
        DispatchQueue.main.async { pending(ok) }
    }

    // MARK: Binary and resource resolution

    private var resourcesDir: URL {
        Bundle.main.resourceURL ?? Bundle.main.bundleURL
    }

    private var renderersDir: URL {
        resourcesDir.appending(path: "Renderers")
    }

    // MARK: Architecture and build preset mapping

    /// CPU architecture of the host: `"arm64"` or `"x86_64"`.
    /// Used at development time to locate the correct CMake build output directory.
    private static var hostArch: String {
        var info = utsname()
        guard uname(&info) == 0 else { return "x86_64" }
        let machine = withUnsafePointer(to: &info.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(_SYS_NAMELEN)) { String(cString: $0) }
        }
        return machine
    }

    /// CMake preset naming convention:
    ///   Intel:    `macos-clang-{config}` (no arch suffix)
    ///   Silicon:  `macos-arm64-clang-{config}`
    private static func sceneRendererPreset(config: String = "release") -> String {
        hostArch == "arm64"
            ? "macos-arm64-clang-\(config)"
            : "macos-clang-\(config)"
    }

    /// Homebrew prefix paths, ordered by current architecture preference.
    private static var brewPrefixes: [URL] {
        hostArch == "arm64"
            ? [URL(fileURLWithPath: "/opt/homebrew"), URL(fileURLWithPath: "/usr/local")]
            : [URL(fileURLWithPath: "/usr/local"), URL(fileURLWithPath: "/opt/homebrew")]
    }

    // Derive the project root from this source file's compile-time path.
    // Used as a dev fallback to locate build artifacts without hardcoding
    // an absolute directory.
    private static let projectRoot: URL = {
        // #filePath is a compile-time constant pointing to this file:
        //   Mirage/Mirage Wallpaper/Services/RendererController.swift
        let srcURL = URL(fileURLWithPath: #filePath)
        // Walk up: .../Services → .../Mirage Wallpaper → .../Mirage → repo root
        return srcURL
            .deletingLastPathComponent() // RendererController.swift → Services
            .deletingLastPathComponent() // Services → Mirage Wallpaper
            .deletingLastPathComponent() // Mirage Wallpaper → Mirage
            .deletingLastPathComponent() // Mirage → repo root
    }()

    private static let devFallback: [WallpaperKind: URL] = {
        let preset = sceneRendererPreset()
        return [
            .scene: projectRoot.appending(path: "SceneRenderer/build/\(preset)/Tools/SceneWallpaper/SceneWallpaper"),
            .web:   projectRoot.appending(path: "WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper"),
            .video: projectRoot.appending(path: "VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper"),
        ]
    }()

    private func binaryURL(for kind: WallpaperKind) -> URL? {
        let name: String
        switch kind {
        case .scene: name = "SceneWallpaper"
        case .web: name = "WebWallpaper"
        case .video: name = "VideoWallpaper"
        case .unsupported: return nil
        }
        let bundled = renderersDir.appending(path: name)
        if FileManager.default.fileExists(atPath: bundled.path) {
            return bundled
        }
        return Self.devFallback[kind]
    }

    private var sceneAssetsDir: URL {
        let bundled = resourcesDir.appending(path: "assets")
        if FileManager.default.fileExists(atPath: bundled.path) { return bundled }
        return Self.projectRoot.appending(path: "assets")
    }

    private var moltenVKICD: URL? {
        let bundled = renderersDir.appending(path: "vulkan/icd.d/MoltenVK_icd.json")
        if FileManager.default.fileExists(atPath: bundled.path) { return bundled }
        let icdSuffix = "etc/vulkan/icd.d/MoltenVK_icd.json"
        for prefix in Self.brewPrefixes {
            let path = prefix.appending(path: icdSuffix)
            if FileManager.default.fileExists(atPath: path.path) { return path }
        }
        return nil
    }

    // MARK: Launch / switch / stop

    @discardableResult
    func render(_ wallpaper: WEWallpaper, on screenIndex: Int = 0, options: RenderOptions) -> Bool {
        guard let displayID = displayID(for: screenIndex) else { return false }
        return render(wallpaper, onDisplay: displayID, options: options)
    }

    @discardableResult
    func render(_ wallpaper: WEWallpaper, onDisplay displayID: CGDirectDisplayID,
                options: RenderOptions, reuseActive: Bool = false,
                completion: ((Bool) -> Void)? = nil) -> Bool {
        guard wallpaper.isValid, wallpaper.kind != .unsupported else {
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        if wallpaper.kind == .web, let isTrusted = isWallpaperTrusted, !isTrusted(wallpaper) {
            NSLog("[Mirage] 已阻止未确认的网页壁纸启动 (显示器=\(displayID)): \(wallpaper.id)")
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        guard binaryURL(for: wallpaper.kind) != nil else {
            NSLog("[Mirage] 找不到 \(wallpaper.kind) 渲染器二进制")
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }

        let requestToken = UUID()
        let leaseID = UUID()
        queue.sync {
            latestRequestTokens[displayID] = requestToken
            launchingRequests[displayID] = LaunchingRequest(
                requestToken: requestToken, leaseID: leaseID, options: options)
        }
        return renderRequest(wallpaper, onDisplay: displayID, options: options,
                             reuseActive: reuseActive, requestToken: requestToken,
                             leaseID: leaseID,
                             completion: completion)
    }

    private func renderRequest(_ wallpaper: WEWallpaper,
                               onDisplay displayID: CGDirectDisplayID,
                               options: RenderOptions, reuseActive: Bool,
                               requestToken: UUID,
                               leaseID: UUID,
                               completion: ((Bool) -> Void)?) -> Bool {
        defer {
            queue.sync {
                if launchingRequests[displayID]?.requestToken == requestToken,
                   launchingRequests[displayID]?.leaseID == leaseID {
                    launchingRequests[displayID] = nil
                }
                reportDeferredActiveExitIfUncoveredLocked(displayID)
            }
        }
        guard wallpaper.isValid, wallpaper.kind != .unsupported else {
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        // Web wallpapers execute untrusted third-party HTML/JS. Never spawn one
        // the user has not explicitly confirmed, whatever code path led here.
        if wallpaper.kind == .web, let isTrusted = isWallpaperTrusted, !isTrusted(wallpaper) {
            NSLog("[Mirage] 已阻止未确认的网页壁纸启动 (显示器=\(displayID)): \(wallpaper.id)")
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        guard let binary = binaryURL(for: wallpaper.kind) else {
            NSLog("[Mirage] 找不到 \(wallpaper.kind) 渲染器二进制")
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        guard queue.sync(execute: {
            latestRequestTokens[displayID] == requestToken &&
                launchingRequests[displayID]?.requestToken == requestToken &&
                launchingRequests[displayID]?.leaseID == leaseID
        }) else {
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }

        var reusedCandidate = false
        var reusedActive = false
        var queuedBehindHandoff = false
        var canceledCandidate: RendererProcess?
        var canceledCompletions: [(Bool) -> Void] = []
        var displacedPendingCompletions: [(Bool) -> Void] = []
        queue.sync {
            guard latestRequestTokens[displayID] == requestToken,
                  launchingRequests[displayID]?.requestToken == requestToken,
                  launchingRequests[displayID]?.leaseID == leaseID else { return }
            if let transition = transitions[displayID],
               let candidate = candidates[displayID],
               transition.candidate === candidate {
                if candidate.process.isRunning,
                   candidate.wallpaper.id == wallpaper.id,
                   canReuseCandidateLocked(candidate, with: options),
                   transition.phase == .preparing ||
                    transition.phase == .waitingForVisibilityBlockers ||
                    transition.phase == .deactivatingActive ||
                    transition.phase == .activatingCandidate {
                    updateDesiredStateLocked(candidate, from: options)
                    if let completion { candidate.transitionCompletions.append(completion) }
                    reusedCandidate = true
                    return
                }

                if transition.phase != .preparing &&
                    transition.phase != .waitingForVisibilityBlockers {
                    if let pending = pendingRequests.removeValue(forKey: displayID) {
                        displacedPendingCompletions = pending.completions
                    }
                    pendingRequests[displayID] = RenderRequest(
                        wallpaper: wallpaper, displayID: displayID, options: options,
                        reuseActive: reuseActive, binary: binary,
                        requestToken: requestToken, completion: completion)
                    queuedBehindHandoff = true
                    return
                }

                // A preparing or blocker-waiting candidate has never received
                // activate and is therefore known hidden. Replace it immediately.
                candidates[displayID] = nil
                transitions[displayID] = nil
                canceledCandidate = candidate
                canceledCompletions = takeTransitionCompletionsLocked(from: candidate)
                if candidate.process.isRunning {
                    retiring[ObjectIdentifier(candidate)] = candidate
                }
            }

            if reuseActive, let active = running[displayID], active.process.isRunning,
               active.wallpaper.id == wallpaper.id {
                updateDesiredStateLocked(active, from: options)
                reusedActive = true
            }
        }
        if queuedBehindHandoff {
            dispatchTransitionCompletions(canceledCompletions, success: false)
            dispatchTransitionCompletions(displacedPendingCompletions, success: false)
            return true
        }
        if reusedCandidate { return true }
        if reusedActive {
            canceledCandidate?.stop(alreadyDeactivated: true)
            dispatchTransitionCompletions(canceledCompletions, success: false)
            if let completion {
                DispatchQueue.main.async { completion(true) }
            }
            return true
        }
        canceledCandidate?.stop(alreadyDeactivated: true)
        dispatchTransitionCompletions(canceledCompletions, success: false)

        // A newer public request may have arrived while the old hidden
        // candidate was being retired. Do not let this older request overtake it.
        guard queue.sync(execute: {
            latestRequestTokens[displayID] == requestToken &&
                launchingRequests[displayID]?.requestToken == requestToken &&
                launchingRequests[displayID]?.leaseID == leaseID
        }) else {
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        NSLog("[Mirage] 启动渲染器: \(binary.path) 显示器=\(displayID)")

        var supersededCandidate: RendererProcess?
        var supersededCompletions: [(Bool) -> Void] = []
        let reservedGeneration: UInt64? = queue.sync {
            guard latestRequestTokens[displayID] == requestToken,
                  launchingRequests[displayID]?.requestToken == requestToken,
                  launchingRequests[displayID]?.leaseID == leaseID else { return nil }
            let next = (generations[displayID] ?? 0) &+ 1
            generations[displayID] = next
            supersededCandidate = candidates.removeValue(forKey: displayID)
            transitions[displayID] = nil
            if let supersededCandidate {
                supersededCompletions = takeTransitionCompletionsLocked(
                    from: supersededCandidate)
                if supersededCandidate.process.isRunning {
                    retiring[ObjectIdentifier(supersededCandidate)] = supersededCandidate
                }
            }
            return next
        }
        guard let generation = reservedGeneration else {
            dispatchTransitionCompletions(completion.map { [$0] } ?? [], success: false)
            return false
        }
        supersededCandidate?.stop(alreadyDeactivated: true)
        dispatchTransitionCompletions(supersededCompletions, success: false)

        let proc = Process()
        proc.executableURL = binary

        var args: [String] = []
        var env = ProcessInfo.processInfo.environment
        if AppDelegate.shared.globalSettingsViewModel.settings.isDeveloperModeEnabled {
            env["WR_DEBUG"] = "1"
        }

        switch wallpaper.kind {
        case .scene:
            let assetsPath = sceneAssetsDir.path
            let pkgPath = wallpaper.resolvedEntryURL.path
            NSLog("[Mirage] 场景参数: assets=\(assetsPath) exists=\(FileManager.default.fileExists(atPath: assetsPath)) pkg=\(pkgPath) exists=\(FileManager.default.fileExists(atPath: pkgPath))")
            args += [assetsPath, pkgPath]
            args += ["--fps", String(options.fps)]
            args += ["--render-scale", String(format: "%.3f", options.renderScale)]
            if options.enableMetalFX { args += ["--metalfx"] }
            args += ["--msaa", String(options.msaaSamples)]
            if options.loadFromMemory { args += ["--load-from-memory"] }
            args += ["--display-id", String(displayID)]
            // Candidates stay transparent and silent until Metal confirms a
            // presented frame and Mirage explicitly activates them.
            args += ["--control-stdin", "--deferred-show", "--muted"]
            if options.enableSpectrum {
                args += ["--external-spectrum"]
            } else {
                args += ["--no-spectrum"]
            }
            if let icd = moltenVKICD {
                env["VK_ICD_FILENAMES"] = icd.path
                env["VK_DRIVER_FILES"] = icd.path
            }
            let fw = Bundle.main.bundleURL.appending(path: "Contents/Frameworks")
            if FileManager.default.fileExists(atPath: fw.path) {
                let existing = env["DYLD_FALLBACK_LIBRARY_PATH"]
                env["DYLD_FALLBACK_LIBRARY_PATH"] = existing.map { "\(fw.path):\($0)" } ?? fw.path
            }

        case .web:
            args += [wallpaper.renderDirectory.path]
            for overlay in wallpaper.assetOverlayDirectories {
                args += ["--asset-overlay", overlay.path]
            }
            args += ["--fps", String(options.fps)]
            if options.loadFromMemory { args += ["--load-from-memory"] }
            args += ["--volume", String(format: "%.3f", options.volume)]
            args += ["--display-id", String(displayID)]
            if options.enableSpectrum {
                args += ["--external-spectrum"]
            } else {
                args += ["--no-spectrum"]
            }
            args += ["--control-stdin", "--deferred-show"]

        case .video:
            args += [wallpaper.renderDirectory.path]
            args += ["--display-id", String(displayID)]
            args += ["--volume", String(format: "%.3f", options.volume)]
            args += ["--fill", options.fillMode.rawValue]
            if options.enableHDRVideo { args += ["--hdr"] }
            if options.loadFromMemory { args += ["--load-from-memory"] }
            // The candidate must decode its first frame, but it cannot become
            // visible or audible before the parent completes the handoff.
            args += ["--control-stdin", "--deferred-show", "--muted"]

        case .unsupported:
            return false
        }

        let stdinPipe = Pipe()
        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        proc.standardInput = stdinPipe
        proc.standardOutput = stdoutPipe
        proc.standardError = stderrPipe

        let handle = RendererProcess(
            process: proc,
            stdinPipe: stdinPipe,
            stdoutPipe: stdoutPipe,
            stderrPipe: stderrPipe,
            wallpaper: wallpaper,
            displayID: displayID,
            generation: generation,
            options: options,
            spectrumEnabled: options.enableSpectrum &&
                (wallpaper.kind == .scene || wallpaper.kind == .web))
        if let completion { handle.transitionCompletions.append(completion) }

        if wallpaper.kind == .scene,
           let propsFile = writeUserPropertiesFile(options.userProperties, for: wallpaper) {
            args += ["--user-properties", propsFile.path]
            handle.tempFiles.append(propsFile)
            handle.usesLaunchUserProperties = true
        }

        proc.arguments = args
        proc.environment = env

        proc.terminationHandler = { [weak self] p in
            // Process retains its termination handler. Drop that edge as soon
            // as it fires so the closure's RendererProcess capture cannot
            // keep a completed renderer alive indefinitely.
            p.terminationHandler = nil
            let status = p.terminationStatus
            let reason = p.terminationReason
            handle.cleanupAfterExit()
            // Decode the signal from the exit status: the low 7 bits hold the signal number.
            let signalNum = status & 0x7F
            let coreDumped = (status & 0x80) != 0
            if reason == .uncaughtSignal {
                let sigName: String
                switch signalNum {
                case 4:  sigName = "SIGILL(4)"
                case 6:  sigName = "SIGABRT(6)"
                case 8:  sigName = "SIGFPE(8)"
                case 10: sigName = "SIGBUS(10)"
                case 11: sigName = "SIGSEGV(11)"
                case 13: sigName = "SIGPIPE(13)"
                default: sigName = "SIGNAL(\(signalNum))"
                }
                NSLog("[Mirage] 渲染器信号退出: \(sigName) core=\(coreDumped) 显示器=\(displayID)")
            }
            NSLog("[Mirage] 渲染器退出 (显示器=\(displayID), 状态=\(status), 原因=\(reason.rawValue))")
            guard let self else { return }
            self.queue.async {
                self.handleProcessExitLocked(handle, status: status)
            }
        }

        var launchError: Error?
        let accepted: Bool = queue.sync {
            // Launch and role registration are one serialized operation. Stop,
            // display-disconnect and app-shutdown calls therefore either cancel
            // this generation before launch or collect the registered process;
            // there is no unowned child between Process.run() and `candidates`.
            guard generations[displayID] == generation,
                  latestRequestTokens[displayID] == requestToken,
                  launchingRequests[displayID]?.requestToken == requestToken,
                  launchingRequests[displayID]?.leaseID == leaseID,
                  transitions[displayID] == nil else { return false }
            do {
                try proc.run()
            } catch {
                launchError = error
                return false
            }
            guard proc.isRunning else { return false }
            let registeredOptions = launchingRequests[displayID]?.options ?? options
            handle.launchOptions = registeredOptions
            updateDesiredStateLocked(handle, from: registeredOptions)
            // The scene's initial property snapshot is already loaded by
            // --user-properties. Only use the stdin fallback when writing that
            // launch file failed.
            if !handle.usesLaunchUserProperties {
                applyInitialProperties(registeredOptions.userProperties, to: handle)
            }
            candidates[displayID] = handle
            transitions[displayID] = ReplacementTransition(
                candidate: handle, generation: generation)
            return true
        }
        guard accepted else {
            if let launchError {
                NSLog("[Mirage] 启动渲染器失败: \(launchError)")
            }
            proc.terminationHandler = nil
            handle.cleanupAfterExit()
            let transitionCompletions = queue.sync {
                takeTransitionCompletionsLocked(from: handle)
            }
            handle.stop(alreadyDeactivated: true)
            dispatchTransitionCompletions(transitionCompletions, success: false)
            return false
        }
        refreshAudioSpectrumService()

        handle.startReadingOutput { [weak self, weak handle] event in
            guard let self, let handle else { return }
            self.handleLifecycleEvent(event, from: handle)
        }

        queue.async { [weak self, weak handle] in
            guard let self, let handle,
                  let transition = self.transitions[displayID],
                  transition.candidate === handle else { return }
            self.schedulePreparationTimeoutLocked(transition)
        }
        return true
    }

    /// Only settings that require a new renderer process participate here.
    /// Volume, pause, frame rate, speed and fill mode are replayed at activation.
    private func canReuseCandidateLocked(_ candidate: RendererProcess,
                                         with options: RenderOptions) -> Bool {
        let launched = candidate.launchOptions
        return launched.enableSpectrum == options.enableSpectrum &&
            launched.renderScale == options.renderScale &&
            launched.enableMetalFX == options.enableMetalFX &&
            launched.msaaSamples == options.msaaSamples &&
            launched.loadFromMemory == options.loadFromMemory &&
            candidate.desiredUserProperties == options.userProperties
    }

    private func updateDesiredStateLocked(_ handle: RendererProcess,
                                          from options: RenderOptions) {
        handle.assignmentID = options.assignmentID
        handle.launchOptions.assignmentID = options.assignmentID
        handle.desiredVolume = options.volume
        handle.desiredMuted = options.muted
        handle.desiredPaused = options.powerState == .pause
        handle.desiredPowerState = options.powerState
        handle.desiredPowerFps = options.powerFps
        handle.desiredFps = options.fps
        handle.desiredSpeed = options.speed
        handle.desiredFillMode = options.fillMode
        handle.desiredHDRVideo = options.enableHDRVideo
        handle.desiredUserProperties = options.userProperties
    }

    private func handleLifecycleEvent(_ message: [String: Any], from handle: RendererProcess) {
        guard let event = message["event"] as? String else { return }
        let elapsed = (message["elapsed_ms"] as? NSNumber)?.intValue
        queue.async { [weak self, weak handle] in
            guard let self, let handle else { return }
            self.handleLifecycleEventLocked(event, message: message,
                                            elapsed: elapsed, from: handle)
        }
    }

    /// Must be called on `queue`.
    private func handleLifecycleEventLocked(_ event: String,
                                            message: [String: Any],
                                            elapsed: Int?,
                                            from handle: RendererProcess) {
        let displayID = handle.displayID
        let transition = transitions[displayID]
        let isActive = running[displayID] === handle
        let isCandidate = transition?.candidate === handle &&
            candidates[displayID] === handle &&
            transition?.generation == handle.generation
        let isStandby = transition?.standby === handle
        let identity = ObjectIdentifier(handle)
        let isRetiring = retiring[identity] === handle
        let isVisibilityBlocker = visibilityBlockers[identity] === handle
        guard isActive || isCandidate || isStandby || isRetiring ||
                isVisibilityBlocker else { return }

        if isVisibilityBlocker {
            if event == "deactivated" || event == "activation-failed" {
                visibilityBlockerBecameHiddenLocked(handle)
            }
            return
        }

        if event == "snapshot-done" {
            guard isActive, let token = message["token"] as? String else { return }
            completeSnapshot(token: token,
                             ok: (message["ok"] as? NSNumber)?.boolValue ?? false)
            return
        }

        if event == "audio-demand" {
            guard !isRetiring else { return }
            handle.spectrumDemanded =
                (message["needed"] as? NSNumber)?.boolValue ?? false
            SystemAudioSpectrumService.shared.setEnabled(hasSpectrumConsumersLocked())
            return
        }

        if event == "video-transcoding" {
            guard isActive || isCandidate else { return }
            if isCandidate, let transition, transition.phase == .preparing {
                schedulePreparationTimeoutLocked(transition)
            }
            let progress = (message["progress"] as? NSNumber)?.doubleValue ?? 0
            let done = (message["done"] as? NSNumber)?.boolValue ?? false
            DispatchQueue.main.async { [weak self] in
                guard let self, let screen = self.screenIndex(for: displayID) else { return }
                self.onVideoTranscodeProgress?(screen, handle.wallpaper, progress, done)
            }
            return
        }

        if event == "video-error" {
            guard isActive || isCandidate else { return }
            let text = (message["message"] as? String) ?? "unknown error"
            NSLog("[Mirage] 视频壁纸无法播放 (显示器=\(displayID)): \(text)")
            DispatchQueue.main.async { [weak self] in
                guard let self, let screen = self.screenIndex(for: displayID) else { return }
                self.onVideoError?(screen, handle.wallpaper, text)
                NotificationCenter.default.post(
                    name: .rendererVideoDidEnd,
                    object: nil,
                    userInfo: ["screen": screen]
                )
            }
            if isCandidate, let transition {
                failCandidateLocked(transition, reason: "video-error")
            }
            return
        }

        if event == "open-shortcut" {
            guard isActive else { return }
            let target = ((message["value"] as? String) ?? "")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard !target.isEmpty else { return }
            let name = (message["name"] as? String) ?? ""
            DispatchQueue.main.async {
                Self.openUserShortcut(name: name, target: target)
            }
            return
        }

        if event == "renderer-error" {
            NSLog("[Mirage] 场景渲染器发生不可恢复错误 (显示器=\(displayID))")
            if isCandidate, let transition {
                failCandidateLocked(transition, reason: "renderer-error")
            }
            return
        }

        if event == "video-did-end" {
            guard isActive else { return }
            DispatchQueue.main.async { [weak self] in
                guard let self, let screen = self.screenIndex(for: displayID) else { return }
                NotificationCenter.default.post(
                    name: .rendererVideoDidEnd,
                    object: nil,
                    userInfo: ["screen": screen]
                )
            }
            return
        }

        if isCandidate, let transition {
            switch event {
            case "vulkan-ready", "scene-ready":
                if let elapsed {
                    NSLog("[Mirage] 场景启动阶段 \(event): \(elapsed)ms 显示器=\(displayID)")
                }
            case "first-frame-presented", "prepared":
                if let elapsed {
                    NSLog("[Mirage] 候选内容已准备: \(elapsed)ms 显示器=\(displayID)")
                }
                candidatePreparedLocked(transition)
            case "activated":
                candidateActivatedLocked(transition)
            case "activation-failed":
                beginCandidateRollbackLocked(transition, reason: "activation-failed")
            case "deactivated":
                candidateDeactivatedLocked(transition)
            default:
                break
            }
            return
        }

        if isActive, event == "deactivated", let transition {
            activeDeactivatedLocked(handle, transition: transition)
            return
        }

        if isStandby, let transition {
            switch event {
            case "activated":
                standbyActivatedLocked(handle, transition: transition)
            case "activation-failed":
                beginHideStandbyLocked(transition, reason: "activation-failed")
            case "deactivated":
                standbyDeactivatedLocked(handle, transition: transition)
            default:
                break
            }
        }
    }

    private func candidatePreparedLocked(_ transition: ReplacementTransition) {
        let displayID = transition.candidate.displayID
        guard transitions[displayID] === transition,
              candidates[displayID] === transition.candidate,
              transition.phase == .preparing,
              transition.candidate.process.isRunning else { return }
        transition.prepared = true

        if hasVisibilityBlockersLocked(for: displayID) {
            transition.phase = .waitingForVisibilityBlockers
            return
        }

        if let active = running[displayID] {
            if active.process.isRunning {
                transition.phase = .deactivatingActive
                active.send(["cmd": "deactivate"])
                scheduleDeactivationTimeoutLocked(transition, active: active)
                return
            }
            // `Process.isRunning` becomes false before Foundation necessarily
            // delivers terminationHandler. Preserve the former active role so a
            // failed candidate cannot make that exit disappear.
            running[displayID] = nil
            awaitVisibleExitLocked(active)
        }
        beginCandidateActivationLocked(transition)
    }

    private func activeDeactivatedLocked(_ active: RendererProcess,
                                         transition: ReplacementTransition) {
        let displayID = active.displayID
        guard transitions[displayID] === transition,
              running[displayID] === active,
              transition.phase == .deactivatingActive else { return }
        running[displayID] = nil
        transition.standby = active
        beginCandidateActivationLocked(transition)
    }

    private func beginCandidateActivationLocked(_ transition: ReplacementTransition) {
        let candidate = transition.candidate
        let displayID = candidate.displayID
        guard transitions[displayID] === transition,
              candidates[displayID] === candidate else { return }
        guard !hasVisibilityBlockersLocked(for: displayID) else {
            transition.phase = .waitingForVisibilityBlockers
            return
        }
        guard candidate.process.isRunning else {
            candidateExitedLocked(transition)
            return
        }
        transition.phase = .activatingCandidate
        transition.candidateKnownHidden = false
        candidate.send(["cmd": "fps", "value": candidate.desiredFps])
        candidate.send(["cmd": "fillmode", "value": candidate.desiredFillMode.rawValue])
        candidate.send(["cmd": "speed", "value": candidate.desiredSpeed])
        if candidate.wallpaper.kind == .video {
            candidate.send(["cmd": "hdr", "value": candidate.desiredHDRVideo])
        }
        if candidate.wallpaper.kind == .web {
            candidate.send(Self.webPlaybackStateCommand(for: candidate))
        }
        candidate.send(["cmd": "activate"])
        scheduleCandidateActivationTimeoutLocked(transition)
    }

    private func candidateActivatedLocked(_ transition: ReplacementTransition) {
        let candidate = transition.candidate
        let displayID = candidate.displayID
        guard transitions[displayID] === transition,
              candidates[displayID] === candidate,
              transition.phase == .activatingCandidate else { return }
        guard candidate.process.isRunning else {
            candidateExitedLocked(transition)
            return
        }

        let standby = transition.standby
        transition.standby = nil
        transitions[displayID] = nil
        candidates[displayID] = nil
        running[displayID] = candidate
        clearAwaitingVisibleExitsLocked(for: displayID)
        deferredActiveExits[displayID] = nil
        drainCoverageSettlementWaitersIfNeededLocked(displayID)
        if let standby {
            retireLocked(standby, alreadyDeactivated: true)
        }
        replayDesiredStateLocked(to: candidate)
        let completions = takeTransitionCompletionsLocked(from: candidate)
        SystemAudioSpectrumService.shared.setEnabled(hasSpectrumConsumersLocked())
        dispatchTransitionCompletions(completions, success: true)
        NSLog("[Mirage] 壁纸切换完成 (显示器=\(displayID), generation=\(candidate.generation))")
        launchPendingRequestLocked(for: displayID)
    }

    private func failCandidateLocked(_ transition: ReplacementTransition,
                                     reason: String) {
        let displayID = transition.candidate.displayID
        guard transitions[displayID] === transition else { return }
        switch transition.phase {
        case .preparing, .waitingForVisibilityBlockers:
            transition.candidateKnownHidden = true
            transition.candidate.stop(alreadyDeactivated: true)
            finishTransitionFailureLocked(transition, reason: reason)
        case .deactivatingActive:
            transition.candidateKnownHidden = true
            transition.candidate.stop(alreadyDeactivated: true)
            if let active = running.removeValue(forKey: displayID) {
                transition.standby = active
            }
            beginStandbyRestoreLocked(transition, reason: reason)
        case .activatingCandidate:
            beginCandidateRollbackLocked(transition, reason: reason)
        case .hidingCandidateForRollback, .terminatingCandidate,
             .restoringStandby, .hidingStandbyAfterRestoreFailure,
             .terminatingStandby:
            break
        }
    }

    private func beginCandidateRollbackLocked(_ transition: ReplacementTransition,
                                              reason: String) {
        let candidate = transition.candidate
        let displayID = candidate.displayID
        guard transitions[displayID] === transition,
              transition.phase == .activatingCandidate else { return }
        guard candidate.process.isRunning else {
            transition.candidateKnownHidden = true
            beginStandbyRestoreLocked(transition, reason: reason)
            return
        }
        transition.phase = .hidingCandidateForRollback
        candidate.send(["cmd": "deactivate"])
        scheduleCandidateHideTimeoutLocked(transition)
    }

    private func candidateDeactivatedLocked(_ transition: ReplacementTransition) {
        let candidate = transition.candidate
        let displayID = candidate.displayID
        guard transitions[displayID] === transition,
              transition.phase == .hidingCandidateForRollback ||
                transition.phase == .terminatingCandidate else { return }
        transition.candidateKnownHidden = true
        candidate.stop(alreadyDeactivated: true)
        beginStandbyRestoreLocked(transition, reason: "candidate-hidden")
    }

    private func beginStandbyRestoreLocked(_ transition: ReplacementTransition,
                                           reason: String) {
        let displayID = transition.candidate.displayID
        guard transitions[displayID] === transition,
              transition.candidateKnownHidden ||
                !transition.candidate.process.isRunning else { return }
        // Blockers are created only by removeDisplayStateLocked(), which also
        // removes the transition and its standby. A live transition restoring a
        // standby must therefore never coexist with a stop blocker.
        assert(!hasVisibilityBlockersLocked(for: displayID))
        guard let standby = transition.standby else {
            transition.standby = nil
            finishTransitionFailureLocked(transition, reason: reason)
            return
        }
        guard standby.process.isRunning else {
            transition.standby = nil
            awaitVisibleExitLocked(standby)
            finishTransitionFailureLocked(transition, reason: reason)
            return
        }
        transition.phase = .restoringStandby
        if standby.wallpaper.kind == .web {
            standby.send(Self.webPlaybackStateCommand(for: standby))
        }
        standby.send(["cmd": "activate"])
        if standby.wallpaper.kind != .web {
            standby.send(Self.powerCommand(state: .run, fps: standby.desiredPowerFps))
        }
        scheduleStandbyRestoreTimeoutLocked(transition, standby: standby)
    }

    private func standbyActivatedLocked(_ standby: RendererProcess,
                                        transition: ReplacementTransition) {
        let displayID = standby.displayID
        guard transitions[displayID] === transition,
              transition.standby === standby,
              transition.phase == .restoringStandby else { return }
        guard standby.process.isRunning else {
            transition.standby = nil
            awaitVisibleExitLocked(standby)
            finishTransitionFailureLocked(transition, reason: "standby-exited")
            return
        }
        transition.standby = nil
        running[displayID] = standby
        clearAwaitingVisibleExitsLocked(for: displayID)
        deferredActiveExits[displayID] = nil
        drainCoverageSettlementWaitersIfNeededLocked(displayID)
        replayDesiredStateLocked(to: standby)
        finishTransitionFailureLocked(transition, reason: "rolled-back")
    }

    private func beginHideStandbyLocked(_ transition: ReplacementTransition,
                                        reason: String) {
        let displayID = transition.candidate.displayID
        guard transitions[displayID] === transition,
              transition.phase == .restoringStandby,
              let standby = transition.standby else { return }
        guard standby.process.isRunning else {
            transition.standby = nil
            awaitVisibleExitLocked(standby)
            finishTransitionFailureLocked(transition, reason: reason)
            return
        }
        transition.phase = .hidingStandbyAfterRestoreFailure
        standby.send(["cmd": "deactivate"])
        scheduleStandbyHideTimeoutLocked(transition, standby: standby)
    }

    private func standbyDeactivatedLocked(_ standby: RendererProcess,
                                          transition: ReplacementTransition) {
        let displayID = standby.displayID
        guard transitions[displayID] === transition,
              transition.standby === standby,
              transition.phase == .hidingStandbyAfterRestoreFailure ||
                transition.phase == .terminatingStandby else { return }
        transition.standby = nil
        if !retireLocked(standby, alreadyDeactivated: true) {
            // The renderer can exit after emitting `deactivated` but before it
            // is registered as a controlled retiree. Keep that real exit
            // reportable if no subsequent request covers the display.
            awaitVisibleExitLocked(standby)
        }
        finishTransitionFailureLocked(transition, reason: "standby-restore-failed")
    }

    private func finishTransitionFailureLocked(_ transition: ReplacementTransition,
                                               reason: String) {
        let candidate = transition.candidate
        let displayID = candidate.displayID
        guard transitions[displayID] === transition else { return }
        guard transition.candidateKnownHidden || !candidate.process.isRunning else {
            NSLog("[Mirage] 拒绝在候选可见性未确认时回滚 (显示器=\(displayID))")
            return
        }
        transitions[displayID] = nil
        if candidates[displayID] === candidate { candidates[displayID] = nil }
        if candidate.process.isRunning {
            retireLocked(candidate, alreadyDeactivated: true)
        }
        let completions = takeTransitionCompletionsLocked(from: candidate)
        SystemAudioSpectrumService.shared.setEnabled(hasSpectrumConsumersLocked())
        dispatchTransitionCompletions(completions, success: false)
        NSLog("[Mirage] 壁纸切换失败，已完成隔离/回滚 (显示器=\(displayID), 原因=\(reason))")
        let launchedPending = launchPendingRequestLocked(for: displayID)
        if !launchedPending { reportDeferredActiveExitIfUncoveredLocked(displayID) }
    }

    private func replayDesiredStateLocked(to handle: RendererProcess) {
        handle.send(["cmd": "fps", "value": handle.desiredFps])
        handle.send(["cmd": "fillmode", "value": handle.desiredFillMode.rawValue])
        handle.send(["cmd": "speed", "value": handle.desiredSpeed])
        if handle.wallpaper.kind == .video {
            handle.send(["cmd": "hdr", "value": handle.desiredHDRVideo])
        }
        if handle.wallpaper.kind == .web {
            handle.send(Self.webPlaybackStateCommand(for: handle))
        } else {
            sendVolumeAndMute(handle.desiredVolume, muted: handle.desiredMuted,
                              to: handle)
            handle.send(Self.powerCommand(state: handle.desiredPowerState,
                                          fps: handle.desiredPowerFps))
        }
        if handle.wallpaper.kind != .scene {
            applyInitialProperties(handle.desiredUserProperties, to: handle)
        }
    }

    @discardableResult
    private func retireLocked(_ handle: RendererProcess,
                              alreadyDeactivated: Bool) -> Bool {
        guard handle.process.isRunning else { return false }
        retiring[ObjectIdentifier(handle)] = handle
        handle.stop(alreadyDeactivated: alreadyDeactivated)
        return true
    }

    @discardableResult
    private func launchPendingRequestLocked(for displayID: CGDirectDisplayID) -> Bool {
        guard transitions[displayID] == nil,
              let request = pendingRequests.removeValue(forKey: displayID) else { return false }
        guard latestRequestTokens[displayID] == request.requestToken else {
            dispatchTransitionCompletions(request.completions, success: false)
            return false
        }
        let completions = request.completions
        let leaseID = UUID()
        launchingRequests[displayID] = LaunchingRequest(
            requestToken: request.requestToken, leaseID: leaseID,
            options: request.options)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else {
                DispatchQueue.main.async { completions.forEach { $0(false) } }
                return
            }
            let combined: (Bool) -> Void = { success in
                self.dispatchTransitionCompletions(completions, success: success)
                if !success {
                    self.queue.async {
                        self.reportDeferredActiveExitIfUncoveredLocked(displayID)
                    }
                }
            }
            _ = self.renderRequest(
                request.wallpaper, onDisplay: displayID, options: request.options,
                reuseActive: request.reuseActive,
                requestToken: request.requestToken, leaseID: leaseID,
                completion: combined)
        }
        return true
    }

    private func schedulePreparationTimeoutLocked(_ transition: ReplacementTransition) {
        let timeout = transition.candidate.wallpaper.kind == .video
            ? Self.videoPreparationTimeout : Self.preparationTimeout
        transition.preparationEpoch &+= 1
        let epoch = transition.preparationEpoch
        let displayID = transition.candidate.displayID
        queue.asyncAfter(deadline: .now() + timeout) { [weak self, weak transition] in
            guard let self, let transition,
                  self.transitions[displayID] === transition,
                  transition.phase == .preparing,
                  transition.preparationEpoch == epoch else { return }
            self.failCandidateLocked(transition, reason: "preparation-timeout")
        }
    }

    private func scheduleDeactivationTimeoutLocked(_ transition: ReplacementTransition,
                                                   active: RendererProcess) {
        let displayID = active.displayID
        queue.asyncAfter(deadline: .now() + Self.deactivationTimeout) { [weak self, weak transition, weak active] in
            guard let self, let transition, let active,
                  self.transitions[displayID] === transition,
                  self.running[displayID] === active,
                  transition.phase == .deactivatingActive else { return }
            NSLog("[Mirage] 旧壁纸隐藏确认超时，恢复旧壁纸 (显示器=\(displayID))")
            transition.candidateKnownHidden = true
            transition.candidate.stop(alreadyDeactivated: true)
            self.running[displayID] = nil
            transition.standby = active
            self.beginStandbyRestoreLocked(transition, reason: "deactivation-timeout")
        }
    }

    private func scheduleCandidateActivationTimeoutLocked(_ transition: ReplacementTransition) {
        let displayID = transition.candidate.displayID
        queue.asyncAfter(deadline: .now() + Self.activationTimeout) { [weak self, weak transition] in
            guard let self, let transition,
                  self.transitions[displayID] === transition,
                  transition.phase == .activatingCandidate else { return }
            self.beginCandidateRollbackLocked(transition, reason: "activation-timeout")
        }
    }

    private func scheduleCandidateHideTimeoutLocked(_ transition: ReplacementTransition) {
        let displayID = transition.candidate.displayID
        queue.asyncAfter(deadline: .now() + Self.hideTimeout) { [weak self, weak transition] in
            guard let self, let transition,
                  self.transitions[displayID] === transition,
                  transition.phase == .hidingCandidateForRollback else { return }
            transition.phase = .terminatingCandidate
            transition.candidate.stop()
        }
    }

    private func scheduleStandbyRestoreTimeoutLocked(_ transition: ReplacementTransition,
                                                     standby: RendererProcess) {
        let displayID = standby.displayID
        queue.asyncAfter(deadline: .now() + Self.restoreTimeout) { [weak self, weak transition, weak standby] in
            guard let self, let transition, let standby,
                  self.transitions[displayID] === transition,
                  transition.standby === standby,
                  transition.phase == .restoringStandby else { return }
            self.beginHideStandbyLocked(transition, reason: "standby-activation-timeout")
        }
    }

    private func scheduleStandbyHideTimeoutLocked(_ transition: ReplacementTransition,
                                                  standby: RendererProcess) {
        let displayID = standby.displayID
        queue.asyncAfter(deadline: .now() + Self.hideTimeout) { [weak self, weak transition, weak standby] in
            guard let self, let transition, let standby,
                  self.transitions[displayID] === transition,
                  transition.standby === standby,
                  transition.phase == .hidingStandbyAfterRestoreFailure else { return }
            transition.phase = .terminatingStandby
            standby.stop()
        }
    }

    private func dispatchProcessExitLocked(_ displayID: CGDirectDisplayID,
                                           record: DeferredActiveExit) {
        let abnormal = record.status != 0 && !record.wasTerminated
        DispatchQueue.main.async { [weak self] in
            guard let self, let screen = self.screenIndex(for: displayID) else { return }
            VideoTranscodeProgressModel.shared.finish(screen: screen)
            self.onProcessExit?(screen, abnormal)
        }
    }

    /// Retains a former visible role until its already-scheduled termination
    /// handler supplies status. Must be called on `queue` after removing the
    /// handle from `running` or `transition.standby`.
    private func awaitVisibleExitLocked(_ handle: RendererProcess) {
        awaitingVisibleExits[ObjectIdentifier(handle)] = handle
    }

    private func hasVisibilityBlockersLocked(
        for displayID: CGDirectDisplayID
    ) -> Bool {
        visibilityBlockers.values.contains { $0.displayID == displayID }
    }

    /// Releases an explicit-stop barrier only after the renderer protocol has
    /// confirmed that WindowServer no longer composites the window.
    private func visibilityBlockerBecameHiddenLocked(_ handle: RendererProcess) {
        let identity = ObjectIdentifier(handle)
        guard visibilityBlockers.removeValue(forKey: identity) === handle else { return }
        if handle.process.isRunning {
            // `stop()` was already issued when the role became a blocker. Keep
            // ownership until its termination handler arrives, without sending
            // a second command or relying on stop()'s idempotent early return.
            retiring[identity] = handle
        }
        resumePreparedTransitionAfterVisibilityBlockersLocked(handle.displayID)
        reportDeferredActiveExitIfUncoveredLocked(handle.displayID)
    }

    private func resumePreparedTransitionAfterVisibilityBlockersLocked(
        _ displayID: CGDirectDisplayID
    ) {
        guard !hasVisibilityBlockersLocked(for: displayID),
              let transition = transitions[displayID],
              transition.phase == .waitingForVisibilityBlockers,
              transition.prepared else { return }
        transition.phase = .preparing
        candidatePreparedLocked(transition)
    }

    /// A successfully activated renderer covers every older visible exit for
    /// this display. Their late termination handlers are intentionally ignored.
    private func clearAwaitingVisibleExitsLocked(for displayID: CGDirectDisplayID) {
        let keys = awaitingVisibleExits.compactMap { key, handle in
            handle.displayID == displayID ? key : nil
        }
        for key in keys { awaitingVisibleExits[key] = nil }
    }

    private func reportDeferredActiveExitIfUncoveredLocked(
        _ displayID: CGDirectDisplayID
    ) {
        defer { drainCoverageSettlementWaitersIfNeededLocked(displayID) }
        if running[displayID] != nil {
            clearAwaitingVisibleExitsLocked(for: displayID)
            deferredActiveExits[displayID] = nil
            return
        }
        guard let record = deferredActiveExits[displayID] else { return }
        guard transitions[displayID] == nil,
              pendingRequests[displayID] == nil,
              launchingRequests[displayID] == nil,
              !hasVisibilityBlockersLocked(for: displayID),
              !awaitingVisibleExits.values.contains(where: {
                  $0.displayID == displayID
              }) else { return }
        deferredActiveExits[displayID] = nil
        dispatchProcessExitLocked(displayID, record: record)
    }

    private func handleProcessExitLocked(_ handle: RendererProcess, status: Int32) {
        let displayID = handle.displayID
        defer { drainCoverageSettlementWaitersIfNeededLocked(displayID) }
        let identity = ObjectIdentifier(handle)
        if visibilityBlockers.removeValue(forKey: identity) != nil {
            resumePreparedTransitionAfterVisibilityBlockersLocked(displayID)
            reportDeferredActiveExitIfUncoveredLocked(displayID)
            return
        }
        if retiring.removeValue(forKey: identity) != nil { return }

        if awaitingVisibleExits.removeValue(forKey: identity) != nil {
            deferredActiveExits[displayID] = DeferredActiveExit(
                status: status, wasTerminated: handle.isTerminated)
            SystemAudioSpectrumService.shared.setEnabled(hasSpectrumConsumersLocked())
            reportDeferredActiveExitIfUncoveredLocked(displayID)
            return
        }

        if running[displayID] === handle {
            running[displayID] = nil
            let exit = DeferredActiveExit(status: status,
                                          wasTerminated: handle.isTerminated)
            if let transition = transitions[displayID] {
                deferredActiveExits[displayID] = exit
                switch transition.phase {
                case .preparing:
                    if transition.prepared { beginCandidateActivationLocked(transition) }
                case .deactivatingActive:
                    beginCandidateActivationLocked(transition)
                default:
                    break
                }
                return
            }
            SystemAudioSpectrumService.shared.setEnabled(hasSpectrumConsumersLocked())
            deferredActiveExits[displayID] = nil
            dispatchProcessExitLocked(displayID, record: exit)
            return
        }

        guard let transition = transitions[displayID] else { return }
        if transition.candidate === handle {
            candidateExitedLocked(transition)
            return
        }
        if transition.standby === handle {
            deferredActiveExits[displayID] = DeferredActiveExit(
                status: status, wasTerminated: handle.isTerminated)
            transition.standby = nil
            switch transition.phase {
            case .activatingCandidate:
                break
            case .restoringStandby, .hidingStandbyAfterRestoreFailure,
                 .terminatingStandby:
                finishTransitionFailureLocked(transition, reason: "standby-exited")
            default:
                break
            }
        }
    }

    private func candidateExitedLocked(_ transition: ReplacementTransition) {
        let displayID = transition.candidate.displayID
        guard transitions[displayID] === transition else { return }
        transition.candidateKnownHidden = true
        switch transition.phase {
        case .preparing, .waitingForVisibilityBlockers:
            finishTransitionFailureLocked(transition, reason: "candidate-exited")
        case .deactivatingActive:
            if let active = running.removeValue(forKey: displayID) {
                transition.standby = active
            }
            beginStandbyRestoreLocked(transition, reason: "candidate-exited")
        case .activatingCandidate, .hidingCandidateForRollback, .terminatingCandidate:
            beginStandbyRestoreLocked(transition, reason: "candidate-exited")
        case .restoringStandby, .hidingStandbyAfterRestoreFailure, .terminatingStandby:
            break
        }
    }

    func stop(on screenIndex: Int) {
        guard let displayID = displayID(for: screenIndex) else { return }
        stop(displayID: displayID)
    }

    /// Removes every controller role for one display. Must run on `queue`.
    private func removeDisplayStateLocked(
        _ displayID: CGDirectDisplayID,
        detachProcessOwnership: Bool = false
    ) -> ([RendererProcess], [(Bool) -> Void]) {
        generations[displayID] = (generations[displayID] ?? 0) &+ 1
        latestRequestTokens[displayID] = UUID()
        launchingRequests[displayID] = nil
        deferredActiveExits[displayID] = nil
        var handles: [RendererProcess] = []
        var seen: Set<ObjectIdentifier> = []
        func append(_ handle: RendererProcess?) {
            guard let handle, seen.insert(ObjectIdentifier(handle)).inserted else { return }
            handles.append(handle)
        }

        append(running.removeValue(forKey: displayID))
        let candidate = candidates.removeValue(forKey: displayID)
        append(candidate)
        let transition = transitions.removeValue(forKey: displayID)
        append(transition?.candidate)
        append(transition?.standby)

        // None of these former roles may be assumed hidden merely because the
        // controller forgot the role. Keep every live process as an activation
        // blocker until its protocol confirms hidden or termination destroys
        // the window. This closes stop -> immediate resume/assign overlap.
        if !detachProcessOwnership {
            for handle in handles {
                let identity = ObjectIdentifier(handle)
                if handle.process.isRunning {
                    visibilityBlockers[identity] = handle
                } else {
                    // Foundation may queue the termination handler after
                    // isRunning turns false. Retain and classify that late exit
                    // as an intentional, already-hidden retirement.
                    retiring[identity] = handle
                }
            }
        }

        var completions: [(Bool) -> Void] = []
        if let candidate {
            completions.append(contentsOf: takeTransitionCompletionsLocked(from: candidate))
        } else if let transition {
            completions.append(contentsOf:
                takeTransitionCompletionsLocked(from: transition.candidate))
        }
        if let pending = pendingRequests.removeValue(forKey: displayID) {
            completions.append(contentsOf: pending.completions)
        }

        let retireeKeys = retiring.compactMap { key, handle in
            handle.displayID == displayID ? key : nil
        }
        for key in retireeKeys {
            append(retiring[key])
            if detachProcessOwnership { retiring[key] = nil }
        }
        let awaitingKeys = awaitingVisibleExits.compactMap { key, handle in
            handle.displayID == displayID ? key : nil
        }
        for key in awaitingKeys {
            let handle = awaitingVisibleExits.removeValue(forKey: key)
            append(handle)
            if !detachProcessOwnership, let handle {
                retiring[key] = handle
            }
        }
        let blockerKeys = visibilityBlockers.compactMap { key, handle in
            handle.displayID == displayID ? key : nil
        }
        for key in blockerKeys {
            append(visibilityBlockers[key])
            if detachProcessOwnership {
                visibilityBlockers[key] = nil
            }
        }
        drainCoverageSettlementWaitersIfNeededLocked(displayID)
        return (handles, completions)
    }

    func stop(displayID: CGDirectDisplayID) {
        let (handles, transitionCompletions) = queue.sync {
            removeDisplayStateLocked(displayID)
        }
        handles.forEach { $0.stop() }
        dispatchTransitionCompletions(transitionCompletions, success: false)
        refreshAudioSpectrumService()
    }

    func stopDisplays(except displayIDs: Set<CGDirectDisplayID>) {
        let (handles, transitionCompletions): ([RendererProcess], [(Bool) -> Void]) = queue.sync {
            let owned = Set(running.keys)
                .union(candidates.keys)
                .union(transitions.keys)
                .union(pendingRequests.keys)
                .union(launchingRequests.keys)
                .union(awaitingVisibleExits.values.map(\.displayID))
                .union(visibilityBlockers.values.map(\.displayID))
                .union(retiring.values.map(\.displayID))
            let removed = owned.subtracting(displayIDs)
            var result: [RendererProcess] = []
            var completions: [(Bool) -> Void] = []
            for displayID in removed {
                let state = removeDisplayStateLocked(displayID)
                result.append(contentsOf: state.0)
                completions.append(contentsOf: state.1)
            }
            return (result, completions)
        }
        handles.forEach { $0.stop() }
        dispatchTransitionCompletions(transitionCompletions, success: false)
        refreshAudioSpectrumService()
    }

    func stopAll() {
        let (handles, transitionCompletions): ([RendererProcess], [(Bool) -> Void]) = queue.sync {
            let owned = Set(running.keys)
                .union(candidates.keys)
                .union(transitions.keys)
                .union(pendingRequests.keys)
                .union(launchingRequests.keys)
                .union(awaitingVisibleExits.values.map(\.displayID))
                .union(visibilityBlockers.values.map(\.displayID))
                .union(retiring.values.map(\.displayID))
            var result: [RendererProcess] = []
            var completions: [(Bool) -> Void] = []
            for displayID in owned {
                let state = removeDisplayStateLocked(displayID)
                result.append(contentsOf: state.0)
                completions.append(contentsOf: state.1)
            }
            return (result, completions)
        }
        handles.forEach { $0.stop() }
        dispatchTransitionCompletions(transitionCompletions, success: false)
        SystemAudioSpectrumService.shared.setEnabled(false)
    }

    /// Synchronous, bounded shutdown for app termination.
    ///
    /// `applicationWillTerminate` returns straight into process exit, so any
    /// escalation deferred with asyncAfter never runs: a hung renderer would
    /// survive as an orphan still drawing a fullscreen desktop-level window
    /// with no parent left to kill it. Escalate here instead — quit, SIGTERM,
    /// SIGKILL — waiting on every renderer in parallel so the worst case stays
    /// around two seconds regardless of how many are running.
    func stopAllAndWait() {
        let (handles, transitionCompletions): ([RendererProcess], [(Bool) -> Void]) = queue.sync {
            let owned = Set(running.keys)
                .union(candidates.keys)
                .union(transitions.keys)
                .union(pendingRequests.keys)
                .union(launchingRequests.keys)
                .union(awaitingVisibleExits.values.map(\.displayID))
                .union(visibilityBlockers.values.map(\.displayID))
                .union(retiring.values.map(\.displayID))
            var result: [RendererProcess] = []
            var completions: [(Bool) -> Void] = []
            for displayID in owned {
                let state = removeDisplayStateLocked(
                    displayID, detachProcessOwnership: true)
                result.append(contentsOf: state.0)
                completions.append(contentsOf: state.1)
            }
            return (result, completions)
        }
        dispatchTransitionCompletions(transitionCompletions, success: false)
        SystemAudioSpectrumService.shared.setEnabled(false)
        // The handles are dropped from `running`/`candidates` above, so nothing
        // else will ever reap them. `defer` covers every early return below;
        // cleanupAfterExit is idempotent and safe on an already-exited process.
        defer { handles.forEach { $0.cleanupAfterExit() } }
        handles.forEach { $0.stop() }
        guard !handles.isEmpty else { return }

        var alive = Self.waitForExit(handles, timeout: 1.2)
        guard !alive.isEmpty else { return }
        alive.forEach { $0.forceTerminate() }
        alive = Self.waitForExit(alive, timeout: 0.5)
        guard !alive.isEmpty else { return }
        alive.forEach { $0.forceKill() }
        _ = Self.waitForExit(alive, timeout: 0.3)
    }

    /// Waits for all `handles` at once — N renderers cost the same wall-clock
    /// time as one. Returns the handles that are still alive at the deadline.
    private static func waitForExit(_ handles: [RendererProcess],
                                    timeout: TimeInterval) -> [RendererProcess] {
        var alive = handles.filter { $0.process.isRunning }
        guard !alive.isEmpty else { return [] }
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            usleep(20_000)
            alive = alive.filter { $0.process.isRunning }
            if alive.isEmpty { return [] }
        }
        return alive
    }

    func isRendering(on screenIndex: Int) -> Bool {
        guard let displayID = displayID(for: screenIndex) else { return false }
        return isRendering(onDisplay: displayID)
    }

    func isRendering(onDisplay displayID: CGDirectDisplayID) -> Bool {
        queue.sync { running[displayID]?.process.isRunning ?? false }
    }

    private func hasCoverageOrWorkLocked(_ displayID: CGDirectDisplayID) -> Bool {
        running[displayID] != nil ||
            candidates[displayID] != nil ||
            transitions[displayID] != nil ||
            pendingRequests[displayID] != nil ||
            launchingRequests[displayID] != nil ||
            hasVisibilityBlockersLocked(for: displayID) ||
            awaitingVisibleExits.values.contains { $0.displayID == displayID }
    }

    private func coverageOrWorkSettledLocked(_ displayID: CGDirectDisplayID) -> Bool {
        running[displayID]?.process.isRunning == true ||
            !hasCoverageOrWorkLocked(displayID)
    }

    private func drainCoverageSettlementWaitersIfNeededLocked(
        _ displayID: CGDirectDisplayID
    ) {
        guard coverageOrWorkSettledLocked(displayID),
              let waiters = coverageSettlementWaiters.removeValue(forKey: displayID)
        else { return }
        let completions = Array(waiters.values)
        DispatchQueue.main.async {
            completions.forEach { $0() }
        }
    }

    /// Returns true while this display is covered or the controller still owns
    /// work that must settle before a recovery launch is considered. Retiring
    /// handles are deliberately excluded: they are confirmed hidden and can
    /// never become coverage again.
    func hasCoverageOrWork(onDisplay displayID: CGDirectDisplayID) -> Bool {
        queue.sync { hasCoverageOrWorkLocked(displayID) }
    }

    /// Runs once after live coverage is restored or all controller work for the
    /// display is gone. Registration and the initial state check are atomic on
    /// the controller queue, avoiding a missed-clear race.
    @discardableResult
    func whenCoverageOrWorkSettles(onDisplay displayID: CGDirectDisplayID,
                                   completion: @escaping () -> Void) -> UUID {
        let token = UUID()
        let settled = queue.sync {
            guard !coverageOrWorkSettledLocked(displayID) else { return true }
            coverageSettlementWaiters[displayID, default: [:]][token] = completion
            return false
        }
        if settled {
            DispatchQueue.main.async(execute: completion)
        }
        return token
    }

    func cancelCoverageOrWorkWaiter(_ token: UUID,
                                    onDisplay displayID: CGDirectDisplayID) {
        queue.sync {
            guard var waiters = coverageSettlementWaiters[displayID] else { return }
            waiters[token] = nil
            coverageSettlementWaiters[displayID] = waiters.isEmpty ? nil : waiters
        }
    }

    func currentWallpaper(on screenIndex: Int) -> WEWallpaper? {
        guard let displayID = displayID(for: screenIndex) else { return nil }
        return currentWallpaper(onDisplay: displayID)
    }

    func currentWallpaper(onDisplay displayID: CGDirectDisplayID) -> WEWallpaper? {
        queue.sync { running[displayID]?.wallpaper }
    }

    var activeScreens: [Int] {
        let displayIDs = queue.sync { Array(running.keys) }
        return displayIDs.compactMap(screenIndex(for:)).sorted()
    }

    var activeDisplayIDs: [CGDirectDisplayID] {
        queue.sync { Array(running.keys).sorted() }
    }

    var processIdentifiers: Set<pid_t> {
        queue.sync {
            let standbys = transitions.values.compactMap(\.standby)
            let handles = Array(running.values) + Array(candidates.values) +
                standbys + Array(visibilityBlockers.values) +
                Array(awaitingVisibleExits.values) + Array(retiring.values)
            return Set(handles.compactMap { handle in
                handle.process.isRunning ? handle.process.processIdentifier : nil
            })
        }
    }

    // MARK: Live control (broadcast or per-screen)

    private func matchesAssignmentLocked(_ process: RendererProcess,
                                         assignmentID: UUID?) -> Bool {
        guard let assignmentID else { return true }
        return process.assignmentID == assignmentID
    }

    /// Callers must already be executing on `queue`. A nil assignment keeps the
    /// legacy display-wide behavior; a concrete ID isolates one UI proposal.
    private func targetsLocked(_ displayID: CGDirectDisplayID?,
                               includeCandidates: Bool,
                               assignmentID: UUID? = nil) -> [RendererProcess] {
        var result: [RendererProcess] = []
        var seen: Set<ObjectIdentifier> = []
        func append(_ process: RendererProcess?) {
            guard let process,
                  matchesAssignmentLocked(process, assignmentID: assignmentID),
                  seen.insert(ObjectIdentifier(process)).inserted else { return }
            result.append(process)
        }
        if let displayID {
            append(running[displayID])
            if includeCandidates {
                append(candidates[displayID])
                append(transitions[displayID]?.standby)
            }
        } else {
            running.values.forEach { append($0) }
            if includeCandidates {
                candidates.values.forEach { append($0) }
                transitions.values.forEach { append($0.standby) }
            }
        }
        return result
    }

    private func liveRunningTargetsLocked(_ displayID: CGDirectDisplayID?,
                                          assignmentID: UUID? = nil) -> [RendererProcess] {
        let handles: [RendererProcess]
        if let displayID {
            handles = running[displayID].map { [$0] } ?? []
        } else {
            handles = Array(running.values)
        }
        return handles.filter { handle in
            guard matchesAssignmentLocked(handle, assignmentID: assignmentID) else {
                return false
            }
            guard let transition = transitions[handle.displayID] else { return true }
            return transition.phase != .deactivatingActive
        }
    }

    private func updatePendingOptionsLocked(
        _ displayID: CGDirectDisplayID?,
        assignmentID: UUID? = nil,
        _ update: (inout RenderOptions) -> Void
    ) {
        func matches(_ options: RenderOptions) -> Bool {
            guard let assignmentID else { return true }
            return options.assignmentID == assignmentID
        }
        if let displayID {
            if let request = pendingRequests[displayID], matches(request.options) {
                var options = request.options
                update(&options)
                request.options = options
            }
            if var request = launchingRequests[displayID], matches(request.options) {
                update(&request.options)
                launchingRequests[displayID] = request
            }
        } else {
            for request in pendingRequests.values where matches(request.options) {
                var options = request.options
                update(&options)
                request.options = options
            }
            for displayID in Array(launchingRequests.keys) {
                guard var request = launchingRequests[displayID],
                      matches(request.options) else { continue }
                update(&request.options)
                launchingRequests[displayID] = request
            }
        }
    }

    private func forEach(_ displayID: CGDirectDisplayID?, includeCandidates: Bool = true,
                         _ body: (RendererProcess) -> Void) {
        // Snapshot the targets under the controller queue, then run `body`
        // outside it. Bodies talk to renderer stdin, which must never happen
        // while the serial queue is held.
        let targets = queue.sync {
            targetsLocked(displayID, includeCandidates: includeCandidates)
        }
        targets.forEach(body)
    }

    private func hasSpectrumConsumersLocked() -> Bool {
        liveRunningTargetsLocked(nil).contains { self.needsSpectrum($0) }
    }

    private func takeTransitionCompletionsLocked(
        from handle: RendererProcess
    ) -> [(Bool) -> Void] {
        let completions = handle.transitionCompletions
        handle.transitionCompletions.removeAll()
        return completions
    }

    private func dispatchTransitionCompletions(
        _ completions: [(Bool) -> Void], success: Bool
    ) {
        guard !completions.isEmpty else { return }
        DispatchQueue.main.async {
            completions.forEach { $0(success) }
        }
    }

    private func needsSpectrum(_ process: RendererProcess) -> Bool {
        process.spectrumEnabled && process.spectrumDemanded &&
            !process.desiredPaused && process.process.isRunning
    }

    private func refreshAudioSpectrumService() {
        let enabled = queue.sync { hasSpectrumConsumersLocked() }
        SystemAudioSpectrumService.shared.setEnabled(enabled)
    }

    private func setAudioSpectrum(_ spectrum: [Float]) {
        guard spectrum.count == 128 else { return }
        let processes = queue.sync {
            liveRunningTargetsLocked(nil).filter { self.needsSpectrum($0) }
        }
        for process in processes {
            guard process.wallpaper.kind == .scene || process.wallpaper.kind == .web else { continue }
            process.sendSpectrum(spectrum)
        }
    }

    private static func mergePlaybackOptions(_ source: RenderOptions,
                                             into target: inout RenderOptions) {
        target.fps = source.fps
        target.volume = source.volume
        target.muted = source.muted
        target.speed = source.speed
        target.fillMode = source.fillMode
        target.userProperties = source.userProperties
        target.powerState = source.powerState
        target.powerFps = source.powerFps
        target.enableHDRVideo = source.enableHDRVideo
        target.assignmentID = source.assignmentID
    }

    private func sendPlaybackOptions(_ options: RenderOptions,
                                     to handle: RendererProcess) {
        handle.send(["cmd": "fps", "value": options.fps])
        handle.send(["cmd": "fillmode", "value": options.fillMode.rawValue])
        handle.send(["cmd": "speed", "value": options.speed])
        if handle.wallpaper.kind == .video {
            handle.send(["cmd": "hdr", "value": options.enableHDRVideo])
        }
        if handle.wallpaper.kind == .web {
            handle.send(Self.webPlaybackStateCommand(volume: options.volume,
                                                       muted: options.muted,
                                                       state: options.powerState,
                                                       fps: options.powerFps))
        } else {
            sendVolumeAndMute(options.volume, muted: options.muted, to: handle)
            handle.send(Self.powerCommand(state: options.powerState,
                                          fps: options.powerFps))
        }
        if handle.wallpaper.kind != .scene {
            applyInitialProperties(options.userProperties, to: handle)
        }
    }

    private func sendVolumeAndMute(_ volume: Float, muted: Bool,
                                   to handle: RendererProcess) {
        // When muting, close the audio gate before changing gain. When unmuting,
        // establish the target gain before opening it. This avoids exposing a
        // renderer's default or previous volume between two JSON commands.
        if muted {
            handle.send(["cmd": "muted", "value": true])
            handle.send(["cmd": "volume", "value": volume])
        } else {
            handle.send(["cmd": "volume", "value": volume])
            handle.send(["cmd": "muted", "value": false])
        }
    }

    /// Atomically updates playback policy for one UI assignment across every
    /// controller role. Hidden candidates and standbys only record desired state;
    /// commands are emitted solely to the matching, currently visible active.
    func applyPlaybackOptions(_ options: RenderOptions, assignmentID: UUID,
                              onDisplay displayID: CGDirectDisplayID) {
        var scopedOptions = options
        scopedOptions.assignmentID = assignmentID
        let actives: [RendererProcess] = queue.sync {
            let handles = targetsLocked(
                displayID, includeCandidates: true, assignmentID: assignmentID)
            handles.forEach { updateDesiredStateLocked($0, from: scopedOptions) }
            updatePendingOptionsLocked(displayID, assignmentID: assignmentID) {
                Self.mergePlaybackOptions(scopedOptions, into: &$0)
            }
            return liveRunningTargetsLocked(displayID, assignmentID: assignmentID)
        }
        actives.forEach { sendPlaybackOptions(scopedOptions, to: $0) }
        refreshAudioSpectrumService()
    }

    func setVolume(_ volume: Float, on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setVolume(volume, onDisplay: displayID)
            return
        }
        setVolume(volume, onDisplay: nil)
    }

    func setVolume(_ volume: Float, onDisplay displayID: CGDirectDisplayID?) {
        let actives: [RendererProcess] = queue.sync {
            let list = targetsLocked(displayID, includeCandidates: true)
            for handle in list { handle.desiredVolume = volume }
            updatePendingOptionsLocked(displayID) { $0.volume = volume }
            return liveRunningTargetsLocked(displayID)
        }
        for active in actives { active.send(["cmd": "volume", "value": volume]) }
    }

    func setMuted(_ muted: Bool, on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setMuted(muted, onDisplay: displayID)
            return
        }
        setMuted(muted, onDisplay: nil)
    }

    func setMuted(_ muted: Bool, onDisplay displayID: CGDirectDisplayID?) {
        let actives: [RendererProcess] = queue.sync {
            let list = targetsLocked(displayID, includeCandidates: true)
            for handle in list { handle.desiredMuted = muted }
            updatePendingOptionsLocked(displayID) { $0.muted = muted }
            return liveRunningTargetsLocked(displayID)
        }
        for active in actives { active.send(["cmd": "muted", "value": muted]) }
    }

    // Pause/resume route through the same power directive as everything else so
    // `desiredPowerState` can never disagree with what the renderer was told.
    // Passing no fps leaves the renderer's current frame rate untouched.
    func pause(on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setPower(.pause, fps: nil, onDisplay: displayID)
        } else {
            setPower(.pause, fps: nil, onDisplay: nil)
        }
    }

    func resume(on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setPower(.run, fps: nil, onDisplay: displayID)
        } else {
            setPower(.run, fps: nil, onDisplay: nil)
        }
    }

    /// Single authoritative power directive for the renderers.
    ///
    /// The renderers deliberately observe nothing themselves: their windows are
    /// `canHide = NO` + `orderFrontRegardless`, so AppKit never reports them as
    /// occluded and they cannot see the truth even if they looked. All of the
    /// policy — occlusion, lock, sleep, battery, thermal — is decided here, and
    /// the renderer only ever learns the final state, never the reason.
    func setPower(_ state: MiragePowerState, fps: Int?, on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setPower(state, fps: fps, onDisplay: displayID)
            return
        }
        setPower(state, fps: fps, onDisplay: nil)
    }

    func setPower(_ state: MiragePowerState, fps: Int?, onDisplay displayID: CGDirectDisplayID?) {
        let paused = state == .pause
        // Hidden candidates/standbys only record policy. Sending resume to a
        // hidden Web renderer would restart its global input monitors, and
        // unmuting any candidate would violate the handoff barrier.
        let actives: [RendererProcess] = queue.sync {
            func record(_ process: RendererProcess) {
                process.desiredPaused = paused
                process.desiredPowerState = state
                process.desiredPowerFps = fps
            }
            targetsLocked(displayID, includeCandidates: true).forEach(record)
            updatePendingOptionsLocked(displayID) {
                $0.powerState = state
                $0.powerFps = fps
            }
            return liveRunningTargetsLocked(displayID)
        }
        let command = Self.powerCommand(state: state, fps: fps)
        for active in actives { active.send(command) }
        refreshAudioSpectrumService()
    }

    private static func powerCommand(state: MiragePowerState, fps: Int?) -> [String: Any] {
        var command: [String: Any] = ["cmd": "power", "state": state.rawValue]
        if let fps { command["fps"] = fps }
        return command
    }

    private static func webPlaybackStateCommand(for handle: RendererProcess) -> [String: Any] {
        webPlaybackStateCommand(volume: handle.desiredVolume,
                                muted: handle.desiredMuted,
                                state: handle.desiredPowerState,
                                fps: handle.desiredPowerFps)
    }

    private static func webPlaybackStateCommand(volume: Float, muted: Bool,
                                                state: MiragePowerState,
                                                fps: Int?) -> [String: Any] {
        var command: [String: Any] = [
            "cmd": "playbackState",
            "volume": volume,
            "muted": muted,
            "state": state.rawValue
        ]
        if let fps { command["fps"] = fps }
        return command
    }

    func setFps(_ fps: Int, on screenIndex: Int? = nil) {
        let displayID: CGDirectDisplayID?
        if let screenIndex {
            guard let resolved = self.displayID(for: screenIndex) else { return }
            displayID = resolved
        } else { displayID = nil }
        let actives: [RendererProcess] = queue.sync {
            targetsLocked(displayID, includeCandidates: true).forEach { $0.desiredFps = fps }
            updatePendingOptionsLocked(displayID) { $0.fps = fps }
            return liveRunningTargetsLocked(displayID)
        }
        actives.forEach { $0.send(["cmd": "fps", "value": fps]) }
    }

    func setSpeed(_ speed: Float, on screenIndex: Int? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setSpeed(speed, onDisplay: displayID)
        } else {
            setSpeed(speed, onDisplay: nil)
        }
    }

    func setSpeed(_ speed: Float, onDisplay displayID: CGDirectDisplayID?) {
        let actives: [RendererProcess] = queue.sync {
            let list = targetsLocked(displayID, includeCandidates: true)
            for handle in list { handle.desiredSpeed = speed }
            updatePendingOptionsLocked(displayID) { $0.speed = speed }
            return liveRunningTargetsLocked(displayID)
        }
        for active in actives { active.send(["cmd": "speed", "value": speed]) }
    }

    func setFillMode(_ mode: FillMode, on screenIndex: Int? = nil,
                     assignmentID: UUID? = nil) {
        if let screenIndex {
            guard let displayID = displayID(for: screenIndex) else { return }
            setFillMode(mode, onDisplay: displayID, assignmentID: assignmentID)
        } else {
            setFillMode(mode, onDisplay: nil, assignmentID: assignmentID)
        }
    }

    func setFillMode(_ mode: FillMode, onDisplay displayID: CGDirectDisplayID?,
                     assignmentID: UUID? = nil) {
        let actives: [RendererProcess] = queue.sync {
            targetsLocked(displayID, includeCandidates: true,
                          assignmentID: assignmentID).forEach {
                $0.desiredFillMode = mode
            }
            updatePendingOptionsLocked(displayID, assignmentID: assignmentID) {
                $0.fillMode = mode
            }
            return liveRunningTargetsLocked(displayID, assignmentID: assignmentID)
        }
        actives.forEach { $0.send(["cmd": "fillmode", "value": mode.rawValue]) }
    }

    func setProperty(key: String, property: WEProjectProperty,
                     on screenIndex: Int? = nil, assignmentID: UUID? = nil) {
        let displayID: CGDirectDisplayID?
        if let screenIndex {
            guard let resolved = self.displayID(for: screenIndex) else { return }
            displayID = resolved
        } else {
            displayID = nil
        }
        setProperty(key: key, property: property, onDisplay: displayID,
                    assignmentID: assignmentID)
    }

    func setProperty(key: String, property: WEProjectProperty,
                     onDisplay displayID: CGDirectDisplayID?,
                     assignmentID: UUID? = nil) {
        let actives: [RendererProcess] = queue.sync {
            targetsLocked(displayID, includeCandidates: true,
                          assignmentID: assignmentID).forEach {
                $0.desiredUserProperties[key] = property
            }
            updatePendingOptionsLocked(displayID, assignmentID: assignmentID) {
                $0.userProperties[key] = property
            }
            return liveRunningTargetsLocked(displayID, assignmentID: assignmentID)
        }
        for proc in actives {
            proc.send(Self.propertyCommand(key: key, property: property))
        }
    }

    private static func openUserShortcut(name: String, target: String) {
        let expanded = (target as NSString).expandingTildeInPath
        if FileManager.default.fileExists(atPath: expanded) {
            NSWorkspace.shared.open(URL(fileURLWithPath: expanded))
            return
        }
        if let url = URL(string: target), let scheme = url.scheme?.lowercased(),
           scheme == "http" || scheme == "https" || scheme == "mailto" {
            NSWorkspace.shared.open(url)
            return
        }
        NSLog("[Mirage] 忽略用户快捷方式 \(name)：既不是本机路径也不是网页链接")
    }

    // MARK: Property → command / file

    private static func propertyCommand(key: String, property: WEProjectProperty) -> [String: Any] {
        var cmd: [String: Any] = ["cmd": "setProperty", "key": key]
        switch property.propertyType {
        case .color:
            cmd["type"] = "color"
            cmd["value"] = property.value.stringValue
        case .bool:
            cmd["value"] = property.value.boolValue
        case .slider:
            cmd["value"] = property.value.doubleValue
        case .scenetexture, .file:
            // Texture / file replacement: the renderer swaps the texture at runtime
            // using the "scenetexture" wire type.
            cmd["type"] = "scenetexture"
            cmd["value"] = property.value.stringValue
        case .combo:
            cmd["value"] = property.value.jsonObjectValue
        case .textinput, .text, .group, .directory, .usershortcut, .unknown:
            cmd["value"] = property.value.stringValue
        }
        return cmd
    }

    private func writeUserPropertiesFile(_ props: [String: WEProjectProperty], for wallpaper: WEWallpaper) -> URL? {
        guard !props.isEmpty else { return nil }
        var obj: [String: Any] = [:]
        for (key, prop) in props {
            switch prop.propertyType {
            case .color:
                obj[key] = ["type": "color", "value": prop.value.stringValue]
            case .bool:
                obj[key] = prop.value.boolValue
            case .slider:
                obj[key] = prop.value.doubleValue
            case .scenetexture, .file:
                obj[key] = ["type": "scenetexture", "value": prop.value.stringValue]
            case .combo:
                obj[key] = prop.value.jsonObjectValue
            default:
                obj[key] = prop.value.stringValue
            }
        }
        guard let data = try? JSONSerialization.data(withJSONObject: obj, options: []) else { return nil }
        let tmp = FileManager.default.temporaryDirectory
            .appending(path: "mirage_props_\(abs(wallpaper.id.hashValue))_\(UUID().uuidString).json")
        do {
            try data.write(to: tmp, options: .atomic)
            return tmp
        } catch {
            return nil
        }
    }

    private func applyInitialProperties(_ props: [String: WEProjectProperty], to handle: RendererProcess) {
        switch handle.wallpaper.kind {
        case .web:
            var values: [String: Any] = [:]
            values.reserveCapacity(props.count)
            for (key, property) in props {
                let command = Self.propertyCommand(key: key, property: property)
                if let value = command["value"] {
                    values[key] = ["value": value]
                }
            }
            handle.send([
                "cmd": "setProperties",
                "generation": UUID().uuidString,
                "values": values
            ])
        case .scene:
            // The normal scene path loads this snapshot through
            // --user-properties before parsing. This stdin path is only a
            // fallback when the temporary launch file could not be written.
            for key in props.keys.sorted() {
                guard let property = props[key] else { continue }
                handle.send(Self.propertyCommand(key: key, property: property))
            }
        case .video, .unsupported:
            break
        }
    }
}
