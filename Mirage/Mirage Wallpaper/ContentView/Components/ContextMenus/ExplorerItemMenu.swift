//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AppKit

struct ExplorerItemMenu: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let creatorStore: WorkshopCreatorStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator

    var hoveredWallpaper: WEWallpaper

    init(contentViewModel viewModel: ContentViewModel,
         wallpaperViewModel: WallpaperViewModel,
         creatorStore: WorkshopCreatorStore,
         interactionStore: WorkshopInteractionStore,
         libraryStore: WorkshopLibraryStore,
         selectionCoordinator: WorkshopSelectionCoordinator,
         current hoveredWallpaper: WEWallpaper) {
        self.wallpaperViewModel = wallpaperViewModel
        self.viewModel = viewModel
        self.creatorStore = creatorStore
        self.interactionStore = interactionStore
        self.libraryStore = libraryStore
        self.selectionCoordinator = selectionCoordinator
        self.hoveredWallpaper = hoveredWallpaper
    }
    
    var body: some View {
        Group {
            Section {
                if displays.count > 1 {
                    Menu {
                        ForEach(displays) { info in
                            Button {
                                apply(to: info)
                            } label: {
                                Label(displayTitle(info), systemImage: "display")
                            }
                        }
                        Divider()
                        Button {
                            applyToAll()
                        } label: {
                            Label("所有显示器", systemImage: "rectangle.on.rectangle")
                        }
                    } label: {
                        Label("设为壁纸", systemImage: "photo.fill")
                    }
                    .disabled(!canApply)
                } else {
                    Button {
                        if let info = displays.first { apply(to: info) }
                    } label: {
                        Label("设为壁纸", systemImage: "photo.fill")
                    }
                    .disabled(!canApply || displays.isEmpty)
                }

                Button(action: setAsScreenSaver) {
                    Label("设为屏保", systemImage: "sparkles.tv")
                }
                .disabled(!canApply || (hoveredWallpaper.kind != .video && hoveredWallpaper.kind != .scene))

                Button(action: setAsDynamicLockScreen) {
                    Label("设为动态锁屏", systemImage: "lock.rectangle")
                }
                .disabled(!canApply || (hoveredWallpaper.kind != .video && hoveredWallpaper.kind != .scene))
            }

            Section {
                if displays.count > 1 {
                    Menu {
                        ForEach(displays) { info in
                            Button {
                                PlaylistManager.shared.add(hoveredWallpaper, to: info.index)
                            } label: {
                                Label(displayTitle(info), systemImage: "display")
                            }
                        }
                    } label: {
                        Label("加入播放列表", systemImage: "plus")
                    }
                    .disabled(!hoveredWallpaper.isValid)
                } else {
                    Button {
                        PlaylistManager.shared.add(hoveredWallpaper, to: 0)
                    } label: {
                        Label("加入播放列表", systemImage: "plus")
                    }
                    .disabled(!hoveredWallpaper.isValid)
                }
                Button {
                    viewModel.hoveredWallpaper = hoveredWallpaper
                    viewModel.isUnsubscribeConfirming = true
                } label: {
                    Label("删除壁纸", systemImage: "xmark")
                }
                Button {
                    toggleFavorite()
                } label: {
                    Label(
                        LocalizedStringKey(isFavorite ? "取消收藏" : "加入收藏"),
                        systemImage: isFavorite ? "heart.slash.fill" : "heart.fill"
                    )
                }
                .disabled(workshopID.map {
                    interactionStore.changingFavoriteIDs.contains($0)
                } == true)
            }
            
            Section {
                Button {
                    openInWorkshop()
                } label: {
                    Label("在创意工坊中打开", systemImage: "cloud.fill")
                }
                .disabled(workshopURL == nil)
                if let creator = libraryStore.installedCreator(
                    for: hoveredWallpaper
                ) {
                    Button {
                        creatorStore.open(creator)
                    } label: {
                        Label(LocalizedStringKey("查看作者主页和作品"), systemImage: "person.crop.circle")
                    }
                }
                Menu("相关壁纸") {
                    Link(destination: URL(string: "https://github.com/laobamac/MirageWallpaper")!) {
                        Label("浏览该作者全部", systemImage: "person.fill")
                    }
                    Link(destination: URL(string: "https://github.com/laobamac/MirageWallpaper")!) {
                        Label("浏览预设", systemImage: "cloud.fill")
                    }
                }.disabled(true)
                Menu("举报与屏蔽") {
                    Button(role: .destructive) {
                        
                    } label: {
                        Label("举报", systemImage: "exclamationmark.triangle.fill")
                    }
                    Button {
                        
                    } label: {
                        Label("管理屏蔽列表", systemImage: "hand.raised.fill")
                    }
                }.disabled(true)
            }
            
            Section {
                Button {
                    WallpaperShortcutManager.shared.presentRecorder(for: hoveredWallpaper)
                } label: {
                    Label(
                        LocalizedStringKey(hasShortcut ? "更改快捷键" : "设置快捷键"),
                        systemImage: "command.square"
                    )
                }
                if hasShortcut {
                    Button {
                        WallpaperShortcutManager.shared.clearShortcut(for: hoveredWallpaper)
                    } label: {
                        Label(LocalizedStringKey("移除快捷键"), systemImage: "command.square.fill")
                    }
                }
                Button {
                    NSWorkspace.shared.selectFile(nil,
                                                  inFileViewerRootedAtPath: hoveredWallpaper.wallpaperDirectory.path(percentEncoded: false))
                } label: {
                    Label("在访达中显示", systemImage: "folder.badge.gearshape")
                }
            }
        }
        .labelStyle(.titleAndIcon)
    }

    private var displays: [DisplayInfo] {
        wallpaperViewModel.connectedDisplays
    }

    private var canApply: Bool {
        hoveredWallpaper.isValid && hoveredWallpaper.kind != .unsupported
    }

    private var isFavorite: Bool {
        if let workshopID {
            return interactionStore.isFavorite(workshopID)
        }
        return FavoritesManager.shared.isFavorite(hoveredWallpaper.id)
    }

    private var workshopID: String? {
        hoveredWallpaper.steamFavoriteWorkshopID()
    }

    private var workshopURL: URL? {
        hoveredWallpaper.verifiedWorkshopURL()
    }

    private var hasShortcut: Bool {
        WallpaperShortcutManager.shared.shortcut(for: hoveredWallpaper) != nil
    }

    private func displayTitle(_ info: DisplayInfo) -> String {
        var text = L("显示器 %d", info.index + 1)
        text += " · " + info.name
        if info.isMain { text += L(" · 主屏") }
        return text
    }

    private func apply(to info: DisplayInfo) {
        wallpaperViewModel.requestApply(hoveredWallpaper, to: info.key)
    }

    private func openInWorkshop() {
        guard let id = hoveredWallpaper.verifiedWorkshopID() else { return }
        Task { @MainActor in
            let fetched = try? await SteamWebAPI.shared.getFileDetails(workshopIds: [id])
            let item = libraryStore.installedWorkshopItems[id]
                ?? fetched?.first(where: {
                    $0.publishedFileId == id && $0.consumerAppId == 431960
                })
            guard let item else {
                if let workshopURL {
                    NSWorkspace.shared.open(workshopURL)
                }
                return
            }
            selectionCoordinator.showDetail(item)
            AppDelegate.shared.navigationModel.selection = .workshop
        }
    }

    private func toggleFavorite() {
        if let workshopID {
            interactionStore.toggleFavorite(workshopID: workshopID)
        } else {
            FavoritesManager.shared.toggle(hoveredWallpaper.id)
            NotificationCenter.default.post(name: .favoritesChanged, object: nil)
        }
    }

    private func applyToAll() {
        if hoveredWallpaper.kind == .web, !wallpaperViewModel.isTrusted(hoveredWallpaper) {
            viewModel.warningUnsafeWallpaperModal(which: hoveredWallpaper,
                                                  action: .applyToAllDisplays)
            return
        }
        wallpaperViewModel.applyToAllDisplays(hoveredWallpaper)
    }

    private func setAsScreenSaver() {
        let wallpaper = hoveredWallpaper
        let runtime = wallpaperViewModel.loadRuntime(for: wallpaper)
        let properties = wallpaperViewModel.effectiveProperties(for: wallpaper, runtime: runtime)
        let fps = Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
        let manager = ScreenSaverManager.shared
        let needsInstallation = !manager.isInstalled

        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result {
                if needsInstallation { try manager.install() }
                try manager.configure(
                    with: wallpaper,
                    runtime: runtime,
                    properties: properties,
                    fps: fps
                )
            }
            DispatchQueue.main.async {
                switch result {
                case .success:
                    viewModel.screenSaverFeedback = ScreenSaverFeedback(
                        title: "已设为屏保",
                        message: "“\(wallpaper.project.title)”将在下次启动屏保时显示。"
                    )
                case .failure(let error):
                    viewModel.screenSaverFeedback = ScreenSaverFeedback(
                        title: "设置屏保失败",
                        message: error.localizedDescription
                    )
                }
            }
        }
    }

    private func setAsDynamicLockScreen() {
        if DynamicLockScreenModeStore.active == .screenSaver || !DynamicLockScreenManager.shared.isAvailable {
            setAsScreenSaverDynamicLockScreen()
            return
        }
        let manager = DynamicLockScreenManager.shared
        guard manager.isAvailable else {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("动态锁屏不可用"),
                message: L("动态锁屏需要 macOS 26 或更高版本。")
            )
            return
        }
        guard manager.canUse else {
            manager.requestEnable()
            return
        }
        guard hoveredWallpaper.kind == .video || hoveredWallpaper.kind == .scene else {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("设置动态锁屏失败"),
                message: L("当前壁纸不能用作动态锁屏")
            )
            return
        }
        let displayIDs = displays.compactMap { DisplayRegistry.shared.displayID(for: $0.key) }
        let runtime = wallpaperViewModel.loadRuntime(for: hoveredWallpaper)
        let properties = wallpaperViewModel.effectiveProperties(for: hoveredWallpaper, runtime: runtime)
        do {
            try manager.configureCurrentWallpaper(
                hoveredWallpaper,
                runtime: runtime,
                properties: properties,
                fps: Int(AppDelegate.shared.globalSettingsViewModel.settings.fps),
                displayIDs: displayIDs
            )
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("已设为动态锁屏"),
                message: L("“%@”已部署到锁屏扩展。", hoveredWallpaper.project.title)
            )
        } catch {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("设置动态锁屏失败"),
                message: error.localizedDescription
            )
        }
    }

    private func setAsScreenSaverDynamicLockScreen() {
        let manager = ScreenSaverDynamicLockScreenManager.shared
        guard manager.isAvailable else {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("动态锁屏不可用"),
                message: L("动态锁屏方案 B 需要 macOS 14.2 或更高版本。")
            )
            return
        }
        guard manager.isEnabled else {
            manager.requestEnable()
            return
        }
        guard hoveredWallpaper.kind == .video || hoveredWallpaper.kind == .scene else {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("设置动态锁屏失败"),
                message: L("当前壁纸不能用作动态锁屏")
            )
            return
        }
        let runtime = wallpaperViewModel.loadRuntime(for: hoveredWallpaper)
        let properties = wallpaperViewModel.effectiveProperties(for: hoveredWallpaper, runtime: runtime)
        do {
            try manager.configureCurrentWallpaper(
                hoveredWallpaper,
                runtime: runtime,
                properties: properties,
                fps: Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
            )
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("已设为动态锁屏"),
                message: L("“%@”已设为方案 B 锁屏壁纸。", hoveredWallpaper.project.title)
            )
        } catch {
            viewModel.screenSaverFeedback = ScreenSaverFeedback(
                title: L("设置动态锁屏失败"),
                message: error.localizedDescription
            )
        }
    }
}

