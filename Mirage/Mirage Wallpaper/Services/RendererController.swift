//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
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

final class RendererProcess {
    let process: Process
    let stdinPipe: Pipe
    let stdoutPipe: Pipe?
    let stderrPipe: Pipe
    let wallpaper: WEWallpaper
    let screenIndex: Int
    let generation: UInt64
    var desiredVolume: Float
    var desiredMuted: Bool
    var desiredPaused = false
    let spectrumEnabled: Bool
    var spectrumDemanded: Bool

    private let stateLock = NSLock()
    private let writeLock = NSLock()
    private let spectrumStateLock = NSLock()
    private let spectrumQueue = DispatchQueue(label: "cn.laobamac.Mirage.renderer.spectrum")
    private var terminated = false
    private var pendingSpectrum: [Float]?
    private var spectrumWriterActive = false
    private var stdoutBuffer = Data()

    var isTerminated: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return terminated
    }

    /// Temporary files associated with this renderer process, cleaned up after exit.
    var tempFiles: [URL] = []

    init(process: Process, stdinPipe: Pipe, stdoutPipe: Pipe?, stderrPipe: Pipe,
         wallpaper: WEWallpaper, screenIndex: Int, generation: UInt64,
         desiredVolume: Float, desiredMuted: Bool, spectrumEnabled: Bool) {
        self.process = process
        self.stdinPipe = stdinPipe
        self.stdoutPipe = stdoutPipe
        self.stderrPipe = stderrPipe
        self.wallpaper = wallpaper
        self.screenIndex = screenIndex
        self.generation = generation
        self.desiredVolume = desiredVolume
        self.desiredMuted = desiredMuted
        self.spectrumEnabled = spectrumEnabled
        self.spectrumDemanded = false
    }

    func send(_ command: [String: Any]) {
        write(command, allowAfterStop: false)
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
            send(["cmd": "audioSpectrum", "data": spectrum])
        }
    }

    private func write(_ command: [String: Any], allowAfterStop: Bool) {
        guard let data = try? JSONSerialization.data(withJSONObject: command, options: []) else {
            NSLog("[Mirage] 无法序列化控制指令: \(command)")
            return
        }
        var line = data
        line.append(0x0A)
        let handle = stdinPipe.fileHandleForWriting
        writeLock.lock()
        defer { writeLock.unlock() }
        stateLock.lock()
        let canWrite = (allowAfterStop || !terminated) && process.isRunning
        stateLock.unlock()
        guard canWrite else { return }
        do {
            try handle.write(contentsOf: line)
        } catch {
            NSLog("[Mirage] 发送控制指令失败 (屏幕=\(screenIndex)): \(error.localizedDescription)")
        }
    }

    func stop() {
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

        // The graceful quit must be written after marking the handle stopped,
        // so no later live-control command can race it. Use the private bypass
        // instead of send(), whose normal guard intentionally rejects it.
        write(["cmd": "quit"], allowAfterStop: true)
        let handle = stdinPipe.fileHandleForWriting
        try? handle.close()
        let proc = process
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.5) {
            if proc.isRunning { proc.terminate() }
            DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
                if proc.isRunning { kill(proc.processIdentifier, SIGKILL) }
            }
        }
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
        stderrPipe.fileHandleForReading.readabilityHandler = { file in
            let data = file.availableData
            if data.isEmpty {
                file.readabilityHandler = nil
                return
            }
            if let text = String(data: data, encoding: .utf8), !text.isEmpty {
                NSLog("[Mirage] 渲染器 stderr: \(text)")
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
        stdoutPipe?.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        for url in tempFiles {
            try? FileManager.default.removeItem(at: url)
        }
        tempFiles.removeAll()
    }
}

struct RenderOptions {
    var fps: Int = 30
    var volume: Float = 1.0
    var muted: Bool = false
    var speed: Float = 1.0
    var fillMode: FillMode = .cover
    var enableSpectrum: Bool = true
    var renderScale: Double = 1.0
    var msaaSamples: Int = 1
    var loadFromMemory: Bool = false
    var userProperties: [String: WEProjectProperty] = [:]
}

// Subprocess control: the renderer receives JSON-line commands via stdin.
final class RendererController {
    private var running: [Int: RendererProcess] = [:]
    private var candidates: [Int: RendererProcess] = [:]
    private var generations: [Int: UInt64] = [:]
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.renderer")
    // The renderer's Vulkan and scene-load guards are 30 seconds each. Keep
    // the old wallpaper alive slightly longer so a slow-but-valid cold start
    // can still complete without exposing the desktop.
    private static let sceneStartupTimeout: TimeInterval = 75

