//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import Darwin
import Foundation
import ImageIO

private final class MiragePreviewCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var action: (() -> Void)?
    private var cancelled = false

    func install(_ action: @escaping () -> Void) {
        lock.lock()
        let shouldCancel = cancelled
        if !shouldCancel { self.action = action }
        lock.unlock()
        if shouldCancel { action() }
    }

    func cancel() {
        lock.lock()
        cancelled = true
        let action = self.action
        self.action = nil
        lock.unlock()
        action?()
    }
}

struct DynamicLockScreenDisplayConfiguration: Codable {
    let displayID: UInt32
    let wallpaperID: String
    let title: String
    let kind: String
    let renderDirectory: String
    let entryPath: String
    let previewPath: String?
    var desktopFallbackPath: String?
    var systemFallbackPath: String?
    let rawProperties: [String: AnyCodableValue]
    let fps: Int
    var fillMode: String
    var position: WallpaperPosition? = nil
    var loadFromMemory: Bool?
    var renderedPreviewPath: String? = nil
}

struct DynamicLockScreenConfiguration: Codable {
    let version: Int
    var enabled: Bool?
    var displays: [String: DynamicLockScreenDisplayConfiguration]
}

enum AnyCodableValue: Codable {
    case string(String)
    case number(Double)
    case bool(Bool)
    case object([String: AnyCodableValue])
    case array([AnyCodableValue])
    case null

    init(_ value: Any) {
        switch value {
        case let value as String: self = .string(value)
        case let value as NSString: self = .string(value as String)
        case let value as Bool: self = .bool(value)
        case let value as NSNumber: self = .number(value.doubleValue)
        case let value as [String: Any]: self = .object(value.mapValues(AnyCodableValue.init))
        case let value as [Any]: self = .array(value.map(AnyCodableValue.init))
        default: self = .null
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null; return }
        if let value = try? container.decode(Bool.self) { self = .bool(value); return }
        if let value = try? container.decode(Double.self) { self = .number(value); return }
        if let value = try? container.decode(String.self) { self = .string(value); return }
        if let value = try? container.decode([String: AnyCodableValue].self) { self = .object(value); return }
        self = .array(try container.decode([AnyCodableValue].self))
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .bool(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .null: try container.encodeNil()
        }
    }
}

@MainActor
final class DynamicLockScreenManager: ObservableObject {
    static let shared = DynamicLockScreenManager()

    @Published private(set) var isEnabled: Bool
    @Published var isConfirmationPresented = false
    @Published private(set) var registrationErrorMessage: String?
    @Published private(set) var connectionState = ConnectionState.disabled

    enum ConnectionState {
        case disabled, needsWallpaper, preparing, awaitingSystemSettings, awaitingSelection, connected, ready, failed
    }

    private let enabledKey = "Mirage.DynamicLockScreen.Enabled"
    private let confirmationKey = "Mirage.DynamicLockScreen.Confirmed"
    private let appGroupID = MirageLockBridge.groupID
    private let configurationName = "dynamic-lock-screen.json"
    private let registeredExtensionFingerprintKey =
        "Mirage.DynamicLockScreen.RegisteredExtensionFingerprint"
    private let extensionQueue = WallpaperServiceCoordinator.queue
    private let registrationQueue = DispatchQueue(label: "cn.laobamac.Mirage.dynamicLockScreen.registration", qos: .utility)
    private let extensionRequests = WallpaperExtensionRequest()
    private var requiresSettingsAcknowledgement = false
    private var activeProbe: (probe: MirageLockProbe, url: URL, fingerprint: String, container: URL)?
    private let configurationUpdates = CoalescingWorkQueue(label: "cn.laobamac.Mirage.dynamicLockScreen.configuration")
    private let statusUpdates = CoalescingWorkQueue(label: "cn.laobamac.Mirage.dynamicLockScreen.status")
    private let configurationLock = NSRecursiveLock()
    private let deploymentQueue = DispatchQueue(label: "cn.laobamac.Mirage.dynamicLockScreen.deployment", qos: .userInitiated)
    private let previewQueue = DispatchQueue(label: "cn.laobamac.Mirage.dynamicLockScreen.preview", qos: .utility)
    private var configurationRequestID: UUID?
    private var previewTask: Task<Void, Never>?

    struct DisplaySnapshot {
        let displayID: UInt32
        let fallbackSource: URL?
        let systemFallbackSource: URL?
        let position: WallpaperPosition
        var renderedPreviewSource: URL? = nil
    }

    struct PreparedConfiguration {
        let stagingRoot: URL
        let root: URL
        let data: Data
    }

