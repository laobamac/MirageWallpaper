//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Combine
import Foundation

extension Notification.Name {
    static let playlistCurrentDidChange = Notification.Name("MiragePlaylistCurrentDidChange")
    static let playlistSavedDidChange   = Notification.Name("MiragePlaylistSavedDidChange")
    static let rendererVideoDidEnd      = Notification.Name("MirageRendererVideoDidEnd")
}

final class PlaylistManager: ObservableObject {
    static let shared = PlaylistManager()

    @Published private(set) var currents: [Int: Playlist] = [:]
    @Published private(set) var saved: [Playlist] = []

    @Published private(set) var legacyCurrents: [String: Playlist] = [:]
    @Published private(set) var unidentifiedScreens = Set<Int>()
    @Published private(set) var storageError: String?
    private var currentsByDisplay: [DisplayKey: Playlist] = [:]
    private let displayKeys: () -> [Int: DisplayKey]
    private let storageURL: URL
    private let ioQueue = DispatchQueue(label: "cn.laobamac.Mirage.playlist.io", qos: .utility)
    private var writeWorkItem: DispatchWorkItem?
    private var rotators: [Int: PlaylistRotator] = [:]
    private weak var wallpaperViewModel: (any PlaylistPlayback)?
    private var displayObserver: NSObjectProtocol?

    private struct Persisted: Codable {
        var currents: [String: Playlist]
        var saved: [Playlist]
        var legacyCurrents: [String: Playlist]?
    }

