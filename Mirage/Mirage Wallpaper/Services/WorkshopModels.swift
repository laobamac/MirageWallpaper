//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

// MARK: - Workshop Item

struct WorkshopItem: Identifiable, Codable, Equatable, Hashable {
    var publishedFileId: String
    var title: String
    var itemDescription: String
    var previewImageURL: URL?
    var tags: [String]
    var subscriptions: Int
    var favorited: Int
    var views: Int
    var fileSize: Int64
    var timeCreated: Date
    var timeUpdated: Date
    var creatorSteamId: String
    var wallpaperType: String

    var id: String { publishedFileId }

    var kind: WallpaperKind {
        WallpaperKind(rawType: wallpaperType)
    }

    var isPreset: Bool {
        tags.contains { $0.caseInsensitiveCompare("Preset") == .orderedSame }
    }

    var displayTypeName: String {
        isPreset ? "预设 · \(kind.displayName)" : kind.displayName
    }

    var formattedFileSize: String {
        ByteCountFormatter.string(fromByteCount: fileSize, countStyle: .file)
    }

    var formattedSubscriptions: String {
        if subscriptions >= 1_000_000 {
            return String(format: "%.1fM", Double(subscriptions) / 1_000_000)
        } else if subscriptions >= 1_000 {
            return String(format: "%.1fK", Double(subscriptions) / 1_000)
        }
        return "\(subscriptions)"
    }

    var formattedViews: String {
        if views >= 1_000_000 {
            return String(format: "%.1fM", Double(views) / 1_000_000)
        } else if views >= 1_000 {
            return String(format: "%.1fK", Double(views) / 1_000)
        }
        return "\(views)"
    }

    var formattedFavorited: String {
        if favorited >= 1_000_000 {
            return String(format: "%.1fM", Double(favorited) / 1_000_000)
        } else if favorited >= 1_000 {
            return String(format: "%.1fK", Double(favorited) / 1_000)
        }
        return "\(favorited)"
    }

    static let placeholder = WorkshopItem(
        publishedFileId: "0",
        title: "加载中...",
        itemDescription: "",
        previewImageURL: nil,
        tags: [],
        subscriptions: 0,
        favorited: 0,
        views: 0,
        fileSize: 0,
        timeCreated: Date(),
        timeUpdated: Date(),
        creatorSteamId: "",
        wallpaperType: "scene"
    )

    static func dependencyPlaceholder(id: String) -> WorkshopItem {
        WorkshopItem(
            publishedFileId: id,
            title: "基础壁纸 \(id)",
            itemDescription: "",
            previewImageURL: nil,
            tags: [],
            subscriptions: 0,
            favorited: 0,
            views: 0,
            fileSize: 0,
            timeCreated: .distantPast,
            timeUpdated: .distantPast,
            creatorSteamId: "",
            wallpaperType: "scene"
        )
    }
}

// MARK: - Sort Order

enum WorkshopSortOrder: Int, CaseIterable, Identifiable {
    case trending = 0
    case mostRecent = 1
    case mostSubscribed = 2
    case topRated = 3

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .trending: return L("热门趋势")
        case .mostRecent: return L("最新发布")
        case .mostSubscribed: return L("订阅最多")
        case .topRated: return L("评分最高")
        }
    }

    var apiValue: Int {
        switch self {
        case .trending: return 3
        case .mostRecent: return 1
        case .mostSubscribed: return 9
        case .topRated: return 0
        }
    }
}

// MARK: - Workshop Tag

