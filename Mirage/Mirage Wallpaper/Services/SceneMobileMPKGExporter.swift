//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

//  Native C++20 conversion of desktop scene packages and loose scenes for Wallpaper Engine Android.
//

import AVFoundation
import Foundation

private typealias SceneMobileProgressTrampoline = @convention(c) (Double, UnsafeMutableRawPointer?) -> Void

@_silgen_name("MirageSceneMobileExportFlat")
private func MirageSceneMobileExportFlat(
    _ wallpaperDir: UnsafePointer<CChar>,
    _ renderDir: UnsafePointer<CChar>,
    _ overlayDirs: UnsafePointer<UnsafePointer<CChar>?>?,
    _ overlayDirCount: Int,
    _ outputPath: UnsafePointer<CChar>,
    _ zcompatDir: UnsafePointer<CChar>?,
    _ propertyOverridesJSON: UnsafePointer<CChar>?,
    _ textureReduction: Int32,
    _ pixelArt: Int32,
    _ compressionMode: Int32,
    _ keepSounds: Int32,
    _ progressCallback: SceneMobileProgressTrampoline?,
    _ progressContext: UnsafeMutableRawPointer?,
    _ cancelFlag: UnsafePointer<Int32>?,
    _ outDetailInt: UnsafeMutablePointer<Int32>?,
    _ outErrorDetail: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
) -> Int32

@_silgen_name("MirageSceneMobileFreeString")
private func MirageSceneMobileFreeString(_ value: UnsafeMutablePointer<CChar>?)

enum SceneMobileMPKGExporter {
    private static let statusOK: Int32 = 0
    private static let statusCancelled: Int32 = 1
    private static let statusInvalidProject: Int32 = 2
    private static let statusUnsafeProjectPath: Int32 = 3
    private static let statusInvalidPackage: Int32 = 4
    private static let statusTruncatedPackage: Int32 = 5
    private static let statusUnsupportedPackageVersion: Int32 = 6
    private static let statusInvalidTexture: Int32 = 7
    private static let statusUnsupportedTextureLayout: Int32 = 8
    private static let statusUnsupportedTextureFormat: Int32 = 9
    private static let statusWallpaperTooLarge: Int32 = 10

    private struct PackageEntry {
        let name: String
        let absoluteOffset: UInt64
        let size: UInt64
    }

    private final class ProgressBox {
        let handler: (Double) -> Void
        init(_ handler: @escaping (Double) -> Void) {
            self.handler = handler
        }
    }

