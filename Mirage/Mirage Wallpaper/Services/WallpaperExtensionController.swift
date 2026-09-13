//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Darwin
import Foundation

enum WallpaperExtensionController {
    struct Registration {
        let fingerprint: String
        let probe: MirageLockProbe
        let report: MirageLockReport?
        var errorMessage: String? = nil
    }

    struct System {
        var run: (String, [String]) -> (success: Bool, output: String)
        var restart: (String, () -> Bool) throws -> Void
        var selected: (String) -> Bool
        var reports: (MirageLockProbe, URL, String, URL) -> [MirageLockReport]
        var now: () -> TimeInterval
        var sleep: (TimeInterval) -> Void

        static var live: Self {
            Self(run: { runTool($0, arguments: $1) }, restart: restartServices,
                 selected: isSelected, reports: liveReports,
                 now: { ProcessInfo.processInfo.systemUptime }, sleep: Thread.sleep(forTimeInterval:))
        }
    }

    static func register(appURL: URL, extensionURL: URL, container: URL,
                         previousFingerprint: String?, forceRestart: Bool,
                         system: System = .live,
                         isCurrent: () -> Bool) throws -> Registration {
        WallpaperServiceCoordinator.lock.lock()
        defer { WallpaperServiceCoordinator.lock.unlock() }
        try check(isCurrent)
        let fingerprint = try MirageLockBridge.fingerprint(at: extensionURL)
        guard let identifier = Bundle(url: extensionURL)?.bundleIdentifier else {
            throw MirageLockBridge.failure("Wallpaper extension identifier is missing")
        }
        let targetPath = MirageLockBridge.normalizedPath(extensionURL)
        let previous = try records(identifier: identifier, all: true, run: system.run)
        let conflicts = previous.filter { $0.path != targetPath }
        let versionChanged = previousFingerprint != fingerprint
        let registrationNeeded = versionChanged || !conflicts.isEmpty
            || !previous.contains(where: { $0.path == targetPath && $0.elected })
        if registrationNeeded {
            for record in conflicts {
                try check(isCurrent)
                NSLog("[MirageLock] removing duplicate registration: %@", record.path)
                try runRequired("/usr/bin/pluginkit", ["-r", record.path], run: system.run)
            }
            try check(isCurrent)
            try runRequired("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
                            ["-f", appURL.path], run: system.run)
            var found = false
            for attempt in 0..<3 {
                try check(isCurrent)
                if attempt > 0 { system.sleep(0.3) }
                try runRequired("/usr/bin/pluginkit", ["-a", extensionURL.path], run: system.run)
                try runRequired("/usr/bin/pluginkit", ["-e", "use", "-i", identifier], run: system.run)
                let selected = try records(identifier: identifier, all: false, run: system.run)
                if selected.contains(where: { $0.path == targetPath && $0.elected }) {
                    found = true
                    break
                }
            }
            guard found else {
                throw MirageLockBridge.failure("The system selected a different copy of the wallpaper extension")
            }
        }
        try check(isCurrent)
        let data = try Data(contentsOf: container.appendingPathComponent(MirageLockBridge.configurationName))
        let probe = MirageLockProbe(configurationData: data)
        try MirageLockBridge.writeProbe(probe, in: container)
        let selected = system.selected(identifier)
        var restarted = false
        if versionChanged || forceRestart || registrationNeeded {
            try system.restart(identifier, isCurrent)
            restarted = true
        }
        for recovery in 0..<2 {
            try check(isCurrent)
            MirageLockBridge.post(MirageLockBridge.probeNotification)
            let deadline = system.now() + (selected ? 5 : 1)
            var reportedError: String?
            while system.now() < deadline {
                try check(isCurrent)
                let reports = system.reports(probe, extensionURL, fingerprint, container)
                reportedError = reports.compactMap(\.error).first
                if reportedError == nil,
                   let report = reports.filter({
                       $0.settingsReady && $0.configurationDigest == probe.configurationDigest
                   }).max(by: { $0.readyDisplayIDs.count < $1.readyDisplayIDs.count }) {
                    NSLog("[MirageLock] extension connected: %@ build %@ pid %d",
                          report.extensionPath, report.version, report.processID)
                    return Registration(fingerprint: fingerprint, probe: probe, report: report)
                }
                system.sleep(0.1)
            }
            if let reportedError {
                return Registration(fingerprint: fingerprint, probe: probe, report: nil, errorMessage: reportedError)
            }
            if !selected {
                return Registration(fingerprint: fingerprint, probe: probe, report: nil)
            }
            guard recovery == 0, !restarted else { break }
            try system.restart(identifier, isCurrent)
            restarted = true
        }
        return Registration(fingerprint: fingerprint, probe: probe, report: nil,
                            errorMessage: "The selected wallpaper extension did not acknowledge its configuration")
    }

