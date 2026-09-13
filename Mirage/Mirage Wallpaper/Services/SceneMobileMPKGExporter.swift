//
//  Mirage Wallpaper
//
//  Native conversion of desktop scene packages for Wallpaper Engine Android.
//

import Compression
import Foundation

enum SceneMobileMPKGExporter {
    private static let textureFormatRGBA8: Int32 = 0
    private static let textureFormatBC3: Int32 = 4
    private static let textureFormatETC2RGBA8: Int32 = 5
    private static let textureFormatBC2: Int32 = 6
    private static let textureFormatBC1: Int32 = 7
    private static let textureFormatRG8: Int32 = 8
    private static let textureFormatR8: Int32 = 9
    private static let spriteFlag: UInt32 = 1 << 2
    private static let rawMobileFlag: UInt32 = 1 << 3
    private static let streamingTextureFlag: UInt32 = 1 << 5
    private static let maximumMobileTextureDimension = 4096
    private static let effort = 10
    private static let copyBufferSize = 1024 * 1024
    private static let excludedMobileSuffixes: Set<String> = [
        "flac", "m4a", "mp3", "ogg", "wav",
    ]

    private struct PackageEntry {
        let name: String
        let absoluteOffset: UInt64
        let size: UInt64
    }

    private struct StagedEntry {
        let name: String
        let url: URL
        let offset: UInt64
        let size: UInt64
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
        let payload: Data
        let compressed: Data
    }

    private struct MobileMipSelection {
        let mip: TextureMip
        let sourceWidth: Int
        let sourceHeight: Int
        let reduced: Bool
    }

    static func export(
        _ wallpaper: WEWallpaper,
        to outputURL: URL,
        progress: ((Double) -> Void)? = nil
    ) throws {
        guard wallpaper.isValid, wallpaper.kind == .scene else {
            throw SceneMobileExportError.unsupportedWallpaperType(wallpaper.kind)
        }
        guard wallpaper.wallpaperDirectory.standardizedFileURL
            == wallpaper.renderDirectory.standardizedFileURL else {
            throw SceneMobileExportError.scenePresetNotSupported
        }

        let projectURL = wallpaper.wallpaperDirectory.appending(path: "project.json")
        let packageURL = wallpaper.renderDirectory.appending(path: "scene.pkg")
        let projectData = try Data(contentsOf: projectURL)
        guard let project = try JSONSerialization.jsonObject(with: projectData) as? [String: Any],
              (project["type"] as? String)?.lowercased() == "scene",
              let previewName = project["preview"] as? String,
              let previewURL = containedFile(previewName, in: wallpaper.wallpaperDirectory) else {
            throw SceneMobileExportError.invalidProject
        }

        let package = try readPackage(at: packageURL)
        let mobileVersion = try mobileVersion(for: package.version)
        let tools = try conversionTools()
        let fileManager = FileManager.default
        let staging = fileManager.temporaryDirectory
            .appending(path: "Mirage-Mobile-Scene-\(UUID().uuidString)", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: staging) }

        let packageHandle = try FileHandle(forReadingFrom: packageURL)
        defer { try? packageHandle.close() }
        var stagedNames = Set(package.entries.map { $0.name.lowercased() })
        guard stagedNames.insert("project.json").inserted,
              stagedNames.insert(previewName.lowercased()).inserted else {
            throw SceneMobileExportError.unsafeProjectPath
        }
        var stagedEntries: [StagedEntry] = []
        var textureEntries: [(index: Int, entry: PackageEntry)] = []
        var processed = 0
        let conversionCount = max(package.entries.count + 2, 1)
        progress?(0)

