//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation
import IOSurface

@objc protocol SnapshotRegressionProtocol {
    func snapshot(reply: @escaping (NSObject?, Int32, NSError?) -> Void)
    func settings(enabled: Bool, reply: @escaping (NSObject?, Int32, NSError?) -> Void)
}

private struct RegressionFailure: Error, CustomStringConvertible {
    let description: String
}

private func require(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw RegressionFailure(description: message) }
}

private final class SnapshotService: NSObject, SnapshotRegressionProtocol {
    func settings(enabled: Bool, reply: @escaping (NSObject?, Int32, NSError?) -> Void) {
        do {
            let display = MirageLockDisplayConfiguration(displayID: 7, wallpaperID: "preserved-wallpaper",
                title: "Preserved wallpaper", kind: "scene", renderDirectory: "/tmp/fixture",
                entryPath: "/tmp/fixture/scene.pkg", previewPath: nil, desktopFallbackPath: nil,
                rawProperties: [:], fps: 30, fillMode: "cover", loadFromMemory: true)
            let configuration = MirageLockConfiguration(version: 2, enabled: enabled, displays: ["display-7": display])
            let models = try buildMirageSettingsViewModels(configuration: configuration)
            reply(models as? NSObject, getpid(), nil)
        } catch {
            reply(nil, getpid(), error as NSError)
        }
    }

    func snapshot(reply: @escaping (NSObject?, Int32, NSError?) -> Void) {
        let context = CGContext(data: nil, width: 4, height: 4, bitsPerComponent: 8,
            bytesPerRow: 16, space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        context.setFillColor(CGColor(colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!, components: [1, 0, 0, 1])!)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        let snapshot = MirageSnapshotProvider.makeSnapshot(from: context.makeImage()!) as? NSObject
        reply(snapshot, ProcessInfo.processInfo.processIdentifier,
              snapshot == nil ? MirageLockBridge.failure("Snapshot construction failed") : nil)
    }
}

private func snapshotInterface() -> NSXPCInterface {
    let interface = NSXPCInterface(with: SnapshotRegressionProtocol.self)
    let classes = NSSet(objects: NSClassFromString("WallpaperSnapshotXPC")!, NSError.self) as! Set<AnyHashable>
    interface.setClasses(classes, for: #selector(SnapshotRegressionProtocol.snapshot(reply:)),
                         argumentIndex: 0, ofReply: true)
    let settingsClasses = NSSet(objects: NSClassFromString("WallpaperSettingsViewModelsXPC")!, NSError.self) as! Set<AnyHashable>
    interface.setClasses(settingsClasses, for: #selector(SnapshotRegressionProtocol.settings(enabled:reply:)),
                         argumentIndex: 0, ofReply: true)
    return interface
}

private final class SnapshotListener: NSObject, NSXPCListenerDelegate {
    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        connection.exportedInterface = snapshotInterface()
        connection.exportedObject = SnapshotService()
        connection.resume()
        return true
    }
}

private final class SnapshotResult: @unchecked Sendable {
    private let lock = NSLock()
    private var result: Result<Void, Error>?

    func finish(_ value: Result<Void, Error>) {
        lock.lock()
        if result == nil { result = value }
        lock.unlock()
    }

    func read() -> Result<Void, Error>? {
        lock.lock()
        defer { lock.unlock() }
        return result
    }
}

private final class RegistrySimulation {
    var commands: [[String]] = []
    var restarts = 0
    var duplicate = false
    var responds = true
    var time: TimeInterval = 0
    var onCommand: (() -> Void)?

