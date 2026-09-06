import AppKit
import CryptoKit
import Darwin
import Foundation

enum MirageScreenSaverError: LocalizedError {
    case noWallpaper
    case unsupportedWallpaper
    case bundledSaverMissing
    case invalidConfiguration
    case invalidBundle
    case screenSaverHostDidNotTerminate
    case installationVerificationFailed

    var errorDescription: String? {
        switch self {
        case .noWallpaper: return L("请先播放一张壁纸")
        case .unsupportedWallpaper: return L("当前壁纸不能用作屏保")
        case .bundledSaverMissing: return L("App 内没有找到 Mirage 屏保组件")
        case .invalidConfiguration: return L("无法生成屏保配置")
        case .invalidBundle: return L("Mirage 屏保组件不完整")
        case .screenSaverHostDidNotTerminate: return L("无法结束旧的系统屏保进程，请稍后重试")
        case .installationVerificationFailed: return L("屏保安装校验失败")
        }
    }
}

final class ScreenSaverManager {
    static let shared = ScreenSaverManager()

    private let fm = FileManager.default
    private let hostBundleIdentifiers = [
        "com.apple.ScreenSaver.Engine",
        "com.apple.ScreenSaver.Engine.legacyScreenSaver"
    ]
    private let wallpaperAgentBundleIdentifier = "com.apple.wallpaper.agent"
    private let fingerprintResourcePaths = [
        "Contents/Info.plist",
        "Contents/Frameworks/libMirageSceneSaver.dylib",
        "Contents/Resources/thumbnail.png",
        "Contents/Resources/thumbnail@2x.png"
    ]

    private init() {
        migrateObsoleteDynamicLockScreenIfNeeded()
    }

    var installedURL: URL {
        fm.homeDirectoryForCurrentUser.appending(path: "Library/Screen Savers/MirageScreenSaver.saver")
    }

    var configurationURL: URL {
        fm.homeDirectoryForCurrentUser.appending(path: "Library/Application Support/Mirage/screensaver.json")
    }

    var dynamicLockScreenInstalledURL: URL {
        fm.homeDirectoryForCurrentUser.appending(
            path: "Library/Screen Savers/MirageDynamicLockScreen.saver")
    }

    private var obsoleteDotDynamicLockScreenInstalledURL: URL {
        fm.homeDirectoryForCurrentUser.appending(
            path: "Library/Screen Savers/.MirageDynamicLockScreen.saver")
    }

    private var hiddenDynamicLockScreenInstalledURL: URL {
        fm.homeDirectoryForCurrentUser.appending(
            path: "Library/Application Support/Mirage/Components/MirageDynamicLockScreen.saver")
    }

    private var wallpaperStoreURL: URL {
        fm.homeDirectoryForCurrentUser.appending(
            path: "Library/Application Support/com.apple.wallpaper/Store/Index.plist")
    }

    var dynamicLockScreenConfigurationURL: URL {
        fm.homeDirectoryForCurrentUser.appending(
            path: "Library/Application Support/Mirage/dynamic-lock-screen-screensaver.json")
    }

    var isInstalled: Bool { fm.fileExists(atPath: installedURL.path) }

    var isDynamicLockScreenInstalled: Bool {
        fm.fileExists(atPath: dynamicLockScreenInstalledURL.path)
    }

