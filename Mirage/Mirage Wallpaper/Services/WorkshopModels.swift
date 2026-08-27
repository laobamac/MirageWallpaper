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
    var additionalPreviewImageURLs: [URL]? = nil
    var tags: [String]
    var subscriptions: Int
    var favorited: Int
    var views: Int
    var fileSize: Int64
    var timeCreated: Date
    var timeUpdated: Date
    var creatorSteamId: String
    var creatorName: String? = nil
    var creatorAvatarURL: URL? = nil
    var creatorProfileURL: URL? = nil
    var consumerAppId: Int? = nil
    var wallpaperType: String
    /// Nil for the rare published file that carries no rating tag, matching how
    /// the local library treats a `project.json` without `contentrating`.
    var ageRating: WorkshopAgeRating? = nil
    var isApproved: Bool = false
    var isAudioResponsive: Bool = false
    var isCustomizable: Bool = false

    var id: String { publishedFileId }

    var creatorDisplayName: String {
        let name = creatorName?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !name.isEmpty { return name }
        if !creatorSteamId.isEmpty { return creatorSteamId }
        return L("未知作者")
    }

    var creatorWorkshopURL: URL? {
        guard !creatorSteamId.isEmpty else { return nil }
        return URL(string: "https://steamcommunity.com/profiles/\(creatorSteamId)/myworkshopfiles/?appid=431960")
    }

    var kind: WallpaperKind {
        WallpaperKind(rawType: wallpaperType)
    }

    var isPreset: Bool {
        tags.contains { $0.caseInsensitiveCompare("Preset") == .orderedSame }
    }

    var displayTypeName: String {
        isPreset ? L("预设 · %@", kind.displayName) : kind.displayName
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
        title: L("加载中..."),
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
        wallpaperType: "scene",
        ageRating: nil
    )

    static func dependencyPlaceholder(id: String) -> WorkshopItem {
        WorkshopItem(
            publishedFileId: id,
            title: L("基础壁纸 %@", id),
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
            wallpaperType: "scene",
            ageRating: nil
        )
    }

    static func unavailableSubscription(id: String) -> WorkshopItem {
        WorkshopItem(
            publishedFileId: id,
            title: L("不可用的创意工坊作品 %@", id),
            itemDescription: L("Steam 未返回该订阅作品的可用详情。"),
            previewImageURL: nil,
            tags: [],
            subscriptions: 0,
            favorited: 0,
            views: 0,
            fileSize: 0,
            timeCreated: .distantPast,
            timeUpdated: .distantPast,
            creatorSteamId: "",
            wallpaperType: "unsupported",
            ageRating: nil
        )
    }
}

// MARK: - Age Rating

/// The three mutually exclusive rating tags Steam exposes for Wallpaper Engine.
/// Raw values match `project.json`'s `contentrating`, so a Workshop item and an
/// installed wallpaper name the same rating the same way.
enum WorkshopAgeRating: String, CaseIterable, Identifiable, Codable, Hashable {
    case everyone = "Everyone"
    case questionable = "Questionable"
    case mature = "Mature"

    var id: String { rawValue }

    var steamTag: String { rawValue }

    var displayName: String {
        switch self {
        case .everyone: return L("所有人")
        case .questionable: return L("轻度裸露")
        case .mature: return L("成人")
        }
    }

    init?(steamTag: String) {
        guard let match = Self.allCases.first(where: {
            $0.rawValue.caseInsensitiveCompare(steamTag) == .orderedSame
        }) else { return nil }
        self = match
    }

    init?(contentRating: String?) {
        guard let contentRating else { return nil }
        self.init(steamTag: contentRating)
    }
}

// MARK: - Age Rating Filter

/// Mirrors `FRAgeRating` so the Workshop browser and the local library express
/// the same filter the same way.
struct WorkshopAgeRatingFilter: OptionSet, Codable, Equatable {
    let rawValue: Int