    private convenience init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        self.init(storageURL: base.appending(path: "playlists.json"))
    }

    init(storageURL: URL, displayKeys: @escaping () -> [Int: DisplayKey] = {
        Dictionary(uniqueKeysWithValues: DisplayRegistry.shared.connected.map { ($0.index, $0.key) })
    }) {
        self.storageURL = storageURL
        self.displayKeys = displayKeys
        load()
        synchronizeDisplays()
        displayObserver = NotificationCenter.default.addObserver(
            forName: DisplayRegistry.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.synchronizeDisplays() }
    }

    deinit {
        if let displayObserver { NotificationCenter.default.removeObserver(displayObserver) }
    }

    static func remapPersistedWallpaperIDs(_ mappings: [String: String]) {
        guard !mappings.isEmpty else { return }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage")
        let url = base.appending(path: "playlists.json")
        guard let data = try? Data(contentsOf: url),
              var persisted = try? JSONDecoder().decode(Persisted.self, from: data) else { return }
        let remapper = WallpaperPathRemapper(mappings)
        for key in persisted.currents.keys {
            guard var playlist = persisted.currents[key] else { continue }
            playlist.items = playlist.items.map {
                PlaylistItem(wallpaperID: remapper.path($0.wallpaperID), addedAt: $0.addedAt)
            }
            persisted.currents[key] = playlist
        }
        if let legacy = persisted.legacyCurrents {
            persisted.legacyCurrents = legacy.mapValues { source in
                var playlist = source
                playlist.items = playlist.items.map {
                    PlaylistItem(wallpaperID: remapper.path($0.wallpaperID), addedAt: $0.addedAt)
                }
                return playlist
            }
        }
        persisted.saved = persisted.saved.map { source in
            var playlist = source
            playlist.items = playlist.items.map {
                PlaylistItem(wallpaperID: remapper.path($0.wallpaperID), addedAt: $0.addedAt)
            }
            return playlist
        }
        guard let remapped = try? JSONEncoder().encode(persisted) else { return }
        try? remapped.write(to: url, options: .atomic)
    }

    // MARK: Load / persist

    private static func isStable(_ key: DisplayKey) -> Bool {
        (key.rawValue.hasPrefix("uuid:") || key.rawValue.hasPrefix("vms:")) && !key.rawValue.contains("#")
    }

    private func stableDisplayKeys() -> [Int: DisplayKey] {
        displayKeys().filter { Self.isStable($0.value) }
    }

    private func load() {
        guard FileManager.default.fileExists(atPath: storageURL.path) else { return }
        do {
            let obj = try JSONDecoder().decode(Persisted.self, from: Data(contentsOf: storageURL))
            legacyCurrents = obj.legacyCurrents ?? [:]
            for (key, value) in obj.currents {
                if Int(key) != nil || !Self.isStable(DisplayKey(rawValue: key)) { legacyCurrents[key] = value }
                else { currentsByDisplay[DisplayKey(rawValue: key)] = value }
            }
            saved = obj.saved
            if obj.currents.keys.contains(where: { Int($0) != nil }) {
                // Index-only files cannot tell us which physical display was used.
                // Keep the original before the user explicitly chooses a destination.
                let backup = storageURL.appendingPathExtension("legacy-backup")
                if !FileManager.default.fileExists(atPath: backup.path) {
                    try FileManager.default.copyItem(at: storageURL, to: backup)
                }
            }
        } catch {
            storageError = error.localizedDescription
            NSLog("[Playlist] Could not load or back up playlists: %@", error.localizedDescription)
        }
    }

    func synchronizeDisplays() {
        let allKeys = displayKeys()
        unidentifiedScreens = Set(allKeys.filter { !Self.isStable($0.value) }.keys)
        let keys = stableDisplayKeys()
        for key in keys.values where currentsByDisplay[key] == nil {
            currentsByDisplay[key] = Playlist(name: L("默认播放列表"))
        }
        currents = Dictionary(uniqueKeysWithValues: keys.map { ($0.key, currentsByDisplay[$0.value]!) })
        synchronizeRotators()
    }

    /// Migration is explicit: do not infer ownership from today's screen order.
    func bindLegacy(_ legacyKey: String, to screen: Int) {
        guard storageError == nil, let key = stableDisplayKeys()[screen],
              let playlist = legacyCurrents[legacyKey] else { return }
        currentsByDisplay[key] = playlist
        legacyCurrents[legacyKey] = nil
        synchronizeDisplays()
        scheduleSave()
        NotificationCenter.default.post(name: .playlistCurrentDidChange, object: nil, userInfo: ["screen": screen])
        rotator(on: screen)?.rebuild(reason: .listChanged)
    }

    private func scheduleSave() {
        guard storageError == nil else { return }
        writeWorkItem?.cancel()
        let snapshot = Persisted(
            currents: Dictionary(uniqueKeysWithValues: currentsByDisplay.map { ($0.key.rawValue, $0.value) }),
            saved: saved, legacyCurrents: legacyCurrents
        )
        let url = storageURL
        let work = DispatchWorkItem {
            guard let data = try? JSONEncoder().encode(snapshot) else { return }
            do { try data.write(to: url, options: .atomic) }
            catch {
                NSLog("[Playlist] Could not save playlists: %@", error.localizedDescription)
                DispatchQueue.main.async { [weak self] in self?.storageError = error.localizedDescription }
            }
        }
        writeWorkItem = work
        ioQueue.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    // MARK: Rotator lifecycle

    func startRotators(wallpaperViewModel: any PlaylistPlayback) {
        stopAllRotators()
        self.wallpaperViewModel = wallpaperViewModel
        for screen in currents.keys {
            _ = rotator(on: screen, startingWith: .appLaunch)
        }

    }

    func kickRotator(on screen: Int) {
        rotator(on: screen)?.rebuild(reason: .manualAdvance)
    }

    func kickAllRotators() {
        rotators.values.forEach { $0.rebuild(reason: .settingsChanged) }
    }

    func stopAllRotators() {
        rotators.values.forEach { $0.stop() }
        rotators.removeAll()
        wallpaperViewModel = nil

    }

    private func rotator(on screen: Int,
                         startingWith reason: PlaylistRotator.StartReason = .manualAdvance) -> PlaylistRotator? {
        guard currents[screen] != nil, let wallpaperViewModel,
              let key = DisplayRegistry.shared.key(forScreenIndex: screen) else { return nil }
        if let existing = rotators[screen], existing.displayKey == key { return existing }
        rotators[screen]?.stop()
        let rotator = PlaylistRotator(screen: screen, displayKey: key,
                                      wallpaperViewModel: wallpaperViewModel, manager: self)
        rotators[screen] = rotator
        rotator.start(reason: reason)
        return rotator
    }

    private func synchronizeRotators() {
        for screen in Array(rotators.keys) {
            guard let rotator = rotators[screen] else { continue }
            if DisplayRegistry.shared.key(forScreenIndex: screen) != rotator.displayKey {
                rotator.stop()
                rotators[screen] = nil
            }
        }
        for screen in currents.keys { _ = rotator(on: screen) }
    }

    func canAdvance(_ direction: PlaylistDirection, on display: DisplayKey, library: [WEWallpaper]) -> Bool {
        guard let screen = DisplayRegistry.shared.screenIndex(for: display),
              let rotator = rotator(on: screen) else { return false }
        return rotator.canAdvance(direction, library: library)
    }

    func advance(_ direction: PlaylistDirection, on display: DisplayKey, library: [WEWallpaper]) {
        guard let screen = DisplayRegistry.shared.screenIndex(for: display),
              let rotator = rotator(on: screen) else { return }
        rotator.advanceManually(direction, library: library)
    }

    // MARK: Current-playlist mutations

    private func mutateCurrent(_ screen: Int, _ transform: (inout Playlist) -> Void) {
        guard storageError == nil, let key = stableDisplayKeys()[screen] else { return }
        var playlist = currentsByDisplay[key] ?? Playlist(name: L("默认播放列表"))
        transform(&playlist)
        playlist.touch()
        currentsByDisplay[key] = playlist
        currents[screen] = playlist
        scheduleSave()
        NotificationCenter.default.post(name: .playlistCurrentDidChange, object: nil, userInfo: ["screen": screen])
        rotator(on: screen)?.rebuild(reason: .listChanged)
    }

    func add(_ wallpaper: WEWallpaper, to screen: Int) {
        guard wallpaper.isValid else { return }
        mutateCurrent(screen) { p in
            if p.items.contains(where: { $0.wallpaperID == wallpaper.id }) { return }
            p.items.append(PlaylistItem(wallpaperID: wallpaper.id, addedAt: Date()))
        }
    }

    func remove(itemID: String, from screen: Int) {
        mutateCurrent(screen) { p in
            p.items.removeAll { $0.wallpaperID == itemID }
        }
    }

    func move(from source: Int, to destination: Int, on screen: Int) {
        mutateCurrent(screen) { p in
            guard source >= 0, source < p.items.count else { return }
            let clamped = max(0, min(destination, p.items.count))
            let item = p.items.remove(at: source)
            let insertion = clamped > source ? clamped - 1 : clamped
            p.items.insert(item, at: min(insertion, p.items.count))
        }
    }

    func clear(screen: Int) {
        mutateCurrent(screen) { $0.items.removeAll() }
    }

    func trimItems(to limit: Int, on screen: Int) {
        mutateCurrent(screen) { p in
            guard p.items.count > limit else { return }
            p.items = Array(p.items.prefix(limit))
        }
    }

    func updateSettings(on screen: Int, _ transform: (inout PlaylistSettings) -> Void) {
        mutateCurrent(screen) { transform(&$0.settings) }
    }

    func resetSettings(on screen: Int) {
        mutateCurrent(screen) { $0.settings = .default() }
    }

    // MARK: Saved playlists

    @discardableResult
    func saveAs(name: String, from screen: Int) -> Playlist? {
        guard var current = currents[screen] else { return nil }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        current.name = trimmed
        current.updatedAt = Date()
        if let existing = saved.firstIndex(where: { $0.name == trimmed }) {
            var replacement = current
            replacement.id = saved[existing].id
            saved[existing] = replacement
        } else {
            var copy = current
            copy.id = UUID()
            saved.append(copy)
        }
        scheduleSave()
        NotificationCenter.default.post(name: .playlistSavedDidChange, object: nil)
        return current
    }

    func load(saved playlist: Playlist, into screen: Int) {
        guard storageError == nil, let key = stableDisplayKeys()[screen] else { return }
        var target = playlist
        target.updatedAt = Date()
        currentsByDisplay[key] = target
        currents[screen] = target
        scheduleSave()
        NotificationCenter.default.post(name: .playlistCurrentDidChange, object: nil, userInfo: ["screen": screen])
        rotator(on: screen)?.rebuild(reason: .listChanged)
    }

    func deleteSaved(_ id: UUID) {
        saved.removeAll { $0.id == id }
        scheduleSave()
        NotificationCenter.default.post(name: .playlistSavedDidChange, object: nil)
    }

    // MARK: Queries

    func current(on screen: Int) -> Playlist {
        stableDisplayKeys()[screen].flatMap { currentsByDisplay[$0] } ?? Playlist(name: L("默认播放列表"))
    }

    func ensureScreen(_ screen: Int) {
        guard let key = stableDisplayKeys()[screen] else { return }
        if currentsByDisplay[key] == nil {
            let playlist = Playlist(name: L("默认播放列表"))
            currentsByDisplay[key] = playlist
            currents[screen] = playlist
            scheduleSave()
        }
        _ = rotator(on: screen)
    }

    func resolvedItems(on screen: Int, library: [WEWallpaper]) -> [WEWallpaper] {
        let byID = Dictionary(uniqueKeysWithValues: library.map { ($0.id, $0) })
        return current(on: screen).items.compactMap { byID[$0.wallpaperID] }
    }

    func resolveWallpaper(id: String, library: [WEWallpaper]) -> WEWallpaper? {
        library.first(where: { $0.id == id })
    }
}
