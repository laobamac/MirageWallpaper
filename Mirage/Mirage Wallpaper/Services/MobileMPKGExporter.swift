//
//  Mirage Wallpaper
//
//  Native Wallpaper Engine Android MPKG export for verified video wallpapers.
//

import Foundation

enum MobileMPKGExporter {
    private static let version = "PKGM0014"
    private static let copyBufferSize = 1024 * 1024

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
            Entry(name: wallpaper.project.file, source: .file(videoURL)),
            Entry(name: wallpaper.project.preview, source: .file(previewURL)),
            Entry(name: "project.json", source: .data(projectData)),
        ]
        try writeArchive(entries: entries, to: outputURL, progress: progress)
    }

    static func suggestedFilename(for wallpaper: WEWallpaper) -> String {
        let preferred = wallpaper.project.workshopid?.rawValue
            ?? wallpaper.wallpaperDirectory.lastPathComponent
        let cleaned = preferred
            .replacingOccurrences(of: "[^A-Za-z0-9._-]+", with: "-", options: .regularExpression)
            .trimmingCharacters(in: CharacterSet(charactersIn: ".-"))
        return "\(cleaned.isEmpty ? "wallpaper" : cleaned).mpkg"
    }

    private struct Entry {
        enum Source {
            case file(URL)
            case data(Data)
        }

        let name: String
        let source: Source

        var size: UInt64 {
            get throws {
                switch source {
                case .file(let url):
                    let values = try url.resourceValues(forKeys: [.fileSizeKey])
                    guard let size = values.fileSize else {
                        throw MobileMPKGExportError.cannotReadSource(url.lastPathComponent)
                    }
                    return UInt64(size)
                case .data(let data):
                    return UInt64(data.count)
                }
            }
        }
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
        guard !value.isEmpty,
              !value.contains("\0"),
              !value.contains("\\"),
              !value.hasPrefix("/") else { return false }
        let components = value.split(separator: "/", omittingEmptySubsequences: false)
        return components.allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
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

    private static func writeArchive(
        entries: [Entry],
        to outputURL: URL,
        progress: ((UInt64, UInt64) -> Void)?
    ) throws {
        guard !entries.isEmpty else { throw MobileMPKGExportError.invalidProject }
        var foldedNames = Set<String>()
        var offset: UInt64 = 0
        var table: [(entry: Entry, name: Data, offset: UInt32, size: UInt32)] = []
        for entry in entries {
            guard isSafeEntryName(entry.name),
                  foldedNames.insert(entry.name.lowercased()).inserted else {
                throw MobileMPKGExportError.unsafeProjectPath
            }
            let nameData = Data(entry.name.utf8)
            let size = try entry.size
            guard nameData.count <= 1024,
                  size <= UInt64(UInt32.max),
                  offset + size <= UInt64(UInt32.max) else {
                throw MobileMPKGExportError.wallpaperTooLarge
            }
            table.append((entry, nameData, UInt32(offset), UInt32(size)))
            offset += size
        }
        let totalDataSize = offset
        var writtenDataSize: UInt64 = 0
        progress?(0, totalDataSize)

        let fileManager = FileManager.default
        let destination = outputURL.standardizedFileURL
        try fileManager.createDirectory(
            at: destination.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let temporary = destination.deletingLastPathComponent().appending(
            path: ".\(destination.lastPathComponent).\(UUID().uuidString).tmp"
        )
        fileManager.createFile(atPath: temporary.path, contents: nil)
        defer { try? fileManager.removeItem(at: temporary) }

        let handle = try FileHandle(forWritingTo: temporary)
        do {
            try handle.write(contentsOf: littleEndianData(UInt32(version.utf8.count)))
            try handle.write(contentsOf: Data(version.utf8))
            try handle.write(contentsOf: littleEndianData(UInt32(table.count)))
            for item in table {
                try handle.write(contentsOf: littleEndianData(UInt32(item.name.count)))
                try handle.write(contentsOf: item.name)
                try handle.write(contentsOf: littleEndianData(item.offset))
                try handle.write(contentsOf: littleEndianData(item.size))
            }
            for item in table {
                switch item.entry.source {
                case .data(let data):
                    try handle.write(contentsOf: data)
                    writtenDataSize += UInt64(data.count)
                    progress?(writtenDataSize, totalDataSize)
                case .file(let sourceURL):
                    try copyFile(sourceURL, to: handle) { bytes in
                        writtenDataSize += bytes
                        progress?(writtenDataSize, totalDataSize)
                    }
                }
            }
            try handle.synchronize()
            try handle.close()
        } catch {
            try? handle.close()
            throw error
        }

        if fileManager.fileExists(atPath: destination.path) {
            try fileManager.removeItem(at: destination)
        }
        try fileManager.moveItem(at: temporary, to: destination)
    }

    private static func copyFile(
        _ sourceURL: URL,
        to output: FileHandle,
        progress: (UInt64) -> Void
    ) throws {
        let input = try FileHandle(forReadingFrom: sourceURL)
        defer { try? input.close() }
        while true {
            let data = try input.read(upToCount: copyBufferSize) ?? Data()
            if data.isEmpty { return }
            try output.write(contentsOf: data)
            progress(UInt64(data.count))
        }
    }

    private static func littleEndianData(_ value: UInt32) -> Data {
        var value = value.littleEndian
        return Swift.withUnsafeBytes(of: &value) { Data($0) }
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