enum WorkshopTag: String, CaseIterable, Identifiable {
    case anime = "Anime"
    case nature = "Nature"
    case abstract = "Abstract"
    case landscape = "Landscape"
    case sciFi = "Sci-Fi"
    case cartoon = "Cartoon"
    case cyberpunk = "Cyberpunk"
    case fantasy = "Fantasy"
    case girl = "Girl"
    case game = "Game"
    case animal = "Animal"
    case music = "Music"
    case vehicle = "Vehicle"
    case technology = "Technology"
    case retro = "Retro"
    case city = "City"
    case space = "Space"
    case dark = "Dark"
    case pixel = "Pixel Art"
    case minimal = "Minimalist"
    case underwater = "Underwater"
    case relaxing = "Relaxing"
    case medieval = "Medieval"
    case unspecified = "Unspecified"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .anime: return L("动漫")
        case .nature: return L("自然")
        case .abstract: return L("抽象")
        case .landscape: return L("风景")
        case .sciFi: return L("科幻")
        case .cartoon: return L("卡通")
        case .cyberpunk: return L("赛博朋克")
        case .fantasy: return L("奇幻")
        case .girl: return L("女孩")
        case .game: return L("游戏")
        case .animal: return L("动物")
        case .music: return L("音乐")
        case .vehicle: return L("车辆")
        case .technology: return L("科技")
        case .retro: return L("复古")
        case .city: return L("城市")
        case .space: return L("太空")
        case .dark: return L("暗黑")
        case .pixel: return L("像素")
        case .minimal: return L("极简")
        case .underwater: return L("水下")
        case .relaxing: return L("放松")
        case .medieval: return L("中世纪")
        case .unspecified: return L("未分类")
        }
    }

    var sfSymbol: String {
        switch self {
        case .anime: return "sparkles"
        case .nature: return "leaf.fill"
        case .abstract: return "circle.hexagongrid.fill"
        case .landscape: return "mountain.2.fill"
        case .sciFi: return "atom"
        case .cartoon: return "face.smiling.inverse"
        case .cyberpunk: return "cpu"
        case .fantasy: return "wand.and.stars"
        case .girl: return "person.fill"
        case .game: return "gamecontroller.fill"
        case .animal: return "pawprint.fill"
        case .music: return "music.note"
        case .vehicle: return "car.fill"
        case .technology: return "desktopcomputer"
        case .retro: return "clock.arrow.circlepath"
        case .city: return "building.2.fill"
        case .space: return "moon.stars.fill"
        case .dark: return "moon.fill"
        case .pixel: return "square.grid.3x3.fill"
        case .minimal: return "minus"
        case .underwater: return "drop.fill"
        case .relaxing: return "wind"
        case .medieval: return "shield.fill"
        case .unspecified: return "questionmark"
        }
    }
}

// MARK: - Wallpaper Type Filter

enum WorkshopTypeFilter: String, CaseIterable, Identifiable {
    case all = "all"
    case scene = "scene"
    case web = "web"
    case video = "video"
    case preset = "preset"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .all: return L("全部")
        case .scene: return L("场景")
        case .web: return L("网页")
        case .video: return L("视频")
        case .preset: return L("预设")
        }
    }
}

// MARK: - Download Task

struct DownloadTask: Identifiable, Equatable {
    var id: String { workshopItem.publishedFileId }
    var workshopItem: WorkshopItem
    var state: DownloadState
    var startedAt: Date?
    var completedAt: Date?
    var purpose: DownloadPurpose

    static func == (lhs: DownloadTask, rhs: DownloadTask) -> Bool {
        lhs.id == rhs.id && lhs.state == rhs.state
    }
}

enum DownloadPurpose: Equatable {
    case wallpaper
    case presetDependency
}

struct PresetDependencyPrompt: Identifiable {
    let presetID: String
    let presetTitle: String
    let dependencyID: String
    let dependencyItem: WorkshopItem

    var id: String { "\(presetID):\(dependencyID)" }

    var message: String {
        let size = dependencyItem.fileSize > 0 ? "（\(dependencyItem.formattedFileSize)）" : ""
        return L("预设“%@”需要基础壁纸“%@”%@才能使用。是否一起下载？", presetTitle, dependencyItem.title, size)
    }
}

enum DownloadState: Equatable {
    case queued
    case starting
    case downloading(percent: Double?)
    case validating
    case completed
    case failed(String)
}

// MARK: - Steam Setup State

enum SteamSetupState: Equatable {
    case notConfigured
    case steamCMDMissing
    case needsLogin
    case ready
}

