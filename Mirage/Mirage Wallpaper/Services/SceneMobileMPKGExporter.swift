//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

//  Native conversion of desktop scene packages for Wallpaper Engine Android.
//

import AVFoundation
import Accelerate
import Compression
import CoreFoundation
import Foundation
import ImageIO

enum SceneMobileMPKGExporter {
    private static let textureFormatRGBA8: Int32 = 0
    private static let textureFormatRGB8: Int32 = 1
    private static let textureFormatBC3: Int32 = 4
    private static let textureFormatETC2RGBA8: Int32 = 5
    private static let textureFormatBC2: Int32 = 6
    private static let textureFormatBC1: Int32 = 7
    private static let textureFormatRG8: Int32 = 8
    private static let textureFormatR8: Int32 = 9
    private static let textureFormatBC7: Int32 = 12
    private static let desktopBlockFormats: [Int32: String] = [
        textureFormatBC1: "DXT1", textureFormatBC2: "DXT3", textureFormatBC3: "DXT5",
    ]
    private static let spriteFlag: UInt32 = 1 << 2
    private static let rawMobileFlag: UInt32 = 1 << 3
    private static let streamingTextureFlag: UInt32 = 1 << 5
    private static let effort = 10
    // Use ImageIO for small mips and FFmpeg's threaded decoder for large PNGs.
    private static let maximumNativePNGDimension = 512
    private static let maximumProjectBytes = 1024 * 1024
    private static let maximumSceneBytes = 16 * 1024 * 1024
    private static let maximumShaderBytes = 4 * 1024 * 1024

    private struct StagedEntry {
        let name: String
        let url: URL
        let offset: UInt64
        let size: UInt64

        func read(limit: Int = .max) throws -> Data {
            guard size <= UInt64(limit) else { throw SceneMobileExportError.wallpaperTooLarge }
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            try handle.seek(toOffset: offset)
            return try readExactly(Int(size), from: handle)
        }
    }

    private struct SceneSource {
        let version: String
        let entries: [String: StagedEntry]
        let scene: StagedEntry
        let projectData: Data
    }

    private struct TextureMip {
        let width: Int32
        let height: Int32
        let isLZ4Compressed: Bool
        let decompressedSize: Int
        let data: Data
    }

    private struct TextureAsset {
        let format: Int32
        let flags: UInt32
        let width: Int32
        let height: Int32
        let mapWidth: Int32
        let mapHeight: Int32
        let reservedA: Int32
        let imageType: Int32?
        let slots: [[TextureMip]]
        let spriteData: Data?
    }

    private struct ConvertedTextureSlot {
        let width: Int
        let height: Int
        let contentWidth: Int
        let contentHeight: Int
        let payloadSize: Int
        let compressed: Data
    }

    private struct MobileMipSelection {
        let mip: TextureMip
        let sourceWidth: Int
        let sourceHeight: Int
        let outputWidth: Int
        let outputHeight: Int
        let coordinateScale: Double
    }

    static func export(
        _ wallpaper: WEWallpaper,
        to outputURL: URL,
        options: SceneMobileExportOptions = .init(),
        snapshot: WallpaperRenderSnapshot? = nil,
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
        let fileManager = FileManager.default
        let staging = fileManager.temporaryDirectory
            .appending(path: "Mirage-Mobile-Scene-\(UUID().uuidString)", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: staging) }
        progress?(0)
        let source = try sceneSource(for: wallpaper, in: staging)
        let entries = source.entries.values.sorted(by: stagedEntryOrder)
        var stagedEntries: [StagedEntry] = []
        var textureEntries: [(index: Int, entry: StagedEntry)] = []
        var processed = 0
        let conversionCount = entries.count + 1

        for (index, entry) in entries.enumerated() {
            let suffix = (entry.name as NSString).pathExtension.lowercased()
            if suffix == "tex" {
                textureEntries.append((index, entry))
                continue
            }

            if entry.name == source.scene.name {
                let data = try options.sceneData(entry.read(limit: maximumSceneBytes))
                stagedEntries.append(try stage(data, named: entry.name, in: staging))
            } else if suffix == "vert" || suffix == "frag" ||
                        (["geom", "h"].contains(suffix) && entry.name.lowercased().hasPrefix("shaders/")) {
                let data = try shaderSource(for: entry, in: source.entries)
                stagedEntries.append(try stage(rewriteMobileShader(data, label: entry.name), named: entry.name, in: staging))
            } else {
                stagedEntries.append(entry)
            }
            processed += 1
            progress?(Double(processed) / Double(conversionCount) * 0.9)
        }

        let conversionParallelism = textureConversionParallelism()
        let encoderJobs = max(
            1,
            min(8, ProcessInfo.processInfo.activeProcessorCount / conversionParallelism)
        )
        let queue = OperationQueue()
        queue.name = "Mirage.SceneMobileTextureConversion"
        queue.qualityOfService = .userInitiated
        queue.maxConcurrentOperationCount = conversionParallelism
        let resultLock = NSLock()
        var textureResults: [Int: StagedEntry] = [:]
        var firstError: Error?
        // Start large assets first so a late 8K texture does not leave the
        // other worker idle at the end of the conversion. Archive order is
        // determined separately from the original entry indices below.
        for item in textureEntries.sorted(by: {
            $0.entry.size == $1.entry.size ? $0.index < $1.index : $0.entry.size > $1.entry.size
        }) {
            queue.addOperation {
                resultLock.lock()
                let shouldStop = firstError != nil
                resultLock.unlock()
                guard !shouldStop else { return }
                do {
                    let staged = try autoreleasepool {
                        if try isEmbeddedVideoTexture(item.entry) { return item.entry }
                        let data = try item.entry.read()
                        guard let converted = try mobileTexture(data, label: item.entry.name, options: options, jobs: encoderJobs) else {
                            // Unchanged textures can be streamed from their source by the writer.
                            return item.entry
                        }
                        return try stage(converted, named: item.entry.name, in: staging)
                    }
                    resultLock.lock()
                    textureResults[item.index] = staged
                    processed += 1
                    let fraction = Double(processed) / Double(conversionCount) * 0.9
                    progress?(fraction)
                    resultLock.unlock()
                } catch {
                    resultLock.lock()
                    if firstError == nil { firstError = error }
                    resultLock.unlock()
                    queue.cancelAllOperations()
                }
            }
        }
        queue.waitUntilAllOperationsAreFinished()
        if let firstError { throw firstError }
        stagedEntries.append(contentsOf: textureResults.keys.sorted().compactMap { textureResults[$0] })

