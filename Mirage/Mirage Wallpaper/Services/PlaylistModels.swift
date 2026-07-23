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
        case .timer:     return L("计时器上")
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
