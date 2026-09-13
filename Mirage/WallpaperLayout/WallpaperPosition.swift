//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CoreGraphics
import Foundation

struct WallpaperPosition: Codable, Equatable, Sendable {
    let x: Double
    let y: Double

    static let center = WallpaperPosition()

    init(x: Double = 0.5, y: Double = 0.5) {
        self.x = x.isFinite ? min(max(x, 0), 1) : 0.5
        self.y = y.isFinite ? min(max(y, 0), 1) : 0.5
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        self.init(x: try values.decodeIfPresent(Double.self, forKey: .x) ?? 0.5,
                  y: try values.decodeIfPresent(Double.self, forKey: .y) ?? 0.5)
    }

    init(dictionary: [String: Any]?) {
        self.init(x: (dictionary?["x"] as? NSNumber)?.doubleValue ?? 0.5,
                  y: (dictionary?["y"] as? NSNumber)?.doubleValue ?? 0.5)
    }

    var dictionary: [String: Double] { ["x": x, "y": y] }
}

struct WallpaperLayout {
    let frame: CGRect
    let canMoveX: Bool
    let canMoveY: Bool

    init(source: CGSize, bounds: CGRect, fillMode: String, position: WallpaperPosition,
         flipped: Bool = false) {
        guard source.width.isFinite, source.height.isFinite,
              bounds.width.isFinite, bounds.height.isFinite,
              source.width > 0, source.height > 0,
              bounds.width > 0, bounds.height > 0,
              fillMode != "stretch" else {
            frame = bounds
            canMoveX = false
            canMoveY = false
            return
        }
        let cover = fillMode != "contain"
        let scale = cover
            ? max(bounds.width / source.width, bounds.height / source.height)
            : min(bounds.width / source.width, bounds.height / source.height)
        let size = CGSize(width: source.width * scale, height: source.height * scale)
        let x = cover ? position.x : 0.5
        let y = cover ? position.y : 0.5
        frame = CGRect(x: bounds.minX + (bounds.width - size.width) * x,
                       y: bounds.minY + (bounds.height - size.height) * (flipped ? y : 1 - y),
                       width: size.width, height: size.height)
        canMoveX = cover && size.width - bounds.width > bounds.width * 0.000001
        canMoveY = cover && size.height - bounds.height > bounds.height * 0.000001
    }
}

struct WallpaperPositionAvailability: Equatable, Sendable {
    var known = false
    var x = false
    var y = false
}

func wallpaperDisplayKey(_ displayID: CGDirectDisplayID) -> String? {
    guard displayID != 0 else { return nil }
    if let cfUUID = CGDisplayCreateUUIDFromDisplayID(displayID),
       let string = CFUUIDCreateString(nil, cfUUID.takeRetainedValue()) as String?,
       !string.isEmpty {
        return "uuid:\(string)"
    }
    let vendor = CGDisplayVendorNumber(displayID)
    let model = CGDisplayModelNumber(displayID)
    let serial = CGDisplaySerialNumber(displayID)
    let unit = CGDisplayUnitNumber(displayID)
    guard vendor != 0 || model != 0 || serial != 0 else { return nil }
    return "vms:\(vendor):\(model):\(serial):\(unit)"
}
