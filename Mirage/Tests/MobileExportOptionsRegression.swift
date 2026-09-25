import AppKit
import Compression
import SwiftUI
@testable import Mirage_Wallpaper

private struct Failure: Error { let message: String }
private func require(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !value() { throw Failure(message: message) }
}
private func word(_ value: Int) -> Data {
    var v = UInt32(truncatingIfNeeded: value).littleEndian
    return withUnsafeBytes(of: &v) { Data($0) }
}
private func stamp(_ value: String) -> Data { Data((value + "\0").utf8) }
private func archive(_ entries: [(String, Data)]) -> Data {
    var out = word(8) + Data("PKGV0024".utf8) + word(entries.count)
    var offset = 0
    for (name, bytes) in entries {
        out += word(name.utf8.count) + Data(name.utf8) + word(offset) + word(bytes.count)
        offset += bytes.count
    }
    for (_, bytes) in entries { out += bytes }
    return out
}
private struct Reader {
    let data: Data
    var offset = 0
    mutating func u() -> Int {
        defer { offset += 4 }
        return Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self).littleEndian })
    }
    mutating func bytes(_ count: Int) -> Data {
        defer { offset += count }
        return data.subdata(in: offset..<(offset + count))
    }
}
private func unpack(_ data: Data) -> [String: Data] {
    var r = Reader(data: data)
    _ = r.bytes(r.u())
    let count = r.u()
    var entries: [(String, Int, Int)] = []
    for _ in 0..<count {
        let name = String(data: r.bytes(r.u()), encoding: .utf8)!
        entries.append((name, r.u(), r.u()))
    }
    return Dictionary(uniqueKeysWithValues: entries.map { ($0.0, data.subdata(in: (r.offset + $0.1)..<(r.offset + $0.1 + $0.2))) })
}
private struct Texture {
    let format: Int
    let header: [Int]
    let width: Int
    let height: Int
    let rawCount: Int
    let payload: Data
    let sprite: Data
    init(_ data: Data) {
        var r = Reader(data: data, offset: 18)
        format = r.u()
        _ = r.u()
        header = (0..<4).map { _ in r.u() }
        _ = r.u()
        _ = r.bytes(9)
        _ = r.u(); _ = r.u(); _ = r.u(); _ = r.u()
        width = r.u(); height = r.u()
        let compressed = r.u()
        rawCount = r.u()
        let bytes = r.bytes(r.u())
        if compressed == 1 {
            var output = Data(count: rawCount)
            let rawSize = rawCount
            let decoded = output.withUnsafeMutableBytes { dst in
                bytes.withUnsafeBytes { src in
                    compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, rawSize,
                        src.bindMemory(to: UInt8.self).baseAddress!, bytes.count, nil, COMPRESSION_LZ4_RAW)
                }
            }
            precondition(decoded == rawSize)
            payload = output
        } else { payload = bytes }
        sprite = r.bytes(data.count - r.offset)
    }
}
private func texture(width: Int, height: Int, format: Int = 0, flags: Int = 2, sprite: Bool = false) -> Data {
    let channels = format == 9 ? 1 : format == 8 ? 2 : 4
    var pixels = Data(count: width * height * channels)
    pixels.withUnsafeMutableBytes { (bytes: UnsafeMutableRawBufferPointer) in
        for i in 0..<bytes.count { bytes[i] = UInt8(truncatingIfNeeded: i / channels) }
    }
    var out = stamp("TEXV0005") + stamp("TEXI0001")
    for value in [format, flags | (sprite ? 4 : 0), width, height, width, height, 0] { out += word(value) }
    out += stamp("TEXB0004")
    for value in [1, -1, 0, 1, width, height, 0, pixels.count, pixels.count] { out += word(value) }
    out += pixels
    if sprite {
        out += stamp("TEXS0003") + word(1) + word(50) + word(50) + word(0)
        for value: Float in [1, 0, 0, 50, 0, 0, 50] { out += word(Int(value.bitPattern)) }
    }
    return out
}