struct WorkshopCardContextMenu: View {
    let item: WorkshopItem
    let creatorStore: WorkshopCreatorStore
    let downloadStore: WorkshopDownloadStore
    let interactionStore: WorkshopInteractionStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let subscriptionStore: SubscriptionStore

    var body: some View {
        let subscriptionStatus = subscriptionStore.status(
            for: item.publishedFileId
        )
        Group {
            Section {
                if subscriptionStatus.state == .subscribed {
                    Button(role: .destructive) {
                        subscriptionStore.unsubscribe(item)
                    } label: {
                        Label("取消订阅", systemImage: "xmark.circle.fill")
                    }
                    .disabled(subscriptionStatus.isChanging)
                } else {
                    Button {
                        subscriptionStore.subscribe(item)
                    } label: {
                        Label("订阅并下载", systemImage: "plus.circle.fill")
                    }
                    .disabled(
                        subscriptionStatus.state == .unknown ||
                        subscriptionStatus.isChecking ||
                        subscriptionStatus.isChanging
                    )
                }

                Button {
                    downloadStore.download(item)
                } label: {
                    Label(
                        LocalizedStringKey(item.isPreset ? "下载预设" : "下载壁纸"),
                        systemImage: "arrow.down.circle.fill"
                    )
                }
                .disabled(downloadStore.state(for: item.publishedFileId) != nil)

                Button {
                    selectionCoordinator.selectWorkshopItem(item)
                } label: {
                    Label(LocalizedStringKey("查看壁纸详情"), systemImage: "info.circle")
                }
            }

            Section {
                if interactionStore.changingFavoriteIDs.contains(
                    item.publishedFileId
                ) {
                    Label("正在同步收藏状态…", systemImage: "arrow.triangle.2.circlepath")
                } else {
                    Button {
                        interactionStore.toggleFavorite(item)
                    } label: {
                        Label(
                            LocalizedStringKey(interactionStore.isFavorite(item.publishedFileId) ? "取消收藏" : "加入收藏"),
                            systemImage: interactionStore.isFavorite(item.publishedFileId) ? "heart.slash.fill" : "heart.fill"
                        )
                    }
                }

                Button {
                    guard let url = URL(
                        string: "https://steamcommunity.com/sharedfiles/filedetails/?id=\(item.publishedFileId)"
                    ) else { return }
                    NSWorkspace.shared.open(url)
                } label: {
                    Label(LocalizedStringKey("在 Steam 中查看"), systemImage: "safari")
                }

                if let creator = WorkshopCreator(item: item) {
                    Button {
                        creatorStore.open(creator)
                    } label: {
                        Label(LocalizedStringKey("查看作者主页和作品"), systemImage: "person.crop.circle")
                    }
                }
            }
        }
        .labelStyle(.titleAndIcon)
    }
}