    static func export(
        _ wallpaper: WEWallpaper,
        to outputURL: URL,
        options: SceneMobileExportOptions = .init(),
        snapshot: WallpaperRenderSnapshot? = nil,
        propertyOverridesJSON: String? = nil,
        progress: ((Double) -> Void)? = nil
    ) throws {
        guard wallpaper.isValid, wallpaper.kind == .scene else {
            throw SceneMobileExportError.unsupportedWallpaperType(wallpaper.kind)
        }
        if let preRendered = options.preRendered {
            guard let snapshot else { throw SceneMobileExportError.invalidProject }
            try WallpaperBakeService.exportMobile(wallpaper, to: outputURL, options: preRendered,
                                                  snapshot: snapshot, progress: progress)
            return
        }

        let wallpaperDirPath = wallpaper.wallpaperDirectory.path(percentEncoded: false)
        let renderDirPath = wallpaper.renderDirectory.path(percentEncoded: false)
        let overlayPaths = wallpaper.assetOverlayDirectories.map { $0.path(percentEncoded: false) }
        let outputPath = outputURL.path(percentEncoded: false)
        let zcompatPath = resolveZCompatShadersDirectory()?.path(percentEncoded: false)

        let compressionMode: Int32 = options.pixelArtOptimization ? 0 : 1
        let pixelArtFlag: Int32 = options.pixelArtOptimization ? 1 : 0
        let reduction: Int32 = Int32(options.textureReduction.rawValue)

        let progressBox = progress.map(ProgressBox.init)
        let progressContext = progressBox.map { Unmanaged.passUnretained($0).toOpaque() }
        let progressCallback: SceneMobileProgressTrampoline? = progressBox == nil ? nil : { fraction, ctx in
            guard let ctx else { return }
            let box = Unmanaged<ProgressBox>.fromOpaque(ctx).takeUnretainedValue()
            box.handler(fraction)
        }

        var detailInt: Int32 = 0
        var errorDetailPtr: UnsafeMutablePointer<CChar>?
        defer {
            if let errorDetailPtr {
                MirageSceneMobileFreeString(errorDetailPtr)
            }
        }

        let status: Int32 = withCStringArray(overlayPaths) { overlayPtrs, overlayCount in
            wallpaperDirPath.withCString { wpCStr in
                renderDirPath.withCString { renderCStr in
                    outputPath.withCString { outCStr in
                        withOptionalCString(zcompatPath) { zcompatCStr in
                            withOptionalCString(propertyOverridesJSON) { overridesCStr in
                                MirageSceneMobileExportFlat(
                                    wpCStr,
                                    renderCStr,
                                    overlayPtrs,
                                    overlayCount,
                                    outCStr,
                                    zcompatCStr,
                                    overridesCStr,
                                    reduction,
                                    pixelArtFlag,
                                    compressionMode,
                                    0,
                                    progressCallback,
                                    progressContext,
                                    nil,
                                    &detailInt,
                                    &errorDetailPtr
                                )
                            }
                        }
                    }
                }
            }
        }
        _ = progressBox

        guard status == statusOK else {
            let detail = errorDetailPtr.map { String(cString: $0) } ?? ""
            throw mapStatus(status, detailInt: detailInt, detail: detail)
        }
    }

    static func sourceResolution(_ wallpaper: WEWallpaper) throws -> MobileScreenResolution {
        let source = wallpaper.resolvedEntryURL
        let data: Data
        if source.pathExtension.lowercased() == "pkg" {
            let package = try readPackage(at: source)
            guard let entry = package.entries.first(where: {
                $0.name.caseInsensitiveCompare(wallpaper.project.file) == .orderedSame
            }), entry.size <= 8 * 1024 * 1024 else { throw SceneMobileExportError.invalidProject }
            data = try readPackageEntry(entry, from: source)
        } else {
            guard let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize,
                  size <= 8 * 1024 * 1024 else { throw SceneMobileExportError.invalidProject }
            data = try Data(contentsOf: source)
        }
        guard let scene = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let general = scene["general"] as? [String: Any],
              let projection = general["orthogonalprojection"] as? [String: Any],
              let width = projection["width"] as? Int, let height = projection["height"] as? Int else {
            throw SceneMobileExportError.invalidProject
        }
        let result = MobileScreenResolution(width: width, height: height)
        guard result.isValid else { throw SceneMobileExportError.invalidProject }
        return result
    }

