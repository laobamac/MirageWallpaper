import AppKit
import Foundation

enum MirageScreenSaverError: LocalizedError {
    case noWallpaper
    case unsupportedWallpaper
    case bundledSaverMissing
    case invalidConfiguration

    var errorDescription: String? {
        switch self {
        case .noWallpaper: return "请先播放一张壁纸"
        case .unsupportedWallpaper: return "当前壁纸不能用作屏保"
        case .bundledSaverMissing: return "App 内没有找到 Mirage 屏保组件"
        case .invalidConfiguration: return "无法生成屏保配置"
        }
    }
}

final class ScreenSaverManager {
    static let shared = ScreenSaverManager()

    private let fm = FileManager.default

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
        if fm.fileExists(atPath: installedURL.path) {
            _ = try fm.replaceItemAt(installedURL, withItemAt: stagingURL)
        } else {
            try fm.moveItem(at: stagingURL, to: installedURL)
        }
    }

    func uninstall() throws {
        if fm.fileExists(atPath: installedURL.path) { try fm.removeItem(at: installedURL) }
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
            "fillMode": runtime.fillMode.rawValue
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
        if let url = URL(string: "x-apple.systempreferences:com.apple.ScreenSaver-Settings.extension") {
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
