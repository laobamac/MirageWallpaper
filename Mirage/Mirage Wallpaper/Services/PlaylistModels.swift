//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

enum PlaylistOrder: String, Codable, CaseIterable, Identifiable {
    case sorted
    case random
    var id: Self { self }
    var displayName: String {
        switch self {
        case .sorted: return L("有序")
        case .random: return L("随机")
        }
    }
}

enum PlaylistTiming: String, Codable, CaseIterable, Identifiable {
    case timer
    case logon
    case daytime
    case dayOfWeek
    case never
    var id: Self { self }
    var displayName: String {
        switch self {
        case .timer:     return L("按计时器")
        case .logon:     return L("登录时")
        case .daytime:   return L("当日时间")
        case .dayOfWeek: return L("星期")
        case .never:     return L("从不")
        }
    }
}

enum PlaylistTransitionKind: String, Codable, CaseIterable, Identifiable {
    case disabled
    case enabled
    case random
    var id: Self { self }
    var displayName: String {
        switch self {
        case .disabled: return L("禁用全部")
        case .enabled:  return L("启用全部")
        case .random:   return L("随机")
        }
    }
}

struct PlaylistSettings: Codable, Equatable {
    var order: PlaylistOrder = .sorted
    var timing: PlaylistTiming = .timer
    var timerHours: Int = 0
    var timerMinutes: Int = 30
    var updateOnPause: Bool = false
    var transition: PlaylistTransitionKind = .enabled
    var transitionSeconds: Double = 1.0
    var alwaysBeginFirst: Bool = false
    var introOnStartup: Bool = false
    var videoSequence: Bool = false
    var daytimeAnchors: [Int] = [8, 12, 18, 22]
    var dayOfWeekOrder: [Int] = [0, 1, 2, 3, 4, 5, 6]

    static func `default`() -> PlaylistSettings { PlaylistSettings() }

    var timerIntervalSeconds: TimeInterval {
        let total = max(timerHours, 0) * 3600 + max(timerMinutes, 0) * 60
        return TimeInterval(max(total, 30))
    }
}

struct PlaylistItem: Codable, Identifiable, Equatable, Hashable {
    var wallpaperID: String
    var addedAt: Date
    var id: String { wallpaperID }
}

struct Playlist: Codable, Identifiable, Equatable {
    var id: UUID
    var name: String
    var items: [PlaylistItem]
    var settings: PlaylistSettings
    var updatedAt: Date

    init(id: UUID = UUID(),
         name: String = "",
         items: [PlaylistItem] = [],
         settings: PlaylistSettings = .default(),
         updatedAt: Date = Date()) {
        self.id = id
        self.name = name
        self.items = items
        self.settings = settings
        self.updatedAt = updatedAt
    }

    mutating func touch() { updatedAt = Date() }
}

enum PlaylistDirection {
    case previous
    case next
}

struct PlaylistNavigation {
    struct Target: Equatable {
        let wallpaperID: String
        var historyIndex: Int?
    }

    private(set) var history: [String] = []
    private(set) var historyIndex: Int?
    private var playlistID: UUID?
    private var order: PlaylistOrder?
    private var itemIDs: Set<String> = []

    mutating func synchronize(with playlist: Playlist) {
        if playlistID != playlist.id || order != playlist.settings.order {
            history = []
            historyIndex = nil
        }
        playlistID = playlist.id
        order = playlist.settings.order
        itemIDs = Set(playlist.items.map(\.wallpaperID))
        let oldIndex = historyIndex
        let retained = history.enumerated().filter { itemIDs.contains($0.element) }
        history = retained.map(\.element)
        historyIndex = oldIndex.flatMap { index in
            retained.lastIndex(where: { $0.offset <= index })
        }
    }

    mutating func observe(_ wallpaperID: String?) {
        guard let wallpaperID, itemIDs.contains(wallpaperID) else {
            history = []
            historyIndex = nil
            return
        }
        if let historyIndex, history[historyIndex] == wallpaperID { return }
        history = Array(history.prefix((historyIndex ?? -1) + 1))
        history.append(wallpaperID)
        if history.count > 100 { history.removeFirst(history.count - 100) }
        historyIndex = history.count - 1
    }

    mutating func commit(_ target: Target) {
        if let index = target.historyIndex,
           history.indices.contains(index), history[index] == target.wallpaperID {
            historyIndex = index
        } else {
            observe(target.wallpaperID)
        }
    }

    func candidates(for direction: PlaylistDirection,
                    in playlist: Playlist,
                    availableIDs: Set<String>,
                    currentID: String?,
                    pending: Target? = nil) -> [Target] {
        let ids = playlist.items.map(\.wallpaperID)
        let current = pending?.wallpaperID ?? currentID
        let available = availableIDs.subtracting(current.map { [$0] } ?? [])
        guard !ids.isEmpty, !available.isEmpty else { return [] }

        if playlist.settings.order == .sorted {
            let indices: [Int]
            if let current, let index = ids.firstIndex(of: current) {
                let step = direction == .next ? 1 : -1
                indices = (1...ids.count).map { (index + step * $0 + ids.count) % ids.count }
            } else {
                indices = direction == .next ? Array(ids.indices) : Array(ids.indices.reversed())
            }
            return indices.compactMap {
                available.contains(ids[$0]) ? Target(wallpaperID: ids[$0]) : nil
            }
        }

        let cursor = pending.map { $0.historyIndex ?? history.count } ?? historyIndex ?? -1
        let indices: [Int]
        if direction == .previous {
            indices = cursor > 0 ? Array((0..<min(cursor, history.count)).reversed()) : []
        } else {
            indices = cursor + 1 < history.count ? Array((cursor + 1)..<history.count) : []
        }
        let remembered = indices.compactMap { index -> Target? in
            guard available.contains(history[index]) else { return nil }
            return Target(wallpaperID: history[index], historyIndex: index)
        }
        if !remembered.isEmpty || direction == .previous { return remembered }
        return ids.filter { available.contains($0) }.map { Target(wallpaperID: $0) }
    }
}
