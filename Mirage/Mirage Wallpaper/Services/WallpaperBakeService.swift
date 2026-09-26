//
//  WallpaperBakeService.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import AVFoundation
import Combine
import CryptoKit
import Foundation
import Darwin

struct WallpaperBakeSettings: Codable, Equatable {
    var width = 1920
    var height = 1080
    var fps = 30
    var duration = 30
    var warmup = 2
    var audio = false
    var highQuality = false
    var seed: UInt32 = 1

    var isValid: Bool {
        (128...4096).contains(width) && (128...4096).contains(height) &&
        width.isMultiple(of: 2) && height.isMultiple(of: 2) && [24, 30, 60].contains(fps) &&
        (1...600).contains(duration) && (0...10).contains(warmup)
    }

    var estimatedBytes: Int64 {
        guard isValid else { return 0 }
        let bitrate = min(80_000_000, max(2_000_000, Double(width * height * fps) * (highQuality ? 0.16 : 0.09)))
        return Int64((bitrate + (audio ? 192_000 : 0)) * Double(duration) / 8)
    }
}

enum WallpaperBakeError: Error, LocalizedError {
    case code(String)
    var errorDescription: String? {
        switch self {
        case .code(let value):
            switch value {
            case "cancelled": return L("烘焙已取消")
            case "capture_permission": return L("网页烘焙需要屏幕录制权限。请在系统设置中允许 Mirage，重新启动后重试。")
            case "capture_too_slow": return L("网页捕获速度不足，请降低分辨率或帧率后重试。")
            case "capture_timeout", "render_timeout": return L("烘焙等待画面超时，请检查壁纸资源后重试。")
            case "source_changed": return L("壁纸源文件在烘焙期间发生变化，请重新烘焙。")
            case "disk_space": return L("可用磁盘空间不足，无法完成烘焙。")
            case "helper_missing": return L("未找到烘焙组件，请重新构建或安装完整的 Mirage。")
            case "invalid_request": return L("烘焙参数无效，请检查尺寸、时长和播放速度。")
            case "hdr_unsupported": return L("当前烘焙输出仅支持 SDR，不能烘焙 HDR 视频。")
            case "encoder_unavailable": return L("当前设备无法以所选尺寸启动视频编码器。")
            case "decode_failed", "invalid_source": return L("无法读取壁纸或解码其视频。")
            case "audio_failed": return L("无法读取壁纸音轨，烘焙未完成。")
            default: return L("烘焙失败，未生成壁纸。请检查任务日志后重试。")
            }
        }
    }
}

final class WallpaperBakeCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    private var process: Process?

    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }

    func launch(_ process: Process) throws {
        lock.lock()
        defer { lock.unlock() }
        if stopped { throw WallpaperBakeError.code("cancelled") }
        try process.run()
        self.process = process
    }

    func clearProcess() { lock.lock(); process = nil; lock.unlock() }

    func cancel() {
        lock.lock(); stopped = true; let running = process; lock.unlock()
        guard let running, running.isRunning else { return }
        running.terminate()
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 3) {
            if running.isRunning { kill(running.processIdentifier, SIGKILL) }
        }
    }

    func check() throws { if isCancelled { throw WallpaperBakeError.code("cancelled") } }
}

@MainActor
final class WallpaperBakeService: ObservableObject {
    static let shared = WallpaperBakeService()

    struct Job: Identifiable {
        let id: UUID
        let wallpaper: WEWallpaper
        let settings: WallpaperBakeSettings
        var state = "queued"
        var progress = 0.0
        var error: String?
        var output: URL?
        var log: URL?
        var finished = false
        let cancellation: WallpaperBakeCancellation
    }

    struct Request {
        let id: UUID
        let title: String
        let wallpaper: WEWallpaper
        let settings: WallpaperBakeSettings
        let snapshot: WallpaperRenderSnapshot
        let volume: Float
        let destination: URL
        let cancellation: WallpaperBakeCancellation
    }

    @Published var presentedWallpaper: WEWallpaper?
    @Published var showsTasks = false
    @Published private(set) var jobs: [Job] = []
    private var pending: [Request] = []
    private var running = false

