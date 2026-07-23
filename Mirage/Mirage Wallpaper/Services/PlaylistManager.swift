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

    private let storageURL: URL
    private let ioQueue = DispatchQueue(label: "cn.laobamac.Mirage.playlist.io", qos: .utility)
    private var writeWorkItem: DispatchWorkItem?
    private var rotators: [Int: PlaylistRotator] = [:]

    private struct Persisted: Codable {
        var currents: [String: Playlist]
        var saved: [Playlist]
    }

    private init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        self.storageURL = base.appending(path: "playlists.json")
        load()
    }

    // MARK: Load / persist

    private func load() {
        guard let data = try? Data(contentsOf: storageURL),
              let obj = try? JSONDecoder().decode(Persisted.self, from: data) else {
            currents = [0: Playlist(name: L("默认播放列表"))]
            return
        }
        var mapped: [Int: Playlist] = [:]
        for (key, value) in obj.currents {
            if let idx = Int(key) { mapped[idx] = value }
        }
        if mapped[0] == nil { mapped[0] = Playlist(name: L("默认播放列表")) }
        self.currents = mapped
        self.saved = obj.saved
    }

    private func scheduleSave() {
        writeWorkItem?.cancel()
        let snapshot = Persisted(
            currents: Dictionary(uniqueKeysWithValues: currents.map { (String($0.key), $0.value) }),
            saved: saved
        )
        let url = storageURL
        let work = DispatchWorkItem {
            guard let data = try? JSONEncoder().encode(snapshot) else { return }
            try? data.write(to: url, options: .atomic)
        }
        writeWorkItem = work
        ioQueue.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    // MARK: Rotator lifecycle

    func startRotators(wallpaperViewModel: WallpaperViewModel) {
        rotators.values.forEach { $0.stop() }
        rotators.removeAll()
        for screen in currents.keys {
            let rotator = PlaylistRotator(screen: screen, wallpaperViewModel: wallpaperViewModel, manager: self)
            rotators[screen] = rotator
            rotator.start(reason: .appLaunch)
        }
    }

    func kickRotator(on screen: Int) {
        rotators[screen]?.rebuild(reason: .settingsChanged)
    }

    func kickAllRotators() {
        rotators.values.forEach { $0.rebuild(reason: .settingsChanged) }
    }

    func stopAllRotators() {
        rotators.values.forEach { $0.stop() }
    }

    // MARK: Current-playlist mutations

    private func mutateCurrent(_ screen: Int, _ transform: (inout Playlist) -> Void) {
        var playlist = currents[screen] ?? Playlist(name: L("默认播放列表"))
        transform(&playlist)
        playlist.touch()
        currents[screen] = playlist
        scheduleSave()
        NotificationCenter.default.post(name: .playlistCurrentDidChange, object: nil, userInfo: ["screen": screen])
        rotators[screen]?.rebuild(reason: .listChanged)
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
        var target = playlist
        target.updatedAt = Date()
        currents[screen] = target
        scheduleSave()
        NotificationCenter.default.post(name: .playlistCurrentDidChange, object: nil, userInfo: ["screen": screen])
        rotators[screen]?.rebuild(reason: .listChanged)
    }

    func deleteSaved(_ id: UUID) {
        saved.removeAll { $0.id == id }
        scheduleSave()
        NotificationCenter.default.post(name: .playlistSavedDidChange, object: nil)
    }

    // MARK: Queries

    func current(on screen: Int) -> Playlist {
        currents[screen] ?? Playlist(name: L("默认播放列表"))
    }

    func ensureScreen(_ screen: Int) {
        if currents[screen] == nil {
            currents[screen] = Playlist(name: L("默认播放列表"))
            scheduleSave()
        }
    }

    func resolvedItems(on screen: Int, library: [WEWallpaper]) -> [WEWallpaper] {
        let byID = Dictionary(uniqueKeysWithValues: library.map { ($0.id, $0) })
        return current(on: screen).items.compactMap { byID[$0.wallpaperID] }
    }

    func resolveWallpaper(id: String, library: [WEWallpaper]) -> WEWallpaper? {
        library.first(where: { $0.id == id })
    }
}