    func system(extensionURL: URL, identifier: String) -> WallpaperExtensionController.System {
        let path = MirageLockBridge.normalizedPath(extensionURL)
        return .init(run: { [self] executable, arguments in
            commands.append([executable] + arguments)
            onCommand?()
            if arguments.first?.hasPrefix("-m") == true {
                var output = "+    \(identifier)(1)\t11111111-1111-1111-1111-111111111111\t2026-09-12 00:00:00 +0000\t\(path)\n"
                if duplicate, arguments.first == "-mADv" {
                    output += "+    \(identifier)(1)\t22222222-2222-2222-2222-222222222222\t2026-09-12 00:00:00 +0000\t/tmp/Obsolete Build/Mirage.appex\n"
                }
                return (true, output)
            }
            if arguments.first == "-r" { duplicate = false }
            return (true, "")
        }, restart: { [self] _, isCurrent in
            if !isCurrent() { throw CancellationError() }
            restarts += 1
        }, selected: { _ in true }, reports: { [self] probe, url, fingerprint, _ in
            guard responds else { return [] }
            return [MirageLockReport(probeID: probe.id, instanceID: UUID(), processID: getpid(),
                extensionPath: MirageLockBridge.normalizedPath(url), fingerprint: fingerprint,
                version: "2", configurationDigest: probe.configurationDigest,
                settingsReady: true, readyDisplayIDs: [7], error: nil)]
        }, now: { [self] in time }, sleep: { [self] duration in time += duration })
    }
}

@main
private struct DynamicLockScreenRegression {
    static func main() {
        guard dlopen("/System/Library/PrivateFrameworks/WallpaperExtensionKit.framework/WallpaperExtensionKit", RTLD_NOW) != nil else {
            fatalError("WallpaperExtensionKit unavailable")
        }
        if Bundle.main.bundleIdentifier?.hasSuffix(".Service") == true {
            let listener = NSXPCListener.service()
            let delegate = SnapshotListener()
            listener.delegate = delegate
            withExtendedLifetime(delegate) { listener.resume() }
            return
        }
        do {
            try testRegistry()
            try testRequestCancellation()
            try testConfigurationAndSettings()
            try testRegistrationLifecycle()
            try testExtensionProcessDiscovery()
            try testToolTimeout()
            try testSnapshotAcrossProcesses()
            try testSettingsAcrossProcesses()
            print("PASS: registry, cancellation, configuration preservation, disabled catalog, registration lifecycle, extension process discovery, tool timeout, cross-process snapshot and settings")
        } catch {
            fputs("FAIL: \(error)\n", stderr)
            exit(1)
        }
    }

    static func testRegistry() throws {
        let id = "cn.laobamac.Mirage.WallpaperExtension"
        let output = """
        +    \(id)((null))\t11111111-1111-1111-1111-111111111111\t2026-09-12 00:00:00 +0000\t/Applications/Mirage.app/Contents/Extensions/MirageWallpaperExtension.appex
        +    \(id)(1.1.2)\t22222222-2222-2222-2222-222222222222\t2026-09-12 00:00:00 +0000\t/tmp/Build Products/Mirage Wallpaper.app/Contents/Extensions/MirageWallpaperExtension.appex
        -    \(id).Other(1.1.2)\t33333333-3333-3333-3333-333333333333\t2026-09-12 00:00:00 +0000\t/tmp/Other.appex
         (3 plug-ins)
        """
        let records = WallpaperExtensionRecord.parse(output, identifier: id)
        try require(records.count == 2, "Duplicate physical instances were not preserved")
        try require(records.allSatisfy(\.elected), "Election state parsing failed")
        try require(records[1].path.contains("Build Products"), "A path containing spaces was truncated")
    }

    static func testRequestCancellation() throws {
        let requests = WallpaperExtensionRequest()
        let first = requests.begin()
        let second = requests.begin()
        try require(!requests.isCurrent(first) && requests.isCurrent(second), "An obsolete enable request remains active")
        requests.cancel()
        try require(!requests.isCurrent(second), "Disable did not cancel pending registration")
    }

