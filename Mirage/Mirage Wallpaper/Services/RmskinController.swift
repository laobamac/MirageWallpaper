//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation

// Launches and controls the RmskinWallpaper host process for the currently
// applied widget theme. Independent of RendererController (widgets are multi-
// window floating components with a different control protocol than the single
// scene/web/video wallpapers), but reuses the same subprocess + stdin-JSON idea.
final class RmskinController {
    private var process: Process?
    private var stdinPipe: Pipe?
    private(set) var currentTheme: RmskinTheme?
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.rmskin")

    var onExit: ((Bool) -> Void)?

    // MARK: - Binary resolution (mirrors RendererController)

    private var resourcesDir: URL { Bundle.main.resourceURL ?? Bundle.main.bundleURL }
    private var renderersDir: URL { resourcesDir.appending(path: "Renderers") }

    private static let projectRoot: URL = {
        let srcURL = URL(fileURLWithPath: #filePath)
        return srcURL
            .deletingLastPathComponent() // Services
            .deletingLastPathComponent() // Mirage Wallpaper
            .deletingLastPathComponent() // Mirage
            .deletingLastPathComponent() // repo root
    }()

    private static let devFallback: URL =
        projectRoot.appending(path: "RmskinRenderer/build/release/Tools/RmskinWallpaper/RmskinWallpaper")

    private func binaryURL() -> URL? {
        let bundled = renderersDir.appending(path: "RmskinWallpaper")
        if FileManager.default.fileExists(atPath: bundled.path) { return bundled }
        if FileManager.default.fileExists(atPath: Self.devFallback.path) { return Self.devFallback }
        return nil
    }

    // MARK: - Apply / stop

    var isRunning: Bool {
        queue.sync { process?.isRunning ?? false }
    }

    @discardableResult
    func apply(_ theme: RmskinTheme, screenIndex: Int = 0) -> Bool {
        guard let binary = binaryURL() else {
            NSLog("[Mirage] 找不到 RmskinWallpaper 渲染器二进制")
            return false
        }
        // Multi-theme: no longer call stop() here — each theme gets its
        // own controller instance. Caller is responsible for lifecycle.

        let proc = Process()
        proc.executableURL = binary
        var args = [theme.themeDirectory.path]
        if !theme.load.isEmpty     { args += ["--load", theme.load] }
        if !theme.loadType.isEmpty { args += ["--load-type", theme.loadType] }
        args += ["--screen", String(screenIndex), "--control-stdin"]
        proc.arguments = args

        let stdin = Pipe()
        let stderr = Pipe()
        proc.standardInput = stdin
        proc.standardOutput = FileHandle.standardOutput
        proc.standardError = stderr

        proc.terminationHandler = { [weak self] p in
            let stderrData = try? stderr.fileHandleForReading.readToEnd()
            if let s = stderrData.flatMap({ String(data: $0, encoding: .utf8) }), !s.isEmpty {
                NSLog("[Mirage] RmskinWallpaper stderr:\n\(s)")
            }
            let abnormal = p.terminationStatus != 0
            self?.queue.async {
                if self?.process === p { self?.process = nil }
                DispatchQueue.main.async { self?.onExit?(abnormal) }
            }
        }

        do {
            try proc.run()
        } catch {
            NSLog("[Mirage] 启动 RmskinWallpaper 失败: \(error)")
            return false
        }
        queue.sync {
            process = proc
            stdinPipe = stdin
            currentTheme = theme
        }
        return true
    }

    func reload() { send(["cmd": "reload"]) }

    func stop() {
        queue.sync {
            guard let proc = process else { return }
            send(["cmd": "quit"], locked: true)
            try? stdinPipe?.fileHandleForWriting.close()
            let p = proc
            DispatchQueue.global().asyncAfter(deadline: .now() + 1.2) {
                if p.isRunning { p.terminate() }
            }
            process = nil
            stdinPipe = nil
            currentTheme = nil
        }
    }

    // MARK: - Control channel

    private func send(_ command: [String: Any], locked: Bool = false) {
        let work = {
            guard let pipe = self.stdinPipe, self.process?.isRunning == true else { return }
            guard var data = try? JSONSerialization.data(withJSONObject: command) else { return }
            data.append(0x0A)
            try? pipe.fileHandleForWriting.write(contentsOf: data)
        }
        if locked { work() } else { queue.sync(execute: work) }
    }
}
