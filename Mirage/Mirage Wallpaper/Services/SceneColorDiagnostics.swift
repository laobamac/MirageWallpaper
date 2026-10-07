private let sceneColorDiagnosticsCopyright = "Copyright © 2026 王孝慈. All rights reserved."

import AppKit
import CryptoKit
import Foundation
import Metal
import SwiftUI
import UniformTypeIdentifiers

struct SceneDiagnosticRequest {
    var package: URL
    var assets: URL
    var renderer: URL
    var frameworks: URL
    var payload: URL
    var properties: Data
    var runtime: Data
    var arguments: [String]
    var metadata: [String: String]
    var fastMath: String
    var metalFX: Bool
}

struct SceneDiagnosticCase: Identifiable, Codable, Equatable {
    var id: String
    var title: String
    var library: String
    var fastMath: String
    var metalFX: Bool
    var status = "pending"
    var observation: String?
    var exitStatus: Int32?
    var reason: String?

    static func plan(fastMath: String, metalFX: Bool) -> [Self] {
        var result = [
            Self(id: "A", title: "诊断：当前依赖", library: "installed", fastMath: fastMath, metalFX: metalFX),
            Self(id: "R", title: "诊断：受控基线", library: "baseline", fastMath: fastMath, metalFX: metalFX),
            Self(id: "B", title: "诊断：官方保护修复", library: "patched", fastMath: fastMath, metalFX: metalFX),
            Self(id: "C", title: "诊断：关闭快速数学", library: "installed", fastMath: "0", metalFX: metalFX),
            Self(id: "D", title: "诊断：关闭 MetalFX", library: "installed", fastMath: fastMath, metalFX: false),
        ]
        if fastMath == "0" {
            result[3].status = "skipped"
            result[3].reason = "fast_math_already_disabled"
        }
        if !metalFX {
            result[4].status = "skipped"
            result[4].reason = "metalfx_already_disabled"
        }
        return result
    }

    static func conclusion(_ cases: [Self]) -> String {
        func observed(_ id: String, _ value: String) -> Bool {
            cases.contains { $0.id == id && $0.status == "completed" && $0.observation == value }
        }
        guard observed("A", "green") else { return "baseline_not_reproduced_or_unconfirmed" }
        if observed("R", "green") && observed("B", "normal") { return "controlled_patch_supported_not_yet_release_verified" }
        if observed("R", "normal") { return "dependency_change_requires_further_analysis" }
        if observed("C", "normal") { return "fast_math_path_requires_analysis" }
        if observed("D", "normal") { return "metalfx_path_requires_analysis" }
        return "stage_data_requires_analysis"
    }
}

@MainActor
final class SceneColorDiagnostics: ObservableObject {
    @Published private(set) var cases: [SceneDiagnosticCase] = []
    @Published private(set) var running = false
    @Published private(set) var ready = false
    @Published private(set) var capturing = false
    @Published private(set) var currentTitle = ""
    @Published private(set) var errorText = ""
    @Published private(set) var finished = false
    @Published private(set) var exporting = false
    private let request: SceneDiagnosticRequest
    private var task: Task<Void, Never>?
    private var process: Process?
    private var input: Pipe?
    private var activeIndex: Int?
    private var root: URL?
    private var report: URL?
    private var observation: String?
    private var cancelled = false
    private var rawLog: URL?
    private var logHandle: FileHandle?
    var reportURL: URL? { report }
    private var manifest: [String: Any] = [:]
    private static let maximumLogBytes = 4 * 1024 * 1024

    init(request: SceneDiagnosticRequest) { self.request = request }