    var onProcessExit: ((Int, Bool) -> Void)?

    init() {
        SystemAudioSpectrumService.shared.onSpectrum = { [weak self] spectrum in
            self?.setAudioSpectrum(spectrum)
        }
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
        guard wallpaper.isValid, wallpaper.kind != .unsupported else { return false }
        guard let binary = binaryURL(for: wallpaper.kind) else {
            NSLog("[Mirage] 找不到 \(wallpaper.kind) 渲染器二进制")
            return false
        }
        NSLog("[Mirage] 启动渲染器: \(binary.path) 屏幕=\(screenIndex)")

        // Scene wallpapers have a real first-frame protocol. Keep the active
        // renderer alive while its transparent candidate prepares. Other
        // renderer kinds retain their existing immediate-switch behaviour.
        let deferredScene = wallpaper.kind == .scene
        var supersededCandidate: RendererProcess?
        var replacedActive: RendererProcess?
        let generation: UInt64 = queue.sync {
            let next = (generations[screenIndex] ?? 0) &+ 1
            generations[screenIndex] = next
            supersededCandidate = candidates.removeValue(forKey: screenIndex)
            if !deferredScene {
                replacedActive = running.removeValue(forKey: screenIndex)
            }
            return next
        }
        supersededCandidate?.stop()
        replacedActive?.stop()

        let proc = Process()
        proc.executableURL = binary

        var args: [String] = []
        var env = ProcessInfo.processInfo.environment

        switch wallpaper.kind {
        case .scene:
            let assetsPath = sceneAssetsDir.path
            let pkgPath = wallpaper.resolvedEntryURL.path
            NSLog("[Mirage] 场景参数: assets=\(assetsPath) exists=\(FileManager.default.fileExists(atPath: assetsPath)) pkg=\(pkgPath) exists=\(FileManager.default.fileExists(atPath: pkgPath))")
            args += [assetsPath, pkgPath]
            args += ["--fps", String(options.fps)]
            args += ["--render-scale", String(format: "%.3f", options.renderScale)]
            args += ["--msaa", String(options.msaaSamples)]
            if options.loadFromMemory { args += ["--load-from-memory"] }
            args += ["--screen", String(screenIndex)]
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
            args += ["--volume", String(format: "%.3f", options.muted ? 0 : options.volume)]
            args += ["--screen", String(screenIndex)]
            if options.enableSpectrum {
                args += ["--external-spectrum"]
            } else {
                args += ["--no-spectrum"]
            }
            args += ["--control-stdin"]

        case .video:
            args += [wallpaper.renderDirectory.path]
            args += ["--screen", String(screenIndex)]
            args += ["--volume", String(format: "%.3f", options.volume)]
            args += ["--fill", options.fillMode.rawValue]
            if options.loadFromMemory { args += ["--load-from-memory"] }
            if options.muted { args += ["--muted"] }
            args += ["--control-stdin"]

        case .unsupported:
            return false
        }

        let stdinPipe = Pipe()
        let stdoutPipe = (deferredScene || wallpaper.kind == .web) ? Pipe() : nil
        let stderrPipe = Pipe()
        proc.standardInput = stdinPipe
        if let stdoutPipe {
            proc.standardOutput = stdoutPipe
        } else {
            proc.standardOutput = FileHandle.standardOutput
        }
        proc.standardError = stderrPipe

        let handle = RendererProcess(
            process: proc,
            stdinPipe: stdinPipe,
            stdoutPipe: stdoutPipe,
            stderrPipe: stderrPipe,
            wallpaper: wallpaper,
            screenIndex: screenIndex,
            generation: generation,
            desiredVolume: options.volume,
            desiredMuted: options.muted,
            spectrumEnabled: options.enableSpectrum &&
                (wallpaper.kind == .scene || wallpaper.kind == .web))

        if wallpaper.kind == .scene,
           let propsFile = writeUserPropertiesFile(options.userProperties, for: wallpaper) {
            args += ["--user-properties", propsFile.path]
            handle.tempFiles.append(propsFile)
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
                NSLog("[Mirage] 渲染器信号退出: \(sigName) core=\(coreDumped) 屏幕=\(screenIndex)")
            }
            NSLog("[Mirage] 渲染器退出 (屏幕=\(screenIndex), 状态=\(status), 原因=\(reason.rawValue))")
            guard let self else { return }
            self.queue.async {
                let wasActive = self.running[screenIndex] === handle
                let wasCandidate = self.candidates[screenIndex] === handle
                if wasActive {
                    self.running[screenIndex] = nil
                }
                if wasCandidate {
                    self.candidates[screenIndex] = nil
                }
                if wasActive || wasCandidate {
                    let abnormal = status != 0 && !handle.isTerminated
                    SystemAudioSpectrumService.shared.setEnabled(
                        self.hasSpectrumConsumersLocked())
                    DispatchQueue.main.async { self.onProcessExit?(screenIndex, abnormal) }
                }
            }
        }