    static func liveReports(probe: MirageLockProbe, extensionURL: URL,
                            fingerprint: String, container: URL) -> [MirageLockReport] {
        let path = MirageLockBridge.normalizedPath(extensionURL)
        return MirageLockBridge.readReports(in: container).filter {
            $0.probeID == probe.id && $0.extensionPath == path && $0.fingerprint == fingerprint
                && $0.processID > 0 && (Darwin.kill($0.processID, 0) == 0 || errno == EPERM)
        }
    }

    static func records(identifier: String, all: Bool,
                        run: (String, [String]) -> (success: Bool, output: String) = { runTool($0, arguments: $1) }) throws -> [WallpaperExtensionRecord] {
        let result = run("/usr/bin/pluginkit", [all ? "-mADv" : "-mv", "-i", identifier])
        guard result.success else { throw MirageLockBridge.failure(result.output) }
        return WallpaperExtensionRecord.parse(result.output, identifier: identifier)
    }

    static func runTool(_ executable: String, arguments: [String], timeout: TimeInterval = 10)
        -> (success: Bool, output: String) {
        let process = Process()
        let outputURL = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-extension-\(UUID().uuidString).log")
        defer { try? FileManager.default.removeItem(at: outputURL) }
        do {
            try Data().write(to: outputURL, options: .withoutOverwriting)
            let handle = try FileHandle(forWritingTo: outputURL)
            defer { try? handle.close() }
            let completion = DispatchSemaphore(value: 0)
            process.executableURL = URL(fileURLWithPath: executable)
            process.arguments = arguments
            process.standardOutput = handle
            process.standardError = handle
            process.terminationHandler = { _ in completion.signal() }
            try process.run()
            let timedOut = completion.wait(timeout: .now() + timeout) == .timedOut
            if timedOut {
                process.terminate()
                if completion.wait(timeout: .now() + 0.5) == .timedOut, process.isRunning {
                    _ = Darwin.kill(process.processIdentifier, SIGKILL)
                    _ = completion.wait(timeout: .now() + 1)
                }
            }
            let output = String(data: try Data(contentsOf: outputURL), encoding: .utf8) ?? ""
            if timedOut { return (false, "\(executable) timed out\n\(output)") }
            return (!process.isRunning && process.terminationStatus == 0, output)
        } catch {
            return (false, error.localizedDescription)
        }
    }

    private static func runRequired(_ executable: String, _ arguments: [String],
                                    run: (String, [String]) -> (success: Bool, output: String)) throws {
        let result = run(executable, arguments)
        guard result.success else {
            throw MirageLockBridge.failure("\(URL(fileURLWithPath: executable).lastPathComponent): \(result.output)")
        }
    }

    private static func check(_ isCurrent: () -> Bool) throws {
        if !isCurrent() { throw CancellationError() }
    }

    private static func restartServices(identifier: String, isCurrent: () -> Bool) throws {
        try check(isCurrent)
        let agentID = "com.apple.wallpaper.agent"
        let agents = NSRunningApplication.runningApplications(withBundleIdentifier: agentID)
        let extensions = WallpaperExtensionProcess.running(identifier: identifier)
        guard !agents.isEmpty || !extensions.isEmpty else { return }
        extensions.forEach { $0.terminate() }
        agents.forEach { _ = $0.terminate() }
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while ProcessInfo.processInfo.systemUptime < deadline,
              agents.contains(where: { !hasExited($0) }) || extensions.contains(where: \.isRunning) {
            try check(isCurrent)
            Thread.sleep(forTimeInterval: 0.05)
        }
        extensions.filter(\.isRunning).forEach { $0.terminate(force: true) }
        for application in agents where !hasExited(application) {
            if !application.forceTerminate() { _ = Darwin.kill(application.processIdentifier, SIGKILL) }
        }
        let recoveryDeadline = ProcessInfo.processInfo.systemUptime + 5
        let oldAgentIDs = Set(agents.map(\.processIdentifier))
        while ProcessInfo.processInfo.systemUptime < recoveryDeadline {
            try check(isCurrent)
            let relaunched = NSRunningApplication.runningApplications(withBundleIdentifier: agentID)
                .contains { !oldAgentIDs.contains($0.processIdentifier) && !hasExited($0) }
            if agents.allSatisfy(hasExited), !extensions.contains(where: \.isRunning),
               agents.isEmpty || relaunched { return }
            Thread.sleep(forTimeInterval: 0.1)
        }
        throw MirageLockBridge.failure("Wallpaper services did not restart successfully")
    }

    private static func hasExited(_ application: NSRunningApplication) -> Bool {
        application.isTerminated || (Darwin.kill(application.processIdentifier, 0) != 0 && errno == ESRCH)
    }

    private static func isSelected(identifier: String) -> Bool {
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/com.apple.wallpaper/Store/Index.plist")
        guard let data = try? Data(contentsOf: url),
              let root = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else { return false }
        func contains(_ value: Any) -> Bool {
            if let dictionary = value as? [String: Any] {
                if dictionary["Provider"] as? String == identifier { return true }
                return dictionary.values.contains(where: contains)
            }
            return (value as? [Any])?.contains(where: contains) ?? false
        }
        return [root["Displays"], root["SystemDefault"]].compactMap { $0 }.contains(where: contains)
    }
}