/// Each Workshop dependency is intentionally tracked on its own.  A reachable
/// Web API must not imply that SteamCMD, the Steam session, or the content CDN
/// is also usable.
enum SteamServiceState: Equatable {
    case unknown
    case checking
    case available(String)
    case needsAction(String)
    case unavailable(String)

    var summary: String {
        switch self {
        case .unknown: return L("尚未检测")
        case .checking: return L("检测中")
        case .available(let detail): return detail
        case .needsAction(let detail): return detail
        case .unavailable(let detail): return detail
        }
    }
}

struct SteamServiceStatus: Equatable {
    var browsingAPI: SteamServiceState = .unknown
    var steamCMD: SteamServiceState = .unknown
    var authentication: SteamServiceState = .unknown
    var workshopDownload: SteamServiceState = .unknown
}

enum SteamCMDInstallState: Equatable {
    case detecting
    case found(String)
    case notFound
    case downloading(Double)
    case extracting
    case initializing
    case installed(String)
    case failed(String)
}

enum SteamLoginState: Equatable {
    case idle
    case loggingIn
    case waitingForGuard(SteamGuardType)
    case success
    case failed(String)
}

enum SteamGuardType: Equatable {
    case email
    case mobile
    case mobileConfirm
}

enum SteamDiagnosticCategory: String, Codable, CaseIterable {
    case browsingAPI = "浏览 API"
    case steamCMDInstall = "SteamCMD 安装"
    case authentication = "Steam 认证"
    case workshopDownload = "创意工坊下载"
}

struct SteamDiagnosticEvent: Identifiable, Equatable {
    let id = UUID()
    let timestamp: Date
    let category: SteamDiagnosticCategory
    let domain: String
    let message: String
}

// MARK: - Steam API Response Models

struct SteamAPIResponse: Codable {
    var response: SteamAPIResponseBody
}

struct SteamAPIResponseBody: Codable {
    var total: Int?
    var publishedfiledetails: [SteamPublishedFile]?
}

struct SteamPublishedFile: Codable {
    var publishedfileid: String?
    var title: String?
    var file_description: String?
    var preview_url: String?
    var tags: [SteamTag]?
    var subscriptions: Int?
    var favorited: Int?
    var views: Int?
    var file_size: StringOrInt?
    var time_created: Int?
    var time_updated: Int?
    var creator: String?

    func toWorkshopItem() -> WorkshopItem {
        let wallpaperType = tags?.first(where: {
            let v = ($0.tag ?? "").lowercased()
            return v == "scene" || v == "web" || v == "video"
        })?.tag ?? "scene"

        let tagStrings = tags?
            .compactMap { $0.tag }
            .filter { t in
                let l = t.lowercased()
                return l != "scene" && l != "web" && l != "video" &&
                       l != "wallpaper" && l != "approved" &&
                       l != "everyone" && l != "questionable" && l != "mature"
            } ?? []

        return WorkshopItem(
            publishedFileId: publishedfileid ?? "",
            title: title ?? "无标题",
            itemDescription: file_description ?? "",
            previewImageURL: URL(string: preview_url ?? ""),
            tags: tagStrings,
            subscriptions: subscriptions ?? 0,
            favorited: favorited ?? 0,
            views: views ?? 0,
            fileSize: file_size?.int64Value ?? 0,
            timeCreated: Date(timeIntervalSince1970: TimeInterval(time_created ?? 0)),
            timeUpdated: Date(timeIntervalSince1970: TimeInterval(time_updated ?? 0)),
            creatorSteamId: creator ?? "",
            wallpaperType: wallpaperType
        )
    }
}

struct SteamTag: Codable {
    var tag: String?
    var display_name: String?
}

enum StringOrInt: Codable, Equatable {
    case string(String)
    case int(Int)

    var int64Value: Int64 {
        switch self {
        case .string(let s): return Int64(s) ?? 0
        case .int(let i): return Int64(i)
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let i = try? container.decode(Int.self) {
            self = .int(i); return
        }
        if let s = try? container.decode(String.self) {
            self = .string(s); return
        }
        self = .int(0)
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let s): try container.encode(s)
        case .int(let i): try container.encode(i)
        }
    }
}