        do {
            try proc.run()
        } catch {
            NSLog("[Mirage] 启动渲染器失败: \(error)")
            handle.cleanupAfterExit()
            return false
        }

        var accepted = false
        queue.sync {
            // A renderer can fail immediately after Process.run() succeeds.
            // Never register an already-dead handle after its termination
            // callback has raced past the controller queue.
            guard generations[screenIndex] == generation, proc.isRunning else { return }
            if deferredScene {
                candidates[screenIndex] = handle
            } else {
                running[screenIndex] = handle
            }
            accepted = true
        }
        guard accepted else {
            handle.stop()
            return false
        }
        refreshAudioSpectrumService()

        handle.startReadingOutput { [weak self, weak handle] event in
            guard let self, let handle else { return }
            self.handleLifecycleEvent(event, from: handle)
        }

        applyInitialProperties(options.userProperties, to: handle)
        if deferredScene {
            queue.asyncAfter(deadline: .now() + Self.sceneStartupTimeout) { [weak self, weak handle] in
                guard let self, let handle,
                      self.candidates[screenIndex] === handle else { return }
                self.candidates[screenIndex] = nil
                handle.stop()
                SystemAudioSpectrumService.shared.setEnabled(
                    self.hasSpectrumConsumersLocked())
                NSLog("[Mirage] 场景首帧超时，保留旧壁纸 (屏幕=\(screenIndex), generation=\(generation))")
                DispatchQueue.main.async { self.onProcessExit?(screenIndex, true) }
            }
        }
        return true
    }

    private func handleLifecycleEvent(_ message: [String: Any], from handle: RendererProcess) {
        guard let event = message["event"] as? String else { return }
        let elapsed = (message["elapsed_ms"] as? NSNumber)?.intValue
        queue.async { [weak self, weak handle] in
            guard let self, let handle,
                  self.generations[handle.screenIndex] == handle.generation else { return }

            if event == "audio-demand" {
                guard self.running[handle.screenIndex] === handle ||
                      self.candidates[handle.screenIndex] === handle else { return }
                handle.spectrumDemanded = (message["needed"] as? NSNumber)?.boolValue ?? false
                SystemAudioSpectrumService.shared.setEnabled(self.hasSpectrumConsumersLocked())
                return
            }

            guard self.candidates[handle.screenIndex] === handle else { return }

            switch event {
            case "vulkan-ready", "scene-ready":
                if let elapsed {
                    NSLog("[Mirage] 场景启动阶段 \(event): \(elapsed)ms 屏幕=\(handle.screenIndex)")
                }

            case "first-frame-presented":
                if let elapsed {
                    NSLog("[Mirage] 场景候选首帧已显示: \(elapsed)ms 屏幕=\(handle.screenIndex)")
                }
                handle.send(["cmd": "activate"])

            case "activated":
                let previous = self.running.updateValue(handle, forKey: handle.screenIndex)
                self.candidates[handle.screenIndex] = nil
                // The candidate is visibly covering the old window before the
                // old process is stopped, so switching never exposes desktop
                // or a black placeholder. Unmute only after the old audio is
                // on its way out.
                previous?.stop()
                handle.send(["cmd": "volume", "value": handle.desiredVolume])
                handle.send(["cmd": "muted", "value": handle.desiredMuted])
                handle.send(["cmd": handle.desiredPaused ? "pause" : "resume"])
                SystemAudioSpectrumService.shared.setEnabled(
                    self.hasSpectrumConsumersLocked())
                NSLog("[Mirage] 场景切换完成 (屏幕=\(handle.screenIndex), generation=\(handle.generation))")

            default:
                break
            }
        }
    }

    func stop(on screenIndex: Int) {
        let handles: [RendererProcess] = queue.sync {
            generations[screenIndex] = (generations[screenIndex] ?? 0) &+ 1
            return [running.removeValue(forKey: screenIndex),
                    candidates.removeValue(forKey: screenIndex)].compactMap { $0 }
        }
        handles.forEach { $0.stop() }
        refreshAudioSpectrumService()
    }

    func stopAll() {
        let handles: [RendererProcess] = queue.sync {
            let result = Array(running.values) + Array(candidates.values)
            for screen in Set(running.keys).union(candidates.keys) {
                generations[screen] = (generations[screen] ?? 0) &+ 1
            }
            running.removeAll()
            candidates.removeAll()
            return result
        }
        handles.forEach { $0.stop() }
        SystemAudioSpectrumService.shared.setEnabled(false)
    }

    func isRendering(on screenIndex: Int) -> Bool {
        queue.sync { running[screenIndex]?.process.isRunning ?? false }
    }

    func currentWallpaper(on screenIndex: Int) -> WEWallpaper? {
        queue.sync { running[screenIndex]?.wallpaper }
    }

    var activeScreens: [Int] {
        queue.sync { Array(running.keys).sorted() }
    }

    var processIdentifiers: Set<pid_t> {
        queue.sync {
            Set((Array(running.values) + Array(candidates.values)).compactMap { handle in
                handle.process.isRunning ? handle.process.processIdentifier : nil
            })
        }
    }

    // MARK: Live control (broadcast or per-screen)

    private func forEach(_ screenIndex: Int?, includeCandidates: Bool = true,
                         _ body: (RendererProcess) -> Void) {
        queue.sync {
            if let s = screenIndex {
                if let p = running[s] { body(p) }
                if includeCandidates, let p = candidates[s] { body(p) }
            } else {
                for (_, p) in running { body(p) }
                if includeCandidates {
                    for (_, p) in candidates { body(p) }
                }
            }
        }
    }

    private func hasSpectrumConsumersLocked() -> Bool {
        running.values.contains { self.needsSpectrum($0) }
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
            running.values.filter { self.needsSpectrum($0) }
        }
        for process in processes {
            guard process.wallpaper.kind == .scene || process.wallpaper.kind == .web else { continue }
            process.sendSpectrum(spectrum)
        }
    }

    func setVolume(_ volume: Float, on screenIndex: Int? = nil) {
        forEach(screenIndex) {
            $0.desiredVolume = volume
            $0.send(["cmd": "volume", "value": volume])
        }
    }

    func setMuted(_ muted: Bool, on screenIndex: Int? = nil) {
        // A scene candidate must remain silent until activation. Remember the
        // latest policy for it, but only send the live mute command to active
        // renderers.
        queue.sync {
            if let screenIndex {
                candidates[screenIndex]?.desiredMuted = muted
                if let active = running[screenIndex] {
                    active.desiredMuted = muted
                    active.send(["cmd": "muted", "value": muted])
                }
            } else {
                for candidate in candidates.values { candidate.desiredMuted = muted }
                for active in running.values {
                    active.desiredMuted = muted
                    active.send(["cmd": "muted", "value": muted])
                }
            }
        }
    }

    func pause(on screenIndex: Int? = nil) {
        updatePaused(true, on: screenIndex)
    }

    func resume(on screenIndex: Int? = nil) {
        updatePaused(false, on: screenIndex)
    }

    private func updatePaused(_ paused: Bool, on screenIndex: Int?) {
        // Pausing a candidate before its first frame would prevent the
        // presentation handshake from ever completing. Defer that state until
        // activation, just like unmuting.
        queue.sync {
            if let screenIndex {
                candidates[screenIndex]?.desiredPaused = paused
                if let active = running[screenIndex] {
                    active.desiredPaused = paused
                    active.send(["cmd": paused ? "pause" : "resume"])
                }
            } else {
                for candidate in candidates.values { candidate.desiredPaused = paused }
                for active in running.values {
                    active.desiredPaused = paused
                    active.send(["cmd": paused ? "pause" : "resume"])
                }
            }
        }
        refreshAudioSpectrumService()
    }

    func setFps(_ fps: Int, on screenIndex: Int? = nil) {
        forEach(screenIndex) { $0.send(["cmd": "fps", "value": fps]) }
    }

    func setSpeed(_ speed: Float, on screenIndex: Int? = nil) {
        forEach(screenIndex) { $0.send(["cmd": "speed", "value": speed]) }
    }

    func setFillMode(_ mode: FillMode, on screenIndex: Int? = nil) {
        forEach(screenIndex) { $0.send(["cmd": "fillmode", "value": mode.rawValue]) }
    }

    func setProperty(key: String, property: WEProjectProperty, on screenIndex: Int? = nil) {
        forEach(screenIndex) { proc in
            proc.send(Self.propertyCommand(key: key, property: property))
        }
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
        guard handle.wallpaper.kind == .web else { return }
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
    }
}
