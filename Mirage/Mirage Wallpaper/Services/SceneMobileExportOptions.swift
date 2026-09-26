import Foundation

/// Value copied into each export job so changing the sheet cannot affect a running conversion.
struct SceneMobileExportOptions: Equatable, Sendable {
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
