//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa

extension AppDelegate {
    @objc func mute() {
        wallpaperViewModel.playVolume = 0
    }

    @objc func unmute() {
        wallpaperViewModel.playVolume = wallpaperViewModel.lastPlayVolume == 0 ? 1 : wallpaperViewModel.lastPlayVolume
    }

    @objc func pause() {
        wallpaperViewModel.playRate = 0
    }

    @objc func resume() {
        wallpaperViewModel.playRate = wallpaperViewModel.lastPlayRate == 0 ? 1 : wallpaperViewModel.lastPlayRate
    }

    @objc func coverAllScreens() {
        wallpaperViewModel.applyToAllScreens()
    }

    @objc func stopWallpaperMenu() {
        wallpaperViewModel.stopWallpaper()
    }

    @objc func openProjectPage() {
        NSWorkspace.shared.open(URL(string: "https://github.com/laobamac/MirageWallpaper")!)
    }

    @objc func importWallpaperMenu() {
        openImportFromFolderPanel()
    }

    func setStatusMenu() {
        let menu = NSMenu()
        menu.items = [
            .init(title: L("打开 Mirage"), systemImage: "photo",
                  action: #selector(openMainWindow), keyEquivalent: "o"),

            .init(title: L("导入壁纸…"), systemImage: "square.and.arrow.down",
                  action: #selector(importWallpaperMenu), keyEquivalent: "i"),

            .separator(),

            .init(title: L("设置"), systemImage: "gearshape.fill",
                  action: #selector(openSettingsWindow), keyEquivalent: ","),

            .init(title: L("检查更新…"), systemImage: "arrow.triangle.2.circlepath",
                  action: #selector(UpdateManager.checkForUpdates(_:)), keyEquivalent: ""),

            .separator(),

            .init(title: L("项目主页"), systemImage: "globe",
                  action: #selector(openProjectPage), keyEquivalent: ""),

            .separator(),

            .init(title: L("静音"), systemImage: "speaker.slash.fill",
                  action: #selector(mute), keyEquivalent: "m"),

            .init(title: L("暂停"), systemImage: "pause.fill",
                  action: #selector(pause), keyEquivalent: "p"),

            .init(title: L("覆盖到所有显示器"), systemImage: "rectangle.on.rectangle",
                  action: #selector(coverAllScreens), keyEquivalent: ""),

            .init(title: L("停止壁纸"), systemImage: "stop.fill",
                  action: #selector(stopWallpaperMenu), keyEquivalent: ""),

            .separator(),

            .init(title: L("退出 Mirage"), systemImage: "power",
                  action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        ]
        // A status-item menu has no reliable first-responder chain. Explicitly
        // target AppDelegate so right-click menu actions (especially pause) are
        // delivered to the renderer controller instead of being discarded.
        for item in menu.items where item.action != #selector(NSApplication.terminate(_:)) {
            item.target = item.action == #selector(UpdateManager.checkForUpdates(_:))
                ? UpdateManager.shared
                : self
        }

        if self.statusItem == nil {
            self.statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        }
        self.statusItem.menu = menu

        if let button = self.statusItem.button {
            if let icon = NSImage(named: "MenuBarIcon") {
                icon.isTemplate = false
                icon.size = NSSize(width: 18, height: 18)
                button.image = icon
            } else {
                let image = NSImage(systemSymbolName: "photo.on.rectangle.angled", accessibilityDescription: "Mirage")
                button.image = image
            }
        }
    }
}
