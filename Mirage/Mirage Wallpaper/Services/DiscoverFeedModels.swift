//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

struct WEExploreResponse: Decodable {
    let response: WEExploreResponseBody
}

struct WEExploreResponseBody: Decodable {
    let items: [WEExploreDefinition]
}

struct WEExploreDefinition: Decodable {
    let querytypes: [String]?
    let tags: [String]?
    let includetags: [String]?
    let excludetags: [String]?
    let dependenttags: [String]?
    let category: String
    let itemid: String?
    let coversubmission: String?
    let keyword: String?
    let exact: Bool?
}

enum DiscoverQueryKind: Equatable {
    case workshop
    case creator(steamId: String, sortMethod: String)
    case collection(id: String)
}

struct DiscoverQuery: Identifiable, Equatable {
    let id: String
    var title: String
    let kind: DiscoverQueryKind
    let searchText: String
    let sortOrder: WorkshopSortOrder
    let trendDays: Int?
    let requiredTags: [String]
    let excludedTags: [String]
    let exact: Bool
    let relevanceTags: [String]
}

struct DiscoverRow: Identifiable, Equatable {
    let id: String
    var query: DiscoverQuery
    var items: [WorkshopItem] = []
    var total = 0
    var page = 1
    var isLoading = false
    var error: String?
}

struct DiscoverBrowseState: Equatable {
    let query: DiscoverQuery
    var items: [WorkshopItem] = []
    var total = 0
    var page = 1
    var isLoading = false
    var error: String?

    var totalPages: Int {
        min(1000, max(1, (total + 49) / 50))
    }
}

enum DiscoverFeedBuilder {
    static func build(definitions: [WEExploreDefinition]) -> [DiscoverRow] {
        var feedQueries = standardQueries()
        feedQueries.append(contentsOf: definitions.flatMap { queries(from: $0) })
        var seen = Set<String>()
        return feedQueries.compactMap { query in
            guard seen.insert(query.id).inserted else { return nil }
            return DiscoverRow(id: query.id, query: query)
        }
    }

    static func searchRow(text: String) -> DiscoverRow {
        let normalized = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let query = DiscoverQuery(
            id: "search.\(normalized.lowercased())",
            title: L("“%@”的搜索结果", normalized),
            kind: .workshop,
            searchText: normalized,
            sortOrder: .textRelevance,
            trendDays: nil,
            requiredTags: [],
            excludedTags: [],
            exact: false,
            relevanceTags: []
        )
        return DiscoverRow(id: query.id, query: query)
    }

    private static func standardQueries() -> [DiscoverQuery] {
        var result = [
            DiscoverQuery(
                id: "standard.recent-approved",
                title: L("最近的好评壁纸"),
                kind: .workshop,
                searchText: "",
                sortOrder: .recentlyReleased,
                trendDays: nil,
                requiredTags: ["Approved"],
                excludedTags: [],
                exact: false,
                relevanceTags: ["Approved"]
            ),
            DiscoverQuery(
                id: "standard.popular-now",
                title: L("当下最热门"),
                kind: .workshop,
                searchText: "",
                sortOrder: .trending,
                trendDays: 30,
                requiredTags: [],
                excludedTags: [],
                exact: false,
                relevanceTags: []
            ),
            DiscoverQuery(
                id: "standard.mobile",
                title: L("手机必用"),
                kind: .workshop,
                searchText: "",
                sortOrder: .trending,
                trendDays: 365,
                requiredTags: ["Portrait 1080 x 1920"],
                excludedTags: [],
                exact: false,
                relevanceTags: ["Portrait"]
            ),
            DiscoverQuery(
                id: "standard.audio-responsive",
                title: L("最热门的音频响应壁纸"),
                kind: .workshop,
                searchText: "",
                sortOrder: .trending,
                trendDays: 365,
                requiredTags: ["Audio responsive"],
                excludedTags: [],
                exact: false,
                relevanceTags: ["Audio responsive"]
            )
        ]
        for entry in primaryGenres {
            result.append(DiscoverQuery(
                id: "standard.genre.\(entry.tag.lowercased().replacingOccurrences(of: " ", with: "-"))",
                title: L("最热门的%@壁纸", entry.name),
                kind: .workshop,
                searchText: "",
                sortOrder: .trending,
                trendDays: 365,
                requiredTags: [entry.tag],
                excludedTags: [],
                exact: false,
                relevanceTags: [entry.tag]
            ))
        }
        return result
    }