    static let everyone     = WorkshopAgeRatingFilter(rawValue: 1 << 0)
    static let questionable = WorkshopAgeRatingFilter(rawValue: 1 << 1)
    static let mature       = WorkshopAgeRatingFilter(rawValue: 1 << 2)

    static let all: WorkshopAgeRatingFilter = [.everyone, .questionable, .mature]
    static let none: WorkshopAgeRatingFilter = []

    /// Adult content stays opt-in: a fresh install browses the all-ages subset.
    static let `default`: WorkshopAgeRatingFilter = [.everyone]

    static func bit(for rating: WorkshopAgeRating) -> WorkshopAgeRatingFilter {
        switch rating {
        case .everyone: return .everyone
        case .questionable: return .questionable
        case .mature: return .mature
        }
    }

    func contains(_ rating: WorkshopAgeRating) -> Bool {
        contains(Self.bit(for: rating))
    }

    var selectedRatings: [WorkshopAgeRating] {
        WorkshopAgeRating.allCases.filter { contains($0) }
    }

    /// Ratings to hand to Steam's `excludedtags`.  Empty when everything is
    /// selected, and also when nothing is: excluding all three would return an
    /// empty page, so a cleared filter stops narrowing rather than showing
    /// nothing.
    var excludedRatings: [WorkshopAgeRating] {
        if self == .all || isEmpty { return [] }
        return WorkshopAgeRating.allCases.filter { !contains($0) }
    }
}

// MARK: - Sort Order

enum WorkshopSortOrder: Int, CaseIterable, Identifiable {
    case trending = 0
    case mostSubscribed = 2
    case topRated = 3
    case playtimeTrend = 5
    case totalPlaytime = 6
    case averagePlaytimeTrend = 7
    case lifetimeAveragePlaytime = 8
    case sessionsTrend = 9
    case lifetimeSessions = 10
    case lastUpdated = 11
    case textRelevance = 12
    case recentlyReleased = 13

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .trending: return L("热门趋势")
        case .mostSubscribed: return L("订阅最多")
        case .topRated: return L("评分最高")
        case .playtimeTrend: return L("播放时长最多")
        case .totalPlaytime: return L("总播放时长最多")
        case .averagePlaytimeTrend: return L("平均播放时长最长")
        case .lifetimeAveragePlaytime: return L("终身平均播放时长")
        case .sessionsTrend: return L("播放次数最多")
        case .lifetimeSessions: return L("总播放次数最多")
        case .lastUpdated: return L("最近更新")
        case .textRelevance: return L("文本相关性")
        case .recentlyReleased: return L("最近发行")
        }
    }

    var apiValue: Int {
        switch self {
        case .trending: return 3
        case .mostSubscribed: return 9
        case .topRated: return 0
        case .playtimeTrend: return 13
        case .totalPlaytime: return 14
        case .averagePlaytimeTrend: return 15
        case .lifetimeAveragePlaytime: return 16
        case .sessionsTrend: return 17
        case .lifetimeSessions: return 18
        case .lastUpdated: return 21
        case .textRelevance: return 12
        case .recentlyReleased: return 1
        }
    }

    var usesTrendPeriod: Bool {
        switch self {
        case .trending, .playtimeTrend, .averagePlaytimeTrend, .sessionsTrend:
            return true
        default:
            return false
        }
    }

    func workshopLabel(period: WorkshopTrendPeriod) -> String {
        switch self {
        case .topRated: return L("评分最高")
        case .trending: return L("最热门（%@）", period.workshopLabel)
        case .mostSubscribed: return L("最多订阅")
        default:
            return usesTrendPeriod ? L("%@（%@）", label, period.workshopLabel) : label
        }
    }
}

enum WorkshopTrendPeriod: Int, CaseIterable, Identifiable {
    case day = 1
    case week = 7
    case month = 30
    case year = 365

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .day: return L("今日")
        case .week: return L("本周")
        case .month: return L("本月")
        case .year: return L("今年")
        }
    }

    var workshopLabel: String {
        label
    }
}