        stagedEntries.append(try stage(source.projectData, named: "project.json", in: staging))
        progress?(0.9)
        try writeArchive(entries: stagedEntries.sorted(by: stagedEntryOrder), version: source.version, to: outputURL) { fraction in
            progress?(0.9 + fraction * 0.1)
        }
        progress?(1)
    }

    private static func sceneSource(for wallpaper: WEWallpaper, in staging: URL) throws -> SceneSource {
        let roots = [wallpaper.renderDirectory] + wallpaper.assetOverlayDirectories.reversed()
        var entries: [String: StagedEntry] = [:]
        var version = "PKGM0014"
        var projects: [[String: Any]] = []
        var originalProjectData = Data()
        var seenRoots = Set<URL>()

        // Resolved Workshop presets supply the base directory and overlays in priority order.
        for root in roots {
            let root = root.standardizedFileURL.resolvingSymlinksInPath()
            guard seenRoots.insert(root).inserted else { throw SceneMobileExportError.invalidProject }
            guard let projectURL = containedFile("project.json", in: root) else {
                throw SceneMobileExportError.invalidProject
            }
            let data = try sourceEntry(named: "project.json", at: projectURL).read(limit: maximumProjectBytes)
            guard let project = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw SceneMobileExportError.invalidProject
            }
            if projects.isEmpty {
                guard (project["type"] as? String)?.lowercased() == "scene" else {
                    throw SceneMobileExportError.invalidProject
                }
                originalProjectData = data
            } else if !(project["preset"] is [String: Any]) {
                // Published presets can omit both `type` and `file` and inherit them from the base.
                throw SceneMobileExportError.invalidProject
            }
            projects.append(project)

            let file = project["file"] as? String ?? ""
            guard file.isEmpty || isSafeEntryName(file) else { throw SceneMobileExportError.unsafeProjectPath }
            let candidate: String
            if file.isEmpty {
                candidate = "scene.pkg"
            } else if (file as NSString).pathExtension.lowercased() == "pkg" {
                candidate = file
            } else {
                candidate = (file as NSString).deletingPathExtension + ".pkg"
            }
            let packageURL = containedFile(candidate, in: root) ?? containedFile("scene.pkg", in: root)
            if let packageURL {
                let package = try readPackage(at: packageURL)
                version = max(version, try mobileVersion(for: package.version))
                for entry in package.entries { entries[entry.name.lowercased()] = entry }
            }
            for entry in try looseEntries(in: root, excluding: packageURL) {
                entries[entry.name.lowercased()] = entry
            }
        }
        entries = entries.filter { name, _ in
            name != "project.json" && !(name.hasPrefix("shaders/blobssm40/") && name.hasSuffix(".dxs"))
        }

        guard var project = projects.first else { throw SceneMobileExportError.invalidProject }
        let sceneName = sceneEntryName(project["file"] as? String ?? "scene.json")
        guard let scene = entries[sceneName.lowercased()] ?? entries["scene.json"],
              (scene.name as NSString).pathExtension.lowercased() == "json" else {
            throw SceneMobileExportError.invalidProject
        }
        for preset in projects.dropFirst() { applyPreset(preset, to: &project) }
        if let preset = projects.dropFirst().last {
            // Workshop presets often omit their own ID in the downloaded manifest.
            if preset["workshopid"] == nil || preset["workshopid"] is NSNull {
                project["workshopid"] = wallpaper.project.workshopid?.rawValue
            }
            if preset["workshopurl"] == nil || preset["workshopurl"] is NSNull {
                project.removeValue(forKey: "workshopurl")
            }
        }

        let previewCandidates = projects.reversed().compactMap { $0["preview"] as? String }
            + ["preview.jpg", "preview.png", "preview.gif", "preview.webp"]
        let preview: StagedEntry
        if let existing = previewCandidates.lazy.compactMap({ entries[$0.lowercased()] }).first {
            preview = existing
        } else {
            var name = "mirage-preview.png"
            while entries.keys.contains(where: { $0 == name || $0.hasPrefix(name + "/") }) { name = "_" + name }
            // A transparent 1×1 PNG for scenes that ship without a preview.
            let placeholder = Data([
                0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
                0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
                0x0B, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x00, 0x02, 0x00,
                0x00, 0x05, 0x00, 0x01, 0x7A, 0x5E, 0xAB, 0x3F, 0x00, 0x00, 0x00, 0x00,
                0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
            ])
            preview = try stage(placeholder, named: name, in: staging)
            entries[name] = preview
        }

        if projects.count == 1, project["dependency"] == nil, project["preset"] == nil,
           project["file"] as? String == scene.name, project["preview"] as? String == preview.name {
            return SceneSource(version: version, entries: entries, scene: scene, projectData: originalProjectData)
        }
        project.removeValue(forKey: "dependency")
        project.removeValue(forKey: "preset")
        project["file"] = scene.name
        project["preview"] = preview.name
        let data = try JSONSerialization.data(withJSONObject: project, options: [.sortedKeys, .withoutEscapingSlashes])
        guard data.count <= maximumProjectBytes else { throw SceneMobileExportError.invalidProject }
        return SceneSource(version: version, entries: entries, scene: scene, projectData: data)
    }

    private static func sceneEntryName(_ manifestFile: String) -> String {
        (manifestFile as NSString).pathExtension.lowercased() == "pkg" ? "scene.json" : manifestFile
    }

    private static func applyPreset(_ preset: [String: Any], to project: inout [String: Any]) {
        // Keep keys unknown to WEProject, including settings on the original property definitions.
        if let overrides = preset["preset"] as? [String: Any], !overrides.isEmpty {
            var general = project["general"] as? [String: Any] ?? [:]
            var properties = general["properties"] as? [String: Any] ?? [:]
            for (key, value) in overrides where !(value is NSNull) {
                var property = properties[key] as? [String: Any] ?? [:]
                if property.isEmpty {
                    if let number = value as? NSNumber {
                        property["type"] = CFGetTypeID(number) == CFBooleanGetTypeID() ? "bool" : "slider"
                    } else {
                        property["type"] = "textinput"
                    }
                }
                property["value"] = value
                properties[key] = property
            }
            general["properties"] = properties
            project["general"] = general
        }
        for key in ["title", "author", "approved", "contentrating", "description", "tags",
                    "visibility", "workshopid", "workshopurl"] {
            if let value = preset[key], !(value is NSNull) { project[key] = value }
        }
    }

    private static func looseEntries(in root: URL, excluding packageURL: URL?) throws -> [StagedEntry] {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]
        var enumerationError: Error?
        guard let enumerator = FileManager.default.enumerator(
            at: root, includingPropertiesForKeys: Array(keys), options: [.skipsHiddenFiles],
            errorHandler: { _, error in enumerationError = error; return false }
        ) else { throw SceneMobileExportError.invalidProject }
        let prefix = root.path + "/"
        var names = Set<String>()
        var entries: [StagedEntry] = []
        for case let url as URL in enumerator {
            let values = try url.resourceValues(forKeys: keys)
            guard values.isSymbolicLink != true else { throw SceneMobileExportError.unsafeProjectPath }
            guard values.isRegularFile == true, let size = values.fileSize else { continue }
            // The enumerator may return /private/var for a root supplied as /var.
            let file = url.standardizedFileURL.resolvingSymlinksInPath()
            guard file.path.hasPrefix(prefix) else { throw SceneMobileExportError.unsafeProjectPath }
            let name = String(file.path.dropFirst(prefix.count))
            let key = name.lowercased()
            if key == "project.json" || key == "scene.pkg" || key.hasSuffix(".mpkg") || file == packageURL { continue }
            guard isSafeEntryName(name), names.insert(key).inserted else {
                throw SceneMobileExportError.unsafeProjectPath
            }
            entries.append(StagedEntry(name: name, url: file, offset: 0, size: UInt64(size)))
            guard entries.count <= 100_000 else { throw SceneMobileExportError.wallpaperTooLarge }
        }
        if let enumerationError { throw enumerationError }
        return entries
    }

    private static func shaderSource(for shader: StagedEntry, in entries: [String: StagedEntry]) throws -> Data {
        let parts = shader.name.lowercased().split(separator: "/")
        let suffix = (shader.name as NSString).pathExtension.lowercased()
        if parts.count == 5, parts[0] == "shaders", parts[1] == "workshop", ["vert", "frag"].contains(suffix) {
            for prefix in ["assets/zcompat/scene/shaders/", "zcompat/scene/shaders/"] {
                let directory = prefix + parts[2] + "/"
                guard let config = entries[directory + "config.json"], config.size <= 64 * 1024,
                      let object = try? JSONSerialization.jsonObject(with: config.read()) as? [String: Any],
                      let path = object[suffix] as? String, isSafeEntryName(path),
                      let replacement = entries[directory + path.lowercased()],
                      replacement.size <= UInt64(maximumShaderBytes) else { continue }
                return try replacement.read()
            }
        }
        return try shader.read(limit: maximumShaderBytes)
    }

    static func sourceResolution(_ wallpaper: WEWallpaper) throws -> MobileScreenResolution {
        let source = wallpaper.resolvedEntryURL
        let data: Data
        if source.pathExtension.lowercased() == "pkg" {
            let package = try readPackage(at: source)
            guard let entry = package.entries.first(where: {
                $0.name.caseInsensitiveCompare(sceneEntryName(wallpaper.project.file)) == .orderedSame
            }) ?? package.entries.first(where: { $0.name.lowercased() == "scene.json" }) else {
                throw SceneMobileExportError.invalidProject
            }
            data = try entry.read(limit: maximumSceneBytes)
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
        let projectRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bundledAssets = Bundle.main.resourceURL?.appending(path: "assets")
        let assets = bundledAssets.flatMap { fileManager.fileExists(atPath: $0.path) ? $0 : nil }
            ?? projectRoot.appending(path: "assets")
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
                    return .init(url: entry.url, offset: entry.offset, size: entry.size)
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
        var pending = [sceneEntryName(wallpaper.project.file)], visited = Set<String>()
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

    private static func readPackage(at url: URL) throws -> (version: String, entries: [StagedEntry]) {
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
        guard (1...100_000).contains(count) else { throw SceneMobileExportError.invalidPackage }

        var directory: [(String, UInt64, UInt64)] = []
        var foldedNames = Set<String>()
        for _ in 0..<count {
            let nameLength = Int(try readUInt32(from: handle))
            guard (1...1024).contains(nameLength),
                  let rawName = String(data: try readExactly(nameLength, from: handle), encoding: .utf8) else {
                throw SceneMobileExportError.invalidPackage
            }
            let name = rawName.replacingOccurrences(of: "\\", with: "/")
            guard isSafeEntryName(name),
                  foldedNames.insert(name.lowercased()).inserted else {
                throw SceneMobileExportError.invalidPackage
            }
            directory.append((
                name,
                UInt64(try readUInt32(from: handle)),
                UInt64(try readUInt32(from: handle))
            ))
            guard handle.offsetInFile <= 16 * 1024 * 1024 else { throw SceneMobileExportError.invalidPackage }
        }

        let dataOffset = handle.offsetInFile
        var entries: [StagedEntry] = []
        var ranges: [(UInt64, UInt64)] = []
        for (name, offset, size) in directory {
            let start = dataOffset + offset
            let end = start + size
            guard start >= dataOffset, end <= UInt64(fileSize) else {
                throw SceneMobileExportError.invalidPackage
            }
            entries.append(StagedEntry(name: name, url: url, offset: start, size: size))
            if size > 0 { ranges.append((start, end)) }
        }
        ranges.sort { $0.0 < $1.0 }
        for pair in zip(ranges, ranges.dropFirst()) where pair.1.0 < pair.0.1 {
            throw SceneMobileExportError.invalidPackage
        }
        return (version, entries)
    }

    private static func mobileVersion(for sourceVersion: String) throws -> String {
        guard let value = Int(sourceVersion.suffix(4)),
              let mobile = [
                  1: 14,
                  13: 14,
                  14: 14,
                  16: 14,
                  17: 15,
                  18: 16,
                  19: 17,
                  20: 18,
                  21: 19,
                  22: 19,
                  23: 19,
                  24: 20,
              ][value] else {
            throw SceneMobileExportError.unsupportedPackageVersion(sourceVersion)
        }
        return String(format: "PKGM%04d", mobile)
    }

    /// TEXB4 video mips have a condition field before the dimensions. Copy the validated entry as-is.
    private static func isEmbeddedVideoTexture(_ entry: StagedEntry) throws -> Bool {
        let headerLength = 67
        guard entry.size >= UInt64(headerLength) else { return false }
        let handle = try FileHandle(forReadingFrom: entry.url)
        defer { try? handle.close() }
        try handle.seek(toOffset: entry.offset)
        let header = try readExactly(headerLength, from: handle)
        guard header.starts(with: Data("TEXV0005\0TEXI0001\0".utf8)),
              header[46..<55].elementsEqual("TEXB0004\0".utf8) else { return false }
        var reader = BinaryReader(data: header, label: entry.name, offset: 55)
        let slots = try reader.readUInt32()
        let imageType = try reader.readInt32()
        let videoFlag = try reader.readInt32()
        guard imageType == 35 || (imageType == -1 && videoFlag == 1) else { return false }

        let end = entry.offset + entry.size
        let invalid = SceneMobileExportError.invalidTexture(entry.name)
        func read(_ count: Int) throws -> Data {
            guard handle.offsetInFile <= end, UInt64(count) <= end - handle.offsetInFile else { throw invalid }
            return try readExactly(count, from: handle)
        }
        func word() throws -> UInt32 {
            try read(4).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian }
        }
        func skipCondition() throws {
            let start = handle.offsetInFile
            let maximum = min(end, start + 64 * 1024 + 1)
            while handle.offsetInFile < maximum {
                let offset = handle.offsetInFile
                let bytes = try read(Int(min(1024, maximum - offset)))
                if let terminator = bytes.firstIndex(of: 0) {
                    try handle.seek(toOffset: offset + UInt64(terminator) + 1)
                    return
                }
            }
            throw invalid
        }
        guard (1...4096).contains(slots) else { throw invalid }
        for _ in 0..<slots {
            let mips = try word()
            guard (1...64).contains(mips) else { throw invalid }
            for _ in 0..<mips {
                guard try word() == 1, try word() == 2 else { throw invalid }
                try skipCondition()
                guard try word() == 1, (1...16384).contains(try word()), (1...16384).contains(try word()) else {
                    throw invalid
                }
                _ = try word() // Compression flag
                guard try word() <= UInt32(Int32.max) else { throw invalid }
                let size = UInt64(try word())
                let start = handle.offsetInFile
                guard size > 0, size <= end - start else { throw invalid }
                try handle.seek(toOffset: start + size)
            }
        }
        if handle.offsetInFile < end {
            let flags = header.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 22, as: UInt32.self).littleEndian }
            guard flags & spriteFlag != 0,
                  try read(9).starts(with: Data("TEXS".utf8)), try word() > 0 else { throw invalid }
        }
        return true
    }

    private static func mobileTexture(
        _ source: Data,
        label: String,
        options: SceneMobileExportOptions,
        jobs: Int
    ) throws -> Data? {
        let asset = try parseTexture(source, label: label)
        // Masks and streaming/native mobile textures must retain their layout and sampling data.
        if asset.flags & streamingTextureFlag != 0
            || ((asset.imageType ?? -1) < 0 && [textureFormatETC2RGBA8, textureFormatRG8, textureFormatR8].contains(asset.format)) {
            return nil
        }
        let plainRGB = asset.format == textureFormatRGB8 && (asset.imageType ?? -1) < 0
        let forceETC2 = ["materials", "materials/masks"].contains((label.lowercased() as NSString).deletingLastPathComponent)
        if plainRGB, !forceETC2, !options.pixelArtOptimization { return nil }
        guard (asset.imageType ?? -1) >= 0 || [textureFormatRGBA8, textureFormatRGB8, textureFormatBC7].contains(asset.format) ||
                desktopBlockFormats[asset.format] != nil else {
            throw SceneMobileExportError.unsupportedTextureFormat(asset.format, label)
        }
        guard asset.spriteData != nil || asset.slots.count == 1,
              asset.slots.allSatisfy({ !$0.isEmpty }) else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }
        // Pixel-art optimization preserves RGBA pixels instead of introducing ETC2 artifacts.
        // BC textures have already been compressed by the author and still require ETC2 on mobile.
        let preserveRGBA = asset.format == textureFormatRGBA8
            && (options.pixelArtOptimization || asset.flags == 0 || preservesMobileRGBA8(label: label, asset: asset))
            || asset.format == textureFormatRGB8 && (options.pixelArtOptimization || !forceETC2)
        let useMapSize = asset.spriteData == nil || (preserveRGBA && asset.slots.count == 1)
        let temporary = FileManager.default.temporaryDirectory
            .appending(path: "Mirage-Scene-Texture-\(UUID().uuidString)", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        var convertedSlots: [ConvertedTextureSlot] = []
        var slotScales: [(x: Double, y: Double)] = []
        for (slotIndex, slot) in asset.slots.enumerated() {
            let slotLabel = "\(label) slot \(slotIndex)"
            let selection = try selectMobileMip(asset, slot: slot, options: options, useMapSize: useMapSize, label: slotLabel)
            let payload: Data
            let width: Int
            let height: Int
            if preserveRGBA {
                payload = try rawMobilePixels(asset, selection: selection, label: slotLabel,
                                              temporary: temporary, nearestNeighbor: options.pixelArtOptimization)
                width = selection.outputWidth
                height = selection.outputHeight
            } else {
                let decoded = temporary.appending(path: "decoded-\(slotIndex).png")
                _ = try decodeTexture(
                    asset, mip: selection.mip, label: slotLabel,
                    ffmpeg: conversionTool(named: "ffmpeg"), temporary: temporary, destination: decoded,
                    sourceDimensions: (selection.sourceWidth, selection.sourceHeight),
                    outputDimensions: (selection.outputWidth, selection.outputHeight)
                )
                let encoded = temporary.appending(path: "encoded-\(slotIndex).ktx")
                try run(conversionTool(named: "EtcTool"), arguments: [
                    decoded.path, "-format", "RGBA8", "-errormetric", "rgbx", "-effort", String(effort),
                    "-j", String(jobs), "-output", encoded.path,
                ], label: "ETC2 \(slotLabel)")
                payload = try ktxPayload(Data(contentsOf: encoded), expectedWidth: selection.outputWidth,
                                         expectedHeight: selection.outputHeight)
                width = ((selection.outputWidth + 3) / 4) * 4
                height = ((selection.outputHeight + 3) / 4) * 4
            }
            convertedSlots.append(ConvertedTextureSlot(
                width: width, height: height, contentWidth: selection.outputWidth, contentHeight: selection.outputHeight,
                payloadSize: payload.count, compressed: try lz4Compress(payload, label: slotLabel)
            ))
            slotScales.append((selection.coordinateScale, selection.coordinateScale))
        }
        guard let firstSlot = convertedSlots.first else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }
        let mapWidth = asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(asset.width)
        let mapHeight = asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(asset.height)
        // TEXI records the logical backing size; TEXB records the actual reduced pixels.
        // Account for ETC block padding at the original scale so UVs do not stretch cropped edges.
        let headerWidth = useMapSize
            ? Int((Double(mapWidth) * Double(firstSlot.width) / Double(firstSlot.contentWidth)).rounded())
            : Int(asset.width)
        let headerHeight = useMapSize
            ? Int((Double(mapHeight) * Double(firstSlot.height) / Double(firstSlot.contentHeight)).rounded())
            : Int(asset.height)
        guard headerWidth > 0, headerHeight > 0, headerWidth <= Int32.max, headerHeight <= Int32.max else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let mobileFlags = asset.flags | (asset.slots[0].count == 1 ? rawMobileFlag : 0)
        var output = Data()
        output.appendStamp("TEXV0005")
        output.appendStamp("TEXI0001")
        output.appendInt32(preserveRGBA ? textureFormatRGBA8 : textureFormatETC2RGBA8)
        output.appendUInt32(mobileFlags)
        output.appendInt32(Int32(headerWidth))
        output.appendInt32(Int32(headerHeight))
        output.appendInt32(Int32(mapWidth))
        output.appendInt32(Int32(mapHeight))
        output.appendInt32(asset.reservedA)
        output.appendStamp("TEXB0004")
        output.appendInt32(Int32(convertedSlots.count))
        output.appendInt32(-1)
        output.appendInt32(0)
        for slot in convertedSlots {
            output.appendInt32(1)
            output.appendInt32(Int32(slot.width))
            output.appendInt32(Int32(slot.height))
            output.appendInt32(1)
            output.appendInt32(Int32(slot.payloadSize))
            output.appendInt32(Int32(slot.compressed.count))
            output.append(slot.compressed)
        }
        if let spriteData = asset.spriteData {
            output.append(try mobileSpriteData(spriteData, slotScales: slotScales,
                                               mapWidth: mapWidth, mapHeight: mapHeight, label: label))
        }
        return output
    }

    private static func preservesMobileRGBA8(label: String, asset: TextureAsset) -> Bool {
        let normalized = label.replacingOccurrences(of: "\\", with: "/").lowercased()
        guard asset.format == textureFormatRGBA8, asset.slots.count == 1 else { return false }
        if normalized.hasPrefix("materials/workshop/") {
            return asset.spriteData == nil || asset.spriteData?.starts(with: Data("TEXS0003\0".utf8)) == true
        }
        return normalized.hasPrefix("materials/particle/") && asset.spriteData == nil
    }

    private static func parseTexture(_ data: Data, label: String) throws -> TextureAsset {
        var reader = BinaryReader(data: data, label: label)
        guard try reader.readStamp(prefix: "TEXV") == "TEXV0005",
              try reader.readStamp(prefix: "TEXI") == "TEXI0001" else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let format = try reader.readInt32()
        let flags = try reader.readUInt32()
        let width = try reader.readInt32()
        let height = try reader.readInt32()
        let mapWidth = try reader.readInt32()
        let mapHeight = try reader.readInt32()
        let reservedA = try reader.readInt32()
        let texb = try reader.readStamp(prefix: "TEXB")
        guard let texbVersion = Int(texb.suffix(4)), (1...4).contains(texbVersion) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let count = Int(try reader.readInt32())
        guard (1...4096).contains(count) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let imageType = texbVersion >= 3 ? try reader.readInt32() : nil
        if texbVersion >= 4 { _ = try reader.readInt32() }

        var slots: [[TextureMip]] = []
        for _ in 0..<count {
            let mipCount = Int(try reader.readInt32())
            guard (1...64).contains(mipCount) else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            var mips: [TextureMip] = []
            for _ in 0..<mipCount {
                let mipWidth = try reader.readInt32()
                let mipHeight = try reader.readInt32()
                guard mipWidth > 0, mipHeight > 0 else {
                    throw SceneMobileExportError.invalidTexture(label)
                }
                let compressed = texbVersion >= 2 ? try reader.readInt32() == 1 : false
                let decompressedSize = texbVersion >= 2 ? Int(try reader.readInt32()) : 0
                let size = Int(try reader.readInt32())
                guard size > 0, decompressedSize >= 0 else {
                    throw SceneMobileExportError.invalidTexture(label)
                }
                mips.append(TextureMip(
                    width: mipWidth,
                    height: mipHeight,
                    isLZ4Compressed: compressed,
                    decompressedSize: decompressedSize,
                    data: try reader.readData(count: size)
                ))
            }
            slots.append(mips)
        }

        let trailing = reader.remainingData
        let spriteData: Data?
        if trailing.isEmpty {
            spriteData = nil
        } else {
            guard flags & spriteFlag != 0,
                  trailing.count >= 13,
                  let stamp = String(data: trailing.prefix(8), encoding: .ascii),
                  stamp.range(of: #"^TEXS\d{4}$"#, options: .regularExpression) != nil,
                  trailing[8] == 0 else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            spriteData = trailing
        }
        return TextureAsset(
            format: format,
            flags: flags,
            width: width,
            height: height,
            mapWidth: mapWidth,
            mapHeight: mapHeight,
            reservedA: reservedA,
            imageType: imageType,
            slots: slots,
            spriteData: spriteData
        )
    }

    private static func decodedMip(_ mip: TextureMip, label: String) throws -> Data {
        guard mip.isLZ4Compressed else { return mip.data }
        return try lz4Decompress(mip.data, size: mip.decompressedSize, label: label)
    }

    private static func decodeTexture(
        _ asset: TextureAsset,
        mip: TextureMip,
        label: String,
        ffmpeg: URL,
        temporary: URL,
        destination: URL,
        sourceDimensions: (width: Int, height: Int)? = nil,
        outputDimensions: (width: Int, height: Int)? = nil,
        nearestNeighbor: Bool = false,
        decodedBody: Data? = nil
    ) throws -> (width: Int, height: Int) {
        let body = try decodedBody ?? decodedMip(mip, label: label)
        let sourceWidth = sourceDimensions?.width ?? (asset.spriteData == nil
            ? (asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(mip.width))
            : Int(mip.width))
        let sourceHeight = sourceDimensions?.height ?? (asset.spriteData == nil
            ? (asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(mip.height))
            : Int(mip.height))
        guard sourceWidth > 0, sourceHeight > 0,
              sourceWidth <= Int(mip.width), sourceHeight <= Int(mip.height) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let outputWidth = outputDimensions?.width ?? sourceWidth
        let outputHeight = outputDimensions?.height ?? sourceHeight
        // Embedded PNG mips often already contain exactly the requested pixels.
        // EtcTool can read them directly; avoid a PNG decode/encode round trip.
        if destination.pathExtension == "png", asset.format == textureFormatRGBA8,
           sourceWidth == Int(mip.width), sourceHeight == Int(mip.height),
           outputWidth == sourceWidth, outputHeight == sourceHeight,
           canReusePNG(body, width: outputWidth, height: outputHeight) {
            try body.write(to: destination)
            return (outputWidth, outputHeight)
        }
        var filters = ["crop=\(sourceWidth):\(sourceHeight):0:0:exact=1"]
        if outputWidth != sourceWidth || outputHeight != sourceHeight {
            let filter = nearestNeighbor ? "neighbor" : "lanczos"
            filters.append("scale=\(outputWidth):\(outputHeight):flags=\(filter)")
        }
        let source: URL
        var arguments = ["-hide_banner", "-loglevel", "error", "-y"]
        if max(mip.width, mip.height) <= maximumNativePNGDimension {
            // Decoder/filter thread setup can dominate small texture work.
            // Large inputs retain FFmpeg's default decoder parallelism.
            arguments += ["-threads", "1", "-filter_threads", "1"]
        }
        if let pixels = try decodeBC7(body, asset: asset, mip: mip, label: label) {
            source = temporary.appending(path: "source.rgba")
            try pixels.write(to: source)
            arguments += ["-f", "rawvideo", "-pixel_format", "rgba", "-video_size", "\(mip.width)x\(mip.height)", "-i", source.path]
        } else if let suffix = imageSuffix(body) {
            source = temporary.appending(path: "source\(suffix)")
            try body.write(to: source)
            arguments += ["-i", source.path]
        } else if let fourCC = desktopBlockFormats[asset.format] {
            let blockBytes = asset.format == textureFormatBC1 ? 8 : 16
            let expected = ((Int(mip.width) + 3) / 4) * ((Int(mip.height) + 3) / 4) * blockBytes
            guard body.count == expected else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            source = temporary.appending(path: "source.dds")
            var dds = ddsHeader(
                width: Int(mip.width),
                height: Int(mip.height),
                fourCC: fourCC,
                bodySize: body.count
            )
            dds.append(body)
            try dds.write(to: source)
            arguments += ["-i", source.path]
        } else if [textureFormatRGBA8, textureFormatRGB8].contains(asset.format) {
            let rgba = asset.format == textureFormatRGBA8
            let rgbaSize = try rgbaByteCount(width: Int(mip.width), height: Int(mip.height), label: label)
            let expected = rgba ? rgbaSize : rgbaSize / 4 * 3
            guard body.count == expected else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            source = temporary.appending(path: rgba ? "source.rgba" : "source.rgb")
            try body.write(to: source)
            arguments += [
                "-f", "rawvideo",
                "-pixel_format", rgba ? "rgba" : "rgb24",
                "-video_size", "\(mip.width)x\(mip.height)",
                "-i", source.path,
            ]
        } else {
            throw SceneMobileExportError.unsupportedTextureFormat(asset.format, label)
        }
        arguments += ["-vf", filters.joined(separator: ","), "-frames:v", "1", "-pix_fmt", "rgba", "-threads", "1"]
        if destination.pathExtension == "rgba" { arguments += ["-f", "rawvideo"] }
        if destination.pathExtension == "png" {
            // This disposable PNG is immediately decoded by EtcTool. Deflate
            // and PNG prediction spend CPU without improving the final MPKG.
            arguments += ["-compression_level", "0", "-pred", "none"]
        }
        arguments.append(destination.path)
        try run(ffmpeg, arguments: arguments, label: "FFmpeg \(label)")
        guard FileManager.default.fileExists(atPath: destination.path) else {
            throw SceneMobileExportError.toolFailed("FFmpeg \(label)")
        }
        return (outputWidth, outputHeight)
    }

    private static func decodeBC7(_ body: Data, asset: TextureAsset, mip: TextureMip, label: String) throws -> Data? {
        let width = Int(mip.width), height = Int(mip.height)
        let blocks: Data
        if body.starts(with: Data("DDS ".utf8)) {
            guard body.count >= 148, body[84..<88].elementsEqual("DX10".utf8) else { return nil }
            func word(at offset: Int) -> UInt32 {
                body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self).littleEndian }
            }
            guard [98, 99].contains(word(at: 128)) else { return nil }
            guard word(at: 12) == mip.height, word(at: 16) == mip.width, word(at: 140) == 1 else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            blocks = body.dropFirst(148)
        } else {
            guard asset.format == textureFormatBC7, (asset.imageType ?? -1) < 0 else { return nil }
            blocks = body
        }
        let size = try rgbaByteCount(width: width, height: height, label: label)
        let expected = ((width + 3) / 4) * ((height + 3) / 4) * 16
        guard blocks.count >= expected else { throw SceneMobileExportError.invalidTexture(label) }
        var pixels = Data(count: size)
        blocks.withUnsafeBytes { source in
            pixels.withUnsafeMutableBytes { output in
                mirage_decode_bc7(source.bindMemory(to: UInt8.self).baseAddress!,
                                  output.bindMemory(to: UInt8.self).baseAddress!, mip.width, mip.height)
            }
        }
        return pixels
    }

    private static func selectMobileMip(
        _ asset: TextureAsset, slot: [TextureMip], options: SceneMobileExportOptions,
        useMapSize: Bool, label: String
    ) throws -> MobileMipSelection {
        guard let base = slot.first else { throw SceneMobileExportError.unsupportedTextureLayout(label) }
        let baseWidth = useMapSize && asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(base.width)
        let baseHeight = useMapSize && asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(base.height)
        guard baseWidth > 0, baseHeight > 0, baseWidth <= Int(base.width), baseHeight <= Int(base.height) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let factor = options.reductionFactor(width: baseWidth, height: baseHeight)
        let outputWidth = max(1, baseWidth / factor)
        let outputHeight = max(1, baseHeight / factor)
        let coordinateScale = 1 / Double(factor)
        // Wallpaper Engine keeps the selected resolution even for 8K scenes.
        // A fixed 4096px cap silently changed the advanced "original" option
        // and could distort sprite coordinates. Keep a byte-size guard instead.
        _ = try rgbaByteCount(width: outputWidth, height: outputHeight, label: label)
        // Reuse the smallest mip that still supplies all requested pixels, never upscale a smaller mip.
        // Floor the cropped size: embedded PNG mips use floor(size / 2), not nearest rounding.
        var selected = base
        var sourceWidth = baseWidth
        var sourceHeight = baseHeight
        for mip in slot.dropFirst() {
            let width = max(1, Int(Double(baseWidth) * Double(mip.width) / Double(base.width)))
            let height = max(1, Int(Double(baseHeight) * Double(mip.height) / Double(base.height)))
            if width >= outputWidth, height >= outputHeight, width <= sourceWidth, height <= sourceHeight {
                selected = mip
                sourceWidth = width
                sourceHeight = height
            }
        }
        return MobileMipSelection(mip: selected, sourceWidth: sourceWidth, sourceHeight: sourceHeight,
                                  outputWidth: outputWidth, outputHeight: outputHeight, coordinateScale: coordinateScale)
    }

    private static func mobileSpriteData(
        _ source: Data,
        slotScales: [(x: Double, y: Double)],
        mapWidth: Int,
        mapHeight: Int,
        label: String
    ) throws -> Data {
        var reader = BinaryReader(data: source, label: label)
        let texs = try reader.readStamp(prefix: "TEXS")
        guard let version = Int(texs.suffix(4)), (1...3).contains(version) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let frameCount = Int(try reader.readInt32())
        guard (1...1_000_000).contains(frameCount) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let atlasWidth: Int
        let atlasHeight: Int
        if version >= 3 {
            atlasWidth = Int(try reader.readInt32())
            atlasHeight = Int(try reader.readInt32())
        } else {
            atlasWidth = mapWidth
            atlasHeight = mapHeight
        }
        guard atlasWidth > 0, atlasHeight > 0 else {
            throw SceneMobileExportError.invalidTexture(label)
        }

        var output = Data()
        output.appendStamp("TEXS0003")
        output.appendInt32(Int32(frameCount))
        output.appendInt32(Int32(atlasWidth))
        output.appendInt32(Int32(atlasHeight))
        for _ in 0..<frameCount {
            let imageID = Int(try reader.readInt32())
            guard slotScales.indices.contains(imageID) else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            output.appendInt32(Int32(imageID))
            output.appendUInt32(try reader.readUInt32())
            let scale = slotScales[imageID]
            for coordinate in 0..<6 {
                let value: Float
                if version == 1 {
                    value = Float(try reader.readInt32())
                } else {
                    value = Float(bitPattern: try reader.readUInt32())
                }
                let usesXScale = coordinate.isMultiple(of: 2)
                output.appendFloat(value * Float(usesXScale ? scale.x : scale.y))
            }
        }
        guard reader.remainingData.isEmpty else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        return output
    }

    private static func rawMobilePixels(
        _ asset: TextureAsset, selection: MobileMipSelection, label: String,
        temporary: URL, nearestNeighbor: Bool
    ) throws -> Data {
        let mip = selection.mip
        let body = try decodedMip(mip, label: label)
        let sourceSize = try rgbaByteCount(width: Int(mip.width), height: Int(mip.height), label: label)
        if asset.format == textureFormatRGBA8, body.count == sourceSize, imageSuffix(body) == nil,
           selection.sourceWidth == Int(mip.width), selection.sourceHeight == Int(mip.height),
           selection.outputWidth == Int(mip.width), selection.outputHeight == Int(mip.height) {
            return body
        }
        // ImageIO exposes the original unpremultiplied RGB(A) PNG samples.
        // Read its provider directly: drawing into a CGContext would lose RGB
        // values under transparent pixels and may apply color conversion.
        // Any resize or unsupported provider layout retains the FFmpeg path.
        if selection.outputWidth == selection.sourceWidth,
           selection.outputHeight == selection.sourceHeight,
           let pixels = nativePNGPixels(body, width: Int(mip.width), height: Int(mip.height),
                                        cropWidth: selection.sourceWidth, cropHeight: selection.sourceHeight) {
            return pixels
        }
        let decoded = temporary.appending(path: "decoded.rgba")
        _ = try decodeTexture(asset, mip: mip, label: label, ffmpeg: conversionTool(named: "ffmpeg"),
                              temporary: temporary, destination: decoded,
                              sourceDimensions: (selection.sourceWidth, selection.sourceHeight),
                              outputDimensions: (selection.outputWidth, selection.outputHeight),
                              nearestNeighbor: nearestNeighbor, decodedBody: body)
        let payload = try Data(contentsOf: decoded)
        guard payload.count == (try rgbaByteCount(width: selection.outputWidth, height: selection.outputHeight, label: label)) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        return payload
    }

    private static func nativePNGPixels(
        _ data: Data, width: Int, height: Int, cropWidth: Int, cropHeight: Int
    ) -> Data? {
        guard max(width, height) <= maximumNativePNGDimension,
              canReusePNG(data, width: width, height: height),
              cropWidth > 0, cropHeight > 0, cropWidth <= width, cropHeight <= height,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, [
                  kCGImageSourceShouldCacheImmediately: true,
              ] as CFDictionary),
              image.width == width, image.height == height,
              image.bitsPerComponent == 8, image.bitsPerPixel == 32,
              image.colorSpace?.model == .rgb,
              image.alphaInfo == .last || image.alphaInfo == .noneSkipLast,
              !image.bitmapInfo.contains(.floatComponents),
              [.byteOrderDefault, .byteOrder32Big].contains(image.bitmapInfo.intersection(.byteOrderMask)),
              image.bytesPerRow >= width * 4,
              let providerData = image.dataProvider?.data else { return nil }
        let pixels = providerData as Data
        let (requiredBytes, overflow) = image.bytesPerRow.multipliedReportingOverflow(by: height)
        guard !overflow, pixels.count >= requiredBytes else { return nil }
        let rowBytes = cropWidth * 4
        var output: Data
        if cropWidth == width, cropHeight == height, image.bytesPerRow == rowBytes {
            output = pixels.prefix(rowBytes * cropHeight)
        } else {
            output = Data(count: rowBytes * cropHeight)
            output.withUnsafeMutableBytes { destination in
                pixels.withUnsafeBytes { input in
                    for row in 0..<cropHeight {
                        destination.baseAddress!.advanced(by: row * rowBytes).copyMemory(
                            from: input.baseAddress!.advanced(by: row * image.bytesPerRow), byteCount: rowBytes)
                    }
                }
            }
        }
        if image.alphaInfo == .noneSkipLast {
            // Skip bytes have no defined alpha. Fill them explicitly instead
            // of relying on ImageIO's current value in the padding channel.
            let status = output.withUnsafeMutableBytes { bytes in
                var buffer = vImage_Buffer(data: bytes.baseAddress!, height: vImagePixelCount(cropHeight),
                                           width: vImagePixelCount(cropWidth), rowBytes: rowBytes)
                var destination = buffer
                return vImageOverwriteChannelsWithScalar_ARGB8888(
                    255, &buffer, &destination, 0x1, vImage_Flags(kvImageDoNotTile))
            }
            guard status == kvImageNoError else { return nil }
        }
        return output
    }

    private static func rgbaByteCount(width: Int, height: Int, label: String) throws -> Int {
        let (pixels, overflow) = width.multipliedReportingOverflow(by: height)
        guard width > 0, height > 0, !overflow, pixels <= 512 * 1024 * 1024 / 4 else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        return pixels * 4
    }

    private static func ktxPayload(
        _ data: Data,
        expectedWidth: Int,
        expectedHeight: Int
    ) throws -> Data {
        let identifier = Data([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
        guard data.count >= 68, data.prefix(12) == identifier else {
            throw SceneMobileExportError.invalidKTX
        }
        var reader = BinaryReader(data: data, label: "KTX", offset: 12)
        let endianness = try reader.readUInt32()
        let glType = try reader.readUInt32()
        _ = try reader.readUInt32()
        let glFormat = try reader.readUInt32()
        let internalFormat = try reader.readUInt32()
        _ = try reader.readUInt32()
        let width = try reader.readUInt32()
        let height = try reader.readUInt32()
        let depth = try reader.readUInt32()
        let arrayElements = try reader.readUInt32()
        let faces = try reader.readUInt32()
        let mipLevels = try reader.readUInt32()
        let metadataSize = Int(try reader.readUInt32())
        guard endianness == 0x04030201,
              glType == 0,
              glFormat == 0,
              internalFormat == 0x9278,
              width == expectedWidth,
              height == expectedHeight,
              depth == 0,
              arrayElements == 0,
              faces == 1,
              mipLevels == 0 || mipLevels == 1 else {
            throw SceneMobileExportError.invalidKTX
        }
        _ = try reader.readData(count: metadataSize)
        let imageSize = Int(try reader.readUInt32())
        let expectedSize = ((expectedWidth + 3) / 4) * ((expectedHeight + 3) / 4) * 16
        guard imageSize == expectedSize else { throw SceneMobileExportError.invalidKTX }
        return try reader.readData(count: imageSize)
    }

    private static func imageSuffix(_ data: Data) -> String? {
        if data.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) { return ".png" }
        if data.starts(with: [0xFF, 0xD8, 0xFF]) { return ".jpg" }
        if data.starts(with: Data("GIF87a".utf8)) || data.starts(with: Data("GIF89a".utf8)) { return ".gif" }
        if data.starts(with: Data("BM".utf8)) { return ".bmp" }
        if data.starts(with: Data("DDS ".utf8)) { return ".dds" }
        return nil
    }

    private static func canReusePNG(_ data: Data, width: Int, height: Int) -> Bool {
        guard data.count >= 33,
              data.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) else { return false }
        return data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
            func word(_ offset: Int) -> UInt32 {
                bytes.loadUnaligned(fromByteOffset: offset, as: UInt32.self).bigEndian
            }
            // Keep 16-bit, indexed, interlaced and animated PNG normalization
            // on the existing FFmpeg path. Only ordinary 8-bit RGB(A) mips
            // with an exact IHDR size qualify for direct reuse.
            guard word(8) == 13, word(12) == 0x49484452,
                  word(16) == width, word(20) == height,
                  bytes[24] == 8, [2, 6].contains(bytes[25]),
                  bytes[26] == 0, bytes[27] == 0, bytes[28] == 0 else { return false }
            var offset = 8
            var hasImageData = false
            while offset <= bytes.count - 12 {
                let count = Int(word(offset))
                guard count <= bytes.count - offset - 12 else { return false }
                switch word(offset + 4) {
                case 0x6163544C, 0x74524E53: return false // acTL, tRNS
                case 0x49444154: hasImageData = true // IDAT
                case 0x49454E44: // IEND
                    return hasImageData && count == 0 && offset + 12 == bytes.count
                default: break
                }
                offset += count + 12
            }
            return false
        }
    }

    private static func ddsHeader(
        width: Int,
        height: Int,
        fourCC: String,
        bodySize: Int
    ) -> Data {
        var data = Data("DDS ".utf8)
        [124, 0x00081007, height, width, bodySize, 0, 1].forEach {
            data.appendUInt32(UInt32($0))
        }
        for _ in 0..<11 { data.appendUInt32(0) }
        data.appendUInt32(32)
        data.appendUInt32(0x4)
        data.append(Data(fourCC.utf8))
        for _ in 0..<5 { data.appendUInt32(0) }
        data.appendUInt32(0x00001000)
        for _ in 0..<4 { data.appendUInt32(0) }
        return data
    }

    private static func rewriteMobileShader(_ source: Data, label: String) -> Data {
        // Preserve the original bytes of comments in legacy shaders as well as UTF-8 sources.
        let utf8 = String(data: source, encoding: .utf8)
        let encoding: String.Encoding = utf8 == nil ? .isoLatin1 : .utf8
        guard let text = utf8 ?? String(data: source, encoding: encoding) else { return source }
        // Keep comments, directives, strings and complete numeric literals intact.
        let pattern = #"^[ \t]*#(?:[^\r\n\\]|\\[^\r\n]|\\\r?\n)*|//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFuU]?|\S"#
        guard let regex = try? NSRegularExpression(
            pattern: pattern, options: [.dotMatchesLineSeparators, .anchorsMatchLines]
        ) else { return source }
        let string = text as NSString
        let matches = regex.matches(in: text, range: NSRange(location: 0, length: string.length))
        let tokens = matches.map { string.substring(with: $0.range) }
        let code = tokens.indices.filter {
            !isComment(tokens[$0]) && !tokens[$0].trimmingCharacters(in: .whitespaces).hasPrefix("#")
        }
        var replacements: [Int: String] = [:]
        for index in tokens.indices where tokens[index].trimmingCharacters(in: .whitespaces).hasPrefix("#") {
            var token = tokens[index]
            if let version = token.range(of: #"^[ \t]*#[ \t]*version[ \t]+120(?=[ \t]*(?:$|//))"#,
                                         options: .regularExpression) {
                let digits = token.index(version.upperBound, offsetBy: -3)..<version.upperBound
                token.replaceSubrange(digits, with: "100")
                replacements[index] = token
            }
        }
        let floatCalls: Set<String> = [
            "vec2", "vec3", "vec4", "mat2", "mat3", "mat4",
            "mix", "min", "max", "clamp", "pow", "step", "smoothstep", "mod",
            "abs", "sign", "floor", "trunc", "round", "ceil", "fract",
            "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
            "exp", "exp2", "log", "log2", "sqrt", "inversesqrt",
        ]
        let integerOverloads: Set<String> = ["mix", "min", "max", "clamp", "abs", "sign"]
        let floatingTypes: Set<String> = [
            "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4",
            "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
            "mat4x2", "mat4x3", "mat4x4",
        ]
        let valueTypes = floatingTypes.union([
            "int", "uint", "bool", "ivec2", "ivec3", "ivec4",
            "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
        ])
        var declaredTypes: [String: Set<String>] = [:]
        for position in code.indices where position + 1 < code.count {
            let type = tokens[code[position]]
            let name = tokens[code[position + 1]]
            guard valueTypes.contains(type), let first = name.first,
                  first.isLetter || first == "_" else { continue }
            // Conflicting declarations in different scopes stay ambiguous.
            declaredTypes[name, default: []].insert(type)
        }
        func isIntegerLiteral(_ token: String) -> Bool {
            !token.isEmpty && token.utf8.allSatisfy { (48...57).contains($0) }
        }
        func hasFloatingType(_ argument: [Int]) -> Bool {
            let signed = argument.first.map { ["+", "-"].contains(tokens[$0]) } ?? false
            guard argument.count > (signed ? 1 : 0) else { return false }
            let token = tokens[argument[signed ? 1 : 0]]
            if let types = declaredTypes[token], !types.isEmpty,
               types.isSubset(of: floatingTypes) { return true }
            if floatingTypes.contains(token) || ["CAST2", "CAST3", "CAST4"].contains(token) {
                return true
            }
            if floatCalls.contains(token), !integerOverloads.contains(token) { return true }
            return token.contains(where: { ".eEfF".contains($0) })
                && Double(token.trimmingCharacters(in: CharacterSet(charactersIn: "fF"))) != nil
        }

        for index in code where tokens[index] == "sample" {
            replacements[index] = "_sample"
        }
        // Only normalize whole scalar arguments. Expressions, array indices and
        // nested integer calls retain their types and integer division semantics.
        for position in code.indices where floatCalls.contains(tokens[code[position]]) {
            guard position + 1 < code.count, tokens[code[position + 1]] == "(" else { continue }
            var depth = 1
            var brackets = 0
            var argumentStart = position + 2
            var cursor = argumentStart
            var arguments: [[Int]] = []
            while cursor < code.count, depth > 0 {
                let token = tokens[code[cursor]]
                if token == "(" { depth += 1 }
                if token == "[" { brackets += 1 }
                if token == "]" { brackets -= 1 }
                if (token == "," && depth == 1 && brackets == 0) || (token == ")" && depth == 1) {
                    arguments.append(Array(code[argumentStart..<cursor]))
                    argumentStart = cursor + 1
                }
                if token == ")" { depth -= 1 }
                cursor += 1
            }
            // Integer overloads must retain integer arguments. Unknown types are
            // left intact rather than guessed from the function name alone.
            if integerOverloads.contains(tokens[code[position]]),
               !arguments.contains(where: hasFloatingType) {
                // A desktop shader can assign an integer result to a float.
                // Cast the result explicitly instead of changing the overload's arguments.
                if depth == 0, position >= 3, tokens[code[position - 1]] == "=",
                   floatingTypes.contains(tokens[code[position - 3]]) {
                    replacements[code[position]] = tokens[code[position - 3]] + "(" + tokens[code[position]]
                    replacements[code[cursor - 1]] = "))"
                }
                continue
            }
            for argument in arguments {
                let literal: Int?
                if argument.count == 1 {
                    literal = argument.first
                } else if argument.count == 2, ["+", "-"].contains(tokens[argument[0]]) {
                    literal = argument[1]
                } else {
                    literal = nil
                }
                if let literal, isIntegerLiteral(tokens[literal]) {
                    replacements[literal] = tokens[literal] + ".0"
                }
            }
        }
        for position in code.indices where tokens[code[position]] == "float" {
            guard position + 4 < code.count,
                  tokens[code[position + 2]] == "=" else { continue }
            let signOffset = ["+", "-"].contains(tokens[code[position + 3]]) ? 1 : 0
            let literalPosition = position + 3 + signOffset
            guard literalPosition + 1 < code.count,
                  isIntegerLiteral(tokens[code[literalPosition]]),
                  [";", ","].contains(tokens[code[literalPosition + 1]]) else { continue }
            let literal = code[literalPosition]
            replacements[literal] = tokens[literal] + ".0"
        }
        let result = NSMutableString(string: text)
        for index in replacements.keys.sorted(by: >) {
            result.replaceCharacters(in: matches[index].range, with: replacements[index]!)
        }
        var rewritten = result as String
        let normalized = label.replacingOccurrences(of: "\\", with: "/").lowercased()
        let filename = (normalized as NSString).lastPathComponent
        let effect = (filename as NSString).deletingPathExtension
        let sampler = "uniform sampler2D g_Texture0;"
        if rewritten.hasPrefix("#include \"common_blur.h\"") {
            rewritten = sampler + "\n" + rewritten.replacingOccurrences(of: sampler, with: "")
        }
        switch effect {
        case "foliagesway":
            if rewritten.contains("offset = saturate((offset - v_Bounds.x) * v_Bounds.y);"),
               !rewritten.contains("varying vec2 v_Bounds;") {
                replaceFirst("uniform sampler2D g_Texture1;",
                             with: "varying vec2 v_Bounds;\nuniform sampler2D g_Texture1;", in: &rewritten)
            }
        case "blend":
            rewritten = rewritten.replacingOccurrences(of: #"step\(0(?:\.0)?, uv\) \* step\(uv, 1(?:\.0)?\)"#,
                                                       with: "step(CAST2(0.0), uv) * step(uv, CAST2(1.0))",
                                                       options: .regularExpression)
        case "shift_hue":
            rewritten = rewritten.replacingOccurrences(of: "albedo.rgb = mix(albedo, newAlbedo, mask)",
                                                       with: "albedo.rgb = mix(albedo.rgb, newAlbedo, mask)")
        case "skew":
            let resolution = "uniform vec4 g_Texture0Resolution;"
            let scale = "uniform float g_TextureReductionScale;"
            let assignment = "vec2 textureScale = g_Texture0Resolution.zw;"
            if rewritten.contains(resolution), rewritten.contains(assignment) {
                if !rewritten.contains(scale) {
                    replaceFirst(resolution, with: resolution + "\n" + scale, in: &rewritten)
                }
                replaceFirst(assignment, with: "vec2 textureScale = g_Texture0Resolution.zw * g_TextureReductionScale;",
                             in: &rewritten)
            }
        default: break
        }
        if normalized.hasSuffix("xray.frag") || normalized.hasSuffix("xray.vert") {
            let pointerVarying = "varying float v_PointerScale;"
            let pointerUniform = "uniform vec4 g_PointerState;"
            if rewritten.contains(pointerVarying), !rewritten.contains(pointerUniform) {
                replaceFirst(
                    pointerVarying,
                    with: pointerVarying + "\n" + pointerUniform,
                    in: &rewritten
                )
            }
            let pointerScale = "unprojectedUVs *= v_PointerScale * vec2(1.0, v_PointerUV.w);"
            if rewritten.contains(pointerScale), !rewritten.contains("g_PointerState.y") {
                replaceFirst(
                    pointerScale,
                    with: pointerScale
                        + "\n\tunprojectedUVs *= CAST2(1.0 / max(0.00001, g_PointerState.y));",
                    in: &rewritten
                )
            }
        }
        if normalized.hasSuffix("perspective.vert") {
            let resolutionUniform = "uniform vec4 g_Texture0Resolution;"
            let reductionUniform = "uniform float g_TextureReductionScale;"
            if rewritten.contains(resolutionUniform), !rewritten.contains(reductionUniform) {
                replaceFirst(
                    resolutionUniform,
                    with: resolutionUniform + "\n" + reductionUniform,
                    in: &rewritten
                )
            }
            if let expression = try? NSRegularExpression(
                pattern: #"\bmix\s*\(\s*-1(?:\.0*)?[fF]?\s*,\s*1(?:\.0*)?[fF]?\s*,\s*step\s*\("#
            ) {
                let range = NSRange(rewritten.startIndex..<rewritten.endIndex, in: rewritten)
                if let match = expression.firstMatch(in: rewritten, range: range),
                   let matchRange = Range(match.range, in: rewritten) {
                    rewritten.replaceSubrange(
                        matchRange,
                        with: "mix(-g_TextureReductionScale, g_TextureReductionScale, step("
                    )
                }
            }
        }
        return rewritten.data(using: encoding) ?? source
    }

    private static func replaceFirst(_ target: String, with replacement: String, in value: inout String) {
        guard let range = value.range(of: target) else { return }
        value.replaceSubrange(range, with: replacement)
    }


    private static func isComment(_ token: String) -> Bool {
        token.hasPrefix("//") || token.hasPrefix("/*")
    }

    private static func lz4Decompress(_ source: Data, size: Int, label: String) throws -> Data {
        guard size > 0, size <= 512 * 1024 * 1024 else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        var destination = Data(count: size)
        let decoded = destination.withUnsafeMutableBytes { destinationBytes in
            source.withUnsafeBytes { sourceBytes in
                compression_decode_buffer(
                    destinationBytes.bindMemory(to: UInt8.self).baseAddress!,
                    size,
                    sourceBytes.bindMemory(to: UInt8.self).baseAddress!,
                    source.count,
                    nil,
                    COMPRESSION_LZ4_RAW
                )
            }
        }
        guard decoded == size else { throw SceneMobileExportError.invalidTexture(label) }
        return destination
    }

    private static func lz4Compress(_ source: Data, label: String) throws -> Data {
        guard !source.isEmpty, source.count <= 512 * 1024 * 1024 else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        var capacity = source.count + source.count / 255 + 32
        for _ in 0..<2 {
            var destination = Data(count: capacity)
            let encoded = destination.withUnsafeMutableBytes { destinationBytes in
                source.withUnsafeBytes { sourceBytes in
                    compression_encode_buffer(
                        destinationBytes.bindMemory(to: UInt8.self).baseAddress!,
                        capacity,
                        sourceBytes.bindMemory(to: UInt8.self).baseAddress!,
                        source.count,
                        nil,
                        COMPRESSION_LZ4_RAW
                    )
                }
            }
            if encoded > 0 {
                destination.count = encoded
                return destination
            }
            capacity *= 2
        }
        throw SceneMobileExportError.invalidTexture(label)
    }

    private static func conversionTool(named name: String) throws -> URL {
        let fileManager = FileManager.default
        let bundled = Bundle.main.resourceURL?
            .appending(path: "SceneMobileTools", directoryHint: .isDirectory)
        if let candidate = bundled?.appending(path: name),
           fileManager.isExecutableFile(atPath: candidate.path) {
            return candidate
        }
        throw SceneMobileExportError.conversionToolsMissing
    }

    private static func run(_ executable: URL, arguments: [String], label: String) throws {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        let detailData: Data
        do {
            try process.run()
            // Drain the pipe while the child is running. Waiting first can
            // deadlock when an encoder emits more than the pipe buffer.
            detailData = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
        } catch {
            throw SceneMobileExportError.toolFailed("\(label)：\(error.localizedDescription)")
        }
        let detail = String(
            data: detailData,
            encoding: .utf8
        )?.trimmingCharacters(in: .whitespacesAndNewlines)
        guard process.terminationStatus == 0 else {
            throw SceneMobileExportError.toolFailed(detail?.isEmpty == false ? detail! : label)
        }
    }

    private static func stage(
        _ data: Data,
        named name: String,
        in directory: URL
    ) throws -> StagedEntry {
        guard isSafeEntryName(name) else {
            throw SceneMobileExportError.unsafeProjectPath
        }
        let target = directory.appending(path: UUID().uuidString)
        try data.write(to: target)
        return StagedEntry(name: name, url: target, offset: 0, size: UInt64(data.count))
    }

    private static func sourceEntry(named name: String, at url: URL) throws -> StagedEntry {
        guard isSafeEntryName(name) else { throw SceneMobileExportError.unsafeProjectPath }
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard values.isRegularFile == true, let size = values.fileSize else {
            throw SceneMobileExportError.invalidProject
        }
        return StagedEntry(name: name, url: url, offset: 0, size: UInt64(size))
    }

    private static func stagedEntryOrder(_ left: StagedEntry, _ right: StagedEntry) -> Bool {
        let foldedLeft = left.name.lowercased()
        let foldedRight = right.name.lowercased()
        return foldedLeft == foldedRight ? left.name < right.name : foldedLeft < foldedRight
    }

    private static func textureConversionParallelism() -> Int {
        let info = ProcessInfo.processInfo
        let cpuBound = max(1, min(2, info.activeProcessorCount / 4))
        let bytesPerWorker: UInt64 = 8 * 1024 * 1024 * 1024
        let memoryBound = max(1, min(2, Int(info.physicalMemory / bytesPerWorker)))
        return min(cpuBound, memoryBound)
    }

    private static func writeArchive(
        entries: [StagedEntry],
        version: String,
        to outputURL: URL,
        progress: @escaping (Double) -> Void
    ) throws {
        let archiveEntries = entries.map {
            MobilePackageArchive.Entry(name: $0.name, source: .file($0.url, offset: $0.offset, size: $0.size))
        }
        try MobilePackageArchive.write(entries: archiveEntries, version: version, to: outputURL) { written, total in
            progress(total == 0 ? 1 : Double(written) / Double(total))
        }
    }

    private static func containedFile(_ name: String, in directory: URL) -> URL? {
        guard isSafeEntryName(name), let candidate = PathContainment.containedURL(name, in: directory),
              (try? candidate.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else { return nil }
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

    private struct BinaryReader {
        let data: Data
        let label: String
        var offset: Int = 0

        mutating func readData(count: Int) throws -> Data {
            guard count >= 0, offset + count <= data.count else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            defer { offset += count }
            return data.subdata(in: offset..<(offset + count))
        }

        mutating func readUInt32() throws -> UInt32 {
            guard offset + 4 <= data.count else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            let value = data.withUnsafeBytes {
                $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self).littleEndian
            }
            offset += 4
            return value
        }

        mutating func readInt32() throws -> Int32 {
            Int32(bitPattern: try readUInt32())
        }

        mutating func readStamp(prefix: String) throws -> String {
            let value = try readData(count: 9)
            guard value.last == 0,
                  let stamp = String(data: value.dropLast(), encoding: .ascii),
                  stamp.count == 8,
                  stamp.hasPrefix(prefix),
                  stamp.suffix(4).allSatisfy(\.isNumber) else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            return stamp
        }

        var remainingData: Data {
            data.subdata(in: offset..<data.count)
        }
    }
}