    private init() {
        let storedEnabled = UserDefaults.standard.bool(forKey: enabledKey)
        isEnabled = storedEnabled
            && (DynamicLockScreenModeStore.active == nil
                || DynamicLockScreenModeStore.active == .extensionMode)
        if DynamicLockScreenModeStore.active == nil, isEnabled {
            DynamicLockScreenModeStore.activate(.extensionMode)
        }
        connectionState = isEnabled ? .needsWallpaper : .disabled
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(),
            Unmanaged.passUnretained(self).toOpaque(), { _, object, _, _, _ in
                guard let object else { return }
                let manager = Unmanaged<DynamicLockScreenManager>.fromOpaque(object).takeUnretainedValue()
                Task { @MainActor in manager.refreshConnectionStatus() }
            }, MirageLockBridge.statusNotification as CFString, nil, .deliverImmediately)
    }

    var connectionStatusMessage: String {
        switch connectionState {
        case .disabled: return L("动态锁屏已关闭")
        case .needsWallpaper: return L("已启用，请设置锁屏壁纸")
        case .preparing: return L("正在连接动态锁屏")
        case .awaitingSystemSettings: return L("请打开系统墙纸设置以载入动态锁屏")
        case .awaitingSelection: return L("动态锁屏已载入，请在系统墙纸设置中选择 Mirage")
        case .connected: return L("动态锁屏已连接，等待画面就绪")
        case .ready: return L("动态锁屏已就绪")
        case .failed: return L("动态锁屏恢复失败")
        }
    }

    var canRetryConnection: Bool {
        switch connectionState {
        case .awaitingSystemSettings, .awaitingSelection, .connected, .failed: return true
        default: return false
        }
    }

    func prepareAtLaunch() {
        if !isEnabled { clearConfiguration() }
        extensionQueue.async { ScreenSaverManager.shared.refreshInstalledVersionIfNeeded() }
    }

    var isAvailable: Bool {
        if #available(macOS 26.0, *) { return true }
        return false
    }

    var hasConfirmedWarning: Bool {
        UserDefaults.standard.bool(forKey: confirmationKey)
    }

    var canUse: Bool {
        isAvailable && isEnabled && hasConfirmedWarning
            && DynamicLockScreenModeStore.active == .extensionMode
            && sharedContainerURL != nil
    }

    var isConfigured: Bool {
        guard let url = configurationURL,
              let data = try? Data(contentsOf: url),
              let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data) else { return false }
        return configuration.enabled != false && !configuration.displays.isEmpty && configuration.displays.values.allSatisfy {
            $0.kind == WallpaperKind.video.rawValue || $0.kind == WallpaperKind.scene.rawValue
        }
    }

    var configurationURL: URL? {
        sharedContainerURL?.appendingPathComponent(configurationName)
    }

    var configuredWallpaperTitle: String? {
        guard let url = configurationURL,
              let data = try? Data(contentsOf: url),
              let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data) else { return nil }
        return configuration.displays.values.first?.title
    }

    func requestEnable() {
        guard isAvailable else { return }
        isConfirmationPresented = true
    }

    func confirmAndEnable(input: String) -> Bool {
        guard input == "我同意" || input.caseInsensitiveCompare("Agree") == .orderedSame else { return false }
        UserDefaults.standard.set(true, forKey: confirmationKey)
        isConfirmationPresented = false
        setEnabled(true)
        return true
    }

    func setEnabled(_ enabled: Bool) {
        guard enabled else {
            isEnabled = false
            UserDefaults.standard.set(false, forKey: enabledKey)
            DynamicLockScreenModeStore.deactivate(.extensionMode)
            extensionRequests.cancel()
            requiresSettingsAcknowledgement = false
            activeProbe = nil
            connectionState = .disabled
            registrationErrorMessage = nil
            clearConfiguration()
            return
        }
        guard hasConfirmedWarning else {
            requestEnable()
            return
        }
        guard isAvailable else { return }
        ScreenSaverDynamicLockScreenManager.shared.disableForModeSwitch()
        DynamicLockScreenModeStore.activate(.extensionMode)
        isEnabled = true
        UserDefaults.standard.set(true, forKey: enabledKey)
        restoreConfigurationAndRegister()
    }

    func disableForModeSwitch() {
        guard isEnabled || DynamicLockScreenModeStore.active == .extensionMode else { return }
        setEnabled(false)
    }

    func configureCurrentWallpaper(_ wallpaper: WEWallpaper,
                                   runtime: WallpaperRuntimeState,
                                   properties: [String: WEProjectProperty],
                                   fps: Int,
                                   displayIDs: [UInt32]) async throws {
        guard canUse else { throw DynamicLockScreenError.notEnabled }
        guard wallpaper.isValid else { throw DynamicLockScreenError.noWallpaper }
        guard wallpaper.kind == .video || wallpaper.kind == .scene else {
            throw DynamicLockScreenError.unsupportedWallpaper
        }
        guard let container = sharedContainerURL else { throw DynamicLockScreenError.appGroupUnavailable }
        let positions = AppDelegate.shared.wallpaperViewModel.positions(for: wallpaper.id)
        let displays = Array(Set(displayIDs)).map { displayID in
            DisplaySnapshot(
                displayID: displayID,
                fallbackSource: DesktopOverrideService.shared.dynamicLockScreenFallbackURL(forDisplay: displayID),
                systemFallbackSource: DesktopOverrideService.shared.dynamicLockScreenSystemFallbackURL(forDisplay: displayID),
                position: DisplayRegistry.shared.info(forDisplay: displayID).flatMap {
                    positions[$0.key.rawValue]
                } ?? .center)
        }
        let loadFromMemory = (AppDelegate.shared.globalSettingsViewModel.settings.wallpaperLoadSource ?? .disk) == .memory
        let requestID = UUID()
        configurationRequestID = requestID
        defer {
            if configurationRequestID == requestID { configurationRequestID = nil }
        }
        let prepared: PreparedConfiguration
        do {
            prepared = try await withCheckedThrowingContinuation { continuation in
                deploymentQueue.async {
                    continuation.resume(with: Result {
                        try Self.prepareConfiguration(wallpaper, runtime: runtime, properties: properties,
                                                      fps: fps, displays: displays,
                                                      loadFromMemory: loadFromMemory, container: container)
                    })
                }
            }
        } catch {
            guard configurationRequestID == requestID, canUse, !Task.isCancelled else {
                throw CancellationError()
            }
            throw error
        }
        var committed = false
        defer {
            if !committed {
                deploymentQueue.async {
                    try? FileManager.default.removeItem(at: prepared.stagingRoot)
                    try? FileManager.default.removeItem(at: prepared.root)
                }
            }
        }
        guard configurationRequestID == requestID, canUse, !Task.isCancelled else {
            throw CancellationError()
        }
        try commitConfiguration(prepared, in: container)
        committed = true
        notifyConfigurationChanged()
        cleanupDeployments(except: prepared.root)
        cleanupDesktopFallbacks()
        registerExtension()
        startPreviewUpdate(for: wallpaper, runtime: runtime, properties: properties, fps: fps,
                           displays: displays, loadFromMemory: loadFromMemory,
                           deployment: prepared.root, configurationURL: container.appendingPathComponent(configurationName))
    }

    private func startPreviewUpdate(for wallpaper: WEWallpaper, runtime: WallpaperRuntimeState,
                                    properties: [String: WEProjectProperty], fps: Int,
                                    displays: [DisplaySnapshot], loadFromMemory: Bool,
                                    deployment: URL, configurationURL: URL) {
        previewTask?.cancel()
        previewTask = Task { @MainActor [weak self] in
            guard let self else { return }
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("mirage-lock-preview-\(UUID().uuidString)", isDirectory: true)
            defer { previewQueue.async { try? FileManager.default.removeItem(at: directory) } }
            let captured = await capturePreviews(for: wallpaper, runtime: runtime, properties: properties,
                                                 fps: fps, displays: displays, loadFromMemory: loadFromMemory,
                                                 directory: directory)
            guard !Task.isCancelled else { return }
            let lock = configurationLock
            let updated = await withCheckedContinuation { continuation in
                previewQueue.async {
                    let images = captured.compactMap { display -> (DisplaySnapshot, Data)? in
                        guard let source = display.renderedPreviewSource,
                              let data = Self.renderedPreviewData(source: source) else { return nil }
                        return (display, data)
                    }
                    lock.lock()
                    defer { lock.unlock() }
                    continuation.resume(returning: Self.publishPreviews(images, deployment: deployment,
                        configurationURL: configurationURL, wallpaperID: wallpaper.id, fillMode: runtime.fillMode))
                }
            }
            if updated { MirageLockBridge.post(MirageLockBridge.previewNotification) }
        }
    }

    nonisolated static func publishPreviews(_ images: [(DisplaySnapshot, Data)], deployment: URL,
                                            configurationURL: URL, wallpaperID: String, fillMode: FillMode) -> Bool {
        guard let data = try? Data(contentsOf: configurationURL),
              let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data),
              configuration.enabled != false else { return false }
        var updated = false
        for (snapshot, image) in images {
            let url = renderedPreviewURL(displayID: snapshot.displayID, in: deployment)
            guard let display = configuration.displays["display-\(snapshot.displayID)"],
                  display.wallpaperID == wallpaperID, display.fillMode == fillMode.rawValue,
                  (display.position ?? .center) == snapshot.position,
                  display.renderedPreviewPath == url.path,
                  URL(fileURLWithPath: display.renderDirectory).deletingLastPathComponent().standardizedFileURL
                    == deployment.standardizedFileURL else { continue }
            if (try? image.write(to: url, options: .atomic)) != nil { updated = true }
        }
        return updated
    }

    private func capturePreviews(for wallpaper: WEWallpaper, runtime: WallpaperRuntimeState,
                                 properties: [String: WEProjectProperty], fps: Int,
                                 displays: [DisplaySnapshot], loadFromMemory: Bool,
                                 directory: URL) async -> [DisplaySnapshot] {
        guard (try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)) != nil else {
            return displays
        }
        let renderer = AppDelegate.shared.wallpaperViewModel.renderer
        return await withTaskGroup(of: DisplaySnapshot.self) { group in
            for display in displays {
                group.addTask { @MainActor in
                    var result = display
                    guard !Task.isCancelled else { return result }
                    let url = directory.appendingPathComponent("display-\(display.displayID).heic")
                    var options = RenderOptions()
                    options.fps = min(max(fps, 10), 60)
                    options.muted = true
                    options.fillMode = runtime.fillMode
                    options.position = display.position
                    options.loadFromMemory = loadFromMemory
                    options.userProperties = properties
                    let cancellation = MiragePreviewCancellation()
                    let captured = await withTaskCancellationHandler {
                        await withCheckedContinuation { continuation in
                            let token = renderer.snapshot(wallpaper: wallpaper, onDisplay: display.displayID,
                                                          options: options, path: url.path) {
                                continuation.resume(returning: $0)
                            }
                            cancellation.install { renderer.cancelPreview(token) }
                        }
                    } onCancel: {
                        cancellation.cancel()
                    }
                    if captured { result.renderedPreviewSource = url }
                    return result
                }
            }
            var results: [DisplaySnapshot] = []
            for await result in group { results.append(result) }
            return results
        }
    }

    private func commitConfiguration(_ prepared: PreparedConfiguration, in container: URL) throws {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        try Self.commitConfiguration(prepared, configurationURL: container.appendingPathComponent(configurationName))
    }

    nonisolated static func commitConfiguration(_ prepared: PreparedConfiguration, configurationURL: URL) throws {
        do {
            try FileManager.default.createDirectory(at: prepared.root.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: prepared.stagingRoot, to: prepared.root)
            try prepared.data.write(to: configurationURL, options: .atomic)
        } catch {
            try? FileManager.default.removeItem(at: prepared.root)
            if Self.isSharedContainerPermissionError(error) {
                throw DynamicLockScreenError.fullDiskAccessRequired
            }
            throw error
        }
    }

    nonisolated static func prepareConfiguration(_ wallpaper: WEWallpaper,
                                                         runtime: WallpaperRuntimeState,
                                                         properties: [String: WEProjectProperty],
                                                         fps: Int,
                                                         displays: [DisplaySnapshot],
                                                         loadFromMemory: Bool,
                                                         container: URL) throws -> PreparedConfiguration {
        guard wallpaper.isValid else { throw DynamicLockScreenError.noWallpaper }
        guard wallpaper.kind == .video || wallpaper.kind == .scene else {
            throw DynamicLockScreenError.unsupportedWallpaper
        }
        do {
            try FileManager.default.createDirectory(at: container, withIntermediateDirectories: true)
        } catch {
            if Self.isSharedContainerPermissionError(error) {
                throw DynamicLockScreenError.fullDiskAccessRequired
            }
            throw error
        }
        let deployment = try deploy(wallpaper: wallpaper, in: container)
        let root = container.appendingPathComponent("DynamicLockScreen/Deployments", isDirectory: true)
            .appendingPathComponent(deployment.root.lastPathComponent, isDirectory: true)
        func deployedPath(_ path: String) -> String {
            guard path.hasPrefix(deployment.root.path + "/") else { return path }
            return root.path + path.dropFirst(deployment.root.path.count)
        }
        var keepDeployment = false
        defer {
            if !keepDeployment { try? FileManager.default.removeItem(at: deployment.root) }
        }
        var rawPropertyValues: [String: Any] = [:]
        for (key, property) in properties where wallpaper.kind == .scene {
            switch property.propertyType {
            case .color:
                rawPropertyValues[key] = ["type": "color", "value": property.value.stringValue]
            case .bool:
                rawPropertyValues[key] = property.value.boolValue
            case .slider:
                rawPropertyValues[key] = property.value.doubleValue
            case .scenetexture, .file:
                let value = try deployPropertyAsset(property.value.stringValue, key: key, in: deployment.root)
                rawPropertyValues[key] = ["type": "scenetexture", "value": deployedPath(value)]
            case .combo:
                rawPropertyValues[key] = property.value.jsonObjectValue
            case .usershortcut:
                var value: [String: Any] = [
                    "type": "usershortcut",
                    "value": property.value.stringValue
                ]
                if let icon = property.mirageShortcutIcon {
                    value["icon"] = deployedPath(try deployPropertyAsset(
                        icon, key: "\(key)-icon", in: deployment.root))
                }
                rawPropertyValues[key] = value
            default:
                rawPropertyValues[key] = property.value.stringValue
            }
        }
        let configuration = DynamicLockScreenConfiguration(
            version: 2,
            enabled: true,
            displays: Dictionary(uniqueKeysWithValues: displays.map { display in
                let displayID = display.displayID
                let previewURL = renderedPreviewURL(displayID: displayID, in: deployment.root)
                if let source = display.renderedPreviewSource, let data = renderedPreviewData(source: source) {
                    try? data.write(to: previewURL, options: .atomic)
                }
                let fallbackSource = display.fallbackSource
                let fallbackURL = fallbackSource.flatMap {
                    try? deployDesktopFallback(source: $0, displayID: displayID, in: container,
                        directory: deployment.root.appendingPathComponent("desktop-fallbacks", isDirectory: true))
                }
                let systemFallbackSource = display.systemFallbackSource
                let systemFallbackURL: URL?
                if systemFallbackSource?.resolvingSymlinksInPath()
                    == fallbackSource?.resolvingSymlinksInPath() {
                    systemFallbackURL = fallbackURL
                } else {
                    systemFallbackURL = systemFallbackSource.flatMap {
                        try? deployDesktopFallback(source: $0, displayID: displayID, in: container,
                        directory: deployment.root.appendingPathComponent("desktop-fallbacks", isDirectory: true))
                    }
                }
                let record = DynamicLockScreenDisplayConfiguration(
                    displayID: displayID,
                    wallpaperID: wallpaper.id,
                    title: wallpaper.project.title,
                    kind: wallpaper.kind.rawValue,
                    renderDirectory: deployedPath(deployment.renderDirectory.path),
                    entryPath: deployedPath(deployment.entryURL.path),
                    previewPath: deployedPath(previewURL.path),
                    desktopFallbackPath: fallbackURL.map { deployedPath($0.path) },
                    systemFallbackPath: systemFallbackURL.map { deployedPath($0.path) },
                    rawProperties: rawPropertyValues.mapValues(AnyCodableValue.init),
                    fps: min(max(fps, 10), 60),
                    fillMode: runtime.fillMode.rawValue,
                    position: display.position,
                    loadFromMemory: loadFromMemory,
                    renderedPreviewPath: deployedPath(previewURL.path)
                )
                return ("display-\(displayID)", record)
            })
        )
        let data = try JSONEncoder().encode(configuration)
        keepDeployment = true
        return PreparedConfiguration(stagingRoot: deployment.root, root: root, data: data)
    }

    func updatePosition(_ position: WallpaperPosition, fillMode: FillMode,
                        wallpaperID: String, displayID: UInt32) {
        guard let configurationURL else { return }
        let lock = configurationLock
        configurationUpdates.submit(key: "position:\(displayID)") {
            lock.lock()
            defer { lock.unlock() }
            guard let data = try? Data(contentsOf: configurationURL),
                  var configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data),
                  let key = configuration.displays.first(where: {
                      $0.value.displayID == displayID && $0.value.wallpaperID == wallpaperID
                  })?.key,
                  var display = configuration.displays[key],
                  (display.position ?? .center) != position || display.fillMode != fillMode.rawValue else { return }
            display.position = position
            display.fillMode = fillMode.rawValue
            configuration.displays[key] = display
            guard let updated = try? JSONEncoder().encode(configuration),
                  (try? updated.write(to: configurationURL, options: .atomic)) != nil else { return }
            Self.postConfigurationChanged()
        }
    }

    func updateLoadFromMemory(_ enabled: Bool) {
        guard let configurationURL else { return }
        let lock = configurationLock
        configurationUpdates.submit(key: "loadFromMemory") {
            lock.lock()
            defer { lock.unlock() }
            guard let data = try? Data(contentsOf: configurationURL),
                  var configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data)
            else { return }
            for key in configuration.displays.keys {
                configuration.displays[key]?.loadFromMemory = enabled
            }
            guard let updated = try? JSONEncoder().encode(configuration),
                  (try? updated.write(to: configurationURL, options: .atomic)) != nil else { return }
            Self.postConfigurationChanged()
        }
    }

    func flushConfigurationUpdates() {
        configurationUpdates.flush()
    }

    func refreshDesktopFallback(forDisplay displayID: UInt32) {
        guard let source = DesktopOverrideService.shared.dynamicLockScreenFallbackURL(
            forDisplay: displayID)
        else { return }
        _ = try? updateDesktopFallback(from: source, forDisplay: displayID)
    }

    func refreshDesktopFallbacks() {
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data)
        else { return }
        configuration.displays.values.forEach { refreshDesktopFallback(forDisplay: $0.displayID) }
    }

    @discardableResult
    func updateDesktopFallback(from source: URL, forDisplay displayID: UInt32) throws -> Bool {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data),
              var record = configuration.displays["display-\(displayID)"],
              let container = sharedContainerURL else { return false }
        let fallback = try Self.deployDesktopFallback(
            source: source, displayID: displayID, in: container)
        record.desktopFallbackPath = fallback.path
        configuration.displays["display-\(displayID)"] = record
        do {
            let updated = try JSONEncoder().encode(configuration)
            try updated.write(to: configurationURL, options: .atomic)
        } catch {
            try? FileManager.default.removeItem(at: fallback)
            throw error
        }
        notifyDesktopFallbackChanged()
        cleanupDesktopFallbacks()
        return true
    }

    func restoreSystemDesktopFallbacks() {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data)
        else { return }
        var changed = false
        for key in configuration.displays.keys {
            guard var record = configuration.displays[key] else { continue }
            if let path = record.systemFallbackPath,
               FileManager.default.fileExists(atPath: path) {
                if record.desktopFallbackPath != path {
                    record.desktopFallbackPath = path
                    configuration.displays[key] = record
                    changed = true
                }
                continue
            }
            guard let container = sharedContainerURL,
                  let source = DesktopOverrideService.shared.dynamicLockScreenSystemFallbackURL(
                    forDisplay: record.displayID),
                  let fallback = try? Self.deployDesktopFallback(
                    source: source, displayID: record.displayID, in: container)
            else { continue }
            record.desktopFallbackPath = fallback.path
            record.systemFallbackPath = fallback.path
            configuration.displays[key] = record
            changed = true
        }
        guard changed,
              let updated = try? JSONEncoder().encode(configuration),
              (try? updated.write(to: configurationURL, options: .atomic)) != nil
        else { return }
        notifyDesktopFallbackChanged()
        cleanupDesktopFallbacks()
    }

    func clearConfiguration() {
        previewTask?.cancel()
        previewTask = nil
        configurationRequestID = nil
        registrationErrorMessage = nil
        if !isEnabled { connectionState = .disabled }
        configurationLock.lock()
        defer { configurationLock.unlock() }
        restoreSystemDesktopFallbacks()
        do {
            guard let configurationURL else { return }
            let data = try Data(contentsOf: configurationURL)
            var configuration = try JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data)
            configuration.enabled = false
            try JSONEncoder().encode(configuration).write(to: configurationURL, options: .atomic)
        } catch {
            if (error as NSError).domain == NSCocoaErrorDomain,
               (error as NSError).code == NSFileReadNoSuchFileError { return }
            registrationErrorMessage = L("无法保存动态锁屏状态") + ": " + error.localizedDescription
            connectionState = .failed
        }
        notifyConfigurationChanged()
    }

    func openSystemSettings() {
        if canUse, isConfigured {
            restoreConfigurationAndRegister(requireAcknowledgement: true)
        }
        showSystemSettings()
    }

    private func showSystemSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.Wallpaper-Settings.extension") else { return }
        NSWorkspace.shared.open(url)
    }

    func openFullDiskAccessSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"),
           NSWorkspace.shared.open(url) {
            return
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/System Settings.app"))
    }

    private var sharedContainerURL: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroupID)
    }

    private nonisolated static func playableEntryURL(for wallpaper: WEWallpaper) -> URL {
        let source = wallpaper.resolvedEntryURL.resolvingSymlinksInPath()
        guard wallpaper.kind == .video else { return source }
        let digest = SHA256.hash(data: Data(source.path.utf8)).map { String(format: "%02x", $0) }.joined()
        let cache = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Mirage/VideoCache", isDirectory: true)
            .appendingPathComponent("\(digest).mp4")
        return FileManager.default.fileExists(atPath: cache.path) ? cache : source
    }

    private nonisolated static func deploy(wallpaper: WEWallpaper, in container: URL) throws -> (root: URL, renderDirectory: URL, entryURL: URL) {
        let deployments = container.appendingPathComponent("DynamicLockScreen/Staging", isDirectory: true)
        let root = deployments.appendingPathComponent(UUID().uuidString.lowercased(), isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: deployments, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        } catch {
            if Self.isSharedContainerPermissionError(error) {
                throw DynamicLockScreenError.fullDiskAccessRequired
            }
            throw error
        }

        let source = playableEntryURL(for: wallpaper)
        let renderDirectory = root.appendingPathComponent("render", isDirectory: true)
        do {
            let entryURL: URL
            if wallpaper.kind == .scene {
                let sourceRoot = wallpaper.renderDirectory.resolvingSymlinksInPath()
                try FileManager.default.copyItem(at: sourceRoot, to: renderDirectory)
                let relativeEntry = source.path.hasPrefix(sourceRoot.path + "/")
                    ? String(source.path.dropFirst(sourceRoot.path.count + 1))
                    : source.lastPathComponent
                entryURL = renderDirectory.appendingPathComponent(relativeEntry)
                if !FileManager.default.fileExists(atPath: entryURL.path) {
                    try FileManager.default.createDirectory(at: entryURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try linkOrCopy(source, to: entryURL)
                }
            } else {
                try FileManager.default.createDirectory(at: renderDirectory, withIntermediateDirectories: true)
                entryURL = renderDirectory.appendingPathComponent(source.lastPathComponent)
                try linkOrCopy(source, to: entryURL)
            }
            return (root, renderDirectory, entryURL)
        } catch {
            try? FileManager.default.removeItem(at: root)
            throw error
        }
    }

    private nonisolated static func deployPropertyAsset(_ path: String, key: String, in root: URL) throws -> String {
        guard !path.isEmpty, (path as NSString).isAbsolutePath else { return path }
        let source = URL(fileURLWithPath: path).resolvingSymlinksInPath()
        guard FileManager.default.fileExists(atPath: source.path) else { return path }
        let digest = SHA256.hash(data: Data("\(key)|\(source.path)".utf8))
            .map { String(format: "%02x", $0) }.joined()
        let directory = root.appendingPathComponent("property-assets/\(digest)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let destination = directory.appendingPathComponent(source.lastPathComponent, isDirectory: source.hasDirectoryPath)
        try FileManager.default.copyItem(at: source, to: destination)
        return destination.path
    }

    private nonisolated static func renderedPreviewURL(displayID: UInt32, in root: URL) -> URL {
        root.appendingPathComponent("rendered-preview-\(displayID).png")
    }

    private nonisolated static func renderedPreviewData(source url: URL) -> Data? {
        guard (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
              let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 4096
              ] as CFDictionary) else { return nil }
        let data = NSMutableData()
        guard let encoder = CGImageDestinationCreateWithData(data, "public.png" as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(encoder, image, nil)
        guard CGImageDestinationFinalize(encoder) else { return nil }
        return data as Data
    }

    private nonisolated static func deployDesktopFallback(source: URL, displayID: UInt32, in container: URL, directory: URL? = nil) throws -> URL {
        let source = source.resolvingSymlinksInPath()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: source.path, isDirectory: &isDirectory),
              !isDirectory.boolValue else {
            throw CocoaError(.fileReadNoSuchFile)
        }
        let directory = directory ?? container.appendingPathComponent("DynamicLockScreen/DesktopFallbacks", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let extensionName = source.pathExtension.isEmpty ? "png" : source.pathExtension
        let destination = directory.appendingPathComponent(
            "display-\(displayID)-\(UUID().uuidString.lowercased()).\(extensionName)")
        try FileManager.default.copyItem(at: source, to: destination)
        return destination
    }

    private nonisolated static func linkOrCopy(_ source: URL, to destination: URL) throws {
        do {
            try FileManager.default.linkItem(at: source, to: destination)
        } catch {
            try FileManager.default.copyItem(at: source, to: destination)
        }
    }

    private nonisolated static func isSharedContainerPermissionError(_ error: Error) -> Bool {
        let nsError = error as NSError
        if nsError.domain == NSCocoaErrorDomain,
           nsError.code == CocoaError.fileReadNoPermission.rawValue
            || nsError.code == CocoaError.fileWriteNoPermission.rawValue {
            return true
        }
        if nsError.domain == NSPOSIXErrorDomain,
           nsError.code == Int(EACCES) || nsError.code == Int(EPERM) {
            return true
        }
        if let underlying = nsError.userInfo[NSUnderlyingErrorKey] as? Error {
            return isSharedContainerPermissionError(underlying)
        }
        return false
    }

    private func cleanupDeployments(except active: URL) {
        let directory = active.deletingLastPathComponent()
        let configurationURL = configurationURL
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 30) {
            guard let entries = try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil) else { return }
            guard let configurationURL,
                  let data = try? Data(contentsOf: configurationURL),
                  let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data) else {
                return
            }
            let configuredRoots = Set(configuration.displays.values.map {
                URL(fileURLWithPath: $0.renderDirectory).deletingLastPathComponent().standardizedFileURL.path
            })
            let activePath = active.standardizedFileURL.path
            let cutoff = Date().addingTimeInterval(-30)
            for entry in entries {
                let entryPath = entry.standardizedFileURL.path
                guard entryPath != activePath, !configuredRoots.contains(entryPath) else { continue }
                let modified = (try? entry.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
                guard modified.map({ $0 < cutoff }) ?? true else { continue }
                try? FileManager.default.removeItem(at: entry)
            }
        }
    }

    private func cleanupDesktopFallbacks() {
        guard let container = sharedContainerURL else { return }
        let directory = container.appendingPathComponent("DynamicLockScreen/DesktopFallbacks", isDirectory: true)
        let configurationURL = configurationURL
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 30) {
            guard let entries = try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: nil, options: .skipsHiddenFiles
            ), let configurationURL,
                  let data = try? Data(contentsOf: configurationURL),
                  let configuration = try? JSONDecoder().decode(
                    DynamicLockScreenConfiguration.self, from: data)
            else { return }
            let keep = Set(configuration.displays.values.compactMap(\.desktopFallbackPath).map {
                URL(fileURLWithPath: $0).standardizedFileURL.path
            }).union(configuration.displays.values.compactMap(\.systemFallbackPath).map {
                URL(fileURLWithPath: $0).standardizedFileURL.path
            })
            for entry in entries where !keep.contains(entry.standardizedFileURL.path) {
                try? FileManager.default.removeItem(at: entry)
            }
        }
    }

    private func registerExtension(forceRestart: Bool = false, forceRegistration: Bool = false,
                                   requireAcknowledgement: Bool = false) {
        guard canUse, let container = sharedContainerURL else { return }
        let appURL = Bundle.main.bundleURL
        let extensionURL = appURL.appendingPathComponent("Contents/Extensions/MirageWallpaperExtension.appex")
        let requests = extensionRequests
        let requestID = requests.begin()
        let previousFingerprint = UserDefaults.standard.string(forKey: registeredExtensionFingerprintKey)
        let needsAcknowledgement = requireAcknowledgement || requiresSettingsAcknowledgement
        requiresSettingsAcknowledgement = needsAcknowledgement
        activeProbe = nil
        connectionState = .preparing
        registrationErrorMessage = nil
        registrationQueue.async { [weak self] in
            guard requests.isCurrent(requestID) else { return }
            let result = Result {
                try WallpaperExtensionController.register(
                    appURL: appURL, extensionURL: extensionURL, container: container,
                    previousFingerprint: previousFingerprint, forceRestart: forceRestart,
                    forceRegistration: forceRegistration, requireAcknowledgement: needsAcknowledgement,
                    isCurrent: { requests.isCurrent(requestID) })
            }
            Task { @MainActor [weak self] in
                guard let self, self.canUse, requests.isCurrent(requestID) else { return }
                self.requiresSettingsAcknowledgement = false
                switch result {
                case .success(let registration):
                    self.activeProbe = (registration.probe, extensionURL, registration.fingerprint, container)
                    UserDefaults.standard.set(registration.fingerprint, forKey: self.registeredExtensionFingerprintKey)
                    switch registration.outcome {
                    case .awaitingSystemSettings:
                        self.connectionState = .awaitingSystemSettings
                    case .acknowledged(let report):
                        self.applyConnectionReport(report, selection: registration.selection)
                    case .failed(let error):
                        self.connectionState = .failed
                        self.registrationErrorMessage = L("动态锁屏恢复失败，请重试；详细原因已写入日志")
                        MirageLogService.shared.append(error, source: "wallpaper-extension")
                    }
                    self.refreshConnectionStatus()
                case .failure(let error):
                    self.connectionState = .failed
                    self.registrationErrorMessage = L("动态锁屏恢复失败，请重试；详细原因已写入日志")
                    MirageLogService.shared.append(error.localizedDescription, source: "wallpaper-extension")
                }
            }
        }
    }

    func retryConnection() {
        if isEnabled {
            restoreConfigurationAndRegister(forceRestart: true, forceRegistration: true, requireAcknowledgement: true)
            showSystemSettings()
        } else {
            clearConfiguration()
        }
    }

    private func applyConnectionReport(_ report: MirageLockReport,
                                       selection: WallpaperExtensionController.SelectionState) {
        registrationErrorMessage = nil
        if selection == .notSelected {
            connectionState = .awaitingSelection
        } else {
            connectionState = report.readyDisplayIDs.isEmpty ? .connected : .ready
        }
    }

    private func refreshConnectionStatus() {
        guard canUse, let active = activeProbe else { return }
        statusUpdates.submit(key: "status") { [weak self] in
            let reports = WallpaperExtensionController.liveReports(
                probe: active.probe, extensionURL: active.url,
                fingerprint: active.fingerprint, container: active.container)
            let selection = WallpaperExtensionController.selectionState(
                identifier: Bundle(url: active.url)?.bundleIdentifier ?? "cn.laobamac.Mirage.WallpaperExtension")
            let health: (report: MirageLockReport?, errorMessage: String?)
            do {
                let data = try Data(contentsOf: active.container.appendingPathComponent(MirageLockBridge.configurationName))
                health = WallpaperExtensionController.connectionHealth(
                    reports: reports, configurationDigest: MirageLockBridge.digest(data))
            } catch {
                health = (nil, error.localizedDescription)
            }
            Task { @MainActor [weak self] in
                guard let self, self.canUse, self.activeProbe?.probe.id == active.probe.id else { return }
                if let error = health.errorMessage {
                    self.connectionState = .failed
                    self.registrationErrorMessage = L("动态锁屏恢复失败，请重试；详细原因已写入日志")
                    MirageLogService.shared.append(error, source: "wallpaper-extension")
                } else {
                    guard let report = health.report else {
                        if self.registrationErrorMessage == nil { self.connectionState = .awaitingSystemSettings }
                        return
                    }
                    UserDefaults.standard.set(active.fingerprint, forKey: self.registeredExtensionFingerprintKey)
                    self.applyConnectionReport(report, selection: selection)
                }
            }
        }
    }

    private func restoreConfigurationAndRegister(forceRestart: Bool = false, forceRegistration: Bool = false,
                                                  requireAcknowledgement: Bool = false) {
        extensionRequests.cancel()
        activeProbe = nil
        registrationErrorMessage = nil
        do {
            if try activateStoredConfiguration() {
                registerExtension(forceRestart: forceRestart, forceRegistration: forceRegistration,
                                  requireAcknowledgement: requireAcknowledgement)
            } else {
                requiresSettingsAcknowledgement = false
                connectionState = .needsWallpaper
            }
        } catch {
            requiresSettingsAcknowledgement = false
            connectionState = .failed
            registrationErrorMessage = L("动态锁屏配置无法读取，请检查文件访问权限或重新设置壁纸")
            MirageLogService.shared.append(error.localizedDescription, source: "wallpaper-extension")
        }
    }

    private func activateStoredConfiguration() throws -> Bool {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        guard let configurationURL else { throw DynamicLockScreenError.appGroupUnavailable }
        let data: Data
        do {
            data = try Data(contentsOf: configurationURL)
        } catch {
            if (error as NSError).domain == NSCocoaErrorDomain,
               (error as NSError).code == NSFileReadNoSuchFileError { return false }
            throw error
        }
        var configuration = try JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data)
        guard (1...2).contains(configuration.version), !configuration.displays.isEmpty,
              configuration.displays.values.allSatisfy({ $0.kind == "video" || $0.kind == "scene" }) else {
            throw DynamicLockScreenError.unsupportedWallpaper
        }
        for display in configuration.displays.values {
            guard FileManager.default.isReadableFile(atPath: display.entryPath),
                  FileManager.default.isReadableFile(atPath: display.renderDirectory) else {
                throw MirageLockBridge.failure("Stored lock screen files are not readable: \(display.entryPath)")
            }
        }
        if configuration.enabled != true {
            configuration.enabled = true
            try JSONEncoder().encode(configuration).write(to: configurationURL, options: .atomic)
        }
        notifyConfigurationChanged()
        return true
    }

    private func notifyConfigurationChanged() {
        Self.postConfigurationChanged()
    }

    private nonisolated static func postConfigurationChanged() {
        let center = CFNotificationCenterGetDarwinNotifyCenter()
        CFNotificationCenterPostNotification(
            center,
            CFNotificationName(MirageLockBridge.configurationNotification as CFString),
            nil,
            nil,
            true
        )
    }

    private func notifyDesktopFallbackChanged() {
        CFNotificationCenterPostNotification(
            CFNotificationCenterGetDarwinNotifyCenter(),
            CFNotificationName(MirageLockBridge.desktopFallbackNotification as CFString),
            nil,
            nil,
            true
        )
    }

    func refreshExtension() {
        notifyConfigurationChanged()
        notifyDesktopFallbackChanged()
    }
}

enum DynamicLockScreenError: LocalizedError {
    case notEnabled
    case noWallpaper
    case unsupportedWallpaper
    case appGroupUnavailable
    case fullDiskAccessRequired

    var errorDescription: String? {
        switch self {
        case .notEnabled: return L("请先开启动态锁屏并完成确认")
        case .noWallpaper: return L("请先播放一张壁纸")
        case .unsupportedWallpaper: return L("当前壁纸不能用作动态锁屏")
        case .appGroupUnavailable: return L("动态锁屏共享容器不可用")
        case .fullDiskAccessRequired: return L("动态锁屏需要完全磁盘访问权限")
        }
    }
}