enum WorkshopDiscoverCategory: String, CaseIterable, Identifiable {
    case trending
    case mostSubscribed
    case topRated
    case lastUpdated
    case playtimeTrend
    case averagePlaytimeTrend
    case sessionsTrend
    case totalPlaytime
    case lifetimeAveragePlaytime
    case lifetimeSessions
    case anime
    case nature
    case abstract
    case landscape

    var id: String { rawValue }

    var sortOrder: WorkshopSortOrder? {
        switch self {
        case .trending: return .trending
        case .mostSubscribed: return .mostSubscribed
        case .topRated: return .topRated
        case .lastUpdated: return .lastUpdated
        case .playtimeTrend: return .playtimeTrend
        case .averagePlaytimeTrend: return .averagePlaytimeTrend
        case .sessionsTrend: return .sessionsTrend
        case .totalPlaytime: return .totalPlaytime
        case .lifetimeAveragePlaytime: return .lifetimeAveragePlaytime
        case .lifetimeSessions: return .lifetimeSessions
        case .anime, .nature, .abstract, .landscape: return nil
        }
    }

    var tag: String? {
        switch self {
        case .anime: return "Anime"
        case .nature: return "Nature"
        case .abstract: return "Abstract"
        case .landscape: return "Landscape"
        default: return nil
        }
    }

    var usesTrendPeriod: Bool {
        sortOrder?.usesTrendPeriod == true || tag != nil
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

    var steamTag: String? {
        switch self {
        case .all: return nil
        case .scene: return "Scene"
        case .web: return "Web"
        case .video: return "Video"
        case .preset: return "Preset"
        }
    }

    func matches(_ item: WorkshopItem) -> Bool {
        switch self {
        case .all: return true
        case .scene: return item.kind == .scene
        case .web: return item.kind == .web
        case .video: return item.kind == .video
        case .preset: return item.isPreset
        }
    }
}

extension Set where Element == WorkshopTypeFilter {
    var normalizedWorkshopTypes: Set<WorkshopTypeFilter> {
        if isEmpty || contains(.all) { return [.all] }
        return subtracting([.all])
    }

    var hasNoWorkshopTypeConstraint: Bool {
        let selection = normalizedWorkshopTypes
        return selection.contains(.all) ||
            selection.isSuperset(of: [.scene, .web, .video])
    }

    var singleWorkshopType: WorkshopTypeFilter? {
        let selection = normalizedWorkshopTypes
        guard !selection.hasNoWorkshopTypeConstraint, selection.count == 1 else { return nil }
        return selection.first
    }

    func matches(_ item: WorkshopItem) -> Bool {
        let selection = normalizedWorkshopTypes
        return selection.hasNoWorkshopTypeConstraint || selection.contains { $0.matches(item) }
    }
}

// MARK: - Download Task

struct DownloadTask: Identifiable, Equatable {
    var id: String { workshopItem.publishedFileId }
    var workshopItem: WorkshopItem
    var attemptID: String?
    var state: DownloadState
    var startedAt: Date?
    var completedAt: Date?
    var purpose: DownloadPurpose

    static func == (lhs: DownloadTask, rhs: DownloadTask) -> Bool {
        lhs.id == rhs.id &&
            lhs.attemptID == rhs.attemptID &&
            lhs.state == rhs.state &&
            lhs.startedAt == rhs.startedAt &&
            lhs.completedAt == rhs.completedAt &&
            lhs.purpose == rhs.purpose
    }
}

enum DownloadPurpose: Equatable {
    case wallpaper
    case subscription
    case presetDependency
}

struct PresetDependencyPrompt: Identifiable, Equatable {
    let presetID: String
    let presetTitle: String
    let dependencyID: String
    let dependencyItem: WorkshopItem

    var id: String { "\(presetID):\(dependencyID)" }