    static func automaticDuration(_ wallpaper: WEWallpaper, fps: Int, speed: Double) throws -> Double {
        let fileManager = FileManager.default
        let source = wallpaper.resolvedEntryURL
        let package = source.pathExtension.lowercased() == "pkg" ? try readPackage(at: source).entries : []
        let entries = Dictionary(uniqueKeysWithValues: package.map { ($0.name.lowercased(), $0) })
        let assets = resolveAssetsDirectory()
        let roots = wallpaper.assetOverlayDirectories + [wallpaper.renderDirectory, assets]
        let staging = fileManager.temporaryDirectory.appending(path: "Mirage-Mobile-Timing-\(UUID().uuidString)")
        try fileManager.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: staging) }

        func resolve(_ name: String) -> SceneMobileTiming.TextureSource? {
            guard isSafeEntryName(name) else { return nil }
            let suffix = (name as NSString).pathExtension.lowercased()
            let candidates: [String]
            if suffix == "tex" {
                candidates = name.hasPrefix("materials/") ? [name] : [name, "materials/\(name)"]
            } else if ["json", "mp4", "mov", "m4v"].contains(suffix) { candidates = [name] }
            else { candidates = ["materials/\(name).tex"] }
            for candidate in candidates {
                if let entry = entries[candidate.lowercased()] {
                    return .init(url: source, offset: entry.absoluteOffset, size: entry.size)
                }
                for root in roots {
                    if let file = containedFile(candidate, in: root),
                       let values = try? file.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey]),
                       values.isRegularFile == true, let size = values.fileSize, size >= 0 {
                        return .init(url: file, offset: 0, size: UInt64(size))
                    }
                }
            }
            return nil
        }
        func copy(_ asset: SceneMobileTiming.TextureSource, suffix: String) throws -> URL {
            let input = try FileHandle(forReadingFrom: asset.url)
            defer { try? input.close() }
            try input.seek(toOffset: asset.offset)
            let destination = staging.appendingPathComponent(UUID().uuidString + "." + suffix)
            fileManager.createFile(atPath: destination.path, contents: nil)
            let output = try FileHandle(forWritingTo: destination)
            defer { try? output.close() }
            var remaining = asset.size
            while remaining > 0 {
                let count = Int(min(remaining, 1_048_576))
                let data = try readExactly(count, from: input)
                try output.write(contentsOf: data)
                remaining -= UInt64(count)
            }
            return destination
        }
        var loops: [Double] = [], oneShots: [Double] = []
        var cameraDuration = 0.0
        func inspectVideo(_ url: URL) {
            let duration = CMTimeGetSeconds(AVURLAsset(url: url).duration)
            if duration.isFinite, duration > 0 { loops.append(duration) }
        }
        var pending = [wallpaper.project.file], visited = Set<String>()
        var inspected = Set<SceneMobileTiming.TextureSource>()
        while !pending.isEmpty, visited.count < 2048 {
            let name = pending.removeLast()
            guard visited.insert(name.lowercased()).inserted, let asset = resolve(name),
                  asset.size <= 512 * 1024 * 1024, inspected.insert(asset).inserted else { continue }
            let suffix = (name as NSString).pathExtension.lowercased()
            if suffix == "json", asset.size <= 8 * 1024 * 1024 {
                let handle = try FileHandle(forReadingFrom: asset.url)
                defer { try? handle.close() }
                try handle.seek(toOffset: asset.offset)
                let data = try readExactly(Int(asset.size), from: handle)
                guard let json = try? JSONSerialization.jsonObject(with: data) else { continue }
                let summary = SceneMobileTiming.summary(of: json)
                loops += summary.loops; oneShots += summary.oneShots
                cameraDuration += summary.cameraDuration
                pending += summary.references.sorted()
            } else if ["mp4", "mov", "m4v"].contains(suffix) {
                let url = asset.offset == 0 && asset.url != source ? asset.url : try copy(asset, suffix: suffix)
                inspectVideo(url)
                if url != asset.url { try? fileManager.removeItem(at: url) }
            } else if let timing = try? SceneMobileTiming.textureTiming(asset, in: staging) {
                if let duration = timing.period { loops.append(duration) }
                if let video = timing.video {
                    inspectVideo(video)
                    try? fileManager.removeItem(at: video)
                }
            }
        }
        if cameraDuration > 0 { loops.append(cameraDuration) }
        return SceneMobileTiming.duration(loops: loops, oneShots: oneShots, fps: fps, speed: speed)
    }

    private static func resolveAssetsDirectory() -> URL {
        let fileManager = FileManager.default
        let projectRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bundledAssets = Bundle.main.resourceURL?.appending(path: "assets")
        return bundledAssets.flatMap { fileManager.fileExists(atPath: $0.path) ? $0 : nil }
            ?? projectRoot.appending(path: "assets")
    }

    private static func resolveZCompatShadersDirectory() -> URL? {
        let dir = resolveAssetsDirectory().appending(path: "zcompat/scene/shaders")
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: dir.path, isDirectory: &isDir), isDir.boolValue else {
            return nil
        }
        return dir
    }

    private static func mapStatus(_ status: Int32, detailInt: Int32, detail: String) -> Error {
        switch status {
        case statusCancelled:
            return CancellationError()
        case statusInvalidProject:
            return SceneMobileExportError.invalidProject
        case statusUnsafeProjectPath:
            return SceneMobileExportError.unsafeProjectPath
        case statusInvalidPackage:
            return SceneMobileExportError.invalidPackage
        case statusTruncatedPackage:
            return SceneMobileExportError.truncatedPackage
        case statusUnsupportedPackageVersion:
            return SceneMobileExportError.unsupportedPackageVersion(detail)
        case statusInvalidTexture:
            return SceneMobileExportError.invalidTexture(detail)
        case statusUnsupportedTextureLayout:
            return SceneMobileExportError.unsupportedTextureLayout(detail)
        case statusUnsupportedTextureFormat:
            return SceneMobileExportError.unsupportedTextureFormat(detailInt, detail)
        case statusWallpaperTooLarge:
            return SceneMobileExportError.wallpaperTooLarge
        default:
            return SceneMobileExportError.toolFailed(detail.isEmpty ? "status \(status)" : detail)
        }
    }

    private static func withOptionalCString<R>(_ string: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
        guard let string else { return body(nil) }
        return string.withCString { body($0) }
    }

    private static func withCStringArray<R>(
        _ strings: [String],
        _ body: (UnsafePointer<UnsafePointer<CChar>?>?, Int) -> R
    ) -> R {
        guard !strings.isEmpty else { return body(nil, 0) }
        let duplicated = strings.map { strdup($0) }
        defer {
            for ptr in duplicated { free(ptr) }
        }
        let constPtrs: [UnsafePointer<CChar>?] = duplicated.map { $0.map { UnsafePointer($0) } }
        return constPtrs.withUnsafeBufferPointer { buffer in
            body(buffer.baseAddress, buffer.count)
        }
    }

    private static func readPackage(at url: URL) throws -> (version: String, entries: [PackageEntry]) {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let values = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
        guard values.isRegularFile == true, let fileSize = values.fileSize else {
            throw SceneMobileExportError.invalidPackage
        }

        let versionLength = try readUInt32(from: handle)
        guard versionLength == 8 else { throw SceneMobileExportError.invalidPackage }
        let versionData = try readExactly(Int(versionLength), from: handle)
        guard let version = String(data: versionData, encoding: .ascii),
              version.range(of: #"^PKGV\d{4}$"#, options: .regularExpression) != nil else {
            throw SceneMobileExportError.invalidPackage
        }
        let count = Int(try readUInt32(from: handle))
        guard count <= 100_000 else { throw SceneMobileExportError.invalidPackage }

        var directory: [(String, UInt64, UInt64)] = []
        var foldedNames = Set<String>()
        for _ in 0..<count {
            let nameLength = Int(try readUInt32(from: handle))
            guard (1...1024).contains(nameLength),
                  let name = String(data: try readExactly(nameLength, from: handle), encoding: .utf8),
                  isSafeEntryName(name),
                  foldedNames.insert(name.lowercased()).inserted else {
                throw SceneMobileExportError.invalidPackage
            }
            directory.append((
                name,
                UInt64(try readUInt32(from: handle)),
                UInt64(try readUInt32(from: handle))
            ))
        }

        let dataOffset = handle.offsetInFile
        var entries: [PackageEntry] = []
        var ranges: [(UInt64, UInt64)] = []
        for (name, offset, size) in directory {
            let start = dataOffset + offset
            let end = start + size
            guard start >= dataOffset, end <= UInt64(fileSize) else {
                throw SceneMobileExportError.invalidPackage
            }
            entries.append(PackageEntry(name: name, absoluteOffset: start, size: size))
            ranges.append((start, end))
        }
        ranges.sort { $0.0 < $1.0 }
        for pair in zip(ranges, ranges.dropFirst()) where pair.1.0 < pair.0.1 {
            throw SceneMobileExportError.invalidPackage
        }
        return (version, entries)
    }

    private static func readPackageEntry(_ entry: PackageEntry, from packageURL: URL) throws -> Data {
        guard entry.size <= UInt64(Int.max) else {
            throw SceneMobileExportError.wallpaperTooLarge
        }
        let handle = try FileHandle(forReadingFrom: packageURL)
        defer { try? handle.close() }
        try handle.seek(toOffset: entry.absoluteOffset)
        let source = try handle.read(upToCount: Int(entry.size)) ?? Data()
        guard source.count == Int(entry.size) else {
            throw SceneMobileExportError.truncatedPackage
        }
        return source
    }

    private static func containedFile(_ name: String, in directory: URL) -> URL? {
        guard isSafeEntryName(name) else { return nil }
        let root = directory.standardizedFileURL.resolvingSymlinksInPath()
        let candidate = directory.appending(path: name).standardizedFileURL.resolvingSymlinksInPath()
        let prefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        guard candidate.path.hasPrefix(prefix),
              FileManager.default.fileExists(atPath: candidate.path) else { return nil }
        return candidate
    }

    private static func isSafeEntryName(_ name: String) -> Bool {
        MobilePackageArchive.isSafeEntryName(name)
    }

    private static func readExactly(_ count: Int, from handle: FileHandle) throws -> Data {
        let data = try handle.read(upToCount: count) ?? Data()
        guard data.count == count else { throw SceneMobileExportError.truncatedPackage }
        return data
    }

    private static func readUInt32(from handle: FileHandle) throws -> UInt32 {
        let data = try readExactly(4, from: handle)
        return data.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian }
    }
}