    private static func queries(from definition: WEExploreDefinition) -> [DiscoverQuery] {
        let required = definition.includetags ?? []
        let excluded = definition.excludetags ?? []
        let relevance = definition.tags ?? []
        switch definition.category {
        case "creator":
            guard let steamId = definition.itemid, !steamId.isEmpty else { return [] }
            return (definition.querytypes ?? ["published_votes"]).compactMap { type in
                let sortMethod: String
                let suffix: String
                switch type {
                case "published_desc":
                    sortMethod = "newestfirst"
                    suffix = L("最新壁纸")
                case "published_votes":
                    sortMethod = "score"
                    suffix = L("热门壁纸")
                default:
                    return nil
                }
                return DiscoverQuery(
                    id: "creator.\(steamId).\(type)",
                    title: L("精选创作者 · %@", suffix),
                    kind: .creator(steamId: steamId, sortMethod: sortMethod),
                    searchText: "",
                    sortOrder: .topRated,
                    trendDays: nil,
                    requiredTags: required,
                    excludedTags: excluded,
                    exact: false,
                    relevanceTags: relevance
                )
            }
        case "collection":
            guard let collectionId = definition.itemid, !collectionId.isEmpty else { return [] }
            return [DiscoverQuery(
                id: "collection.\(collectionId)",
                title: L("焦点作品集"),
                kind: .collection(id: collectionId),
                searchText: "",
                sortOrder: .topRated,
                trendDays: nil,
                requiredTags: required,
                excludedTags: excluded,
                exact: false,
                relevanceTags: relevance
            )]
        case "keyword":
            guard let keyword = definition.keyword?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !keyword.isEmpty else { return [] }
            return (definition.querytypes ?? ["top_rated"]).compactMap { type in
                let sortOrder: WorkshopSortOrder
                let trendDays: Int?
                let title: String
                switch type {
                case "top_rated":
                    sortOrder = .topRated
                    trendDays = nil
                    title = L("最热门的%@壁纸", keyword)
                case "trend_year":
                    sortOrder = .trending
                    trendDays = 365
                    title = L("今年热门的%@壁纸", keyword)
                case "trend_week":
                    sortOrder = .trending
                    trendDays = 7
                    title = L("本周热门的%@壁纸", keyword)
                case "most_recent":
                    sortOrder = .recentlyReleased
                    trendDays = nil
                    title = L("最新的%@壁纸", keyword)
                default:
                    return nil
                }
                return DiscoverQuery(
                    id: "keyword.\(keyword.lowercased()).\(type)",
                    title: title,
                    kind: .workshop,
                    searchText: keyword,
                    sortOrder: sortOrder,
                    trendDays: trendDays,
                    requiredTags: required,
                    excludedTags: excluded,
                    exact: definition.exact == true,
                    relevanceTags: relevance
                )
            }
        default:
            return []
        }
    }

    private static let primaryGenres: [(tag: String, name: String)] = [
        ("Anime", L("动漫")),
        ("Abstract", L("抽象")),
        ("Animal", L("动物")),
        ("Cartoon", L("卡通")),
        ("CGI", "CGI"),
        ("Cyberpunk", L("赛博朋克")),
        ("Fantasy", L("奇幻")),
        ("Game", L("游戏")),
        ("Girls", L("女性")),
        ("Guys", L("男性")),
        ("Landscape", L("风景")),
        ("Medieval", L("中世纪")),
        ("Memes", L("网红事物")),
        ("MMD", "MMD"),
        ("Music", L("音乐")),
        ("Nature", L("自然")),
        ("Pixel art", L("像素艺术")),
        ("Relaxing", L("放松")),
        ("Retro", L("复古")),
        ("Sci-Fi", L("科幻")),
        ("Sports", L("运动")),
        ("Technology", L("科技")),
        ("Television", L("电视节目")),
        ("Vehicle", L("汽车"))
    ]
}

struct SteamCollectionResponse: Decodable {
    let response: SteamCollectionResponseBody
}

struct SteamCollectionResponseBody: Decodable {
    let collectiondetails: [SteamCollectionDetails]?
}

struct SteamCollectionDetails: Decodable {
    let publishedfileid: String?
    let result: Int?
    let children: [SteamCollectionChild]?
}

struct SteamCollectionChild: Decodable {
    let publishedfileid: String?
    let sortorder: Int?
}
