//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation
import UniformTypeIdentifiers

final class PresetManager {
    static let shared = PresetManager()

    struct PresetFile: Codable {
        var wallpaperTitle: String
        var runtime: WallpaperRuntimeState
    }

    func exportPreset(for wallpaper: WEWallpaper, runtime: WallpaperRuntimeState) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "\(wallpaper.project.title.isEmpty ? "壁纸" : wallpaper.project.title)-预设.json"
        panel.prompt = L("导出")
        panel.begin { resp in
            guard resp == .OK, let url = panel.url else { return }
            let file = PresetFile(wallpaperTitle: wallpaper.project.title, runtime: runtime)
            if let data = try? JSONEncoder().encode(file) {
                try? data.write(to: url, options: .atomic)
            }
        }
    }

    func importPreset() -> WallpaperRuntimeState? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        panel.prompt = L("导入")
        guard panel.runModal() == .OK, let url = panel.url,
              let data = try? Data(contentsOf: url),
              let file = try? JSONDecoder().decode(PresetFile.self, from: data) else {
            return nil
        }
        return file.runtime
    }
}