enum SceneMobileExportError: LocalizedError {
    case unsupportedWallpaperType(WallpaperKind)
    case scenePresetNotSupported
    case invalidProject
    case unsafeProjectPath
    case invalidPackage
    case truncatedPackage
    case unsupportedPackageVersion(String)
    case invalidTexture(String)
    case unsupportedTextureLayout(String)
    case unsupportedTextureFormat(Int32, String)
    case toolFailed(String)
    case wallpaperTooLarge

    var errorDescription: String? {
        switch self {
        case .unsupportedWallpaperType(let kind):
            return L("暂不支持将%@壁纸转换到移动设备。", kind.displayName)
        case .scenePresetNotSupported:
            return L("暂不支持直接转换场景预设，请发送基础场景壁纸。")
        case .invalidProject:
            return L("场景壁纸的 project.json 无效。")
        case .unsafeProjectPath:
            return L("场景壁纸包含不安全或重复的文件路径。")
        case .invalidPackage:
            return L("场景壁纸的 scene.pkg 无效。")
        case .truncatedPackage:
            return L("场景壁纸文件不完整。")
        case .unsupportedPackageVersion(let version):
            return L("暂不支持场景包版本 %@。", version)
        case .invalidTexture(let name):
            return L("无法读取场景纹理：%@", name)
        case .unsupportedTextureLayout(let name):
            return L("暂不支持该场景纹理布局：%@", name)
        case .unsupportedTextureFormat(let format, let name):
            return L("暂不支持纹理 %@ 的格式 %d。", name, format)
        case .toolFailed(let detail):
            return L("场景转换失败：%@", detail)
        case .wallpaperTooLarge:
            return L("移动设备场景壁纸不能超过 4 GB。")
        }
    }
}