    private func migrateObsoleteDynamicLockScreenIfNeeded() {
        let obsoleteURLs = [obsoleteDotDynamicLockScreenInstalledURL, hiddenDynamicLockScreenInstalledURL]
        let oldPaths = obsoleteURLs
            .map { $0.standardizedFileURL.path }
        var changed = false
        if let data = try? Data(contentsOf: wallpaperStoreURL),
           let value = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) {
            func visit(_ value: Any) -> Any {
                if let dictionary = value as? [String: Any] {
                    var result = dictionary
                    if let configuration = dictionary["Configuration"] as? Data,
                       let object = try? PropertyListSerialization.propertyList(from: configuration, options: [], format: nil) as? [String: Any],
                       let module = object["module"] as? [String: Any],
                       let relative = module["relative"] as? String,
                       let path = URL(string: relative)?.standardizedFileURL.path,
                       oldPaths.contains(path) {
                        var updatedModule = module
                        updatedModule["relative"] = dynamicLockScreenInstalledURL.absoluteString
                        var updatedObject = object
                        updatedObject["module"] = updatedModule
                        if let updatedData = try? PropertyListSerialization.data(fromPropertyList: updatedObject, format: .binary, options: 0) {
                            result["Configuration"] = updatedData
                            changed = true
                        }
                    }
                    for (key, nested) in result { result[key] = visit(nested) }
                    return result
                }
                if let array = value as? [Any] { return array.map(visit) }
                return value
            }
            let updated = visit(value)
            if changed, let updatedData = try? PropertyListSerialization.data(fromPropertyList: updated, format: .binary, options: 0) {
                try? fm.createDirectory(at: wallpaperStoreURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? updatedData.write(to: wallpaperStoreURL, options: .atomic)
            }
        }
        for url in obsoleteURLs
            where fm.fileExists(atPath: url.path) {
            do { try fm.removeItem(at: url) }
            catch { NSLog("[Mirage] 清理旧方案 B 锁屏组件失败: %@", error.localizedDescription) }
        }
    }

    func install() throws {
        guard let bundledURL = bundledSaverURL else { throw MirageScreenSaverError.bundledSaverMissing }
        try installComponent(
            bundledURL: bundledURL,
            installedURL: installedURL,
            bundleIdentifier: "cn.laobamac.Mirage.ScreenSaver",
            executableName: "MirageScreenSaver")
    }

    func installDynamicLockScreen() throws {
        guard let bundledURL = bundledDynamicLockScreenSaverURL else {
            throw MirageScreenSaverError.bundledSaverMissing
        }
        try installComponent(
            bundledURL: bundledURL,
            installedURL: dynamicLockScreenInstalledURL,
            bundleIdentifier: "cn.laobamac.Mirage.DynamicLockScreen",
            executableName: "MirageScreenSaver")
    }

    private func installComponent(bundledURL: URL, installedURL: URL,
                                  bundleIdentifier: String,
                                  executableName: String) throws {
        let directory = installedURL.deletingLastPathComponent()
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let stagingURL = directory.appending(
            path: ".\(installedURL.deletingPathExtension().lastPathComponent)-\(UUID().uuidString).saver")
        defer { try? fm.removeItem(at: stagingURL) }
        try fm.copyItem(at: bundledURL, to: stagingURL)
        let expectedFingerprint = try validatedFingerprint(
            of: stagingURL,
            bundleIdentifier: bundleIdentifier,
            executableName: executableName)

        // ScreenSaverEngine and its legacy extension keep the loaded bundle and
        // native dylibs mapped even after the bundle is replaced on disk. Stop
        // those hosts before the atomic swap so the next preview/activation is
        // forced to load the newly installed code.
        try terminateScreenSaverServices(restartWallpaperAgent: false)

        if fm.fileExists(atPath: installedURL.path) {
            _ = try fm.replaceItemAt(installedURL, withItemAt: stagingURL)
        } else {
            try fm.moveItem(at: stagingURL, to: installedURL)
        }
        guard try validatedFingerprint(
            of: installedURL,
            bundleIdentifier: bundleIdentifier,
            executableName: executableName) == expectedFingerprint else {
            throw MirageScreenSaverError.installationVerificationFailed
        }

        // WallpaperAgent owns the modern legacy-screen-saver extension and can
        // relaunch it immediately after ScreenSaverEngine exits. Restart the
        // complete service stack only after the verified swap, so any relaunched
        // host can map only the new executable and renderer dylib.
        try terminateScreenSaverServices(restartWallpaperAgent: true)
    }

    func uninstall() throws {
        if fm.fileExists(atPath: installedURL.path) { try fm.removeItem(at: installedURL) }
        try terminateScreenSaverServices(restartWallpaperAgent: true)
    }

