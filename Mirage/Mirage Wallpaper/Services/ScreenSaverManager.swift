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
    private let fingerprintPaths = [
        "Contents/Info.plist",
        "Contents/MacOS/MirageScreenSaver",
        "Contents/Frameworks/libMirageSceneSaver.dylib",
        "Contents/Resources/thumbnail.png",
        "Contents/Resources/thumbnail@2x.png"
    ]

    var installedURL: URL {
        fm.homeDirectoryForCurrentUser.appending(path: "Library/Screen Savers/MirageScreenSaver.saver")
    }

    var configurationURL: URL {
        fm.homeDirectoryForCurrentUser.appending(path: "Library/Application Support/Mirage/screensaver.json")
    }

    var isInstalled: Bool { fm.fileExists(atPath: installedURL.path) }

    func install() throws {
        guard let bundledURL = bundledSaverURL else { throw MirageScreenSaverError.bundledSaverMissing }
        let directory = installedURL.deletingLastPathComponent()
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let stagingURL = directory.appending(path: ".MirageScreenSaver-\(UUID().uuidString).saver")
        defer { try? fm.removeItem(at: stagingURL) }
        try fm.copyItem(at: bundledURL, to: stagingURL)
        let expectedFingerprint = try validatedFingerprint(of: stagingURL)

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
        guard try validatedFingerprint(of: installedURL) == expectedFingerprint else {
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

    /// The screen saver is copied out of the app bundle, so replacing Mirage.app
    /// alone cannot update an already installed saver. Keep an existing user
    /// installation aligned with the newly updated app on the next launch.
    func refreshInstalledVersionIfNeeded() {
        guard isInstalled,
              let bundledURL = bundledSaverURL,
              let installedBundle = Bundle(url: installedURL),
              let bundledBundle = Bundle(url: bundledURL),
              let installedBuild = installedBundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String,
              let bundledBuild = bundledBundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String,
              installedBuild != bundledBuild else { return }

        do {
            try install()
            NSLog("[Mirage] 已将已安装屏保组件更新至构建 %@", bundledBuild)
        } catch {
            NSLog("[Mirage] 更新已安装屏保组件失败: %@", error.localizedDescription)
        }
    }

    private func validatedFingerprint(of saverURL: URL) throws -> String {
        guard let bundle = Bundle(url: saverURL),
              bundle.bundleIdentifier == "cn.laobamac.Mirage.ScreenSaver",
              bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String != nil else {
            throw MirageScreenSaverError.invalidBundle
        }

        var hasher = SHA256()
        for relativePath in fingerprintPaths {
            let fileURL = saverURL.appending(path: relativePath)
            guard fm.isReadableFile(atPath: fileURL.path) else {
                throw MirageScreenSaverError.invalidBundle
            }
            hasher.update(data: Data(relativePath.utf8))
            hasher.update(data: try Data(contentsOf: fileURL, options: [.mappedIfSafe]))
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
                   properties: [String: WEProjectProperty], fps: Int) throws {
        guard wallpaper.isValid else { throw MirageScreenSaverError.noWallpaper }
        guard wallpaper.kind != .unsupported else { throw MirageScreenSaverError.unsupportedWallpaper }

        var propertyValues: [String: Any] = [:]
        var rawPropertyValues: [String: Any] = [:]
        for (key, property) in properties {
            let value: Any
            switch property.value {
            case .bool(let bool): value = bool
            case .number(let number): value = number
            case .string(let string): value = string
            }
            propertyValues[key] = ["value": value]
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
            default:
                rawPropertyValues[key] = property.value.stringValue
            }
        }

        let object: [String: Any] = [
            "version": 1,
            "wallpaperID": wallpaper.id,
            "title": wallpaper.project.title,
            "kind": wallpaper.kind.rawValue,
            "renderDirectory": wallpaper.renderDirectory.path,
            "entryPath": wallpaper.resolvedEntryURL.path,
            "assetOverlays": wallpaper.assetOverlayDirectories.map(\.path),
            "properties": propertyValues,
            "rawProperties": rawPropertyValues,
            "fps": min(max(fps, 10), 60),
            "fillMode": runtime.fillMode.rawValue,
            "language": MirageLocalization.shared.locale.identifier
        ]
        guard JSONSerialization.isValidJSONObject(object) else { throw MirageScreenSaverError.invalidConfiguration }
        let data = try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
        try fm.createDirectory(at: configurationURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: configurationURL, options: .atomic)
    }

    func configuredWallpaperID() -> String? {
        guard let data = try? Data(contentsOf: configurationURL),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return object["wallpaperID"] as? String
    }

    func configuredWallpaperTitle() -> String? {
        guard let data = try? Data(contentsOf: configurationURL),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return object["title"] as? String
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
        let candidates = [
            Bundle.main.resourceURL?.appending(path: "Screen Savers/MirageScreenSaver.saver"),
            Bundle.main.resourceURL?.appending(path: "MirageScreenSaver.saver")
        ]
        return candidates.compactMap { $0 }.first { fm.fileExists(atPath: $0.path) }
    }
}