@main
struct MobileExportOptionsRegression {
    @MainActor
    static func main() throws {
        if let index = CommandLine.arguments.firstIndex(of: "--export") {
            let arguments = CommandLine.arguments
            let wallpaper = WEWallpaper.load(from: URL(fileURLWithPath: arguments[index + 1]))
            let output = URL(fileURLWithPath: arguments[index + 2])
            let reduction = SceneMobileExportOptions.TextureReduction(rawValue: Int(arguments[index + 3])!)!
            try SceneMobileMPKGExporter.export(wallpaper, to: output, options: .init(
                textureReduction: reduction, pixelArtOptimization: arguments[index + 4] == "true"))
            return
        }
        if CommandLine.arguments.contains("--preview") || Bundle.main.bundleIdentifier == "cn.laobamac.Mirage.MobileOptionsReview" {
            let app = NSApplication.shared
            app.setActivationPolicy(.regular)
            MirageLocalization.shared.apply(.zh_CN)
            let wallpaper = WEWallpaper(using: WEProject(file: "scene.json", preview: "preview.png",
                title: "Kamisato Ayaka Maid Animated X-Ray | Genshin Impact", type: "scene"),
                where: URL(fileURLWithPath: "/tmp/MirageOptionsPreview"))
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 500),
                                  styleMask: [.titled, .closable], backing: .buffered, defer: false)
            window.title = "Mirage Scene Export Options"
            window.contentView = NSHostingView(rootView: SceneMobileExportOptionsView(
                request: SceneMobileExportRequest(wallpaper: wallpaper, destination: .file)
            ) { options in print("Confirmed:", options); app.terminate(nil) })
            window.center()
            window.makeKeyAndOrderFront(nil)
            app.activate(ignoringOtherApps: true)
            app.run()
            return
        }
        let root = FileManager.default.temporaryDirectory.appending(path: "mirage-mobile-options-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let project = WEProject(file: "scenes/main.json", preview: "preview.png", title: "Options regression", type: "scene")
        let wallpaper = WEWallpaper(using: project, where: root)
        try JSONEncoder().encode(project).write(to: root.appending(path: "project.json"))
        try Data([1,2,3]).write(to: root.appending(path: "preview.png"))
        let scene = Data(#"{"general":{"custom":17},"objects":[{"id":1,"size":"260 132"}]}"#.utf8)
        let color = texture(width: 260, height: 132)
        let small = texture(width: 128, height: 256)
        let mask = texture(width: 260, height: 132, format: 9)
        let sprite = texture(width: 450, height: 400, sprite: true)
        let source = archive([("scenes/main.json", scene), ("materials/color.tex", color),
                              ("materials/small.tex", small), ("materials/mask.tex", mask),
                              ("materials/workshop/fixture/sprite.tex", sprite)])
        try source.write(to: root.appending(path: "scene.pkg"))
        try require(SceneMobileExportOptions.highQuality.textureReduction == .half, "High-quality preset mapping")
        try require(SceneMobileExportOptions.balanced.textureReduction == .quarter, "Balanced preset mapping")
        try require(try SceneMobileExportOptions().sceneData(scene) == scene, "Original preserves source JSON")
        for reduction in SceneMobileExportOptions.TextureReduction.allCases {
            for pixel in [false, true] {
                let options = SceneMobileExportOptions(textureReduction: reduction, pixelArtOptimization: pixel)
                let output = root.appending(path: "result.mpkg")
                var progress: [Double] = []
                try SceneMobileMPKGExporter.export(wallpaper, to: output, options: options) { progress.append($0) }
                let entries = unpack(try Data(contentsOf: output))
                let converted = Texture(entries["materials/color.tex"]!)
                let factor = reduction.rawValue
                let w = 260 / factor, h = 132 / factor
                let paddedW = pixel ? w : (w + 3) / 4 * 4
                let paddedH = pixel ? h : (h + 3) / 4 * 4
                try require(converted.format == (pixel ? 0 : 5), "Pixel-art encoding format")
                try require(converted.width == paddedW && converted.height == paddedH, "Requested texture dimensions")
                try require(converted.header[0] == Int((260.0 * Double(paddedW) / Double(w)).rounded()), "Logical backing width accounts for ETC padding")
                try require(converted.header[2...3] == [260, 132], "Map dimensions preserve scene layout")
                let smallResult = Texture(entries["materials/small.tex"]!)
                try require(smallResult.width == 128 && smallResult.height == 256, "Small texture stays intact")
                try require(entries["materials/mask.tex"] == mask, "Mask remains byte-identical")
                let spriteResult = Texture(entries["materials/workshop/fixture/sprite.tex"]!)
                var spriteReader = Reader(data: spriteResult.sprite, offset: 9 + 4 + 8 + 4 + 4 + 8)
                let frameWidth = Float(bitPattern: UInt32(spriteReader.u()))
                try require(frameWidth == 50 / Float(factor), "Sprite coordinates use the requested ratio")
                let convertedScene = try JSONSerialization.jsonObject(with: entries["scenes/main.json"]!) as! [String: Any]
                let general = convertedScene["general"] as! [String: Any]
                try require((general["texturereduction"] as? Int ?? 1) == factor, "Scene reduction metadata")
                try require(general["custom"] as? Int == 17, "Preserve unrelated scene values")
                try require(progress.first == 0 && progress.last == 1 && zip(progress, progress.dropFirst()).allSatisfy { $0 <= $1 }, "Monotonic progress")
                try require(try Data(contentsOf: root.appending(path: "scene.pkg")) == source, "Source wallpaper remains intact")
                print("PASS: reduction=\(factor), pixelArt=\(pixel)")
            }
        }
        // A malformed scene must fail without replacing the previous successful export.
        let destination = root.appending(path: "result.mpkg")
        let previous = try Data(contentsOf: destination)
        try archive([("scenes/main.json", Data("[]".utf8))]).write(to: root.appending(path: "scene.pkg"))
        do {
            try SceneMobileMPKGExporter.export(wallpaper, to: destination, options: .balanced)
            throw Failure(message: "Malformed scene accepted")
        } catch is SceneMobileExportError {}
        try require(try Data(contentsOf: destination) == previous, "Failed export preserves destination")
        print("PASS: malformed scene rejection and atomic destination preservation")
    }
}