    func restartForWallpaperStoreChange() throws {
        try terminateScreenSaverServices(restartWallpaperAgent: true)
    }

    /// The screen saver is copied out of the app bundle, so replacing Mirage.app
    /// alone cannot update an already installed saver. Keep an existing user
    /// installation aligned with the newly updated app on the next launch.
    func refreshInstalledVersionIfNeeded() {
        discardUnsupportedConfiguration()
        refreshInstalledComponent(
            installedURL: installedURL,
            bundledURL: bundledSaverURL,
            bundleIdentifier: "cn.laobamac.Mirage.ScreenSaver",
            executableName: "MirageScreenSaver",
            displayName: "屏保组件",
            install: install)
        refreshInstalledComponent(
            installedURL: dynamicLockScreenInstalledURL,
            bundledURL: bundledDynamicLockScreenSaverURL,
            bundleIdentifier: "cn.laobamac.Mirage.DynamicLockScreen",
            executableName: "MirageScreenSaver",
            displayName: "方案 B 锁屏组件",
            install: installDynamicLockScreen)
    }

    private func refreshInstalledComponent(
        installedURL: URL,
        bundledURL: URL?,
        bundleIdentifier: String,
        executableName: String,
        displayName: String,
        install: () throws -> Void
    ) {
        guard fm.fileExists(atPath: installedURL.path),
              let bundledURL,
              let bundledBundle = Bundle(url: bundledURL),
              let bundledBuild = bundledBundle.object(
                forInfoDictionaryKey: "CFBundleVersion") as? String else { return }
        do {
            let bundledFingerprint = try validatedFingerprint(
                of: bundledURL,
                bundleIdentifier: bundleIdentifier,
                executableName: executableName)
            if let installedFingerprint = try? validatedFingerprint(
                of: installedURL,
                bundleIdentifier: bundleIdentifier,
                executableName: executableName),
               installedFingerprint == bundledFingerprint {
                return
            }
            try install()
            NSLog("[Mirage] 已将已安装%@更新至构建 %@", displayName, bundledBuild)
        } catch {
            NSLog("[Mirage] 更新已安装%@失败: %@", displayName, error.localizedDescription)
        }
    }