    static func testConfigurationAndSettings() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-lock-fixture-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let display = MirageLockDisplayConfiguration(displayID: 7, wallpaperID: "preserved-wallpaper",
            title: "Preserved wallpaper", kind: "scene", renderDirectory: root.path,
            entryPath: root.appendingPathComponent("scene.pkg").path, previewPath: nil,
            desktopFallbackPath: nil, rawProperties: [:], fps: 30, fillMode: "cover", loadFromMemory: true)
        let configuration = MirageLockConfiguration(version: 2, enabled: false, displays: ["display-7": display])
        let data = try JSONEncoder().encode(configuration)
        let url = root.appendingPathComponent(MirageLockBridge.configurationName)
        try data.write(to: url)
        let store = MirageLockConfigurationStore(containerURL: root)
        let loaded = try store.load()
        try require(loaded.configuration.enabled == false, "Disabled configuration was discarded")
        let models = try buildMirageSettingsViewModels(configuration: loaded.configuration)
        let groups = Mirror(reflecting: models).descendant("box", "rawValue", "desktop", "some", "groups") as? [Any]
        try require(groups?.isEmpty == true, "A disabled lock screen still publishes wallpaper choices")
        let enabled = MirageLockConfiguration(version: configuration.version, enabled: true, displays: configuration.displays)
        try JSONEncoder().encode(enabled).write(to: url, options: .atomic)
        let restored = try store.load()
        let restoredModels = try buildMirageSettingsViewModels(configuration: restored.configuration)
        let restoredGroups = Mirror(reflecting: restoredModels).descendant("box", "rawValue", "desktop", "some", "groups") as? [Any]
        let items = restoredGroups?.first.flatMap { Mirror(reflecting: $0).descendant("items") as? [Any] }
        let identifier = items?.first.flatMap { Mirror(reflecting: $0).descendant("id", "id") as? String }
        try require(identifier == "display-7", "Re-enabling did not restore the same system choice identity")
        try require(restored.configuration.displays["display-7"]?.entryPath == display.entryPath,
                    "Re-enabling changed the saved wallpaper")
        try Data("{invalid".utf8).write(to: url)
        var rejected = false
        do { _ = try store.load() } catch { rejected = true }
        try require(rejected, "A read/decode error was treated as an empty catalog")
        try require(store.lastKnownConfiguration?.displays["display-7"]?.wallpaperID == "preserved-wallpaper", "Read failure discarded the last good configuration")
        let probe = MirageLockProbe(configurationData: data)
        try MirageLockBridge.writeProbe(probe, in: root)
        let report = MirageLockReport(probeID: probe.id, instanceID: UUID(), processID: getpid(),
            extensionPath: MirageLockBridge.normalizedPath(root), fingerprint: "new-build", version: "419",
            configurationDigest: probe.configurationDigest, settingsReady: true, readyDisplayIDs: [7], error: nil)
        try report.write(in: root)
        try require(report.matches(probe, extensionURL: root, fingerprint: "new-build"), "A valid extension acknowledgement was rejected")
        try require(!report.matches(probe, extensionURL: root, fingerprint: "old-build"), "An obsolete extension was accepted after an update")
        try require(!report.matches(MirageLockProbe(configurationData: data), extensionURL: root, fingerprint: "new-build"), "A stale acknowledgement was accepted")
    }

    static func testToolTimeout() throws {
        let start = ProcessInfo.processInfo.systemUptime
        let result = WallpaperExtensionController.runTool("/bin/sleep", arguments: ["5"], timeout: 0.1)
        try require(!result.success && result.output.contains("timed out"), "Tool timeout was not reported")
        try require(ProcessInfo.processInfo.systemUptime - start < 3, "Tool timeout blocked the lifecycle worker")
    }

    static func testRegistrationLifecycle() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-lifecycle-fixture-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let app = root.appendingPathComponent("Mirage.app")
        let extensionURL = app.appendingPathComponent("Contents/Extensions/MirageWallpaperExtension.appex")
        let identifier = "cn.laobamac.Mirage.RegressionExtension"
        let info = try PropertyListSerialization.data(fromPropertyList: [
            "CFBundleIdentifier": identifier, "CFBundleVersion": "2", "CFBundlePackageType": "XPC!",
            "CFBundleExecutable": "MirageWallpaperExtension"
        ], format: .xml, options: 0)
        let files: [String: Data] = [
            "Contents/Info.plist": info,
            "Contents/MacOS/MirageWallpaperExtension": Data("executable fixture".utf8),
            "Contents/Frameworks/libMirageSceneSaver.dylib": Data("renderer fixture".utf8),
            "Contents/Resources/vulkan/icd.d/MoltenVK_icd.json": Data("{}".utf8)
        ]
        for (path, data) in files {
            let url = extensionURL.appendingPathComponent(path)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: url)
        }
        try FileManager.default.createDirectory(at: extensionURL.appendingPathComponent("Contents/Resources/assets"), withIntermediateDirectories: true)
        let display = MirageLockDisplayConfiguration(displayID: 7, wallpaperID: "preserved-video",
            title: "Preserved video", kind: "video", renderDirectory: root.path,
            entryPath: root.appendingPathComponent("video.mp4").path, previewPath: nil,
            desktopFallbackPath: nil, rawProperties: [:], fps: 30, fillMode: "cover", loadFromMemory: true)
        let configuration = try JSONEncoder().encode(MirageLockConfiguration(
            version: 2, enabled: true, displays: ["display-7": display]))
        let configurationURL = root.appendingPathComponent(MirageLockBridge.configurationName)
        try configuration.write(to: configurationURL)
        let fingerprint = try MirageLockBridge.fingerprint(at: extensionURL)
        let normal = RegistrySimulation()
        for _ in 0..<20 {
            let registration = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
                container: root, previousFingerprint: fingerprint, forceRestart: false,
                system: normal.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
            try require(registration.errorMessage == nil && registration.report != nil, "A healthy extension was not acknowledged")
        }
        try require(normal.restarts == 0, "Repeated activation restarted an unchanged extension")
        try require(normal.commands.allSatisfy { $0.dropFirst().first == "-mADv" }, "Repeated activation modified registration")
        let update = RegistrySimulation()
        _ = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: "previous-build", forceRestart: false,
            system: update.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(update.restarts == 1, "An update did not reload the extension exactly once")
        let preserved = try Data(contentsOf: configurationURL)
        try require(preserved == configuration, "Upgrade recovery rewrote the selected wallpaper")
        let duplicates = RegistrySimulation()
        duplicates.duplicate = true
        _ = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: duplicates.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(duplicates.commands.contains { $0.contains("-r") && $0.last == "/tmp/Obsolete Build/Mirage.appex" }, "Duplicate registrations were not corrected")
        let missing = RegistrySimulation()
        missing.responds = false
        let failed = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: missing.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(failed.errorMessage != nil && failed.report == nil && missing.restarts == 1,
                    "A missing runtime acknowledgement was reported as success or retried without a bound")
        let cancellation = RegistrySimulation()
        let requests = WallpaperExtensionRequest()
        let request = requests.begin()
        cancellation.onCommand = { requests.cancel() }
        var cancelled = false
        do {
            _ = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
                container: root, previousFingerprint: "previous-build", forceRestart: false,
                system: cancellation.system(extensionURL: extensionURL, identifier: identifier),
                isCurrent: { requests.isCurrent(request) })
        } catch is CancellationError { cancelled = true }
        try require(cancelled && cancellation.restarts == 0 && cancellation.commands.count == 1,
                    "A cancelled enable request continued changing system services")
    }

    static func testSnapshotAcrossProcesses() throws {
        let result = SnapshotResult()
        let connection = NSXPCConnection(serviceName: Bundle.main.bundleIdentifier! + ".Service")
        connection.remoteObjectInterface = snapshotInterface()
        connection.resume()
        defer { connection.invalidate() }
        let proxy = connection.remoteObjectProxyWithErrorHandler { result.finish(.failure($0)) } as! SnapshotRegressionProtocol
        proxy.snapshot { snapshot, processID, error in
            result.finish(Result {
                if let error { throw error }
                try require(processID != getpid(), "Snapshot did not cross a process boundary")
                guard let snapshot, let cls = object_getClass(snapshot),
                      let storage = class_getInstanceVariable(cls, "rawValue") else {
                    throw RegressionFailure(description: "Snapshot was not decoded")
                }
                let pointer = Unmanaged.passUnretained(snapshot).toOpaque()
                    .advanced(by: ivar_getOffset(storage)).load(as: UnsafeMutableRawPointer.self)
                let surface = Unmanaged<IOSurface>.fromOpaque(pointer).takeUnretainedValue()
                try require(surface.width == 4 && surface.height == 4, "IOSurface dimensions were corrupted")
                surface.lock(options: .readOnly, seed: nil)
                defer { surface.unlock(options: .readOnly, seed: nil) }
                let pixel = surface.baseAddress.assumingMemoryBound(to: UInt8.self)
                try require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255,
                            "Unexpected snapshot pixel: \(Array(UnsafeBufferPointer(start: pixel, count: 4)))")
            })
        }
        let deadline = Date().addingTimeInterval(15)
        while result.read() == nil, Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
        guard let value = result.read() else { throw RegressionFailure(description: "Cross-process snapshot timed out") }
        try value.get()
    }

    static func testExtensionProcessDiscovery() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-process-fixture-\(UUID().uuidString).appex")
        defer { try? FileManager.default.removeItem(at: root) }
        let executable = root.appendingPathComponent("Contents/MacOS/MirageWallpaperExtension")
        try FileManager.default.createDirectory(at: executable.deletingLastPathComponent(), withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: URL(fileURLWithPath: "/bin/sleep"), to: executable)
        let identifier = "cn.laobamac.Mirage.LockRegression.Process"
        let info = try PropertyListSerialization.data(fromPropertyList: [
            "CFBundleIdentifier": identifier, "CFBundleVersion": "1", "CFBundlePackageType": "XPC!",
            "CFBundleExecutable": "MirageWallpaperExtension"
        ], format: .xml, options: 0)
        try info.write(to: root.appendingPathComponent("Contents/Info.plist"))
        let process = Process()
        process.executableURL = executable
        process.arguments = ["10"]
        try process.run()
        defer { if process.isRunning { process.terminate() } }
        let deadline = Date().addingTimeInterval(2)
        var discovered: WallpaperExtensionProcess?
        repeat {
            discovered = WallpaperExtensionProcess.running(identifier: identifier)
                .first { $0.processID == process.processIdentifier }
            if discovered == nil { Thread.sleep(forTimeInterval: 0.02) }
        } while discovered == nil && Date() < deadline
        guard let discovered else { throw RegressionFailure(description: "A running extension outside NSRunningApplication was not discovered") }
        let stale = WallpaperExtensionProcess(processID: discovered.processID, executablePath: discovered.executablePath,
            startSeconds: discovered.startSeconds + 1, startMicroseconds: discovered.startMicroseconds)
        try require(!stale.terminate() && process.isRunning, "Process identity was not verified before termination")
        try require(discovered.terminate(), "The verified extension process was not terminated")
        process.waitUntilExit()
        try require(!discovered.isRunning, "An exited extension is still treated as running")
    }

    static func testSettingsAcrossProcesses() throws {
        for enabled in [true, false, true] {
            try testSettingsAcrossProcesses(enabled: enabled)
        }
    }

    static func testSettingsAcrossProcesses(enabled: Bool) throws {
        let result = SnapshotResult()
        let connection = NSXPCConnection(serviceName: Bundle.main.bundleIdentifier! + ".Service")
        connection.remoteObjectInterface = snapshotInterface()
        connection.resume()
        defer { connection.invalidate() }
        let proxy = connection.remoteObjectProxyWithErrorHandler { result.finish(.failure($0)) } as! SnapshotRegressionProtocol
        proxy.settings(enabled: enabled) { models, processID, error in
            result.finish(Result {
                if let error { throw error }
                try require(processID != getpid(), "Settings did not cross a process boundary")
                guard let models else { throw RegressionFailure(description: "Settings were not decoded") }
                let groups = Mirror(reflecting: models).descendant("box", "rawValue", "desktop", "some", "groups") as? [Any]
                if !enabled {
                    try require(groups?.isEmpty == true, "The disabled desktop catalog still contains choices after XPC decoding")
                    return
                }
                let items = groups?.first.flatMap { Mirror(reflecting: $0).descendant("items") as? [Any] }
                let identifier = items?.first.flatMap { Mirror(reflecting: $0).descendant("id", "id") as? String }
                try require(identifier == "display-7", "The desktop catalog was lost across XPC")
            })
        }
        let deadline = Date().addingTimeInterval(15)
        while result.read() == nil, Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
        guard let value = result.read() else { throw RegressionFailure(description: "Cross-process settings timed out") }
        try value.get()
    }
}