    func enqueue(_ wallpaper: WEWallpaper, settings: WallpaperBakeSettings) async {
        guard settings.isValid, wallpaper.presentationIsValid else { return }
        let model = AppDelegate.shared.wallpaperViewModel
        await model.refreshScriptStorage(for: wallpaper)
        let runtime = model.loadRuntime(for: wallpaper)
        let properties = model.effectiveProperties(for: wallpaper, runtime: runtime)
        let snapshot = model.renderSnapshots(for: wallpaper.id)[model.selectedDisplayKey.rawValue] ??
            WallpaperRenderSnapshot(runtime: runtime, properties: properties,
                scriptStorage: WallpaperRenderSnapshot.storedScriptStorage(for: wallpaper))
        let id = UUID(), cancellation = WallpaperBakeCancellation()
        jobs.append(Job(id: id, wallpaper: wallpaper, settings: settings, cancellation: cancellation))
        pending.append(Request(id: id, title: L("%@ · 已烘焙", wallpaper.project.title), wallpaper: wallpaper, settings: settings, snapshot: snapshot,
            volume: runtime.muted ? 0 : runtime.volume, destination: WallpaperLibrary.shared.importedDirectory,
            cancellation: cancellation))
        presentedWallpaper = nil
        showsTasks = true
        startNext()
    }

    func cancel(_ id: UUID) {
        guard let index = jobs.firstIndex(where: { $0.id == id }), !jobs[index].finished else { return }
        jobs[index].cancellation.cancel()
        if let pendingIndex = pending.firstIndex(where: { $0.id == id }) {
            pending.remove(at: pendingIndex)
            jobs[index].state = "cancelled"; jobs[index].finished = true
        } else { jobs[index].state = "cancelling" }
    }

    func cancelAll() { jobs.filter { !$0.finished }.forEach { cancel($0.id) } }
    func clearFinished() { jobs.removeAll { $0.finished } }

    private func startNext() {
        guard !running, !pending.isEmpty else { return }
        running = true
        let request = pending.removeFirst()
        Task {
            let result: Result<URL, Error> = await withCheckedContinuation { continuation in
                DispatchQueue.global(qos: .utility).async {
                    let result = Result { try Self.run(request) { event, progress, log in
                        DispatchQueue.main.sync {
                            MainActor.assumeIsolated {
                                guard let index = self.jobs.firstIndex(where: { $0.id == request.id }) else { return }
                                guard !self.jobs[index].finished else { return }
                                if !self.jobs[index].cancellation.isCancelled { self.jobs[index].state = event }
                                self.jobs[index].progress = max(self.jobs[index].progress, progress)
                                if let log { self.jobs[index].log = log }
                            }
                        }
                    } }
                    continuation.resume(returning: result)
                }
            }
            if let index = jobs.firstIndex(where: { $0.id == request.id }) {
                jobs[index].finished = true
                switch result {
                case .success(let url):
                    jobs[index].state = "complete"; jobs[index].progress = 1; jobs[index].output = url
                    AppDelegate.shared.contentViewModel.refresh()
                case .failure(let error):
                    jobs[index].state = request.cancellation.isCancelled ? "cancelled" : "failed"
                    jobs[index].error = error.localizedDescription
                }
            }
            running = false
            startNext()
        }
    }

