//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

@main
struct WallpaperPositionRegression {
    static func main() throws {
        let portrait = CGRect(x: 0, y: 0, width: 1080, height: 1920)
        let source = CGSize(width: 1920, height: 1080)
        for x in [0.0, 0.25, 0.5, 0.75, 1.0] {
            let layout = WallpaperLayout(source: source, bounds: portrait, fillMode: "cover",
                                         position: WallpaperPosition(x: x, y: 1))
            precondition(layout.canMoveX && !layout.canMoveY)
            precondition(abs(layout.frame.minX + (3413.333333333333 - 1080) * x) < 0.000001)
            precondition(abs(layout.frame.height - 1920) < 0.000001)
            precondition(layout.frame.minX <= 0 && layout.frame.maxX >= portrait.maxX - 0.000001)
        }
        let landscape = CGRect(x: 0, y: 0, width: 1920, height: 1080)
        for flipped in [false, true] {
            for y in [0.0, 0.5, 1.0] {
                let layout = WallpaperLayout(source: portrait.size, bounds: landscape, fillMode: "cover",
                                             position: WallpaperPosition(y: y), flipped: flipped)
                precondition(!layout.canMoveX && layout.canMoveY)
                let expected = (1080 - 3413.333333333333) * (flipped ? y : 1 - y)
                precondition(abs(layout.frame.minY - expected) < 0.000001)
            }
        }
        for mode in ["contain", "stretch"] {
            let a = WallpaperLayout(source: source, bounds: portrait, fillMode: mode,
                                    position: WallpaperPosition(x: 0, y: 0))
            let b = WallpaperLayout(source: source, bounds: portrait, fillMode: mode,
                                    position: WallpaperPosition(x: 1, y: 1))
            precondition(a.frame == b.frame && !a.canMoveX && !a.canMoveY)
        }
        let empty = WallpaperLayout(source: .zero, bounds: portrait, fillMode: "cover", position: .center)
        precondition(empty.frame == portrait && !empty.canMoveX && !empty.canMoveY)
        let retina = WallpaperLayout(source: source, bounds: CGRect(x: 0, y: 0, width: 2160, height: 3840),
                                     fillMode: "cover", position: WallpaperPosition(x: 0.2))
        let logical = WallpaperLayout(source: source, bounds: portrait, fillMode: "cover",
                                      position: WallpaperPosition(x: 0.2))
        precondition(abs(retina.frame.minX - logical.frame.minX * 2) < 0.000001)
        let decoder = JSONDecoder()
        let legacy = try decoder.decode(WallpaperPosition.self, from: Data("{}".utf8))
        precondition(legacy == .center)
        let clamped = try decoder.decode(WallpaperPosition.self, from: Data("{\"x\":-4,\"y\":9}".utf8))
        precondition(clamped == WallpaperPosition(x: 0, y: 1))
        precondition(WallpaperPosition(x: .nan, y: .infinity) == .center)
        let saved = ["display-a": ["wallpaper": WallpaperPosition(x: 0)],
                     "display-b": ["wallpaper": WallpaperPosition(x: 1)]]
        let restored = try decoder.decode([String: [String: WallpaperPosition]].self,
                                           from: JSONEncoder().encode(saved))
        precondition(saved == restored)
        print("WallpaperPositionRegression: ok")
    }
}