struct WallpaperShortcut: Codable, Equatable {
    let keyCode: UInt16
    let modifierRawValue: UInt
    let keyLabel: String

    var modifierFlags: NSEvent.ModifierFlags {
        NSEvent.ModifierFlags(rawValue: modifierRawValue)
    }

    var displayName: String {
        var value = ""
        if modifierFlags.contains(.control) { value += "⌃" }
        if modifierFlags.contains(.option) { value += "⌥" }
        if modifierFlags.contains(.shift) { value += "⇧" }
        if modifierFlags.contains(.command) { value += "⌘" }
        value += keyLabel.isEmpty ? L("按键 %d", Int(keyCode)) : keyLabel
        return value
    }
}

final class WallpaperShortcutManager: ObservableObject {
    static let shared = WallpaperShortcutManager()

    @Published var recordingWallpaper: WEWallpaper?
    @Published private(set) var recordingError: String?

    private static let modifierMask: NSEvent.ModifierFlags = [.command, .option, .control, .shift]
    private let defaultsKey = "WallpaperShortcutsV1"
    private var shortcuts: [String: WallpaperShortcut] = [:]
    private var localMonitor: Any?
    private var globalMonitor: Any?

    private init() {
        load()
        installMonitors()
    }

    static func remapPersistedWallpaperIDs(_ mappings: [String: String]) {
        guard !mappings.isEmpty else { return }
        let key = "WallpaperShortcutsV1"
        guard let data = UserDefaults.standard.data(forKey: key),
              let shortcuts = try? JSONDecoder().decode([String: WallpaperShortcut].self, from: data) else { return }
        let remapper = WallpaperPathRemapper(mappings)
        let remapped = Dictionary(shortcuts.map { (remapper.path($0.key), $0.value) },
                                  uniquingKeysWith: { _, latest in latest })
        guard let encoded = try? JSONEncoder().encode(remapped) else { return }
        UserDefaults.standard.set(encoded, forKey: key)
    }