        for (index, entry) in package.entries.enumerated() {
            let suffix = URL(fileURLWithPath: entry.name).pathExtension.lowercased()
            if excludedMobileSuffixes.contains(suffix) {
                processed += 1
                progress?(Double(processed) / Double(conversionCount) * 0.9)
                continue
            }

            if suffix == "tex" {
                let target = staging.appending(path: entry.name)
                try fileManager.createDirectory(
                    at: target.deletingLastPathComponent(),
                    withIntermediateDirectories: true
                )
                textureEntries.append((index, entry))
                continue
            }

            if suffix != "vert" && suffix != "frag" {
                stagedEntries.append(StagedEntry(
                    name: entry.name,
                    url: packageURL,
                    offset: entry.absoluteOffset,
                    size: entry.size
                ))
                processed += 1
                progress?(Double(processed) / Double(conversionCount) * 0.9)
                continue
            }

            try packageHandle.seek(toOffset: entry.absoluteOffset)
            guard entry.size <= UInt64(Int.max) else {
                throw SceneMobileExportError.wallpaperTooLarge
            }
            let source = try packageHandle.read(upToCount: Int(entry.size)) ?? Data()
            guard source.count == Int(entry.size) else {
                throw SceneMobileExportError.truncatedPackage
            }

            let data = rewriteMobileShader(source, label: entry.name)
            stagedEntries.append(try stage(data, named: entry.name, in: staging))
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
        let progressLock = NSLock()
        var textureResults: [Int: StagedEntry] = [:]
        var firstError: Error?
        for item in textureEntries {
            queue.addOperation {
                resultLock.lock()
                let shouldStop = firstError != nil
                resultLock.unlock()
                guard !shouldStop else { return }
                do {
                    let source = try readPackageEntry(item.entry, from: packageURL)
                    let data = try mobileTexture(
                        source,
                        label: item.entry.name,
                        ffmpeg: tools.ffmpeg,
                        etcTool: tools.etcTool,
                        jobs: encoderJobs
                    )
                    let staged = try stage(data, named: item.entry.name, in: staging)
                    resultLock.lock()
                    textureResults[item.index] = staged
                    processed += 1
                    let fraction = Double(processed) / Double(conversionCount) * 0.9
                    resultLock.unlock()
                    progressLock.lock()
                    progress?(fraction)
                    progressLock.unlock()
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

        stagedEntries.append(try stage(projectData, named: "project.json", in: staging))
        processed += 1
        progress?(Double(processed) / Double(conversionCount) * 0.9)
        stagedEntries.append(try sourceEntry(named: previewName, at: previewURL))
        processed += 1
        progress?(Double(processed) / Double(conversionCount) * 0.9)

        let entries = stagedEntries.sorted(by: stagedEntryOrder)
        try writeArchive(entries: entries, version: mobileVersion, to: outputURL) { fraction in
            progress?(0.9 + fraction * 0.1)
        }
        progress?(1)
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

    private static func mobileVersion(for sourceVersion: String) throws -> String {
        guard let value = Int(sourceVersion.suffix(4)),
              let mobile = [
                  1: 14,
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

    private static func mobileTexture(
        _ source: Data,
        label: String,
        ffmpeg: URL,
        etcTool: URL,
        jobs: Int
    ) throws -> Data {
        let asset = try parseTexture(source, label: label)
        if asset.flags & streamingTextureFlag != 0
            || [textureFormatETC2RGBA8, textureFormatRG8, textureFormatR8].contains(asset.format) {
            return source
        }
        if asset.format == textureFormatRGBA8,
           canEncodeRawMobileTexture(asset),
           (asset.flags == 0 || preservesMobileRGBA8(label: label, asset: asset)) {
            return try rawMobileTexture(asset, label: label, ffmpeg: ffmpeg)
        }
        guard [textureFormatRGBA8, textureFormatBC1, textureFormatBC2, textureFormatBC3]
            .contains(asset.format) else {
            throw SceneMobileExportError.unsupportedTextureFormat(asset.format, label)
        }
        guard asset.spriteData != nil || asset.slots.count == 1,
              asset.slots.allSatisfy({ !$0.isEmpty }) else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }

        let temporary = FileManager.default.temporaryDirectory
            .appending(path: "Mirage-Scene-Texture-\(UUID().uuidString)", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        var convertedSlots: [ConvertedTextureSlot] = []
        var slotScales: [(x: Double, y: Double)] = []
        var safetyReduced = false
        for (slotIndex, slot) in asset.slots.enumerated() {
            let selection = try selectMobileMip(
                asset,
                slot: slot,
                label: "\(label) slot \(slotIndex)"
            )
            let mip = selection.mip
            let decoded = temporary.appending(path: "decoded-\(slotIndex).png")
            let dimensions = try decodeTexture(
                asset,
                mip: mip,
                label: "\(label) slot \(slotIndex)",
                ffmpeg: ffmpeg,
                temporary: temporary,
                destination: decoded,
                maximumDimension: maximumMobileTextureDimension,
                sourceDimensions: (selection.sourceWidth, selection.sourceHeight)
            )
            let encoded = temporary.appending(path: "encoded-\(slotIndex).ktx")
            try run(
                etcTool,
                arguments: [
                    decoded.path,
                    "-format", "RGBA8",
                    "-errormetric", "rgbx",
                    "-effort", String(effort),
                    "-j", String(jobs),
                    "-output", encoded.path,
                ],
                label: "ETC2 \(label) slot \(slotIndex)"
            )
            let payload = try ktxPayload(
                Data(contentsOf: encoded),
                expectedWidth: dimensions.width,
                expectedHeight: dimensions.height
            )
            convertedSlots.append(ConvertedTextureSlot(
                width: ((dimensions.width + 3) / 4) * 4,
                height: ((dimensions.height + 3) / 4) * 4,
                payload: payload,
                compressed: try lz4Compress(payload, label: "\(label) slot \(slotIndex)")
            ))
            slotScales.append((
                Double(dimensions.width) / Double(slot[0].width),
                Double(dimensions.height) / Double(slot[0].height)
            ))
            safetyReduced = safetyReduced || selection.reduced
        }
        guard let firstSlot = convertedSlots.first else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }
        let mapWidth = asset.mapWidth > 0 ? Int(asset.mapWidth) : firstSlot.width
        let mapHeight = asset.mapHeight > 0 ? Int(asset.mapHeight) : firstSlot.height
        let headerWidth = safetyReduced
            ? (asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(asset.width))
            : (asset.spriteData == nil ? firstSlot.width : Int(asset.width))
        let headerHeight = safetyReduced
            ? (asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(asset.height))
            : (asset.spriteData == nil ? firstSlot.height : Int(asset.height))
        let mobileFlags = asset.flags | (asset.slots[0].count == 1 ? rawMobileFlag : 0)

        var output = Data()
        output.appendStamp("TEXV0005")
        output.appendStamp("TEXI0001")
        output.appendInt32(textureFormatETC2RGBA8)
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
            output.appendInt32(Int32(slot.payload.count))
            output.appendInt32(Int32(slot.compressed.count))
            output.append(slot.compressed)
        }
        if let spriteData = asset.spriteData {
            output.append(try mobileSpriteData(
                spriteData,
                slotScales: slotScales,
                mapWidth: mapWidth,
                mapHeight: mapHeight,
                label: label
            ))
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

    private static func canEncodeRawMobileTexture(_ asset: TextureAsset) -> Bool {
        guard asset.slots.count == 1, let mip = asset.slots[0].first else { return false }
        return max(Int(mip.width), Int(mip.height)) <= maximumMobileTextureDimension
            && max(Int(asset.mapWidth), Int(asset.mapHeight)) <= maximumMobileTextureDimension
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
        maximumDimension: Int? = nil,
        sourceDimensions: (width: Int, height: Int)? = nil
    ) throws -> (width: Int, height: Int) {
        let body = try decodedMip(mip, label: label)
        let sourceWidth = sourceDimensions?.width ?? (asset.spriteData == nil
            ? (asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(mip.width))
            : Int(mip.width))
        let sourceHeight = sourceDimensions?.height ?? (asset.spriteData == nil
            ? (asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(mip.height))
            : Int(mip.height))
        guard sourceWidth <= Int(mip.width), sourceHeight <= Int(mip.height) else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        var outputWidth = sourceWidth
        var outputHeight = sourceHeight
        if let maximumDimension, max(outputWidth, outputHeight) > maximumDimension {
            let scale = Double(maximumDimension) / Double(max(outputWidth, outputHeight))
            outputWidth = max(1, Int((Double(outputWidth) * scale).rounded()))
            outputHeight = max(1, Int((Double(outputHeight) * scale).rounded()))
        }
        var filters = ["crop=\(sourceWidth):\(sourceHeight):0:0"]
        if outputWidth != sourceWidth || outputHeight != sourceHeight {
            filters.append("scale=\(outputWidth):\(outputHeight):flags=lanczos")
        }
        let source: URL
        var arguments = ["-hide_banner", "-loglevel", "error", "-y"]
        if let suffix = imageSuffix(body) {
            source = temporary.appending(path: "source\(suffix)")
            try body.write(to: source)
            arguments += ["-i", source.path]
        } else if [textureFormatBC1, textureFormatBC2, textureFormatBC3].contains(asset.format) {
            source = temporary.appending(path: "source.dds")
            let fourCC: String = [
                textureFormatBC1: "DXT1",
                textureFormatBC2: "DXT3",
                textureFormatBC3: "DXT5",
            ][asset.format]!
            var dds = ddsHeader(
                width: Int(mip.width),
                height: Int(mip.height),
                fourCC: fourCC,
                bodySize: body.count
            )
            dds.append(body)
            try dds.write(to: source)
            arguments += ["-i", source.path]
        } else if asset.format == textureFormatRGBA8 {
            let expected = Int(mip.width) * Int(mip.height) * 4
            guard body.count >= expected else {
                throw SceneMobileExportError.invalidTexture(label)
            }
            source = temporary.appending(path: "source.rgba")
            try body.prefix(expected).write(to: source)
            arguments += [
                "-f", "rawvideo",
                "-pixel_format", "rgba",
                "-video_size", "\(mip.width)x\(mip.height)",
                "-i", source.path,
            ]
        } else {
            throw SceneMobileExportError.unsupportedTextureFormat(asset.format, label)
        }
        arguments += ["-vf", filters.joined(separator: ","), "-frames:v", "1", destination.path]
        try run(ffmpeg, arguments: arguments, label: "FFmpeg \(label)")
        guard FileManager.default.fileExists(atPath: destination.path) else {
            throw SceneMobileExportError.toolFailed("FFmpeg \(label)")
        }
        return (outputWidth, outputHeight)
    }

    private static func selectMobileMip(
        _ asset: TextureAsset,
        slot: [TextureMip],
        label: String
    ) throws -> MobileMipSelection {
        guard let base = slot.first else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }
        let baseWidth = asset.spriteData == nil
            ? (asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(base.width))
            : Int(base.width)
        let baseHeight = asset.spriteData == nil
            ? (asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(base.height))
            : Int(base.height)
        guard baseWidth > 0, baseHeight > 0 else {
            throw SceneMobileExportError.invalidTexture(label)
        }

        var selected = base
        var selectedWidth = baseWidth
        var selectedHeight = baseHeight
        for mip in slot {
            selected = mip
            selectedWidth = min(
                Int(mip.width),
                max(1, Int((Double(baseWidth) * Double(mip.width) / Double(base.width)).rounded()))
            )
            selectedHeight = min(
                Int(mip.height),
                max(1, Int((Double(baseHeight) * Double(mip.height) / Double(base.height)).rounded()))
            )
            if max(selectedWidth, selectedHeight) <= maximumMobileTextureDimension { break }
        }

        var outputWidth = selectedWidth
        var outputHeight = selectedHeight
        if max(outputWidth, outputHeight) > maximumMobileTextureDimension {
            let scale = Double(maximumMobileTextureDimension) / Double(max(outputWidth, outputHeight))
            outputWidth = max(1, Int((Double(outputWidth) * scale).rounded()))
            outputHeight = max(1, Int((Double(outputHeight) * scale).rounded()))
        }
        return MobileMipSelection(
            mip: selected,
            sourceWidth: selectedWidth,
            sourceHeight: selectedHeight,
            reduced: outputWidth != baseWidth || outputHeight != baseHeight
        )
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

    private static func rawMobileTexture(
        _ asset: TextureAsset,
        label: String,
        ffmpeg: URL
    ) throws -> Data {
        guard asset.slots.count == 1, let mip = asset.slots.first?.first else {
            throw SceneMobileExportError.unsupportedTextureLayout(label)
        }
        var payload = try decodedMip(mip, label: label)
        var width = Int(mip.width)
        var height = Int(mip.height)
        var expected = width * height * 4
        if payload.count != expected, let suffix = imageSuffix(payload) {
            let temporary = FileManager.default.temporaryDirectory
                .appending(path: "Mirage-Scene-RGBA-\(UUID().uuidString)", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: temporary) }
            let source = temporary.appending(path: "source\(suffix)")
            let decoded = temporary.appending(path: "decoded.rgba")
            try payload.write(to: source)
            width = asset.mapWidth > 0 ? Int(asset.mapWidth) : Int(mip.width)
            height = asset.mapHeight > 0 ? Int(asset.mapHeight) : Int(mip.height)
            try run(
                ffmpeg,
                arguments: [
                    "-hide_banner", "-loglevel", "error", "-y",
                    "-i", source.path,
                    "-vf", "crop=\(width):\(height):0:0",
                    "-frames:v", "1",
                    "-f", "rawvideo",
                    "-pix_fmt", "rgba",
                    decoded.path,
                ],
                label: "FFmpeg raw RGBA \(label)"
            )
            payload = try Data(contentsOf: decoded)
            expected = width * height * 4
        }
        guard payload.count == expected else {
            throw SceneMobileExportError.invalidTexture(label)
        }
        let mobileFlags = asset.flags | (asset.slots[0].count == 1 ? rawMobileFlag : 0)
        let compressed = try lz4Compress(payload, label: label)
        var output = Data()
        output.appendStamp("TEXV0005")
        output.appendStamp("TEXI0001")
        output.appendInt32(textureFormatRGBA8)
        output.appendUInt32(mobileFlags)
        output.appendInt32(Int32(width))
        output.appendInt32(Int32(height))
        output.appendInt32(asset.mapWidth > 0 ? asset.mapWidth : Int32(width))
        output.appendInt32(asset.mapHeight > 0 ? asset.mapHeight : Int32(height))
        output.appendInt32(asset.reservedA)
        output.appendStamp("TEXB0004")
        output.appendInt32(1)
        output.appendInt32(-1)
        output.appendInt32(0)
        output.appendInt32(1)
        output.appendInt32(Int32(width))
        output.appendInt32(Int32(height))
        output.appendInt32(1)
        output.appendInt32(Int32(payload.count))
        output.appendInt32(Int32(compressed.count))
        output.append(compressed)
        if let spriteData = asset.spriteData { output.append(spriteData) }
        return output
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
        return nil
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
        guard let text = String(data: source, encoding: .utf8) else { return source }
        let pattern = #"//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?|\S"#
        guard let regex = try? NSRegularExpression(pattern: pattern, options: [.dotMatchesLineSeparators]) else {
            return source
        }
        let string = text as NSString
        let matches = regex.matches(in: text, range: NSRange(location: 0, length: string.length))
        let tokens = matches.map { string.substring(with: $0.range) }
        var replacements: [Int: String] = [:]
        var constructorRanges: [Range<Int>] = []
        var bracketRanges: [Range<Int>] = []
        var arrayIndexIdentifiers = Set<String>()
        var bracketStack: [Int] = []
        let callNames: Set<String> = ["vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "smoothstep"]

        for index in tokens.indices {
            if tokens[index] == "[" {
                bracketStack.append(index)
            } else if tokens[index] == "]", let start = bracketStack.popLast() {
                bracketRanges.append((start + 1)..<index)
                for cursor in (start + 1)..<index where
                    tokens[cursor].range(of: #"^[A-Za-z_]\w*$"#, options: .regularExpression) != nil {
                    arrayIndexIdentifiers.insert(tokens[cursor])
                }
            }
        }

        var protectedForRanges: [Range<Int>] = []
        for index in tokens.indices where tokens[index] == "for" {
            let open = index + 1
            guard open < tokens.count, tokens[open] == "(" else { continue }
            var depth = 0
            var cursor = open
            var identifiers = Set<String>()
            while cursor < tokens.count {
                let token = tokens[cursor]
                if token == "(" { depth += 1 }
                else if token == ")" {
                    depth -= 1
                    if depth == 0 { break }
                } else if depth > 0,
                          token.range(of: #"^[A-Za-z_]\w*$"#, options: .regularExpression) != nil {
                    identifiers.insert(token)
                }
                cursor += 1
            }
            if depth == 0, !identifiers.isDisjoint(with: arrayIndexIdentifiers) {
                protectedForRanges.append((open + 1)..<cursor)
            }
        }

        // The Android GLSL compiler treats `sample` as a reserved identifier.
        // Wallpaper Engine's mobile exporter prefixes code identifiers with an
        // underscore while leaving strings and larger identifiers untouched.
        for index in tokens.indices where tokens[index] == "sample" {
            if !isPreprocessorToken(matches[index].range, in: string) {
                replacements[index] = "_sample"
            }
        }

        for index in tokens.indices where callNames.contains(tokens[index]) {
            let open = index + 1
            guard open < tokens.count, tokens[open] == "(" else { continue }
            var depth = 0
            var cursor = open
            while cursor < tokens.count {
                let token = tokens[cursor]
                if token == "(" { depth += 1 }
                else if token == ")" {
                    depth -= 1
                    if depth == 0 { break }
                } else if depth > 0,
                          token.range(of: #"^\d+$"#, options: .regularExpression) != nil,
                          !isPreprocessorToken(matches[cursor].range, in: string) {
                    replacements[cursor] = token + ".0"
                }
                cursor += 1
            }
            if depth == 0 { constructorRanges.append((open + 1)..<cursor) }
        }

        for index in tokens.indices {
            let token = tokens[index]
            guard token.range(of: #"^\d+$"#, options: .regularExpression) != nil,
                  !isPreprocessorToken(matches[index].range, in: string),
                  !constructorRanges.contains(where: { $0.contains(index) }),
                  !bracketRanges.contains(where: { $0.contains(index) }),
                  !protectedForRanges.contains(where: { $0.contains(index) }) else { continue }
            let previous = previousCodeToken(tokens, before: index)
            let following = nextCodeToken(tokens, after: index)
            if previous == "#" || previous == "[" || following == ":" { continue }
            if previous == "." || previous == "_" || following == "." { continue }
            if ["return", "=", "+", "-", "*", "/", "<", ">", "<=", ">=", ",", "("].contains(previous)
                || ["+", "-", "*", "/", ")", ",", ";"].contains(following) {
                replacements[index] = token + ".0"
            }
        }

        // Float normalization may otherwise leave invalid declarations such
        // as `int value = 1.0`. The official mobile exporter promotes the
        // declaration too, including loop counters.
        for index in tokens.indices where tokens[index] == "int" {
            let nameIndex = index + 1
            let equalsIndex = index + 2
            let valueIndex = index + 3
            guard valueIndex < tokens.count,
                  tokens[nameIndex].range(of: #"^[A-Za-z_]\w*$"#, options: .regularExpression) != nil,
                  !arrayIndexIdentifiers.contains(tokens[nameIndex]),
                  tokens[equalsIndex] == "=",
                  replacements[valueIndex]?.hasSuffix(".0") == true else { continue }
            replacements[index] = "float"
        }
        let result = NSMutableString(string: text)
        for index in replacements.keys.sorted(by: >) {
            result.replaceCharacters(in: matches[index].range, with: replacements[index]!)
        }
        var rewritten = result as String
        let normalized = label.replacingOccurrences(of: "\\", with: "/").lowercased()
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
            rewritten = rewritten.replacingOccurrences(
                of: "mix(-1.0, 1.0, step(",
                with: "mix(-g_TextureReductionScale, g_TextureReductionScale, step("
            )
        }
        return Data(rewritten.utf8)
    }

    private static func replaceFirst(_ target: String, with replacement: String, in value: inout String) {
        guard let range = value.range(of: target) else { return }
        value.replaceSubrange(range, with: replacement)
    }

    private static func isPreprocessorToken(_ range: NSRange, in string: NSString) -> Bool {
        let prefixRange = NSRange(location: 0, length: range.location)
        let newline = string.range(of: "\n", options: .backwards, range: prefixRange)
        let start = newline.location == NSNotFound ? 0 : newline.location + 1
        let linePrefix = string.substring(with: NSRange(location: start, length: range.location - start))
        return linePrefix.trimmingCharacters(in: .whitespaces).hasPrefix("#")
    }

    private static func previousCodeToken(_ tokens: [String], before index: Int) -> String {
        guard index > 0 else { return "" }
        for cursor in stride(from: index - 1, through: 0, by: -1) {
            if !isComment(tokens[cursor]) { return tokens[cursor] }
        }
        return ""
    }

    private static func nextCodeToken(_ tokens: [String], after index: Int) -> String {
        guard index + 1 < tokens.count else { return "" }
        for cursor in (index + 1)..<tokens.count where !isComment(tokens[cursor]) {
            return tokens[cursor]
        }
        return ""
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

    private static func conversionTools() throws -> (ffmpeg: URL, etcTool: URL) {
        let fileManager = FileManager.default
        let bundled = Bundle.main.resourceURL?
            .appending(path: "SceneMobileTools", directoryHint: .isDirectory)
        var candidates: [(URL?, URL?)] = [
            (bundled?.appending(path: "ffmpeg"), bundled?.appending(path: "EtcTool")),
        ]
#if DEBUG
        let projectRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let architecture = hostArchitecture()
        let brewPrefix = architecture == "x86_64" ? "/usr/local" : "/opt/homebrew"
        candidates.append((
            URL(fileURLWithPath: "\(brewPrefix)/opt/ffmpeg/bin/ffmpeg"),
            projectRoot
                .appending(path: "Mirage/build/SceneMobileTools/etc2comp-\(architecture)/EtcTool/EtcTool")
        ))
#endif
        for (ffmpeg, etcTool) in candidates {
            if let ffmpeg, let etcTool,
               fileManager.isExecutableFile(atPath: ffmpeg.path),
               fileManager.isExecutableFile(atPath: etcTool.path) {
                return (ffmpeg, etcTool)
            }
        }
        throw SceneMobileExportError.conversionToolsMissing
    }

#if DEBUG
    private static func hostArchitecture() -> String {
        var systemInfo = utsname()
        guard uname(&systemInfo) == 0 else { return "arm64" }
        return withUnsafePointer(to: &systemInfo.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(_SYS_NAMELEN)) {
                String(cString: $0)
            }
        }
    }
#endif

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
        let target = directory.appending(path: name)
        try FileManager.default.createDirectory(
            at: target.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
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

    private static func writeArchive(
        entries: [StagedEntry],
        version: String,
        to outputURL: URL,
        progress: (Double) -> Void
    ) throws {
        guard !entries.isEmpty else { throw SceneMobileExportError.invalidProject }
        var offset: UInt64 = 0
        var table: [(StagedEntry, Data, UInt32)] = []
        for entry in entries {
            let name = Data(entry.name.utf8)
            guard name.count <= 1024,
                  entry.size <= UInt64(UInt32.max),
                  offset + entry.size <= UInt64(UInt32.max) else {
                throw SceneMobileExportError.wallpaperTooLarge
            }
            table.append((entry, name, UInt32(offset)))
            offset += entry.size
        }
        let totalSize = max(offset, 1)
        let destination = outputURL.standardizedFileURL
        try FileManager.default.createDirectory(
            at: destination.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let temporary = destination.deletingLastPathComponent().appending(
            path: ".\(destination.lastPathComponent).\(UUID().uuidString).tmp"
        )
        FileManager.default.createFile(atPath: temporary.path, contents: nil)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let output = try FileHandle(forWritingTo: temporary)
        var written: UInt64 = 0
        do {
            try output.write(contentsOf: littleEndianData(UInt32(version.utf8.count)))
            try output.write(contentsOf: Data(version.utf8))
            try output.write(contentsOf: littleEndianData(UInt32(table.count)))
            for item in table {
                try output.write(contentsOf: littleEndianData(UInt32(item.1.count)))
                try output.write(contentsOf: item.1)
                try output.write(contentsOf: littleEndianData(item.2))
                try output.write(contentsOf: littleEndianData(UInt32(item.0.size)))
            }
            for item in table {
                let input = try FileHandle(forReadingFrom: item.0.url)
                do {
                    try input.seek(toOffset: item.0.offset)
                    var remaining = item.0.size
                    while remaining > 0 {
                        let count = min(copyBufferSize, Int(remaining))
                        let data = try input.read(upToCount: count) ?? Data()
                        guard !data.isEmpty else {
                            throw SceneMobileExportError.truncatedPackage
                        }
                        try output.write(contentsOf: data)
                        written += UInt64(data.count)
                        remaining -= UInt64(data.count)
                        progress(min(Double(written) / Double(totalSize), 1))
                    }
                    try input.close()
                } catch {
                    try? input.close()
                    throw error
                }
            }
            try output.synchronize()
            try output.close()
        } catch {
            try? output.close()
            throw error
        }
        if FileManager.default.fileExists(atPath: destination.path) {
            try FileManager.default.removeItem(at: destination)
        }
        try FileManager.default.moveItem(at: temporary, to: destination)
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
        guard !name.isEmpty,
              !name.contains("\0"),
              !name.contains("\\"),
              !name.hasPrefix("/") else { return false }
        let components = name.split(separator: "/", omittingEmptySubsequences: false)
        return components.allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
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

    private static func littleEndianData(_ value: UInt32) -> Data {
        var value = value.littleEndian
        return withUnsafeBytes(of: &value) { Data($0) }
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
    case scenePresetNotSupported
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
