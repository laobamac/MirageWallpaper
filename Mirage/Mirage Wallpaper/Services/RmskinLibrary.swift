//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

enum RmskinImportError: LocalizedError, Identifiable {
    case notAnRmskin
    case extractionFailed(String)
    case copyFailed(String)

    var id: String { errorDescription ?? "unknown" }

    var errorDescription: String? {
        switch self {
        case .notAnRmskin: return "不是有效的 .rmskin 小组件包"
        case .extractionFailed(let m): return "解压失败：\(m)"
        case .copyFailed(let m): return "安装失败：\(m)"
        }
    }
}

// Manages the independent Rmskins library: installing (unzipping) .rmskin
// packages, scanning installed themes, and deleting them. Kept separate from
// WallpaperLibrary so widgets never mix with WEWallpaper items.
final class RmskinLibrary {
    static let shared = RmskinLibrary()

    private let fm = FileManager.default

    var rmskinsDirectory: URL {
        let base = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/Rmskins")
        if !fm.fileExists(atPath: base.path) {
            try? fm.createDirectory(at: base, withIntermediateDirectories: true)
        }
        return base
    }

    // MARK: - Scan

    func loadAll() -> [RmskinTheme] {
        guard let items = try? fm.contentsOfDirectory(
            at: rmskinsDirectory, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]) else { return [] }
        var result: [RmskinTheme] = []
        for url in items {
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            guard isDir else { continue }
            if let theme = RmskinTheme.load(from: url) {
                result.append(theme)
            }
        }
        return result
    }

    // MARK: - Install

    // Import a .rmskin (zip) file or a directory that contains RMSKIN.ini.
    @discardableResult
    func importAny(at url: URL) throws -> RmskinTheme {
        let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
        if isDir {
            if fm.fileExists(atPath: url.appending(path: "RMSKIN.ini").path) {
                return try installExtractedTheme(at: url, suggestedName: url.lastPathComponent)
            }
            throw RmskinImportError.notAnRmskin
        }
        return try installRmskinArchive(at: url)
    }

    // Unzip a .rmskin archive. .rmskin files are ZIPs with a trailing footer,
    // so unzip may return a non-zero "warning" status while still extracting.
    private func installRmskinArchive(at zipURL: URL) throws -> RmskinTheme {
        let tmp = fm.temporaryDirectory.appending(path: "mirage_rmskin_\(UUID().uuidString)")
        try? fm.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: tmp) }

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        proc.arguments = ["-o", "-qq", zipURL.path, "-d", tmp.path]
        let pipe = Pipe()
        proc.standardError = pipe
        proc.standardOutput = pipe
        do {
            try proc.run()
            proc.waitUntilExit()
        } catch {
            throw RmskinImportError.extractionFailed(error.localizedDescription)
        }

        // Locate the directory containing RMSKIN.ini (root or one level down).
        guard let themeRoot = findThemeRoot(in: tmp) else {
            throw RmskinImportError.notAnRmskin
        }
        let suggested = zipURL.deletingPathExtension().lastPathComponent
        return try installExtractedTheme(at: themeRoot, suggestedName: suggested)
    }

    private func findThemeRoot(in dir: URL) -> URL? {
        if fm.fileExists(atPath: dir.appending(path: "RMSKIN.ini").path) { return dir }
        guard let items = try? fm.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.isDirectoryKey]) else { return nil }
        for url in items {
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            if isDir, fm.fileExists(atPath: url.appending(path: "RMSKIN.ini").path) {
                return url
            }
        }
        return nil
    }

    private func installExtractedTheme(at themeRoot: URL, suggestedName: String) throws -> RmskinTheme {
        // Prefer the RMSKIN.ini Name for the destination folder.
        let name = RmskinTheme.load(from: themeRoot)?.name ?? suggestedName
        let dest = uniqueDestination(for: sanitize(name))
        do {
            if themeRoot.path == dest.path { /* already in place */ }
            else { try fm.copyItem(at: themeRoot, to: dest) }
        } catch {
            throw RmskinImportError.copyFailed(error.localizedDescription)
        }
        guard let theme = RmskinTheme.load(from: dest) else {
            throw RmskinImportError.notAnRmskin
        }
        return theme
    }

    private func sanitize(_ name: String) -> String {
        let invalid = CharacterSet(charactersIn: "/\\:*?\"<>|")
        return name.components(separatedBy: invalid).joined(separator: "_")
    }

    private func uniqueDestination(for name: String) -> URL {
        var dest = rmskinsDirectory.appending(path: name)
        var counter = 1
        while fm.fileExists(atPath: dest.path) {
            dest = rmskinsDirectory.appending(path: "\(name)_\(counter)")
            counter += 1
        }
        return dest
    }

    // MARK: - Delete

    func delete(_ theme: RmskinTheme) throws {
        try fm.removeItem(at: theme.themeDirectory)
    }

    func trash(_ theme: RmskinTheme) throws {
        try fm.trashItem(at: theme.themeDirectory, resultingItemURL: nil)
    }
}
