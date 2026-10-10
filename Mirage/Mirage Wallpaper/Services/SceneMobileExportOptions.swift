//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import CoreFoundation

struct MobileScreenResolution: Codable, Equatable, Sendable {
    let width: Int
    let height: Int

    var isValid: Bool { (128...16384).contains(width) && (128...16384).contains(height) }
    var aspectRatio: Double { Double(width) / Double(height) }

    init(width: Int, height: Int) { self.width = width; self.height = height }

    init?(pairingRequest: [String: Any]) {
        // Wallpaper Engine Android's EncAuthRequest supplies physical display pixels.
        guard let width = pairingRequest["displayWidth"] as? NSNumber,
              let height = pairingRequest["displayHeight"] as? NSNumber,
              CFGetTypeID(width) != CFBooleanGetTypeID(), CFGetTypeID(height) != CFBooleanGetTypeID(),
              width.doubleValue.isFinite, height.doubleValue.isFinite,
              (128...16384).contains(width.doubleValue), (128...16384).contains(height.doubleValue),
              width.doubleValue.rounded() == width.doubleValue,
              height.doubleValue.rounded() == height.doubleValue else { return nil }
        self.init(width: width.intValue, height: height.intValue)
    }
}

/// Value copied into each export job so changing the sheet cannot affect a running conversion.
struct SceneMobileExportOptions: Equatable, Sendable {
    enum VideoPreset: String, CaseIterable, Identifiable, Sendable {
        case automatic, fullHD, ultraHD
        var id: String { rawValue }
        var title: String {
            switch self {
            case .automatic: return L("自动")
            case .fullHD: return L("全高清")
            case .ultraHD: return "4K UHD"
            }
        }
    }

    struct PreRendered: Equatable, Sendable {
        var fitPhone = true
        var screenResolution: MobileScreenResolution?
        var preset: VideoPreset = .fullHD
        var fps = 60
        var alignment = 0.5
        var duration: Int? = nil

        func outputResolution(source: MobileScreenResolution) throws -> MobileScreenResolution {
            guard source.isValid, [24, 30, 60].contains(fps), alignment.isFinite,
                  (0...1).contains(alignment), duration.map({ (1...600).contains($0) }) ?? true else {
                throw WallpaperBakeError.code("invalid_request")
            }
            let target: MobileScreenResolution
            if fitPhone {
                guard let screenResolution, screenResolution.isValid else {
                    throw MobilePreRenderError.screenResolutionMissing
                }
                target = screenResolution
            } else { target = source }
            let limit = preset == .fullHD ? 1920 : preset == .ultraHD ? 3840 : 4096
            let scale = min(1, Double(limit) / Double(max(target.width, target.height)))
            // H.264 requires even dimensions; never change the selected aspect ratio by clamping each axis.
            let result = MobileScreenResolution(width: Int(Double(target.width) * scale) / 2 * 2,
                                                height: Int(Double(target.height) * scale) / 2 * 2)
            guard result.isValid else { throw WallpaperBakeError.code("invalid_request") }
            return result
        }
    }

    enum TextureReduction: Int, CaseIterable, Identifiable, Sendable {
        case original = 1
        case half = 2
        case quarter = 4

        var id: Int { rawValue }
        var title: String {
            switch self {
            case .original: return L("最高质量 -（不降低纹理分辨率）")
            case .half: return L("更佳性能 -（将纹理分辨率降低至原来的一半）")
            case .quarter: return L("高性能 -（将纹理分辨率降低至原来的四分之一）")
            }
        }
    }

    var textureReduction: TextureReduction = .original
    var pixelArtOptimization = false
    var preRendered: PreRendered?

    // The supplied mobile exports use half resolution for High Quality and
    // quarter resolution for Balanced; original resolution remains an advanced option.
    static let highQuality = Self(textureReduction: .half)
    static let balanced = Self(textureReduction: .quarter)

    func reductionFactor(width: Int, height: Int) -> Int {
        // Preserve small details such as cursors, text, particles and lookup textures.
        min(width, height) > 128 ? textureReduction.rawValue : 1
    }

    func sceneData(_ source: Data) throws -> Data {
        guard var scene = try JSONSerialization.jsonObject(with: source) as? [String: Any] else {
            throw SceneMobileExportError.invalidProject
        }
        var general = scene["general"] as? [String: Any] ?? [:]
        if textureReduction == .original {
            guard general.removeValue(forKey: "texturereduction") != nil else { return source }
        } else {
            general["texturereduction"] = textureReduction.rawValue
        }
        scene["general"] = general
        return try JSONSerialization.data(withJSONObject: scene, options: [.sortedKeys, .withoutEscapingSlashes])
    }
}

enum MobilePreRenderError: LocalizedError {
    case screenResolutionMissing
    var errorDescription: String? {
        L("尚未获取手机屏幕分辨率，请重新连接手机，或选择保持原始宽高比。")
    }
}