    var message: String {
        let size = dependencyItem.fileSize > 0 ? L("（%@）", dependencyItem.formattedFileSize) : ""
        return L("预设“%@”需要基础壁纸“%@”%@才能使用。是否一起下载？", presetTitle, dependencyItem.title, size)
    }
}

enum DownloadState: Equatable {
    case queued
    case resolving
    case downloading(DownloadProgress)
    case validating
    case completed
    case failed(String)
}

struct DownloadProgress: Equatable {
    var receivedBytes: Int64
    var totalBytes: Int64
    var bytesPerSecond: Double
    var etaSeconds: Double?

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return min(1, max(0, Double(receivedBytes) / Double(totalBytes)))
    }
}

struct WorkshopSubscription: Identifiable, Equatable {
    let publishedFileId: String
    let subscribedAt: Date
    let updatedAt: Date
    let contentHash: String
    let fileSize: Int64

    var id: String { publishedFileId }
}

struct WorkshopSubscriptionPage: Equatable {
    let total: Int
    let startIndex: Int
    let subscriptions: [WorkshopSubscription]
}

enum WorkshopSubscriptionState: Equatable {
    case unknown
    case subscribed
    case unsubscribed
}

struct WorkshopComment: Identifiable, Equatable {
    let id: String
    let authorSteamId: String
    let createdAt: Date
    let text: String
    let upvotes: Int
    let isHidden: Bool
}

struct WorkshopCommentPage: Equatable {
    let total: Int
    let canPost: Bool
    let startIndex: Int
    let nextStartIndex: Int
    let comments: [WorkshopComment]
}

struct SteamServiceRequestError: LocalizedError, Equatable {
    let code: String?
    let message: String

    var errorDescription: String? { message }
}

// MARK: - Steam Setup State

enum SteamSetupState: Equatable {
    case checking
    case serviceUnavailable
    case needsLogin
    case ready
}

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
        case .available(let detail): return L(detail)
        case .needsAction(let detail): return L(detail)
        case .unavailable(let detail): return L(detail)
        }
    }
}

struct SteamServiceStatus: Equatable {
    var browsingAPI: SteamServiceState = .unknown
    var client: SteamServiceState = .unknown
    var authentication: SteamServiceState = .unknown
    var workshopDownload: SteamServiceState = .unknown
}

enum SteamLoginState: Equatable {
    case idle
    case loggingIn
    case waitingForQR(String)
    case waitingForGuard(SteamGuardType)
    case success
    case failed(String)
}

enum SteamGuardType: Equatable {
    case email
    case mobile
    case mobileConfirm
}

// MARK: - Steam API Response Models

struct SteamAPIResponse: Codable {
    var response: SteamAPIResponseBody
}

struct SteamAPIResponseBody: Codable {
    var total: Int?
    var next_cursor: String?
    var publishedfiledetails: [SteamPublishedFile]?
}

struct SteamPublishedFile: Codable {
    var publishedfileid: String?
    var title: String?
    var file_description: String?
    var preview_url: String?
    var previews: [SteamPreview]?
    var tags: [SteamTag]?
    var subscriptions: Int?
    var favorited: Int?
    var views: Int?
    var file_size: StringOrInt?
    var time_created: Int?
    var time_updated: Int?
    var creator: String?
    var consumer_app_id: StringOrInt?