    deinit {
        if let localMonitor { NSEvent.removeMonitor(localMonitor) }
        if let globalMonitor { NSEvent.removeMonitor(globalMonitor) }
    }

    func shortcut(for wallpaper: WEWallpaper) -> WallpaperShortcut? {
        shortcuts[wallpaper.id]
    }

    func presentRecorder(for wallpaper: WEWallpaper) {
        recordingError = nil
        recordingWallpaper = wallpaper
        AppDelegate.shared.openMainWindow()
    }

    func cancelRecording() {
        recordingError = nil
        recordingWallpaper = nil
    }

    func clearShortcut(for wallpaper: WEWallpaper) {
        shortcuts[wallpaper.id] = nil
        persist()
        if recordingWallpaper?.id == wallpaper.id {
            cancelRecording()
        } else {
            objectWillChange.send()
        }
    }

    private func installMonitors() {
        localMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self else { return event }
            if self.recordingWallpaper != nil {
                return self.record(event) ? nil : event
            }
            return self.handleShortcut(event) ? nil : event
        }

        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .keyDown) { [weak self] event in
            DispatchQueue.main.async {
                _ = self?.handleShortcut(event)
            }
        }
    }

    private func record(_ event: NSEvent) -> Bool {
        guard let wallpaper = recordingWallpaper else { return false }
        if event.keyCode == 53 {
            cancelRecording()
            return true
        }

        let modifiers = event.modifierFlags.intersection(Self.modifierMask)
        guard !modifiers.isEmpty else {
            recordingError = L("快捷键必须包含至少一个修饰键。")
            return true
        }

        let shortcut = WallpaperShortcut(
            keyCode: event.keyCode,
            modifierRawValue: modifiers.rawValue,
            keyLabel: Self.keyLabel(for: event)
        )
        guard !Self.isReserved(shortcut) else {
            recordingError = L("该快捷键由 macOS 或 Mirage 保留，请选择其他组合。")
            return true
        }

        if let duplicate = shortcuts.first(where: {
            $0.key != wallpaper.id && $0.value.keyCode == shortcut.keyCode
                && $0.value.modifierRawValue == shortcut.modifierRawValue
        }) {
            let title = WEWallpaper.load(from: URL(fileURLWithPath: duplicate.key)).project.title
            recordingError = L("该快捷键已用于“%@”。", title)
            return true
        }

        shortcuts[wallpaper.id] = shortcut
        persist()
        recordingError = nil
        recordingWallpaper = nil
        objectWillChange.send()
        return true
    }

    private func handleShortcut(_ event: NSEvent) -> Bool {
        guard recordingWallpaper == nil, !event.isARepeat else { return false }
        let modifiers = event.modifierFlags.intersection(Self.modifierMask)
        guard let wallpaperID = shortcuts.first(where: {
            $0.value.keyCode == event.keyCode && $0.value.modifierRawValue == modifiers.rawValue
        })?.key else { return false }

        let wallpaper = WEWallpaper.load(from: URL(fileURLWithPath: wallpaperID))
        guard wallpaper.isValid, wallpaper.kind != .unsupported else {
            shortcuts[wallpaperID] = nil
            persist()
            return true
        }

        AppDelegate.shared.openMainWindow()
        AppDelegate.shared.wallpaperViewModel.requestApply(wallpaper)
        return true
    }

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: defaultsKey),
              let decoded = try? JSONDecoder().decode([String: WallpaperShortcut].self, from: data) else {
            return
        }
        shortcuts = decoded
    }

    private func persist() {
        guard let data = try? JSONEncoder().encode(shortcuts) else { return }
        UserDefaults.standard.set(data, forKey: defaultsKey)
    }

    private static func isReserved(_ shortcut: WallpaperShortcut) -> Bool {
        let flags = shortcut.modifierFlags
        if flags == [.command] && [4, 12, 13, 46].contains(shortcut.keyCode) {
            return true
        }
        if flags == [.command] && [48, 49].contains(shortcut.keyCode) {
            return true
        }
        if flags == [.control] && shortcut.keyCode == 49 {
            return true
        }
        return false
    }

    private static func keyLabel(for event: NSEvent) -> String {
        let named: [UInt16: String] = [
            36: "↩", 48: "⇥", 49: "Space", 51: "⌫", 53: "⎋",
            115: "↖", 116: "⇞", 117: "⌦", 119: "↘", 121: "⇟",
            123: "←", 124: "→", 125: "↓", 126: "↑"
        ]
        if let value = named[event.keyCode] {
            return value == "Space" ? L("空格") : value
        }
        let functionKeys: [UInt16: String] = [
            122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
            98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12"
        ]
        if let value = functionKeys[event.keyCode] { return value }
        return event.charactersIgnoringModifiers?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased() ?? ""
    }
}

struct WallpaperShortcutRecorderSheet: View {
    let wallpaper: WEWallpaper
    @ObservedObject var manager: WallpaperShortcutManager

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "command.square")
                .font(.system(size: 34))
                .foregroundStyle(.secondary)

            Text(L("设置壁纸快捷键"))
                .font(.headline)
            Text(wallpaper.project.title)
                .lineLimit(2)
                .multilineTextAlignment(.center)

            if let shortcut = manager.shortcut(for: wallpaper) {
                Text(L("当前快捷键：%@", shortcut.displayName))
                    .font(.callout.monospaced())
            }

            Text(L("请按下新的快捷键组合。快捷键必须包含 Command、Option、Control 或 Shift。"))
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)

            if let error = manager.recordingError {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
            }

            HStack {
                Button(L("清除快捷键")) {
                    manager.clearShortcut(for: wallpaper)
                }
                .disabled(manager.shortcut(for: wallpaper) == nil)

                Spacer()

                Button(L("取消")) {
                    manager.cancelRecording()
                }
                .keyboardShortcut(.cancelAction)
            }
        }
        .padding(24)
        .frame(width: 420)
    }
}