    private func validatedFingerprint(of saverURL: URL, bundleIdentifier: String,
                                      executableName: String) throws -> String {
        guard let bundle = Bundle(url: saverURL),
              bundle.bundleIdentifier == bundleIdentifier,
              bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String != nil else {
            throw MirageScreenSaverError.invalidBundle
        }

        var hasher = SHA256()
        let paths = fingerprintResourcePaths + ["Contents/MacOS/\(executableName)"]
        for relativePath in paths {
            let fileURL = saverURL.appending(path: relativePath)
            guard fm.isReadableFile(atPath: fileURL.path) else {
                throw MirageScreenSaverError.invalidBundle
            }
            hasher.update(data: Data(relativePath.utf8))
            hasher.update(data: try Data(contentsOf: fileURL, options: [.mappedIfSafe]))
        }
        let codeResourcesPath = "Contents/_CodeSignature/CodeResources"
        let codeResourcesURL = saverURL.appending(path: codeResourcesPath)
        if fm.isReadableFile(atPath: codeResourcesURL.path) {
            hasher.update(data: Data(codeResourcesPath.utf8))
            hasher.update(data: try Data(
                contentsOf: codeResourcesURL, options: [.mappedIfSafe]))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    private func terminateScreenSaverServices(restartWallpaperAgent: Bool) throws {
        let identifiers = restartWallpaperAgent
            ? hostBundleIdentifiers + [wallpaperAgentBundleIdentifier]
            : hostBundleIdentifiers
        let running = identifiers.flatMap {
            NSRunningApplication.runningApplications(withBundleIdentifier: $0)
        }
        guard !running.isEmpty else { return }

        running.forEach { _ = $0.terminate() }
        if waitForTermination(of: running, timeout: 2) { return }

        running.filter { !hasExited($0) }.forEach {
            if !$0.forceTerminate() {
                _ = Darwin.kill($0.processIdentifier, SIGKILL)
            }
        }
        guard waitForTermination(of: running, timeout: 2) else {
            throw MirageScreenSaverError.screenSaverHostDidNotTerminate
        }
    }

    private func waitForTermination(of applications: [NSRunningApplication],
                                    timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if applications.allSatisfy(hasExited) { return true }
            Thread.sleep(forTimeInterval: 0.05)
        }
        return applications.allSatisfy(hasExited)
    }

    private func hasExited(_ application: NSRunningApplication) -> Bool {
        if application.isTerminated { return true }
        guard application.processIdentifier > 0 else { return true }
        return Darwin.kill(application.processIdentifier, 0) == -1 && errno == ESRCH
    }

    func configure(with wallpaper: WEWallpaper, runtime: WallpaperRuntimeState,
                   properties: [String: WEProjectProperty], fps: Int,
                   forDynamicLockScreen: Bool = false) throws {
        guard wallpaper.isValid else { throw MirageScreenSaverError.noWallpaper }
        guard wallpaper.kind == .video || wallpaper.kind == .scene else {
            throw MirageScreenSaverError.unsupportedWallpaper
        }

        var rawPropertyValues: [String: Any] = [:]
        for (key, property) in properties {
            switch property.propertyType {
            case .color:
                rawPropertyValues[key] = ["type": "color", "value": property.value.stringValue]
            case .bool:
                rawPropertyValues[key] = property.value.boolValue
            case .slider:
                rawPropertyValues[key] = property.value.doubleValue
            case .scenetexture, .file:
                rawPropertyValues[key] = ["type": "scenetexture", "value": property.value.stringValue]
            case .combo:
                rawPropertyValues[key] = property.value.jsonObjectValue
            case .usershortcut:
                var value: [String: Any] = [
                    "type": "usershortcut",
                    "value": property.value.stringValue
                ]
                if let icon = property.mirageShortcutIcon { value["icon"] = icon }
                rawPropertyValues[key] = value
            default:
                rawPropertyValues[key] = property.value.stringValue
            }
        }

        let object: [String: Any] = [
            "version": 1,
            "wallpaperID": wallpaper.id,
            "title": wallpaper.project.title,
            "kind": wallpaper.kind.rawValue,
            "entryPath": wallpaper.resolvedEntryURL.path,
            "playableEntryPath": playableVideoCacheURL(for: wallpaper.resolvedEntryURL).path,
            "rawProperties": rawPropertyValues,
            "fps": min(max(fps, 10), 60),
            "fillMode": runtime.fillMode.rawValue,
            "enableHDRVideo": AppDelegate.shared.globalSettingsViewModel.settings.shouldEnableHDRVideo,
            "loadFromMemory": (AppDelegate.shared.globalSettingsViewModel.settings.wallpaperLoadSource ?? .disk) == .memory,
            "language": MirageLocalization.shared.locale.identifier
        ]
        guard JSONSerialization.isValidJSONObject(object) else { throw MirageScreenSaverError.invalidConfiguration }
        let data = try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
        let targetURL = forDynamicLockScreen
            ? dynamicLockScreenConfigurationURL
            : configurationURL
        try fm.createDirectory(at: targetURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: targetURL, options: .atomic)
    }

    func updateLoadFromMemory(_ enabled: Bool, forDynamicLockScreen: Bool = false) {
        let targetURL = forDynamicLockScreen
            ? dynamicLockScreenConfigurationURL
            : configurationURL
        guard let data = try? Data(contentsOf: targetURL),
              var object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }
        object["loadFromMemory"] = enabled
        guard JSONSerialization.isValidJSONObject(object),
              let updated = try? JSONSerialization.data(
                withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
        else { return }
        try? updated.write(to: targetURL, options: .atomic)
    }

    func configuredWallpaperID() -> String? {
        supportedConfigurationObject(at: configurationURL)?["wallpaperID"] as? String
    }

    func configuredDynamicLockScreenWallpaperID() -> String? {
        supportedConfigurationObject(at: dynamicLockScreenConfigurationURL)?["wallpaperID"] as? String
    }

    func remapPersistedPaths(_ mappings: [String: String]) {
        guard !mappings.isEmpty else { return }
        let remapper = WallpaperPathRemapper(mappings)
        for url in [configurationURL, dynamicLockScreenConfigurationURL] {
            guard let data = try? Data(contentsOf: url),
                  let object = try? JSONSerialization.jsonObject(with: data) else { continue }
            func remap(_ value: Any) -> Any {
                if let string = value as? String { return remapper.path(string) }
                if let array = value as? [Any] { return array.map(remap) }
                if let dictionary = value as? [String: Any] {
                    return dictionary.mapValues(remap)
                }
                return value
            }
            let remapped = remap(object)
            guard JSONSerialization.isValidJSONObject(remapped),
                  let encoded = try? JSONSerialization.data(
                    withJSONObject: remapped, options: [.prettyPrinted, .sortedKeys]) else { continue }
            try? encoded.write(to: url, options: .atomic)
        }
    }

    private func playableVideoCacheURL(for source: URL) -> URL {
        let path = source.resolvingSymlinksInPath().path
        let digest = SHA256.hash(data: Data(path.utf8))
        let name = digest.map { String(format: "%02x", $0) }.joined() + ".mp4"
        return fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/VideoCache", directoryHint: .isDirectory)
            .appending(path: name)
    }

    func configuredWallpaperTitle() -> String? {
        supportedConfigurationObject(at: configurationURL)?["title"] as? String
    }

    func configuredDynamicLockScreenWallpaperTitle() -> String? {
        supportedConfigurationObject(at: dynamicLockScreenConfigurationURL)?["title"] as? String
    }

    func migrateDynamicLockScreenConfigurationIfNeeded() {
        guard supportedConfigurationObject(at: dynamicLockScreenConfigurationURL) == nil,
              supportedConfigurationObject(at: configurationURL) != nil,
              let data = try? Data(contentsOf: configurationURL) else { return }
        try? fm.createDirectory(
            at: dynamicLockScreenConfigurationURL.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try? data.write(to: dynamicLockScreenConfigurationURL, options: .atomic)
    }

    private func supportedConfigurationObject(at url: URL) -> [String: Any]? {
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let kind = object["kind"] as? String,
              kind == WallpaperKind.video.rawValue || kind == WallpaperKind.scene.rawValue
        else { return nil }
        return object
    }

    private func discardUnsupportedConfiguration() {
        for url in [configurationURL, dynamicLockScreenConfigurationURL] {
            if fm.fileExists(atPath: url.path),
               supportedConfigurationObject(at: url) == nil {
                try? fm.removeItem(at: url)
            }
        }
    }

    func openSystemSettings() {
        // On macOS 14, Screen Saver is a section of Wallpaper settings.  The
        // standalone ScreenSaver-Settings extension can fall back to General
        // instead of navigating to the intended System Settings page.
        if let url = URL(string: "x-apple.systempreferences:com.apple.Wallpaper-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    private var bundledSaverURL: URL? {
        bundledURL(named: "MirageScreenSaver.saver")
    }

    private var bundledDynamicLockScreenSaverURL: URL? {
        let name = "MirageDynamicLockScreen.saver"
        let candidates = [
            Bundle.main.resourceURL?.appending(path: "Screen Savers/\(name)"),
            Bundle.main.resourceURL?.appending(path: name)
        ]
        return candidates.compactMap { $0 }.first { fm.fileExists(atPath: $0.path) }
    }

    private func bundledURL(named name: String) -> URL? {
        let candidates = [
            Bundle.main.resourceURL?.appending(path: "Mirage Components/\(name)"),
            Bundle.main.resourceURL?.appending(path: "Screen Savers/\(name)"),
            Bundle.main.resourceURL?.appending(path: name)
        ]
        return candidates.compactMap { $0 }.first { fm.fileExists(atPath: $0.path) }
    }
}