    nonisolated static func sha256(_ url: URL) throws -> String {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var hash = SHA256()
        while let bytes = try file.read(upToCount: 1024 * 1024), !bytes.isEmpty { hash.update(data: bytes) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func sanitized(_ text: String, home: String) -> String {
        var result = text.replacingOccurrences(of: home, with: "<HOME>")
        for pattern in [
            #"(?i)(\"?(?:token|password|api[_-]?key|access[_-]?token|refresh[_-]?token|steamguard)\"?\s*[:=]\s*)(?:\"[^\"]*\"|[^\s,}]+)"#,
            #"(?i)([?&](?:token|key|password|api_key)=)[^&\s]+"#,
        ] {
            result = result.replacingOccurrences(of: pattern, with: "$1<REDACTED>", options: .regularExpression)
        }
        return result
    }

    func start() {
        guard !running, !finished else { return }
        running = true
        cancelled = false
        cases = SceneDiagnosticCase.plan(fastMath: request.fastMath, metalFX: request.metalFX)
        task = Task {
            do {
                try await prepare()
                for index in cases.indices {
                    try Task.checkCancellation()
                    if cases[index].status == "skipped" { continue }
                    try await runCase(index)
                    try writeReport()
                    if index == 0 && cases[index].observation != "green" {
                        for next in cases.indices where cases[next].status == "pending" {
                            cases[next].status = "skipped"
                            cases[next].reason = "baseline_not_reproduced_or_unconfirmed"
                        }
                        break
                    }
                }
            } catch is CancellationError {
                cancelled = true
            } catch {
                errorText = Self.sanitized(error.localizedDescription, home: FileManager.default.homeDirectoryForCurrentUser.path)
            }
            await stopProcess()
            if let index = activeIndex, cases[index].status == "running" {
                cases[index].status = cancelled ? "cancelled" : "failed"
            }
            for index in cases.indices where cases[index].status == "pending" {
                cases[index].status = "skipped"
                cases[index].reason = cancelled ? "session_cancelled" : "session_failed"
            }
            do { try writeReport() } catch { errorText = error.localizedDescription }
            running = false
            ready = false
            finished = true
            task = nil
        }
    }

    private func prepare() async throws {
        let fm = FileManager.default
        for file in [request.package, request.renderer, request.frameworks.appending(path: "libMoltenVK.dylib"),
                     request.payload.appending(path: "baseline/libMoltenVK.dylib"),
                     request.payload.appending(path: "patched/libMoltenVK.dylib"), request.payload.appending(path: "manifest.json")] {
            guard fm.fileExists(atPath: file.path) else {
                throw NSError(domain: "SceneDiagnostics", code: 1, userInfo: [NSLocalizedDescriptionKey: L("诊断资源不完整，请使用专用诊断构建。")])
            }
        }
        let directory = fm.temporaryDirectory.appending(path: "Mirage-Scene-Diagnostics-\(UUID().uuidString)")
        try fm.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        root = directory
        let report = directory.appending(path: "Report")
        self.report = report
        try fm.createDirectory(at: report, withIntermediateDirectories: false)
        try request.properties.write(to: directory.appending(path: "properties.json"), options: .atomic)
        try request.runtime.write(to: directory.appending(path: "runtime.json"), options: .atomic)
        let payloadData = try Data(contentsOf: request.payload.appending(path: "manifest.json"))
        let payloadManifest = try JSONSerialization.jsonObject(with: payloadData)
        manifest = ["schema": 1, "copyright": sceneColorDiagnosticsCopyright,
                    "created_at": ISO8601DateFormatter().string(from: Date()),
                    "application": request.metadata, "os": ProcessInfo.processInfo.operatingSystemVersionString,
                    "controlled_dependency": payloadManifest,
                    "controls": ["animation_step": "1/30", "freeze_after_scene_seconds": 20,
                                 "cursor": "center", "audio": "silence", "cache": "isolated",
                                 "wall_clock_date": "not_frozen", "observation": "live_window_not_readback"],
                    "gpus": MTLCopyAllDevices().map { ["name": $0.name, "registry_id": String($0.registryID),
                                                      "unified_memory": String($0.hasUnifiedMemory)] }]
        let properties = try JSONSerialization.jsonObject(with: request.properties)
        let safeProperties = Self.redactedProperties(properties)
        try Self.writeJSON(safeProperties, to: report.appending(path: "properties-redacted.json"))
        let runtime = try JSONSerialization.jsonObject(with: request.runtime) as? [String: Any] ?? [:]
        manifest["runtime"] = ["speed": runtime["speed"] ?? 1,
                               "script_storage_entries": (runtime["scriptStorage"] as? [String: Any])?.count ?? 0,
                               "script_storage_included": false]
        let files = ["package": request.package, "renderer": request.renderer,
                     "installed": request.frameworks.appending(path: "libMoltenVK.dylib"),
                     "baseline": request.payload.appending(path: "baseline/libMoltenVK.dylib"),
                     "patched": request.payload.appending(path: "patched/libMoltenVK.dylib")]
        let hashes = try await Task.detached(priority: .utility) {
            try files.mapValues { try Self.sha256($0) }
        }.value
        manifest["sha256"] = hashes
        manifest["arguments"] = request.arguments
        try writeReport()
    }

    static func redactedProperties(_ object: Any) -> Any {
        guard let properties = object as? [String: Any] else { return [:] }
        return properties.mapValues { value -> Any in
            if value is NSNumber { return value }
            if let string = value as? String,
               string.range(of: #"^[-+0-9.eE ,]+$"#, options: .regularExpression) != nil { return string }
            if let descriptor = value as? [String: Any] {
                return ["type": descriptor["type"] as? String ?? "unknown", "value": "<REDACTED>"]
            }
            return "<REDACTED>"
        }
    }

    private func runCase(_ index: Int) async throws {
        guard let root, let report else { return }
        activeIndex = index
        cases[index].status = "running"
        currentTitle = cases[index].title
        ready = false
        observation = nil
        let test = cases[index]
        let directory = report.appending(path: test.id)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let cache = root.appending(path: "cache-\(test.id)")
        try FileManager.default.createDirectory(at: cache, withIntermediateDirectories: false)
        let library = test.library == "installed"
            ? request.frameworks.appending(path: "libMoltenVK.dylib")
            : request.payload.appending(path: "\(test.library)/libMoltenVK.dylib")
        let icd = cache.appending(path: "icd.json")
        try Self.writeJSON(["file_format_version": "1.0.0", "ICD": ["library_path": library.path,
                            "api_version": "1.4.0", "is_portability_driver": true]], to: icd)
        let process = Process()
        var args = [request.assets.path, request.package.path] + request.arguments
        args += ["--control-stdin", "--muted", "--external-spectrum", "--mouse-position", "0.5,0.5",
                 "--cache-path", cache.path, "--user-properties", root.appending(path: "properties.json").path,
                 "--runtime", root.appending(path: "runtime.json").path]
        if test.metalFX { args.append("--metalfx") }
        process.executableURL = request.renderer
        process.arguments = args
        process.currentDirectoryURL = cache
        var env = ProcessInfo.processInfo.environment.filter {
            !$0.key.hasPrefix("SCENERENDERER_") && !$0.key.hasPrefix("MVK_") && !$0.key.hasPrefix("VK_") && !$0.key.hasPrefix("DYLD_")
        }
        if test.fastMath != "default" { env["MVK_CONFIG_FAST_MATH_ENABLED"] = test.fastMath }
        env["MVK_CONFIG_LOG_LEVEL"] = "3"
        env["VK_ICD_FILENAMES"] = icd.path
        env["VK_DRIVER_FILES"] = icd.path
        env["DYLD_FALLBACK_LIBRARY_PATH"] = request.frameworks.path
        env["SCENERENDERER_DIAGNOSTIC_CACHE"] = cache.appending(path: "pipeline").path
        env["SCENERENDERER_DIAGNOSTICS_DIR"] = directory.path
        env["SCENERENDERER_DUMP_FRAME"] = directory.appending(path: "frame.ppm").path
        env["SCENERENDERER_DUMP_FRAME_AT"] = "0"
        env["SCENERENDERER_DUMP_PRESENT"] = directory.appending(path: "present.ppm").path
        env["RSTD_LOG"] = "info"
        process.environment = env
        let input = Pipe()
        let rawLog = cache.appending(path: "renderer-private.log")
        FileManager.default.createFile(atPath: rawLog.path, contents: nil, attributes: [.posixPermissions: 0o600])
        let logHandle = try FileHandle(forWritingTo: rawLog)
        self.input = input
        self.rawLog = rawLog
        self.logHandle = logHandle
        process.standardInput = input
        process.standardOutput = logHandle
        process.standardError = logHandle
        self.process = process
        try process.run()
        let deadline = ProcessInfo.processInfo.systemUptime + 180
        while observation == nil && process.isRunning {
            try Task.checkCancellation()
            if ProcessInfo.processInfo.systemUptime > deadline {
                cases[index].reason = ready ? "observation_timeout" : "render_timeout"
                break
            }
            if let size = try? rawLog.resourceValues(forKeys: [.fileSizeKey]).fileSize,
               size > Self.maximumLogBytes {
                cases[index].reason = "log_limit_exceeded"
                break
            }
            if !ready && FileManager.default.fileExists(atPath: directory.appending(path: "ready").path) {
                ready = true
            }
            try await Task.sleep(nanoseconds: 200_000_000)
        }
        let didObserve = observation != nil && ready && process.isRunning
        if didObserve {
            ready = false
            capturing = true
            try Data().write(to: directory.appending(path: "capture"), options: .atomic)
            let mode = (try? Data(contentsOf: directory.appending(path: "presentation.json")))
                .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
            let metalPresentation = mode?["metalfx_presenter"] as? Bool == true
            func captureComplete() -> Bool {
                FileManager.default.fileExists(atPath: directory.appending(path: "captured").path)
                    && (!metalPresentation || FileManager.default.fileExists(atPath: directory.appending(path: "metalfx-present.ppm").path))
            }
            let captureDeadline = ProcessInfo.processInfo.systemUptime + 15
            while process.isRunning && !captureComplete()
                    && ProcessInfo.processInfo.systemUptime < captureDeadline {
                try Task.checkCancellation()
                try await Task.sleep(nanoseconds: 100_000_000)
            }
            if !captureComplete() {
                cases[index].reason = "capture_incomplete"
            }
            capturing = false
        }
        await stopProcess()
        cases[index].exitStatus = process.terminationStatus
        cases[index].observation = observation
        cases[index].status = didObserve ? "completed" : "failed"
        if !didObserve && cases[index].reason == nil { cases[index].reason = "renderer_exited_before_observation" }
        let logInput = try FileHandle(forReadingFrom: rawLog)
        let logs = try logInput.read(upToCount: Self.maximumLogBytes) ?? Data()
        try logInput.close()
        let text = Self.sanitized(String(decoding: logs, as: UTF8.self), home: FileManager.default.homeDirectoryForCurrentUser.path)
            .replacingOccurrences(of: root.path, with: "<DIAGNOSTIC_SESSION>")
            .replacingOccurrences(of: request.package.path, with: "<SCENE_PACKAGE>")
        try text.write(to: directory.appending(path: "renderer.log"), atomically: true, encoding: .utf8)
        ready = false
        activeIndex = nil
    }

    func observe(_ value: String) {
        guard ready, running, observation == nil, ["green", "normal", "other", "uncertain"].contains(value) else { return }
        observation = value
    }

    func cancel() { cancelled = true; task?.cancel() }

    private func stopProcess() async {
        guard let process else { return }
        if process.isRunning {
            process.terminate()
            for _ in 0..<20 {
                if !process.isRunning { break }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            if process.isRunning { kill(process.processIdentifier, SIGKILL) }
            await Task.detached { process.waitUntilExit() }.value
        }
        try? logHandle?.close()
        logHandle = nil
        try? input?.fileHandleForWriting.close()
        input = nil
        self.process = nil
    }

    private static func writeJSON(_ value: Any, to url: URL) throws {
        try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]).write(to: url, options: .atomic)
    }

    private func writeReport() throws {
        guard let report else { return }
        var document = manifest
        document["cases"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(cases))
        document["conclusion"] = SceneDiagnosticCase.conclusion(cases)
        document["cancelled"] = cancelled
        document["error"] = errorText
        document["readback_warning"] = "GPU readback may itself be corrupted. Compare with live-window observations. No result proves release correctness."
        try Self.writeJSON(document, to: report.appending(path: "report.json"))
    }

    func archiveReport(to destination: URL) async throws {
        guard finished, let report else { throw CocoaError(.fileReadNoSuchFile) }
        try writeReport()
        let staging = report.deletingLastPathComponent().appending(path: "export-\(UUID().uuidString).zip")
        try await Task.detached(priority: .utility) {
            defer { try? FileManager.default.removeItem(at: staging) }
            let zip = Process()
            zip.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
            zip.arguments = ["-c", "-k", "--keepParent", report.path, staging.path]
            zip.standardOutput = FileHandle.nullDevice
            zip.standardError = FileHandle.nullDevice
            try zip.run()
            zip.waitUntilExit()
            guard zip.terminationStatus == 0 else { throw CocoaError(.fileWriteUnknown) }
            try Data(contentsOf: staging, options: .mappedIfSafe).write(to: destination, options: .atomic)
        }.value
    }

    func export() {
        guard finished, !exporting, report != nil else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.zip]
        panel.nameFieldStringValue = "Mirage-Scene-Diagnostics-\(UUID().uuidString.prefix(8)).zip"
        panel.begin { [weak self] response in
            guard response == .OK, let destination = panel.url, let self else { return }
            self.exporting = true
            Task { @MainActor in
                do {
                    try await self.archiveReport(to: destination)
                    NSWorkspace.shared.activateFileViewerSelecting([destination])
                } catch { self.errorText = error.localizedDescription }
                self.exporting = false
            }
        }
    }

    func cleanup() {
        guard !running, !exporting, let root else { return }
        try? FileManager.default.removeItem(at: root)
        self.root = nil
        report = nil
    }
}

@MainActor
final class SceneColorDiagnosticWindow: NSWindowController, NSWindowDelegate {
    private static var active: SceneColorDiagnosticWindow?
    private let runner: SceneColorDiagnostics