    nonisolated private static func run(_ job: Request, update: @escaping (String, Double, URL?) -> Void) throws -> URL {
        let fm = FileManager.default
        let stage = job.destination.appendingPathComponent(".mirage-bake-\(job.id.uuidString)", isDirectory: true)
        let logRoot = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appending(path: "Mirage/Baking/Logs")
        try fm.createDirectory(at: stage, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: stage) }
        try fm.createDirectory(at: logRoot, withIntermediateDirectories: true)
        let logURL = logRoot.appendingPathComponent("\(job.id.uuidString).log")
        fm.createFile(atPath: logURL.path, contents: nil)
        update("checking", 0, logURL)
        let available = try job.destination.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage ?? 0
        guard available > job.settings.estimatedBytes * 2 + 512 * 1024 * 1024 else { throw WallpaperBakeError.code("disk_space") }
        try job.cancellation.check()
        guard job.settings.isValid, job.snapshot.speed.isFinite, (0.1...4).contains(job.snapshot.speed) else {
            throw WallpaperBakeError.code("invalid_request")
        }
        let source = job.wallpaper.resolvedEntryURL
        let roots = [job.wallpaper.renderDirectory] + job.wallpaper.assetOverlayDirectories
        let before = try digest(roots, cancellation: job.cancellation)
        let projectRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let resources = Bundle.main.resourceURL ?? projectRoot
        let name: String
        switch job.wallpaper.kind {
        case .scene: name = "SceneBaker"
        case .web: name = "WebBaker"
        case .video: name = "VideoBaker"
        case .unsupported: throw WallpaperBakeError.code("invalid_source")
        }
        let bundled = resources.appending(path: "Renderers/\(name)")
        #if arch(arm64)
        let preset = "macos-arm64-clang-release"
        #else
        let preset = "macos-clang-release"
        #endif
        let relative = job.wallpaper.kind == .scene ? "SceneRenderer/build/\(preset)" : job.wallpaper.kind == .web ? "WebRenderer/build/release" : "VideoRenderer/build/release"
        let binary = fm.isExecutableFile(atPath: bundled.path) ? bundled : projectRoot.appending(path: "\(relative)/Tools/\(name)/\(name)")
        guard fm.isExecutableFile(atPath: binary.path) else { throw WallpaperBakeError.code("helper_missing") }
        let assetBundle = resources.appending(path: "assets")
        let assets = fm.fileExists(atPath: assetBundle.path) ? assetBundle : projectRoot.appending(path: "assets")
        let cache = stage.appending(path: "cache")
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let output = stage.appending(path: "wallpaper.mp4")
        var properties = job.snapshot.rawProperties.mapValues(\.foundationValue)
        if job.wallpaper.kind == .web {
            properties = job.snapshot.properties(for: job.wallpaper).mapValues { ["value": $0.value.jsonObjectValue] }
        }
        let request: [String: Any] = [
            "schema": 1, "source": source.path, "directory": job.wallpaper.renderDirectory.path,
            "overlays": job.wallpaper.assetOverlayDirectories.map(\.path), "assets": assets.path,
            "cache": cache.path, "output": output.path, "properties": properties,
            "storage": job.snapshot.scriptStorage ?? [:], "width": job.settings.width, "height": job.settings.height,
            "fps": job.settings.fps, "duration": job.settings.duration, "warmup": job.settings.warmup,
            "audio": job.settings.audio && job.wallpaper.kind != .web, "volume": job.volume,
            "speed": job.wallpaper.kind == .web ? 1.0 : Double(job.snapshot.speed), "seed": job.settings.seed,
            "fillMode": job.snapshot.fillMode, "positionX": job.snapshot.position.x, "positionY": job.snapshot.position.y,
            "quality": job.settings.highQuality ? "high" : "standard"
        ]
        let requestURL = stage.appending(path: "request.json")
        try JSONSerialization.data(withJSONObject: request, options: [.sortedKeys]).write(to: requestURL, options: .atomic)
        let process = Process(), pipe = Pipe()
        process.executableURL = binary; process.arguments = [requestURL.path]
        process.currentDirectoryURL = stage
        process.standardInput = FileHandle.nullDevice; process.standardOutput = pipe
        let log = try FileHandle(forWritingTo: logURL)
        defer { try? log.close() }
        process.standardError = log
        var environment = ProcessInfo.processInfo.environment
        for key in environment.keys where key.hasPrefix("SCENERENDERER_") { environment.removeValue(forKey: key) }
        let icds = [resources.appending(path: "Renderers/vulkan/icd.d/MoltenVK_icd.json"),
            URL(fileURLWithPath: "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"),
            URL(fileURLWithPath: "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json")]
        if let icd = icds.first(where: { fm.fileExists(atPath: $0.path) }) {
            environment["VK_ICD_FILENAMES"] = icd.path; environment["VK_DRIVER_FILES"] = icd.path
        }
        environment["DYLD_FALLBACK_LIBRARY_PATH"] = Bundle.main.bundleURL.appending(path: "Contents/Frameworks").path + ":/usr/local/lib:/opt/homebrew/lib"
        process.environment = environment
        let activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Mirage wallpaper baking")
        defer { ProcessInfo.processInfo.endActivity(activity) }
        try job.cancellation.launch(process)
        defer { job.cancellation.clearProcess() }
        try? pipe.fileHandleForWriting.close()
        defer { try? pipe.fileHandleForReading.close() }
        var buffer = Data(), complete = false, failure: String?
        while true {
            let data = try pipe.fileHandleForReading.read(upToCount: 8192) ?? Data()
            if data.isEmpty { break }
            buffer.append(data)
            if buffer.count > 1_048_576 { job.cancellation.cancel(); throw WallpaperBakeError.code("render_failed") }
            while let newline = buffer.firstIndex(of: 10) {
                let line = buffer.prefix(upTo: newline); buffer.removeSubrange(...newline)
                try log.write(contentsOf: line + Data([10]))
                guard let event = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any], let state = event["event"] as? String else { continue }
                if state == "complete" { complete = true }
                if state == "error" { failure = event["code"] as? String ?? "render_failed" }
                let current = event["completed"] as? Double ?? 0, total = event["total"] as? Double ?? 1
                let fraction = min(1, max(0, current / max(1, total)))
                let progress: Double
                switch state {
                case "warming", "progress" where job.wallpaper.kind == .scene:
                    let warmupFrames = Double(job.settings.warmup * job.settings.fps)
                    let outputFrames = Double(job.settings.duration * job.settings.fps)
                    let done = state == "warming" ? current : warmupFrames + current
                    progress = min(0.97, done / max(1, warmupFrames + outputFrames) * 0.97)
                case "progress": progress = fraction * 0.97
                default: progress = 0
                }
                update(state, progress, nil)
            }
        }
        process.waitUntilExit()
        try job.cancellation.check()
        guard process.terminationStatus == 0, complete, failure == nil else { throw WallpaperBakeError.code(failure ?? "render_failed") }
        update("verifying", 0.98, nil)
        guard before == (try digest(roots, cancellation: job.cancellation)) else { throw WallpaperBakeError.code("source_changed") }
        let generator = AVAssetImageGenerator(asset: AVURLAsset(url: output))
        generator.appliesPreferredTrackTransform = true
        let image = try generator.copyCGImage(at: .zero, actualTime: nil)
        guard let preview = NSBitmapImageRep(cgImage: image).representation(using: .jpeg,
            properties: [.compressionFactor: 0.85]) else { throw WallpaperBakeError.code("decode_failed") }
        try preview.write(to: stage.appending(path: "preview.jpg"), options: .atomic)
        var project = WEProject(author: job.wallpaper.project.resolvedAuthor, contentrating: job.wallpaper.project.contentrating,
            file: "wallpaper.mp4", preview: "preview.jpg", tags: job.wallpaper.project.tags,
            title: job.title, type: "video")
        project.mirageBake = WallpaperBakeMetadata(id: job.id, sourceTitle: job.wallpaper.project.title,
            sourceKind: job.wallpaper.kind.rawValue, sourceDigest: before, createdAt: Date(),
            width: job.settings.width, height: job.settings.height, fps: job.settings.fps, duration: job.settings.duration)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(project).write(to: stage.appending(path: "project.json"), options: .atomic)
        var receipt = request
        receipt.removeValue(forKey: "output"); receipt.removeValue(forKey: "cache")
        receipt["sourceDigest"] = before; receipt["rendererDigest"] = try digest([binary], cancellation: job.cancellation)
        try JSONSerialization.data(withJSONObject: receipt, options: [.prettyPrinted, .sortedKeys]).write(to: stage.appending(path: "bake.json"), options: .atomic)
        try? fm.removeItem(at: cache); try? fm.removeItem(at: requestURL)
        try? fm.removeItem(at: stage.appending(path: "decoded.mp4"))
        try job.cancellation.check()
        let destination = job.destination.appendingPathComponent("Mirage-Baked-\(job.id.uuidString)", isDirectory: true)
        try fm.moveItem(at: stage, to: destination)
        return destination
    }

    nonisolated private static func digest(_ roots: [URL], cancellation: WallpaperBakeCancellation) throws -> String {
        var hash = SHA256()
        for root in roots {
            let isDirectory = (try root.resourceValues(forKeys: [.isDirectoryKey])).isDirectory == true
            let files: [URL]
            if isDirectory {
                guard let enumerator = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey],
                    options: [.skipsHiddenFiles]) else { throw WallpaperBakeError.code("invalid_source") }
                files = enumerator.compactMap { $0 as? URL }.sorted { $0.path < $1.path }
            } else { files = [root] }
            for file in files {
                try cancellation.check()
                guard (try file.resourceValues(forKeys: [.isRegularFileKey])).isRegularFile == true else { continue }
                hash.update(data: Data(file.path.utf8)); hash.update(data: Data([0]))
                let handle = try FileHandle(forReadingFrom: file)
                defer { try? handle.close() }
                while let data = try handle.read(upToCount: 1_048_576), !data.isEmpty {
                    try cancellation.check(); hash.update(data: data)
                }
            }
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