    func toWorkshopItem() -> WorkshopItem {
        let rawTags = tags?.compactMap { $0.tag } ?? []

        func hasTag(_ tag: String) -> Bool {
            rawTags.contains { $0.caseInsensitiveCompare(tag) == .orderedSame }
        }

        let wallpaperType = rawTags.first(where: {
            let v = $0.lowercased()
            return v == "scene" || v == "web" || v == "video"
        }) ?? "scene"

        // Read before the tags are stripped below, which is where the rating
        // used to be discarded along with the other bookkeeping tags.
        let ageRating = rawTags.lazy.compactMap { WorkshopAgeRating(steamTag: $0) }.first

        let tagStrings = rawTags.filter { t in
            let l = t.lowercased()
            return l != "scene" && l != "web" && l != "video" &&
                   l != "wallpaper" && l != "approved" &&
                   l != "everyone" && l != "questionable" && l != "mature"
        }

        return WorkshopItem(
            publishedFileId: publishedfileid ?? "",
            title: title ?? L("无标题"),
            itemDescription: file_description ?? "",
            previewImageURL: URL(string: preview_url ?? ""),
            additionalPreviewImageURLs: previews?.compactMap { preview in
                guard let value = preview.url, !value.isEmpty else { return nil }
                return URL(string: value)
            },
            tags: tagStrings,
            subscriptions: subscriptions ?? 0,
            favorited: favorited ?? 0,
            views: views ?? 0,
            fileSize: file_size?.int64Value ?? 0,
            timeCreated: Date(timeIntervalSince1970: TimeInterval(time_created ?? 0)),
            timeUpdated: Date(timeIntervalSince1970: TimeInterval(time_updated ?? 0)),
            creatorSteamId: creator ?? "",
            consumerAppId: consumer_app_id.map { Int($0.int64Value) },
            wallpaperType: wallpaperType,
            ageRating: ageRating,
            isApproved: hasTag(FRShowOnly.approvedSteamTag),
            isAudioResponsive: hasTag(FRShowOnly.audioResponsiveSteamTag),
            isCustomizable: hasTag(FRShowOnly.customizableSteamTag)
        )
    }
}

struct WorkshopCreator: Identifiable, Hashable {
    var steamId: String
    var name: String
    var avatarURL: URL?
    var profileURL: URL?

    var id: String { steamId }

    var workshopURL: URL? {
        guard !steamId.isEmpty else { return nil }
        return URL(string: "https://steamcommunity.com/profiles/\(steamId)/myworkshopfiles/?appid=431960")
    }

    init?(item: WorkshopItem) {
        guard !item.creatorSteamId.isEmpty else { return nil }
        steamId = item.creatorSteamId
        name = item.creatorDisplayName
        avatarURL = item.creatorAvatarURL
        profileURL = item.creatorProfileURL
    }

    init(steamId: String, name: String, avatarURL: URL? = nil, profileURL: URL? = nil) {
        self.steamId = steamId
        self.name = name
        self.avatarURL = avatarURL
        self.profileURL = profileURL
    }
}

extension WEWallpaper {
    /// Returns a Workshop ID only when the manifest or a trusted Steam source
    /// provides one. Imported folders are never identified by name alone.
    func verifiedWorkshopID() -> String? {
        if let manifestID = project.workshopid?.rawValue,
           Self.isValidWorkshopID(manifestID) {
            return manifestID
        }
        guard WallpaperLibrary.shared.isWorkshopSource(self) else { return nil }
        let directoryID = wallpaperDirectory.lastPathComponent
        return Self.isValidWorkshopID(directoryID) ? directoryID : nil
    }

    func verifiedWorkshopURL() -> URL? {
        guard let id = verifiedWorkshopID() else { return nil }
        if let raw = project.workshopurl,
           let url = URL(string: raw),
           let host = url.host?.lowercased(),
           host == "steamcommunity.com" || host == "www.steamcommunity.com",
           let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
           components.path.contains("sharedfiles/filedetails") {
            let queryID = components.queryItems?.first(where: { $0.name == "id" })?.value
            if queryID == id { return url }
        }
        return URL(string: "https://steamcommunity.com/sharedfiles/filedetails/?id=\(id)")
    }

    func steamFavoriteWorkshopID() -> String? {
        guard WallpaperLibrary.shared.isWorkshopSource(self) else { return nil }
        return verifiedWorkshopID()
    }

    private static func isValidWorkshopID(_ value: String) -> Bool {
        guard !value.isEmpty, value.allSatisfy(\.isNumber), let number = UInt64(value) else {
            return false
        }
        return number > 0
    }
}

struct SteamPlayerSummariesResponse: Codable {
    var response: SteamPlayerSummariesBody
}

struct SteamPlayerSummariesBody: Codable {
    var players: [SteamPlayerSummary]
}

struct SteamPlayerSummary: Codable {
    var steamid: String
    var personaname: String
    var profileurl: String?
    var avatarmedium: String?
}

struct SteamPreview: Codable {
    var url: String?
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
