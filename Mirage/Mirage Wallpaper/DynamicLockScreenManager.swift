//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import Darwin
import Foundation

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
    let fillMode: String
    var loadFromMemory: Bool?
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

    private let enabledKey = "Mirage.DynamicLockScreen.Enabled"
    private let confirmationKey = "Mirage.DynamicLockScreen.Confirmed"
    private let appGroupID = "group.cn.laobamac.Mirage"
    private let configurationName = "dynamic-lock-screen.json"
    private let registeredExtensionFingerprintKey =
        "Mirage.DynamicLockScreen.RegisteredExtensionFingerprint"
    private let extensionQueue = DispatchQueue(label: "cn.laobamac.Mirage.wallpaper-extension", qos: .utility)

    private init() {
        let storedEnabled = UserDefaults.standard.bool(forKey: enabledKey)
        isEnabled = storedEnabled
            && (DynamicLockScreenModeStore.active == nil
                || DynamicLockScreenModeStore.active == .extensionMode)
        if DynamicLockScreenModeStore.active == nil, isEnabled {
            DynamicLockScreenModeStore.activate(.extensionMode)
        }
        discardUnsupportedConfiguration()
        if !isEnabled || DynamicLockScreenModeStore.active != .extensionMode {
            extensionQueue.async {
                if Self.removeRegisteredExtensions(except: nil) {
                    _ = Self.restartWallpaperAgent()
                }
            }
        }
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
        ScreenSaverDynamicLockScreenManager.shared.disableForModeSwitch()
        DynamicLockScreenModeStore.activate(.extensionMode)
        UserDefaults.standard.set(true, forKey: enabledKey)
        isEnabled = true
        isConfirmationPresented = false
        activateStoredConfiguration()
        if isConfigured {
            registerExtension()
        }
        return true
    }

    func setEnabled(_ enabled: Bool) {
        guard enabled else {
            isEnabled = false
            UserDefaults.standard.set(false, forKey: enabledKey)
            DynamicLockScreenModeStore.deactivate(.extensionMode)
            clearConfiguration()
            unregisterExtension()
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
        activateStoredConfiguration()
        if isConfigured {
            registerExtension()
        }
    }

    func disableForModeSwitch() {
        guard isEnabled || DynamicLockScreenModeStore.active == .extensionMode else { return }
        isEnabled = false
        UserDefaults.standard.set(false, forKey: enabledKey)
        DynamicLockScreenModeStore.deactivate(.extensionMode)
        clearConfiguration()
        unregisterExtension()
    }

    func configureCurrentWallpaper(_ wallpaper: WEWallpaper,
                                   runtime: WallpaperRuntimeState,
                                   properties: [String: WEProjectProperty],
                                   fps: Int,
                                   displayIDs: [UInt32]) throws {
        guard canUse else { throw DynamicLockScreenError.notEnabled }
        guard wallpaper.isValid else { throw DynamicLockScreenError.noWallpaper }
        guard wallpaper.kind == .video || wallpaper.kind == .scene else {
            throw DynamicLockScreenError.unsupportedWallpaper
        }
        guard let container = sharedContainerURL else { throw DynamicLockScreenError.appGroupUnavailable }

        do {
            try FileManager.default.createDirectory(at: container, withIntermediateDirectories: true)
        } catch {
            if Self.isSharedContainerPermissionError(error) {
                throw DynamicLockScreenError.fullDiskAccessRequired
            }
            throw error
        }
        let deployment = try deploy(wallpaper: wallpaper, in: container)
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
                rawPropertyValues[key] = ["type": "scenetexture", "value": value]
            case .combo:
                rawPropertyValues[key] = property.value.jsonObjectValue
            default:
                rawPropertyValues[key] = property.value.stringValue
            }
        }
        let configuration = DynamicLockScreenConfiguration(
            version: 2,
            enabled: true,
            displays: Dictionary(uniqueKeysWithValues: displayIDs.map { displayID in
                let fallbackSource = DesktopOverrideService.shared.dynamicLockScreenFallbackURL(
                    forDisplay: displayID)
                let fallbackURL = fallbackSource.flatMap {
                    try? deployDesktopFallback(source: $0, displayID: displayID, in: container)
                }
                let systemFallbackSource = DesktopOverrideService.shared
                    .dynamicLockScreenSystemFallbackURL(forDisplay: displayID)
                let systemFallbackURL: URL?
                if systemFallbackSource?.resolvingSymlinksInPath()
                    == fallbackSource?.resolvingSymlinksInPath() {
                    systemFallbackURL = fallbackURL
                } else {
                    systemFallbackURL = systemFallbackSource.flatMap {
                        try? deployDesktopFallback(source: $0, displayID: displayID, in: container)
                    }
                }
                let record = DynamicLockScreenDisplayConfiguration(
                    displayID: displayID,
                    wallpaperID: wallpaper.id,
                    title: wallpaper.project.title,
                    kind: wallpaper.kind.rawValue,
                    renderDirectory: deployment.renderDirectory.path,
                    entryPath: deployment.entryURL.path,
                    previewPath: deployment.previewURL?.path,
                    desktopFallbackPath: fallbackURL?.path,
                    systemFallbackPath: systemFallbackURL?.path,
                    rawProperties: rawPropertyValues.mapValues(AnyCodableValue.init),
                    fps: min(max(fps, 10), 60),
                    fillMode: runtime.fillMode.rawValue,
                    loadFromMemory: (AppDelegate.shared.globalSettingsViewModel.settings.wallpaperLoadSource ?? .disk) == .memory
                )
                return ("display-\(displayID)", record)
            })
        )
        let data = try JSONEncoder().encode(configuration)
        do {
            try data.write(to: container.appendingPathComponent(configurationName), options: .atomic)
        } catch {
            if Self.isSharedContainerPermissionError(error) {
                throw DynamicLockScreenError.fullDiskAccessRequired
            }
            throw error
        }
        keepDeployment = true
        notifyConfigurationChanged()
        cleanupDeployments(except: deployment.root)
        cleanupDesktopFallbacks()
        registerExtension()
    }

    func updateLoadFromMemory(_ enabled: Bool) {
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data)
        else { return }
        for key in configuration.displays.keys {
            configuration.displays[key]?.loadFromMemory = enabled
        }
        guard let updated = try? JSONEncoder().encode(configuration),
              (try? updated.write(to: configurationURL, options: .atomic)) != nil
        else { return }
        notifyConfigurationChanged()
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
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data),
              var record = configuration.displays["display-\(displayID)"],
              let container = sharedContainerURL else { return false }
        let fallback = try deployDesktopFallback(
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
                  let fallback = try? deployDesktopFallback(
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
        restoreSystemDesktopFallbacks()
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data)
        else {
            notifyConfigurationChanged()
            return
        }
        configuration.enabled = false
        if let updated = try? JSONEncoder().encode(configuration) {
            try? updated.write(to: configurationURL, options: .atomic)
        }
        notifyConfigurationChanged()
    }

    func openSystemSettings() {
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

    private func playableEntryURL(for wallpaper: WEWallpaper) -> URL {
        let source = wallpaper.resolvedEntryURL.resolvingSymlinksInPath()
        guard wallpaper.kind == .video else { return source }
        let digest = SHA256.hash(data: Data(source.path.utf8)).map { String(format: "%02x", $0) }.joined()
        let cache = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Mirage/VideoCache", isDirectory: true)
            .appendingPathComponent("\(digest).mp4")
        return FileManager.default.fileExists(atPath: cache.path) ? cache : source
    }

    private func deploy(wallpaper: WEWallpaper, in container: URL) throws -> (root: URL, renderDirectory: URL, entryURL: URL, previewURL: URL?) {
        let deployments = container.appendingPathComponent("DynamicLockScreen/Deployments", isDirectory: true)
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
            let previewURL = try deployPreview(for: wallpaper, in: root)
            return (root, renderDirectory, entryURL, previewURL)
        } catch {
            try? FileManager.default.removeItem(at: root)
            throw error
        }
    }

    private func deployPropertyAsset(_ path: String, key: String, in root: URL) throws -> String {
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

    private func deployPreview(for wallpaper: WEWallpaper, in root: URL) throws -> URL? {
        let source = wallpaper.previewURL.resolvingSymlinksInPath()
        guard !source.hasDirectoryPath, FileManager.default.fileExists(atPath: source.path) else { return nil }
        let extensionName = source.pathExtension.isEmpty ? "jpg" : source.pathExtension
        let destination = root.appendingPathComponent("preview.\(extensionName)")
        if !FileManager.default.fileExists(atPath: destination.path) {
            try linkOrCopy(source, to: destination)
        }
        return destination
    }

    private func deployDesktopFallback(source: URL, displayID: UInt32, in container: URL) throws -> URL {
        let source = source.resolvingSymlinksInPath()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: source.path, isDirectory: &isDirectory),
              !isDirectory.boolValue else {
            throw CocoaError(.fileReadNoSuchFile)
        }
        let directory = container.appendingPathComponent("DynamicLockScreen/DesktopFallbacks", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let extensionName = source.pathExtension.isEmpty ? "png" : source.pathExtension
        let destination = directory.appendingPathComponent(
            "display-\(displayID)-\(UUID().uuidString.lowercased()).\(extensionName)")
        try FileManager.default.copyItem(at: source, to: destination)
        return destination
    }

    private func linkOrCopy(_ source: URL, to destination: URL) throws {
        do {
            try FileManager.default.linkItem(at: source, to: destination)
        } catch {
            try FileManager.default.copyItem(at: source, to: destination)
        }
    }

    private static func isSharedContainerPermissionError(_ error: Error) -> Bool {
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

    private func registerExtension() {
        guard let appURL = Bundle.main.bundleURL as URL? else { return }
        let extensionURL = appURL.appendingPathComponent("Contents/Extensions/MirageWallpaperExtension.appex")
        guard FileManager.default.fileExists(atPath: extensionURL.path),
              let fingerprint = extensionFingerprint(at: extensionURL) else {
            registrationErrorMessage = L("动态锁屏扩展组件缺失或损坏，请重新安装 Mirage")
            MirageLogService.shared.append(
                "动态锁屏扩展注册失败: 扩展组件缺失或指纹计算失败",
                source: "wallpaper-extension"
            )
            return
        }
        registrationErrorMessage = nil
        extensionQueue.async { [weak self] in
            let result = Self.performRegistration(
                appURL: appURL,
                extensionURL: extensionURL,
                fingerprint: fingerprint
            )
            _ = Task { @MainActor [weak self] in
                guard let self else { return }
                if let message = result.errorMessage {
                    self.registrationErrorMessage = message
                } else if let fingerprint = result.fingerprint {
                    UserDefaults.standard.set(fingerprint, forKey: self.registeredExtensionFingerprintKey)
                }
            }
        }
    }

    private nonisolated static func performRegistration(
        appURL: URL,
        extensionURL: URL,
        fingerprint: String
    ) -> (fingerprint: String?, errorMessage: String?) {
        let identifier = "cn.laobamac.Mirage.WallpaperExtension"
        guard registerContainingApp(appURL) else {
            MirageLogService.shared.append(
                "动态锁屏扩展注册失败: lsregister 无法注册主程序 \(appURL.path)",
                source: "wallpaper-extension"
            )
            return (nil, L("无法向系统注册 Mirage，请重试或重启 Mirage 后重新开启动态锁屏"))
        }
        var registered = false
        for attempt in 1...3 {
            if attempt > 1 {
                Thread.sleep(forTimeInterval: 1)
            }
            MirageLogService.shared.append(
                "动态锁屏扩展注册尝试 \(attempt)/3",
                source: "wallpaper-extension"
            )
            _ = runPluginkit(["-a", extensionURL.path])
            _ = runPluginkit(["-e", "use", "-i", identifier])
            if isRegisteredExtension(at: extensionURL.path) {
                registered = true
                break
            }
        }
        guard registered else {
            MirageLogService.shared.append(
                "动态锁屏扩展注册失败: pluginkit 注册后未找到 \(extensionURL.path)",
                source: "wallpaper-extension"
            )
            return (nil, L("动态锁屏扩展注册失败，请重试；若仍失败请重启 Mirage 后重新开启动态锁屏"))
        }
        if !isRegisteredExtension(at: extensionURL.path, elected: true) {
            MirageLogService.shared.append(
                "动态锁屏扩展已注册但未被选中，重新执行 use 选举",
                source: "wallpaper-extension"
            )
            _ = runPluginkit(["-e", "use", "-i", identifier])
        }
        _ = removeRegisteredExtensions(except: extensionURL.path)
        guard restartWallpaperAgent() else {
            MirageLogService.shared.append(
                "动态锁屏扩展注册失败: 无法重启 com.apple.wallpaper.agent",
                source: "wallpaper-extension"
            )
            return (nil, L("动态锁屏扩展注册失败：无法重启系统墙纸服务，请重试"))
        }
        MirageLogService.shared.append(
            "动态锁屏扩展注册成功: \(extensionURL.path)",
            source: "wallpaper-extension"
        )
        return (fingerprint, nil)
    }

    private func unregisterExtension() {
        registrationErrorMessage = nil
        extensionQueue.async { [weak self] in
            guard let self else { return }
            if Self.removeRegisteredExtensions(except: nil) {
                _ = Self.restartWallpaperAgent()
            }
            UserDefaults.standard.removeObject(forKey: self.registeredExtensionFingerprintKey)
        }
    }

    private nonisolated static func runPluginkit(_ arguments: [String]) -> Bool {
        runTool("/usr/bin/pluginkit", arguments: arguments).success
    }

    private nonisolated static func registerContainingApp(_ appURL: URL) -> Bool {
        let tool = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
        guard FileManager.default.isExecutableFile(atPath: tool) else { return false }
        return runTool(tool, arguments: ["-f", appURL.path]).success
    }

    private nonisolated static func isRegisteredExtension(at path: String, elected: Bool = false) -> Bool {
        let result = runTool("/usr/bin/pluginkit", arguments: ["-mAv", "-i", "cn.laobamac.Mirage.WallpaperExtension"])
        guard result.success else { return false }
        return result.output.split(whereSeparator: \.isNewline).contains { line in
            guard line.contains(path) else { return false }
            guard elected else { return true }
            return line.split(whereSeparator: \.isWhitespace).first?.contains("+") ?? false
        }
    }

    private nonisolated static func runTool(_ executable: String, arguments: [String]) -> (success: Bool, output: String) {
        let process = Process()
        let outputURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("mirage-registration-\(UUID().uuidString).log")
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        do {
            try Data().write(to: outputURL, options: .withoutOverwriting)
            let handle = try FileHandle(forWritingTo: outputURL)
            process.standardOutput = handle
            process.standardError = handle
            try process.run()
            process.waitUntilExit()
            try handle.close()
            let data = try Data(contentsOf: outputURL)
            try? FileManager.default.removeItem(at: outputURL)
            return (process.terminationStatus == 0, String(data: data, encoding: .utf8) ?? "")
        } catch {
            try? FileManager.default.removeItem(at: outputURL)
            return (false, "")
        }
    }

    private nonisolated static func removeRegisteredExtensions(except path: String?) -> Bool {
        let result = runTool("/usr/bin/pluginkit", arguments: ["-mAv", "-p", "com.apple.wallpaper"])
        guard result.success else { return false }
        let output = result.output
        let identifier = "cn.laobamac.Mirage.WallpaperExtension"
        var removed = false
        for line in output.split(whereSeparator: \.isNewline) where line.contains(identifier) {
            guard let slash = line.firstIndex(of: "/") else { continue }
            let candidatePath = String(line[slash...]).trimmingCharacters(in: .whitespacesAndNewlines)
            guard candidatePath.hasSuffix(".appex"), candidatePath != path else { continue }
            removed = runPluginkit(["-r", candidatePath]) || removed
        }
        return removed
    }

    private func extensionFingerprint(at extensionURL: URL) -> String? {
        let paths = [
            "Contents/Info.plist",
            "Contents/MacOS/MirageWallpaperExtension",
            "Contents/Frameworks/libMirageSceneSaver.dylib"
        ]
        var hasher = SHA256()
        for path in paths {
            let url = extensionURL.appendingPathComponent(path)
            guard let data = try? Data(contentsOf: url, options: [.mappedIfSafe]) else {
                return nil
            }
            hasher.update(data: Data(path.utf8))
            hasher.update(data: data)
        }
        let codeResourcesPath = "Contents/_CodeSignature/CodeResources"
        let codeResourcesURL = extensionURL.appendingPathComponent(codeResourcesPath)
        if let data = try? Data(contentsOf: codeResourcesURL, options: [.mappedIfSafe]) {
            hasher.update(data: Data(codeResourcesPath.utf8))
            hasher.update(data: data)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    private nonisolated static func restartWallpaperAgent() -> Bool {
        let applications = NSRunningApplication.runningApplications(
            withBundleIdentifier: "com.apple.wallpaper.agent")
        guard !applications.isEmpty else { return true }
        applications.forEach { _ = $0.terminate() }
        let deadline = Date().addingTimeInterval(2)
        while Date() < deadline {
            if applications.allSatisfy({ $0.isTerminated }) { return true }
            Thread.sleep(forTimeInterval: 0.05)
        }
        applications.filter { !$0.isTerminated }.forEach { application in
            if !application.forceTerminate() {
                _ = Darwin.kill(application.processIdentifier, SIGKILL)
            }
        }
        let forcedDeadline = Date().addingTimeInterval(2)
        while Date() < forcedDeadline {
            if applications.allSatisfy({ $0.isTerminated }) { return true }
            Thread.sleep(forTimeInterval: 0.05)
        }
        return applications.allSatisfy({ $0.isTerminated })
    }

    private func activateStoredConfiguration() {
        guard let configurationURL,
              let data = try? Data(contentsOf: configurationURL),
              var configuration = try? JSONDecoder().decode(
                DynamicLockScreenConfiguration.self, from: data)
        else { return }
        configuration.enabled = true
        guard let updated = try? JSONEncoder().encode(configuration),
              (try? updated.write(to: configurationURL, options: .atomic)) != nil
        else { return }
        notifyConfigurationChanged()
    }

    private func discardUnsupportedConfiguration() {
        guard let url = configurationURL,
              let data = try? Data(contentsOf: url),
              let configuration = try? JSONDecoder().decode(DynamicLockScreenConfiguration.self, from: data) else { return }
        let supported = !configuration.displays.isEmpty && configuration.displays.values.allSatisfy {
            $0.kind == WallpaperKind.video.rawValue || $0.kind == WallpaperKind.scene.rawValue
        }
        if !supported {
            try? FileManager.default.removeItem(at: url)
            if let container = sharedContainerURL {
                try? FileManager.default.removeItem(at: container.appendingPathComponent("DynamicLockScreen/Deployments", isDirectory: true))
            }
            notifyConfigurationChanged()
        }
    }

    private func notifyConfigurationChanged() {
        let center = CFNotificationCenterGetDarwinNotifyCenter()
        CFNotificationCenterPostNotification(
            center,
            CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.configurationChanged" as CFString),
            nil,
            nil,
            true
        )
    }

    private func notifyDesktopFallbackChanged() {
        CFNotificationCenterPostNotification(
            CFNotificationCenterGetDarwinNotifyCenter(),
            CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.desktopFallbackChanged" as CFString),
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
