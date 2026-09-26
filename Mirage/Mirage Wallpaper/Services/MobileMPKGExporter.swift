//
//  Mirage Wallpaper
//
//  Native Wallpaper Engine Android MPKG export for verified video wallpapers.
//

import Foundation

enum MobileMPKGExporter {
    private static let version = "PKGM0014"
    private typealias Entry = MobilePackageArchive.Entry

    static func export(
        _ wallpaper: WEWallpaper,
        to outputURL: URL,
        progress: ((UInt64, UInt64) -> Void)? = nil
    ) throws {
        guard wallpaper.isValid, wallpaper.kind == .video else {
            throw MobileMPKGExportError.unsupportedWallpaperType(wallpaper.kind)
        }
        guard let videoURL = containedFile(
            wallpaper.project.file,
            in: wallpaper.renderDirectory
        ), let previewURL = containedFile(
            wallpaper.project.preview,
            in: wallpaper.wallpaperDirectory
        ) else {
            throw MobileMPKGExportError.unsafeProjectPath
        }

        let projectData = try mobileProjectData(
            file: wallpaper.project.file,
            preview: wallpaper.project.preview,
            title: wallpaper.project.title
        )
        let entries = [
            try Entry.file(named: wallpaper.project.file, at: videoURL),
            try Entry.file(named: wallpaper.project.preview, at: previewURL),
            Entry(name: "project.json", source: .data(projectData)),
        ]
        try MobilePackageArchive.write(entries: entries, version: version, to: outputURL, progress: progress)
    }

    static func suggestedFilename(for wallpaper: WEWallpaper) -> String {
        let preferred = wallpaper.project.workshopid?.rawValue
            ?? wallpaper.wallpaperDirectory.lastPathComponent
        let cleaned = preferred
            .replacingOccurrences(of: "[^A-Za-z0-9._-]+", with: "-", options: .regularExpression)
            .trimmingCharacters(in: CharacterSet(charactersIn: ".-"))
        return "\(cleaned.isEmpty ? "wallpaper" : cleaned).mpkg"
    }

    private static func containedFile(_ relativePath: String, in directory: URL) -> URL? {
        guard isSafeEntryName(relativePath) else { return nil }
        let root = directory.standardizedFileURL.resolvingSymlinksInPath()
        let candidate = directory
            .appending(path: relativePath)
            .standardizedFileURL
            .resolvingSymlinksInPath()
        let rootPath = root.path.hasSuffix("/") ? root.path : root.path + "/"
        guard candidate.path.hasPrefix(rootPath),
              FileManager.default.fileExists(atPath: candidate.path) else { return nil }
        return candidate
    }

    private static func isSafeEntryName(_ value: String) -> Bool {
        MobilePackageArchive.isSafeEntryName(value)
    }

    private static func mobileProjectData(
        file: String,
        preview: String,
        title: String
    ) throws -> Data {
        guard isSafeEntryName(file), isSafeEntryName(preview) else {
            throw MobileMPKGExportError.unsafeProjectPath
        }
        let object: [String: Any] = [
            "file": file,
            "preview": preview,
            "title": title,
            "type": "video",
        ]
        let compact = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        guard let json = String(data: compact, encoding: .utf8) else {
            throw MobileMPKGExportError.invalidProject
        }
        return Data(json.utf8)
    }

}

enum MobileMPKGExportError: LocalizedError {
    case unsupportedWallpaperType(WallpaperKind)
    case unsafeProjectPath
    case invalidProject
    case cannotReadSource(String)
    case wallpaperTooLarge

    var errorDescription: String? {
        switch self {
        case .unsupportedWallpaperType(let kind):
            return L("暂不支持将%@壁纸导出到移动设备。", kind.displayName)
        case .unsafeProjectPath:
            return L("壁纸包含不安全或无效的文件路径。")
        case .invalidProject:
            return L("无法生成移动设备壁纸项目。")
        case .cannotReadSource(let name):
            return L("无法读取壁纸文件：%@", name)
        case .wallpaperTooLarge:
            return L("移动设备壁纸不能超过 4 GB。")
        }
    }
}
