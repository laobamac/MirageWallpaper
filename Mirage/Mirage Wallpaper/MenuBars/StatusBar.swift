//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa

extension AppDelegate: NSMenuDelegate, NSMenuItemValidation {
    @objc func mute() {
        wallpaperViewModel.muteAll()
    }

    @objc func unmute() {
        wallpaperViewModel.unmuteAll()
    }

    @objc func pause() {
        wallpaperViewModel.pauseAll()
    }

    @objc func resume() {
        wallpaperViewModel.resumeAll()
    }

    @objc func previousWallpaper(_ sender: NSMenuItem) {
        advancePlaylist(.previous, sender: sender)
    }

    @objc func nextWallpaper(_ sender: NSMenuItem) {
        advancePlaylist(.next, sender: sender)
    }

    private func advancePlaylist(_ direction: PlaylistDirection, sender: NSMenuItem) {
        guard let rawKey = sender.representedObject as? String else { return }
        PlaylistManager.shared.advance(direction, on: DisplayKey(rawValue: rawKey),
                                       library: contentViewModel.wallpapers)
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        let direction: PlaylistDirection
        switch menuItem.action {
        case #selector(previousWallpaper(_:)): direction = .previous
        case #selector(nextWallpaper(_:)): direction = .next
        default: return true
        }
        guard let rawKey = menuItem.representedObject as? String else { return false }
        return PlaylistManager.shared.canAdvance(direction, on: DisplayKey(rawValue: rawKey),
                                                  library: contentViewModel.wallpapers)
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        guard menu === statusItem?.menu else { return }
        let displays = wallpaperViewModel.connectedDisplays
        for direction in [PlaylistDirection.previous, .next] {
            let identifier = NSUserInterfaceItemIdentifier(
                direction == .previous ? "Mirage.Playlist.previous" : "Mirage.Playlist.next")
            guard let item = menu.items.first(where: { $0.identifier == identifier }) else { continue }
            let action = direction == .previous ? #selector(previousWallpaper(_:)) : #selector(nextWallpaper(_:))
            if displays.count <= 1 {
                item.submenu = nil
                item.action = action
                item.target = self
                item.representedObject = displays.first?.key.rawValue
                item.isEnabled = validateMenuItem(item)
            } else {
                item.action = nil
                item.representedObject = nil
                let submenu = NSMenu(title: item.title)
                for display in displays {
                    let child = NSMenuItem(title: L("显示器 %d", display.index + 1) + " · " + display.name,
                                           action: action, keyEquivalent: "")
                    child.target = self
                    child.representedObject = display.key.rawValue
                    child.isEnabled = validateMenuItem(child)
                    submenu.addItem(child)
                }
                item.submenu = submenu
                item.isEnabled = submenu.items.contains(where: \.isEnabled)
            }
        }
        wallpaperViewModel.syncStatusItems()
    }

    @objc func coverAllScreens() {
        wallpaperViewModel.applyToAllScreens()
    }

    @objc func stopWallpaperMenu() {
        wallpaperViewModel.stopAllWallpapers()
    }

    @objc func openProjectPage() {
        NSWorkspace.shared.open(URL(string: "https://github.com/laobamac/MirageWallpaper")!)
    }

    @objc func importWallpaperMenu() {
        openImportFromFolderPanel()
    }

    func setStatusMenu() {
        let menu = NSMenu()
        menu.delegate = self
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

            .init(title: L("上一张"), systemImage: "backward.end.fill",
                  action: #selector(previousWallpaper(_:)), keyEquivalent: ""),

            .init(title: L("下一张"), systemImage: "forward.end.fill",
                  action: #selector(nextWallpaper(_:)), keyEquivalent: ""),

            .init(title: L("覆盖到所有显示器"), systemImage: "rectangle.on.rectangle",
                  action: #selector(coverAllScreens), keyEquivalent: ""),

            .init(title: L("停止壁纸"), systemImage: "stop.fill",
                  action: #selector(stopWallpaperMenu), keyEquivalent: ""),

            .separator(),

            .init(title: L("退出 Mirage"), systemImage: "power",
                  action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        ]
        for item in menu.items {
            if item.action == #selector(previousWallpaper(_:)) {
                item.identifier = NSUserInterfaceItemIdentifier("Mirage.Playlist.previous")
            } else if item.action == #selector(nextWallpaper(_:)) {
                item.identifier = NSUserInterfaceItemIdentifier("Mirage.Playlist.next")
            }
        }
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
        self.statusItem.isVisible = !globalSettingsViewModel.settings.shouldHideMenuBarIcon

        applyStatusItemIcon(
            monochrome: globalSettingsViewModel.settings.shouldUseMonochromeMenuBarIcon)
    }

    func applyStatusItemIcon(monochrome: Bool) {
        guard let button = statusItem?.button else { return }
        let resourceName = monochrome ? "MenuBarIconMonochrome" : "MenuBarIcon"
        if let source = NSImage(named: resourceName),
           let icon = source.copy() as? NSImage {
            icon.isTemplate = monochrome
            icon.size = NSSize(width: 18, height: 18)
            button.image = icon
        } else {
            button.image = NSImage(
                systemSymbolName: "photo.on.rectangle.angled",
                accessibilityDescription: "Mirage")
        }
    }
}
