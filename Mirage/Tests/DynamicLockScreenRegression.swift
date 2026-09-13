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
    func configuredSnapshot(_ configuration: Data, displayID: UInt32, showWallpaper: Bool,
                            reply: @escaping (NSObject?, Int32, NSError?) -> Void)
    func settings(enabled: Bool, previewPaths: [String], reply: @escaping (NSObject?, Int32, NSError?) -> Void)
}

private struct RegressionFailure: Error, CustomStringConvertible {
    let description: String
}

private func require(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw RegressionFailure(description: message) }
}

private final class SnapshotService: NSObject, SnapshotRegressionProtocol {
    func configuredSnapshot(_ configuration: Data, displayID: UInt32, showWallpaper: Bool,
                            reply: @escaping (NSObject?, Int32, NSError?) -> Void) {
        do {
            let configuration = try JSONDecoder().decode(MirageLockConfiguration.self, from: configuration)
            guard let snapshot = MirageSnapshotProvider.makeSnapshot(from: configuration, displayID: displayID,
                showWallpaper: showWallpaper) as? NSObject else {
                throw MirageLockBridge.failure("Configured snapshot construction failed")
            }
            reply(snapshot, getpid(), nil)
        } catch { reply(nil, getpid(), error as NSError) }
    }

    func settings(enabled: Bool, previewPaths: [String], reply: @escaping (NSObject?, Int32, NSError?) -> Void) {
        do {
            let displays = Dictionary(uniqueKeysWithValues: previewPaths.enumerated().map { index, path in
                let id = UInt32(index + 7)
                return ("display-\(id)", MirageLockDisplayConfiguration(displayID: id, wallpaperID: "wallpaper-\(id)",
                    title: "Wallpaper \(id)", kind: "scene", renderDirectory: "/tmp/fixture",
                    entryPath: "/tmp/fixture/scene.pkg", previewPath: previewPaths.first, desktopFallbackPath: nil,
                    rawProperties: [:], fps: 30, fillMode: "cover", loadFromMemory: true,
                    renderedPreviewPath: path.isEmpty ? nil : path))
            })
            let configuration = MirageLockConfiguration(version: 2, enabled: enabled, displays: displays)
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
    interface.setClasses(classes, for: #selector(SnapshotRegressionProtocol.configuredSnapshot(_:displayID:showWallpaper:reply:)),
                         argumentIndex: 0, ofReply: true)
    let settingsClasses = NSSet(objects: NSClassFromString("WallpaperSettingsViewModelsXPC")!, NSError.self) as! Set<AnyHashable>
    interface.setClasses(settingsClasses, for: #selector(SnapshotRegressionProtocol.settings(enabled:previewPaths:reply:)),
                         argumentIndex: 0, ofReply: true)
    interface.setClasses(NSSet(objects: NSArray.self, NSString.self) as! Set<AnyHashable>,
                         for: #selector(SnapshotRegressionProtocol.settings(enabled:previewPaths:reply:)),
                         argumentIndex: 1, ofReply: false)
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
    var selection = WallpaperExtensionController.SelectionState.selected
    var respondsAfter: TimeInterval = 0
    var requiresRegistryRecovery = false
    var hasEmptySettingsCache = false
    var requiresCacheInvalidation = false
    var cacheInvalidations = 0
    var readyDisplayIDs: [UInt32] = [7]
    var failingArgument: String?
    var transformReport: ((MirageLockReport) -> MirageLockReport)?
    var time: TimeInterval = 0
    var onCommand: (() -> Void)?
    var onSleep: ((TimeInterval) -> Void)?

    func system(extensionURL: URL, identifier: String) -> WallpaperExtensionController.System {
        let path = MirageLockBridge.normalizedPath(extensionURL)
        return .init(run: { [self] executable, arguments in
            commands.append([executable] + arguments)
            onCommand?()
            if let failingArgument, arguments.first == failingArgument {
                return (false, "Simulated registration failure")
            }
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
        }, selection: { [self] _ in selection }, reports: { [self] probe, url, fingerprint, container in
            guard responds, time >= respondsAfter else { return [] }
            if requiresRegistryRecovery,
               (restarts == 0 || !commands.contains(where: { $0.first?.hasSuffix("/lsregister") == true })) { return [] }
            if requiresCacheInvalidation, cacheInvalidations == 0 || restarts == 0 { return [] }
            let data = try? Data(contentsOf: container.appendingPathComponent(MirageLockBridge.configurationName))
            let report = MirageLockReport(probeID: probe.id, instanceID: UUID(), processID: getpid(),
                extensionPath: MirageLockBridge.normalizedPath(url), fingerprint: fingerprint,
                version: "2", configurationDigest: data.map(MirageLockBridge.digest),
                settingsReady: true, readyDisplayIDs: readyDisplayIDs, error: nil)
            return [transformReport?(report) ?? report]
        }, invalidateSettingsCache: { [self] _, policy, isCurrent in
            if !isCurrent() { throw CancellationError() }
            if policy == .emptyDesktop, !hasEmptySettingsCache { return false }
            cacheInvalidations += 1
            hasEmptySettingsCache = false
            return true
        }, notify: {}, now: { [self] in time }, sleep: { [self] duration in
            time += duration
            onSleep?(time)
        })
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
            try testSelectionState()
            try testSettingsCacheInvalidation()
            try testRegistrationLifecycle()
            try testExtensionProcessDiscovery()
            try testToolTimeout()
            try testSnapshotAcrossProcesses()
            try testConfiguredSnapshotsAcrossProcesses()
            try testSettingsAcrossProcesses()
            print("PASS: registry, cancellation, configuration preservation, disabled catalog, selection state, settings cache invalidation, registration and discovery recovery, extension process discovery, tool timeout, cross-process snapshot and settings")
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
        let disabledPolicy = Mirror(reflecting: models).descendant("box", "rawValue", "desktop", "some", "refreshPolicy")
        try require(disabledPolicy.map { String(describing: $0) } == "discretionary",
                    "Disabling the lock screen published an indefinitely cached empty catalog")
        let enabled = MirageLockConfiguration(version: configuration.version, enabled: true, displays: configuration.displays)
        try JSONEncoder().encode(enabled).write(to: url, options: .atomic)
        let restored = try store.load()
        let restoredModels = try buildMirageSettingsViewModels(configuration: restored.configuration)
        let restoredGroups = Mirror(reflecting: restoredModels).descendant("box", "rawValue", "desktop", "some", "groups") as? [Any]
        let enabledPolicy = Mirror(reflecting: restoredModels).descendant("box", "rawValue", "desktop", "some", "refreshPolicy")
        try require(enabledPolicy.map { String(describing: $0) } == "discretionary",
                    "The system did not decode the dynamic catalog refresh policy")
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

    static func testSelectionState() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-selection-fixture-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = root.appendingPathComponent("Index.plist")
        let identifier = "cn.laobamac.Mirage.RegressionExtension"
        try require(WallpaperExtensionController.selectionState(identifier: identifier, storeURL: store) == .unknown,
                    "A missing wallpaper store was treated as an unselected extension")
        try Data("invalid plist".utf8).write(to: store)
        try require(WallpaperExtensionController.selectionState(identifier: identifier, storeURL: store) == .unknown,
                    "A corrupt wallpaper store was treated as an unselected extension")
        let other: [String: Any] = ["Displays": ["display": ["Desktop": ["Content": ["Choices": [["Provider": "other"]]]]]]]
        try PropertyListSerialization.data(fromPropertyList: other, format: .binary, options: 0).write(to: store)
        try require(WallpaperExtensionController.selectionState(identifier: identifier, storeURL: store) == .notSelected,
                    "An unrelated wallpaper was treated as the Mirage extension")
        let spaces: [String: Any] = ["Spaces": ["space": ["Desktop": ["Content": ["Choices": [["Provider": identifier]]]]]]]
        try PropertyListSerialization.data(fromPropertyList: spaces, format: .binary, options: 0).write(to: store)
        try require(WallpaperExtensionController.selectionState(identifier: identifier, storeURL: store) == .selected,
                    "A wallpaper selected for a Space was not detected")
    }

    static func testSettingsCacheInvalidation() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-settings-cache-fixture-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let identifier = "cn.laobamac.Mirage.RegressionExtension"
        let desktop = root.appendingPathComponent("extension-\(identifier)-desktop")
        let screenSaver = root.appendingPathComponent("extension-\(identifier)-screenSaver")
        let other = root.appendingPathComponent("extension-\(identifier).Other-desktop")
        let selection = root.appendingPathComponent("Index.plist")
        let empty = try PropertyListSerialization.data(fromPropertyList: [
            "viewModel": ["groups": [], "refreshPolicy": ["default": [:]]], "osBuildVersion": "fixture"
        ], format: .binary, options: 0)
        let populated = try PropertyListSerialization.data(fromPropertyList: [
            "viewModel": ["groups": [["localizedName": "Mirage"]]]
        ], format: .binary, options: 0)
        try empty.write(to: desktop)
        try populated.write(to: screenSaver)
        try populated.write(to: other)
        try populated.write(to: selection)
        let removed = try WallpaperSettingsCache.invalidate(identifier: identifier, policy: .emptyDesktop,
                                                           directory: root, isCurrent: { true })
        try require(removed && !FileManager.default.fileExists(atPath: desktop.path)
                    && !FileManager.default.fileExists(atPath: screenSaver.path),
                    "An empty cached catalog was not invalidated for the current provider")
        let otherData = try Data(contentsOf: other)
        let selectionData = try Data(contentsOf: selection)
        try require(otherData == populated && selectionData == populated,
                    "Cache recovery changed another provider or the user's wallpaper selection")
        let absent = try WallpaperSettingsCache.invalidate(identifier: identifier, policy: .all,
                                                          directory: root, isCurrent: { true })
        try require(!absent, "Missing cache files were reported as invalidated")
        try populated.write(to: desktop)
        let retained = try WallpaperSettingsCache.invalidate(identifier: identifier, policy: .emptyDesktop,
                                                            directory: root, isCurrent: { true })
        let retainedData = try Data(contentsOf: desktop)
        try require(!retained && retainedData == populated, "A populated catalog was purged during passive registration")
        var cancelled = false
        do {
            try WallpaperSettingsCache.invalidate(identifier: identifier, policy: .all,
                                                 directory: root, isCurrent: { false })
        } catch is CancellationError { cancelled = true }
        try require(cancelled && FileManager.default.fileExists(atPath: desktop.path),
                    "A cancelled request invalidated the settings cache")
        let forced = try WallpaperSettingsCache.invalidate(identifier: identifier, policy: .all,
                                                          directory: root, isCurrent: { true })
        try require(forced && !FileManager.default.fileExists(atPath: desktop.path),
                    "Forced discovery recovery retained a stale populated catalog")
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
        try require(missing.commands.contains { $0.first?.hasSuffix("/lsregister") == true },
                    "Recovery restarted the host without repairing registration")
        let unopened = RegistrySimulation()
        unopened.selection = .notSelected
        unopened.responds = false
        let pending = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: unopened.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        guard case .awaitingSystemSettings = pending.outcome else {
            throw RegressionFailure(description: "A dormant extension was reported as connected or failed before opening settings")
        }
        try require(pending.report == nil && unopened.restarts == 0,
                    "Passive registration required a running extension or unnecessarily restarted the host")
        let emptyCache = RegistrySimulation()
        emptyCache.selection = .notSelected
        emptyCache.hasEmptySettingsCache = true
        emptyCache.requiresCacheInvalidation = true
        emptyCache.readyDisplayIDs = []
        let refreshedCache = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: emptyCache.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(refreshedCache.report?.settingsReady == true && emptyCache.cacheInvalidations == 1
                    && emptyCache.restarts == 1 && emptyCache.commands.count == 1,
                    "Re-enabling left the system's empty wallpaper catalog cached")
        let undiscovered = RegistrySimulation()
        undiscovered.selection = .notSelected
        undiscovered.responds = false
        let notLoaded = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false, requireAcknowledgement: true,
            system: undiscovered.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(notLoaded.errorMessage != nil && notLoaded.report == nil && undiscovered.restarts == 1,
                    "An unselected extension missing from settings was left waiting without recovery")
        try require(undiscovered.time < 85,
                    "Discovery recovery exceeded its bounded acknowledgement waits")
        let delayed = RegistrySimulation()
        delayed.selection = .notSelected
        delayed.respondsAfter = 3
        delayed.readyDisplayIDs = []
        let loaded = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false, requireAcknowledgement: true,
            system: delayed.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(loaded.report?.settingsReady == true && loaded.selection == .notSelected
                    && loaded.report?.readyDisplayIDs.isEmpty == true && delayed.restarts == 0,
                    "A delayed settings acknowledgement required wallpaper selection, rendering, or a service restart")
        let deferred = RegistrySimulation()
        deferred.selection = .notSelected
        deferred.respondsAfter = 61
        deferred.readyDisplayIDs = []
        var checkedServiceLock = false
        var serviceLockAvailable = false
        deferred.onSleep = { _ in
            guard !checkedServiceLock else { return }
            checkedServiceLock = true
            let available = DispatchSemaphore(value: 0)
            DispatchQueue.global().async {
                if WallpaperServiceCoordinator.lock.try() {
                    WallpaperServiceCoordinator.lock.unlock()
                    available.signal()
                }
            }
            serviceLockAvailable = available.wait(timeout: .now() + 1) == .success
        }
        let deferredLoad = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: true,
            forceRegistration: true, requireAcknowledgement: true,
            system: deferred.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(deferredLoad.report?.settingsReady == true && deferred.restarts == 1 && deferred.time < 75,
                    "The system's deferred catalog refresh was reported as a connection failure")
        try require(serviceLockAvailable, "Waiting for the system held the shared wallpaper service lock")
        let recoverable = RegistrySimulation()
        recoverable.selection = .notSelected
        recoverable.requiresRegistryRecovery = true
        recoverable.requiresCacheInvalidation = true
        let recovered = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false, requireAcknowledgement: true,
            system: recoverable.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(recovered.report != nil && recoverable.restarts == 1 && recoverable.cacheInvalidations == 1,
                    "An unchanged registered extension could not recover discovery without a reboot")
        let forced = RegistrySimulation()
        forced.selection = .notSelected
        forced.responds = false
        let forcedFailure = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: true,
            forceRegistration: true, requireAcknowledgement: true,
            system: forced.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(forcedFailure.errorMessage != nil && forced.restarts == 1
                    && forced.commands.filter { $0.first?.hasSuffix("/lsregister") == true }.count == 1,
                    "Explicit retry skipped registration or repeated its forced recovery")
        let unknown = RegistrySimulation()
        unknown.selection = .unknown
        unknown.responds = false
        let unknownResult = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: unknown.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(unknownResult.selection == .unknown && unknownResult.errorMessage != nil && unknown.restarts == 1,
                    "An unknown selection bypassed acknowledgement and recovery")
        let newlySelected = RegistrySimulation()
        newlySelected.selection = .notSelected
        newlySelected.responds = false
        newlySelected.onSleep = { [weak newlySelected] elapsed in
            if elapsed >= 0.3 { newlySelected?.selection = .selected }
        }
        let newlySelectedResult = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: newlySelected.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(newlySelectedResult.errorMessage != nil && newlySelected.restarts == 1,
                    "Selection changes during registration were ignored")
        let changed = RegistrySimulation()
        changed.respondsAfter = 2
        let replacement = configuration + Data("\n".utf8)
        var updateError: Error?
        changed.onSleep = { elapsed in
            guard elapsed >= 0.3 else { return }
            do { try replacement.write(to: configurationURL, options: .atomic) }
            catch { updateError = error }
        }
        let currentConfiguration = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: changed.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        if let updateError { throw updateError }
        try require(currentConfiguration.report?.configurationDigest == MirageLockBridge.digest(replacement)
                    && currentConfiguration.report?.configurationDigest != currentConfiguration.probe.configurationDigest
                    && changed.restarts == 0,
                    "A configuration update during registration invalidated the current extension acknowledgement")
        try configuration.write(to: configurationURL, options: .atomic)
        for invalidField in ["probe", "path", "fingerprint", "configuration", "settings"] {
            let stale = RegistrySimulation()
            stale.transformReport = { report in
                MirageLockReport(probeID: invalidField == "probe" ? UUID() : report.probeID,
                    instanceID: report.instanceID, processID: report.processID,
                    extensionPath: invalidField == "path" ? "/tmp/Obsolete.appex" : report.extensionPath,
                    fingerprint: invalidField == "fingerprint" ? "old-build" : report.fingerprint,
                    version: report.version,
                    configurationDigest: invalidField == "configuration" ? "old-configuration" : report.configurationDigest,
                    settingsReady: invalidField != "settings", readyDisplayIDs: report.readyDisplayIDs, error: nil)
            }
            let rejected = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
                container: root, previousFingerprint: fingerprint, forceRestart: false,
                system: stale.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
            try require(rejected.report == nil && rejected.errorMessage != nil,
                        "An invalid \(invalidField) acknowledgement was accepted")
        }
        let brokenRegistry = RegistrySimulation()
        brokenRegistry.responds = false
        brokenRegistry.failingArgument = "-a"
        let registryError = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
            container: root, previousFingerprint: fingerprint, forceRestart: false,
            system: brokenRegistry.system(extensionURL: extensionURL, identifier: identifier), isCurrent: { true })
        try require(registryError.errorMessage?.contains("Simulated registration failure") == true
                    && brokenRegistry.restarts == 0,
                    "A failed repair lost its diagnostic or continued to restart services")
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
        let waiting = RegistrySimulation()
        waiting.selection = .notSelected
        waiting.responds = false
        let waitingRequest = requests.begin()
        waiting.onSleep = { elapsed in if elapsed >= 0.3 { requests.cancel() } }
        cancelled = false
        do {
            _ = try WallpaperExtensionController.register(appURL: app, extensionURL: extensionURL,
                container: root, previousFingerprint: fingerprint, forceRestart: false, requireAcknowledgement: true,
                system: waiting.system(extensionURL: extensionURL, identifier: identifier),
                isCurrent: { requests.isCurrent(waitingRequest) })
        } catch is CancellationError { cancelled = true }
        try require(cancelled && waiting.restarts == 0 && waiting.commands.count == 1,
                    "Disabling during discovery allowed a later registration repair")
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

    static func testConfiguredSnapshotsAcrossProcesses() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-snapshot-routing-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var paths: [String] = []
        for (index, color) in [NSColor.red, .blue, .green].enumerated() {
            let context = CGContext(data: nil, width: 8, height: 4, bitsPerComponent: 8, bytesPerRow: 32,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.setFillColor(color.cgColor)
            context.fill(CGRect(x: 0, y: 0, width: 8, height: 4))
            let path = root.appendingPathComponent("frame-\(index).png")
            try NSBitmapImageRep(cgImage: context.makeImage()!).representation(using: .png, properties: [:])!.write(to: path)
            paths.append(path.path)
        }
        let displays = Dictionary(uniqueKeysWithValues: (0..<2).map { index in
            let id = UInt32(index + 7)
            return ("display-\(id)", MirageLockDisplayConfiguration(displayID: id, wallpaperID: "fixture-\(id)",
                title: "Fixture \(id)", kind: "scene", renderDirectory: root.path,
                entryPath: root.appendingPathComponent("scene.pkg").path, previewPath: paths[2],
                desktopFallbackPath: paths[2], rawProperties: [:], fps: 30, fillMode: "cover",
                loadFromMemory: false, renderedPreviewPath: paths[index]))
        })
        let cases: [(UInt32, Bool, Bool, [UInt8])] = [
            (7, true, true, [0, 0, 255, 255]),
            (8, true, true, [255, 0, 0, 255]),
            (7, false, true, [0, 255, 0, 255]),
            (8, true, false, [0, 255, 0, 255])
        ]
        for (displayID, showWallpaper, enabled, expected) in cases {
            let result = SnapshotResult()
            let connection = NSXPCConnection(serviceName: Bundle.main.bundleIdentifier! + ".Service")
            connection.remoteObjectInterface = snapshotInterface()
            connection.resume()
            defer { connection.invalidate() }
            let proxy = connection.remoteObjectProxyWithErrorHandler { result.finish(.failure($0)) } as! SnapshotRegressionProtocol
            let configuration = MirageLockConfiguration(version: 2, enabled: enabled, displays: displays)
            proxy.configuredSnapshot(try JSONEncoder().encode(configuration), displayID: displayID,
                showWallpaper: showWallpaper) { snapshot, processID, error in
                result.finish(Result {
                    if let error { throw error }
                    try require(processID != getpid(), "Configured snapshot did not cross a process boundary")
                    guard let snapshot, let cls = object_getClass(snapshot),
                          let storage = class_getInstanceVariable(cls, "rawValue") else {
                        throw RegressionFailure(description: "Configured snapshot was not decoded")
                    }
                    let pointer = Unmanaged.passUnretained(snapshot).toOpaque()
                        .advanced(by: ivar_getOffset(storage)).load(as: UnsafeMutableRawPointer.self)
                    let surface = Unmanaged<IOSurface>.fromOpaque(pointer).takeUnretainedValue()
                    try require(surface.width == 8 && surface.height == 4, "A configured snapshot used the default image")
                    surface.lock(options: .readOnly, seed: nil)
                    defer { surface.unlock(options: .readOnly, seed: nil) }
                    let pixel = surface.baseAddress.assumingMemoryBound(to: UInt8.self)
                    try require(Array(UnsafeBufferPointer(start: pixel, count: 4)) == expected,
                                "A preview used another display, a project cover or the desktop fallback")
                })
            }
            let deadline = Date().addingTimeInterval(15)
            while result.read() == nil, Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
            guard let value = result.read() else { throw RegressionFailure(description: "Configured snapshot timed out") }
            try value.get()
        }
        print("PASS: per-display rendered previews and desktop fallback isolation across XPC")
    }

    static func testSettingsAcrossProcesses() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("mirage-preview-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var previewPaths: [String] = []
        for (index, color) in [NSColor.red, .blue].enumerated() {
            let context = CGContext(data: nil, width: 8, height: 4, bitsPerComponent: 8, bytesPerRow: 32,
                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.setFillColor(color.cgColor)
            context.fill(CGRect(x: 0, y: 0, width: 8, height: 4))
            let data = NSBitmapImageRep(cgImage: context.makeImage()!).representation(using: .png, properties: [:])!
            let url = root.appendingPathComponent("预览 \(index).png")
            try data.write(to: url)
            previewPaths.append(url.path)
        }
        let corrupt = root.appendingPathComponent("corrupt.png")
        try Data("invalid image".utf8).write(to: corrupt)
        previewPaths += ["", root.appendingPathComponent("missing.png").path, corrupt.path, root.path]
        try testSettingsAcrossProcesses(enabled: true, previewPaths: previewPaths)
        try testSettingsAcrossProcesses(enabled: false, previewPaths: previewPaths)
        previewPaths.swapAt(0, 1)
        try testSettingsAcrossProcesses(enabled: true, previewPaths: previewPaths)
    }

    static func testSettingsAcrossProcesses(enabled: Bool, previewPaths: [String]) throws {
        let result = SnapshotResult()
        let connection = NSXPCConnection(serviceName: Bundle.main.bundleIdentifier! + ".Service")
        connection.remoteObjectInterface = snapshotInterface()
        connection.resume()
        defer { connection.invalidate() }
        let proxy = connection.remoteObjectProxyWithErrorHandler { result.finish(.failure($0)) } as! SnapshotRegressionProtocol
        proxy.settings(enabled: enabled, previewPaths: previewPaths) { models, processID, error in
            result.finish(Result {
                if let error { throw error }
                try require(processID != getpid(), "Settings did not cross a process boundary")
                guard let models else { throw RegressionFailure(description: "Settings were not decoded") }
                let groups = Mirror(reflecting: models).descendant("box", "rawValue", "desktop", "some", "groups") as? [Any]
                if !enabled {
                    try require(groups?.isEmpty == true, "The disabled desktop catalog still contains choices after XPC decoding")
                    return
                }
                let items = groups?.first.flatMap { Mirror(reflecting: $0).descendant("items") as? [Any] } ?? []
                try require(items.count == previewPaths.count, "A missing preview removed a wallpaper choice across XPC")
                for (index, item) in items.enumerated() {
                    let mirror = Mirror(reflecting: item)
                    try require(mirror.descendant("id", "id") as? String == "display-\(index + 7)",
                                "Updating a preview changed the system choice identity")
                    guard let thumbnail = mirror.descendant("thumbnail", "image", "url") as? URL,
                          let choiceThumbnail = mirror.descendant("choice", "thumbnail", "image", "url") as? URL else {
                        throw RegressionFailure(description: "Image URLs were lost across XPC")
                    }
                    try require(thumbnail == choiceThumbnail, "The item and selected choice use different previews")
                    try require(NSImage(contentsOf: thumbnail)?.isValid == true, "The receiving process cannot decode the preview")
                    if index < 2 {
                        try require(thumbnail == URL(fileURLWithPath: previewPaths[index]), "The preview belongs to another display or deployment")
                    } else {
                        try require(thumbnail.path != previewPaths[index], "An invalid preview did not fall back")
                        try require(thumbnail.path != previewPaths[0], "A legacy project preview was used instead of a rendered frame")
                    }
                }
            })
        }
        let deadline = Date().addingTimeInterval(15)
        while result.read() == nil, Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
        guard let value = result.read() else { throw RegressionFailure(description: "Cross-process settings timed out") }
        try value.get()
    }
}
