//
//  WallpaperColorAdjustment.swift
//  Mirage Wallpaper
//
//  Created by Codex on 2026/11/25.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import AVFoundation
import CoreImage

struct WallpaperColorAdjustment: Codable, Equatable, Sendable {
    var brightness: Float = 1
    var contrast: Float = 1
    var saturation: Float = 1

    init(brightness: Float = 1, contrast: Float = 1, saturation: Float = 1) {
        self.brightness = Self.validated(brightness)
        self.contrast = Self.validated(contrast)
        self.saturation = Self.validated(saturation)
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        self.init(brightness: try values.decodeIfPresent(Float.self, forKey: .brightness) ?? 1,
                  contrast: try values.decodeIfPresent(Float.self, forKey: .contrast) ?? 1,
                  saturation: try values.decodeIfPresent(Float.self, forKey: .saturation) ?? 1)
    }

    init(dictionary: [String: Any]?) {
        self.init(brightness: (dictionary?["brightness"] as? NSNumber)?.floatValue ?? 1,
                  contrast: (dictionary?["contrast"] as? NSNumber)?.floatValue ?? 1,
                  saturation: (dictionary?["saturation"] as? NSNumber)?.floatValue ?? 1)
    }

    var normalized: Self { Self(brightness: brightness, contrast: contrast, saturation: saturation) }
    var isIdentity: Bool { normalized == Self() }
    var dictionary: [String: Any] {
        let value = normalized
        return ["brightness": value.brightness, "contrast": value.contrast, "saturation": value.saturation]
    }

    private static func validated(_ value: Float) -> Float {
        value.isFinite ? min(2, max(0, value)) : 1
    }
}

enum WallpaperVideoColor {
    private static let context = CIContext(options: [
        .workingColorSpace: CGColorSpace(name: CGColorSpace.extendedSRGB)!,
        .workingFormat: CIFormat.RGBAh,
        .cacheIntermediates: false
    ])

    static func apply(_ adjustment: WallpaperColorAdjustment, to item: AVPlayerItem,
                      hdr: Bool, completion: @escaping (Bool) -> Void) {
        let value = adjustment.normalized
        guard !value.isIdentity else { completion(true); return }
        AVVideoComposition.videoComposition(with: item.asset, applyingCIFiltersWithHandler: { request in
            let s = CGFloat(value.saturation)
            var image = request.sourceImage.applyingFilter("CIColorMatrix", parameters: [
                "inputRVector": CIVector(x: 0.213 + 0.787 * s, y: 0.715 - 0.715 * s, z: 0.072 - 0.072 * s, w: 0),
                "inputGVector": CIVector(x: 0.213 - 0.213 * s, y: 0.715 + 0.285 * s, z: 0.072 - 0.072 * s, w: 0),
                "inputBVector": CIVector(x: 0.213 - 0.213 * s, y: 0.715 - 0.715 * s, z: 0.072 + 0.928 * s, w: 0)
            ])
            if !hdr { image = clamp(image) }
            let c = CGFloat(value.contrast)
            image = image.applyingFilter("CIColorMatrix", parameters: [
                "inputRVector": CIVector(x: c, y: 0, z: 0, w: 0),
                "inputGVector": CIVector(x: 0, y: c, z: 0, w: 0),
                "inputBVector": CIVector(x: 0, y: 0, z: c, w: 0),
                "inputBiasVector": CIVector(x: 0.5 * (1 - c), y: 0.5 * (1 - c), z: 0.5 * (1 - c), w: 0)
            ])
            if !hdr { image = clamp(image) }
            let b = CGFloat(value.brightness)
            image = image.applyingFilter("CIColorMatrix", parameters: [
                "inputRVector": CIVector(x: b, y: 0, z: 0, w: 0),
                "inputGVector": CIVector(x: 0, y: b, z: 0, w: 0),
                "inputBVector": CIVector(x: 0, y: 0, z: b, w: 0)
            ])
            if !hdr { image = clamp(image) }
            request.finish(with: image.cropped(to: request.sourceImage.extent), context: context)
        }, completionHandler: { composition, error in
            DispatchQueue.main.async {
                guard let composition, error == nil else { completion(false); return }
                item.videoComposition = composition
                completion(true)
            }
        })
    }

    private static func clamp(_ image: CIImage) -> CIImage {
        image.applyingFilter("CIColorClamp", parameters: [
            "inputMinComponents": CIVector(x: 0, y: 0, z: 0, w: 0),
            "inputMaxComponents": CIVector(x: 1, y: 1, z: 1, w: 1)
        ])
    }
}