    static func show(_ request: SceneDiagnosticRequest) {
        if let active { active.showWindow(nil); active.window?.makeKeyAndOrderFront(nil); return }
        let controller = SceneColorDiagnosticWindow(request)
        active = controller
        controller.showWindow(nil)
        controller.window?.makeKeyAndOrderFront(nil)
    }

    private init(_ request: SceneDiagnosticRequest) {
        runner = SceneColorDiagnostics(request: request)
        let window = NSWindow(contentViewController: NSHostingController(rootView: SceneColorDiagnosticView(runner: runner)))
        window.title = L("场景颜色诊断")
        window.setContentSize(NSSize(width: 540, height: 460))
        window.styleMask = [.titled, .closable]
        window.level = .floating
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
    }

    required init?(coder: NSCoder) { nil }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if runner.running { runner.cancel(); return false }
        return !runner.exporting
    }

    func windowWillClose(_ notification: Notification) {
        runner.cleanup()
        Self.active = nil
    }
}

private struct SceneColorDiagnosticView: View {
    @ObservedObject var runner: SceneColorDiagnostics

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(L("场景颜色诊断")).font(.title2.bold())
            Text(L("自动对照不会修改壁纸或保存设置。请观察每组的实际画面并选择结果；导出的图片可能与屏幕显示不同。"))
                .font(.callout)
            Text(L("报告包含设备信息、脱敏参数和壁纸渲染画面，不含桌面截图或脚本存储；不会自动上传。"))
                .font(.caption).foregroundStyle(.secondary)
            if runner.running {
                HStack {
                    if !runner.ready { ProgressView().controlSize(.small) }
                    Text(L(runner.currentTitle))
                }
                Text(L(runner.capturing ? "正在收集诊断数据…" : runner.ready ? "请根据诊断窗口中的实际壁纸画面选择，不要根据导出图片判断。" : "正在等待场景开场动画结束，首次编译可能需要较长时间。"))
                    .font(.caption)
                if runner.ready {
                    HStack {
                        Button(L("仍然发绿")) { runner.observe("green") }
                        Button(L("颜色正常")) { runner.observe("normal") }
                        Button(L("其他异常")) { runner.observe("other") }
                        Button(L("无法判断")) { runner.observe("uncertain") }
                    }
                }
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(runner.cases) { item in
                        HStack {
                            Text(item.id).monospaced()
                            Text(L(item.title))
                            Spacer()
                            Image(systemName: item.status == "completed" ? "checkmark.circle" : item.status == "failed" ? "exclamationmark.circle" : item.status == "skipped" ? "minus.circle" : "circle")
                        }
                    }
                }
            }
            if runner.finished {
                Text(L("诊断已结束。结果仅用于定位，不代表问题已经修复。请导出报告并回传到原 issue。"))
                    .font(.callout)
            }
            if !runner.errorText.isEmpty { Text(runner.errorText).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
            HStack {
                if runner.running { Button(L("取消诊断")) { runner.cancel() } }
                Spacer()
                if runner.finished {
                    Button(L("导出诊断报告…")) { runner.export() }.disabled(runner.exporting || runner.reportURL == nil)
                } else if !runner.running {
                    Button(L("开始诊断")) { runner.start() }.buttonStyle(.borderedProminent)
                }
            }
        }
        .padding(20)
    }
}
