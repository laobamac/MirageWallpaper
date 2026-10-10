// Copyright © 2026 王孝慈. All rights reserved.
import Foundation

@main
struct MobilePreRenderRegression {
    static func expect(_ condition: @autoclosure () throws -> Bool) throws {
        let result = try condition()
        precondition(result)
    }

    static func main() throws {
        try testAdaptiveDuration()
        let phone = MobileScreenResolution(pairingRequest: ["displayWidth": 1080, "displayHeight": 2400])!
        precondition(phone == MobileScreenResolution(width: 1080, height: 2400))
        for request: [String: Any] in [[:], ["displayWidth": 1080],
            ["displayWidth": true, "displayHeight": 2400],
            ["displayWidth": 1080.5, "displayHeight": 2400],
            ["displayWidth": 0, "displayHeight": 2400],
            ["displayWidth": 1080, "displayHeight": Double.infinity],
            ["displayWidth": 1080, "displayHeight": 20000]] {
            precondition(MobileScreenResolution(pairingRequest: request) == nil)
        }
        let encoder = JSONEncoder(), decoder = JSONDecoder()
        let legacy = Data(#"{"id":"legacy","name":"Phone","model":"Android","isConnected":false}"#.utf8)
        try expect(try decoder.decode(MobileDevice.self, from: legacy).screenResolution == nil)
        let device = MobileDevice(id: "phone", name: "Phone", model: "Android", screenResolution: phone)
        try expect(try decoder.decode(MobileDevice.self, from: encoder.encode(device)) == device)
        let source = MobileScreenResolution(width: 3840, height: 2160)
        var options = SceneMobileExportOptions.PreRendered(screenResolution: phone)
        precondition(options.duration == nil, "Pre-render duration must default to adaptive")
        try expect(try options.outputResolution(source: source) == MobileScreenResolution(width: 864, height: 1920))
        options.preset = .automatic
        try expect(try options.outputResolution(source: source) == phone)
        options.fitPhone = false; options.preset = .fullHD
        try expect(try options.outputResolution(source: source) == MobileScreenResolution(width: 1920, height: 1080))
        options.fitPhone = true; options.screenResolution = nil
        do {
            _ = try options.outputResolution(source: source)
            preconditionFailure("Missing phone resolution accepted")
        } catch MobilePreRenderError.screenResolutionMissing {}
        options.screenResolution = phone; options.fps = 1000
        do {
            _ = try options.outputResolution(source: source)
            preconditionFailure("Invalid frame rate accepted")
        } catch {}
        options.fps = 60; options.alignment = .nan
        do {
            _ = try options.outputResolution(source: source)
            preconditionFailure("Invalid crop alignment accepted")
        } catch {}
        options.alignment = 0.5
        for size in [(1440, 3200), (1201, 2671), (3840, 2160), (2208, 1840)] {
            options.screenResolution = MobileScreenResolution(width: size.0, height: size.1)
            let output = try options.outputResolution(source: source)
            precondition(output.width <= 4096 && output.height <= 4096)
            precondition(output.width.isMultiple(of: 2) && output.height.isMultiple(of: 2))
            precondition(abs(output.aspectRatio - Double(size.0) / Double(size.1)) < 0.005)
        }
        print("Mobile pre-render regression passed: pairing pixels, legacy records, presets, aspect ratios, adaptive cycles, batched sprite metadata and validation")
    }
    static func testAdaptiveDuration() throws {
        func duration(_ loops: [Double], _ oneShots: [Double] = [], speed: Double = 1) -> Double {
            SceneMobileTiming.duration(loops: loops, oneShots: oneShots, fps: 60, speed: speed)
        }
        precondition(duration([]) == 30)
        precondition(duration([2, 3]) == 6)
        precondition(duration([2.5, 3]) == 15)
        precondition(duration([2.5, 3], speed: 2) == 7.5)
        precondition(abs(duration([2.5, 3], speed: 1.1) - 15 / 1.1) <= 1.0 / 120)
        precondition(abs(duration([2.5, 3], speed: 0.9) - 15 / 0.9) <= 1.0 / 120)
        precondition(duration([4, 4.004]) < 5, "Near-identical periods should not create an enormous common cycle")
        precondition(duration([2], [5]) == 6)
        precondition(duration([0.25]) == 1)
        precondition(duration([10000]) == 600)
        precondition(duration([.nan, .infinity, -1]) == 30)
        precondition(duration([599.9, 599.8]) <= 600)
        let scene: [String: Any] = ["objects": [
            ["alpha": ["animation": ["options": ["fps": 15, "length": 30, "mode": "mirror"]]]],
            ["visible": false, "alpha": ["animation": ["options": ["fps": 1, "length": 99, "mode": "loop"]]]],
            ["scale": ["animation": ["options": ["fps": 30, "length": 90, "mode": "loop", "startpaused": true]]]]
        ]]
        let summary = SceneMobileTiming.summary(of: scene)
        precondition(summary.loops == [4] && summary.oneShots.isEmpty)
        let keys = SceneMobileTiming.summary(of: ["alpha": ["animation": [
            "options": ["fps": 30, "length": 30, "mode": "loop"], "c0": [["frame": 90]]]]])
        precondition(keys.loops == [3])
        let particles = SceneMobileTiming.summary(of: ["length": 60, "fps": 20])
        precondition(particles.loops.isEmpty && particles.oneShots.isEmpty)
        let references = SceneMobileTiming.summary(of: ["title": "Sample wallpaper", "color": "0.5 0.7 1.0",
            "textures": ["util/clouds", "util/clouds.tex"], "image": "models/background.json"])
        precondition(references.references == Set(["util/clouds", "util/clouds.tex", "models/background.json"]))
        let camera = SceneMobileTiming.summary(of: ["paths": [
            ["duration": 2, "transforms": [["timestamp": 2]]],
            ["duration": 3, "transforms": [["timestamp": 3]]]]])
        precondition(camera.cameraDuration == 5)

        let stage = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: stage) }
        func uint(_ value: UInt32) -> Data {
            var value = value.littleEndian
            return withUnsafeBytes(of: &value) { Data($0) }
        }
        func texture(body: Data, sprite: Bool) -> Data {
            var data = Data("TEXV0005\0TEXI0001\0".utf8)
            data += uint(0) + uint(sprite ? 4 : 0) + Data(repeating: 0, count: 20)
            data += Data("TEXB0003\0".utf8) + uint(1) + uint(UInt32.max) + uint(1)
            data += uint(1) + uint(1) + uint(0) + uint(UInt32(body.count)) + uint(UInt32(body.count)) + body
            return data
        }
        var sprite = texture(body: Data([0, 0, 0, 255]), sprite: true)
        sprite += Data("TEXS0003\0".utf8) + uint(2) + uint(1) + uint(1)
        for time: Float in [0.25, 0.75] {
            sprite += uint(0) + uint(time.bitPattern) + Data(repeating: 0, count: 24)
        }
        let spriteURL = stage.appendingPathComponent("sprite.tex")
        try (Data(repeating: 42, count: 13) + sprite).write(to: spriteURL)
        let timing = try SceneMobileTiming.textureTiming(.init(url: spriteURL, offset: 13, size: UInt64(sprite.count)), in: stage)
        precondition(timing.period == 1 && timing.video == nil)
        var largeSprite = texture(body: Data([0, 0, 0, 255]), sprite: true)
        largeSprite += Data("TEXS0003\0".utf8) + uint(100_000) + uint(1) + uint(1)
        let frame = uint(0) + uint(Float(0.01).bitPattern) + Data(repeating: 0, count: 24)
        for _ in 0..<100_000 { largeSprite += frame }
        let largeURL = stage.appendingPathComponent("large.tex")
        try largeSprite.write(to: largeURL)
        let largeTiming = try SceneMobileTiming.textureTiming(.init(url: largeURL, offset: 0, size: UInt64(largeSprite.count)), in: stage)
        precondition(abs(largeTiming.period! - 1000) < 0.001)
        let videoBody = uint(32) + Data("ftypisom".utf8) + Data(repeating: 0, count: 20)
        let videoTexture = texture(body: videoBody, sprite: false)
        let videoURL = stage.appendingPathComponent("video.tex")
        try videoTexture.write(to: videoURL)
        let video = try SceneMobileTiming.textureTiming(.init(url: videoURL, offset: 0, size: UInt64(videoTexture.count)), in: stage)
        try expect(try Data(contentsOf: video.video!) == videoBody)
        do {
            _ = try SceneMobileTiming.textureTiming(.init(url: spriteURL, offset: 13, size: 20), in: stage)
            preconditionFailure("Truncated texture accepted")
        } catch {}
    }

}