enum SceneMobileExportError: LocalizedError {
    case unsupportedWallpaperType(WallpaperKind)
    case invalidProject
    case unsafeProjectPath
    case invalidPackage
    case truncatedPackage
    case unsupportedPackageVersion(String)
    case invalidTexture(String)
    case unsupportedTextureLayout(String)
    case unsupportedTextureFormat(Int32, String)
    case invalidKTX
    case conversionToolsMissing
    case toolFailed(String)
    case wallpaperTooLarge

    var errorDescription: String? {
        switch self {
        case .unsupportedWallpaperType(let kind):
            return L("暂不支持将%@壁纸转换到移动设备。", kind.displayName)
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
        case .invalidKTX:
            return L("ETC2 编码器返回了无效纹理。")
        case .conversionToolsMissing:
            return L("App 缺少场景转换组件，请重新安装 Mirage。")
        case .toolFailed(let detail):
            return L("场景转换失败：%@", detail)
        case .wallpaperTooLarge:
            return L("移动设备场景壁纸不能超过 4 GB。")
        }
    }
}

private extension Data {
    mutating func appendUInt32(_ value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
    }

    mutating func appendInt32(_ value: Int32) {
        appendUInt32(UInt32(bitPattern: value))
    }

    mutating func appendFloat(_ value: Float) {
        appendUInt32(value.bitPattern)
    }

    mutating func appendStamp(_ value: String) {
        append(Data(value.utf8))
        append(0)
    }
}
