//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

final class SteamWebAPI {
    static let shared = SteamWebAPI()

    private let builtInKey: String = {
        let value = (Bundle.main.object(forInfoDictionaryKey: "MirageSteamWebAPIKey") as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.range(of: "^[A-Fa-f0-9]{32}$", options: .regularExpression) != nil ? value : ""
    }()
    private var apiKey: String {
        let settings = AppDelegate.shared.globalSettingsViewModel.settings
        return settings.hasValidCustomSteamAPIKey ? settings.normalizedSteamAPIKey : builtInKey
    }
    private let appId = "431960"
    private let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        configuration.timeoutIntervalForResource = 30
        return URLSession(configuration: configuration)
    }()
    private let decoder = JSONDecoder()
    private let imageCache = NSCache<NSString, CacheEntry>()
    private let creatorCache = NSCache<NSString, CreatorCacheEntry>()
    private let requestThrottle = SteamRequestThrottle(interval: 0.35)

    private static let officialBase = "https://api.steampowered.com/"
    private static let mirrorBase = "https://steams.524228.xyz/"

    private var baseURL: String {
        switch AppDelegate.shared.globalSettingsViewModel.settings.steamAPIEndpoint {
        case .official: return Self.officialBase
        case .mirror: return Self.mirrorBase
        }
    }

    private let cacheDirectory: URL = {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/WorkshopCache")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }()

    final class CacheEntry {
        let data: Data
        let date: Date
        init(data: Data) {
            self.data = data
            self.date = Date()
        }
    }

    final class CreatorCacheEntry {
        let creator: WorkshopCreator
        let date: Date

        init(creator: WorkshopCreator) {
            self.creator = creator
            self.date = Date()
        }
    }

    // MARK: - Query Workshop Files

    func queryFiles(
        searchText: String = "",
        tags: [String] = [],
        sortOrder: WorkshopSortOrder = .trending,
        typeFilters: Set<WorkshopTypeFilter> = [.all],
        ageRating: WorkshopAgeRatingFilter = .all,
        widescreenResolution: FRWidescreenResolution = .all,
        ultraWidescreenResolution: FRUltraWidescreenResolution = .all,
        dualscreenResolution: FRDualscreenResolution = .all,
        triplescreenResolution: FRTriplescreenResolution = .all,
        portraitResolution: FRPortraitScreenResolution = .all,
        miscResolution: FRMiscResolution = .all,
        showOnly: FRShowOnly = .none,
        favoriteIDs: Set<String> = [],
        page: Int = 1,
        perPage: Int = 30,
        trendDays: Int? = nil,
        enrichCreatorProfiles: Bool = true
    ) async throws -> (items: [WorkshopItem], total: Int) {
        let normalizedSearchText = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        var params: [String: String] = [
            "key": apiKey,
            "query_type": "\(sortOrder.apiValue)",
            "appid": appId,
            "filetype": "18",
            "return_tags": "true",
            "return_previews": "true",
            "return_metadata": "true",
            "strip_description_bbcode": "true",
        ]

        if sortOrder.usesTrendPeriod {
            let days = min(WorkshopTrendPeriod.year.rawValue,
                           max(1, trendDays ?? WorkshopTrendPeriod.week.rawValue))
            params["days"] = "\(days)"
        }

        if !normalizedSearchText.isEmpty {
            params["search_text"] = normalizedSearchText
        }

        let selectableTags = Set(WorkshopTag.allCases.map(\.rawValue))
        let requestedTags = Set(tags)
        let selectedSelectableTags = requestedTags.intersection(selectableTags)
        let activeSelectedTags = selectableTags.isSubset(of: selectedSelectableTags)
            ? Set<String>()
            : selectedSelectableTags
        let selectedTagKeys = Set(activeSelectedTags.map { $0.lowercased() })
        let baseTags = tags.filter { !selectableTags.contains($0) }
        var allTags = baseTags
        if activeSelectedTags.count == 1, let selectedTag = activeSelectedTags.first {
            allTags.append(selectedTag)
        }
        let requestedTypes = typeFilters.normalizedWorkshopTypes
        let filterTypesLocally = !requestedTypes.hasNoWorkshopTypeConstraint &&
            requestedTypes.singleWorkshopType == nil
        if let typeTag = requestedTypes.singleWorkshopType?.steamTag {
            allTags.append(typeTag)
        }
        let selectedRatings = ageRating.selectedRatings
        let hasEveryone = selectedRatings.contains(WorkshopAgeRating.everyone)
        var filterRatingsLocally = false

        if selectedRatings.count == 1 && !hasEveryone {
            allTags.append(selectedRatings[0].steamTag)
        } else if selectedRatings.count == 2 && !hasEveryone {
            filterRatingsLocally = true
        } else {
            let excluded = ageRating.excludedRatings
            for (index, rating) in excluded.enumerated() {
                params["excludedtags[\(index)]"] = rating.steamTag
            }
        }

        let resolutionTags = FRResolutionFilter.selectedSteamTags(
            widescreen: widescreenResolution,
            ultraWidescreen: ultraWidescreenResolution,
            dualscreen: dualscreenResolution,
            triplescreen: triplescreenResolution,
            portrait: portraitResolution,
            misc: miscResolution
        )
        if resolutionTags?.isEmpty == true {
            return ([], 0)
        }
        let filterFavoritesLocally = showOnly.contains(.myFavourites)
        if filterFavoritesLocally && favoriteIDs.isEmpty {
            return ([], 0)
        }
        var filterResolutionLocally = false
        if let resolutionTags {
            if resolutionTags.count == 1 {
                allTags.append(resolutionTags[0])
            } else {
                filterResolutionLocally = true
            }
        }

        for tag in showOnly.requiredSteamTags where !allTags.contains(where: {
            $0.caseInsensitiveCompare(tag) == .orderedSame
        }) {
            allTags.append(tag)
        }
        let filterTagsLocally = activeSelectedTags.count > 1 && !allTags.isEmpty
        for (index, tag) in allTags.enumerated() {
            params["requiredtags[\(index)]"] = tag
        }
        let requestedPage = max(1, page)
        let requestedPageSize = max(1, perPage)
        if filterTagsLocally {
            var mergedItems: [WorkshopItem] = []
            var seenIDs = Set<String>()
            var total = 0
            for tag in activeSelectedTags.sorted() {
                let result = try await queryFiles(
                    searchText: normalizedSearchText,
                    tags: baseTags + [tag],
                    sortOrder: sortOrder,
                    typeFilters: typeFilters,
                    ageRating: ageRating,
                    widescreenResolution: widescreenResolution,
                    ultraWidescreenResolution: ultraWidescreenResolution,
                    dualscreenResolution: dualscreenResolution,
                    triplescreenResolution: triplescreenResolution,
                    portraitResolution: portraitResolution,
                    miscResolution: miscResolution,
                    showOnly: showOnly,
                    favoriteIDs: favoriteIDs,
                    page: requestedPage,
                    perPage: requestedPageSize,
                    trendDays: trendDays,
                    enrichCreatorProfiles: false
                )
                total += result.total
                for item in result.items where seenIDs.insert(item.publishedFileId).inserted {
                    mergedItems.append(item)
                }
            }
            let pageItems = Array(mergedItems.prefix(requestedPageSize))
            let items = enrichCreatorProfiles ? await enrichCreators(in: pageItems) : pageItems
            return (items, total)
        }
        if activeSelectedTags.count > 1 {
            params["match_all_tags"] = "false"
            for (index, tag) in activeSelectedTags.sorted().enumerated() {
                params["requiredtags[\(index)]"] = tag
            }
        }
        let resultItems: [WorkshopItem]
        let resultTotal: Int
        if filterTagsLocally || filterTypesLocally || filterRatingsLocally || filterResolutionLocally || filterFavoritesLocally {
            var cursor = "*"
            var seenCursors = Set<String>()
            var matchingItems: [WorkshopItem] = []
            var exhausted = false
            let requestedEnd = requestedPage * requestedPageSize
            while matchingItems.count < requestedEnd + 1 {
                try Task.checkCancellation()
                guard seenCursors.insert(cursor).inserted else {
                    exhausted = true
                    break
                }
                var cursorParams = params
                cursorParams["numperpage"] = "100"
                cursorParams["cursor"] = cursor
                let batch = try await fetchQueryBatch(params: cursorParams)
                matchingItems.append(contentsOf: batch.items.filter { item in
                    (!filterTagsLocally || item.tags.contains { selectedTagKeys.contains($0.lowercased()) }) &&
                        (!filterTypesLocally || requestedTypes.matches(item)) &&
                        (!filterRatingsLocally || item.ageRating.map(selectedRatings.contains) == true) &&
                        (!filterResolutionLocally || FRResolutionFilter.matches(
                            tags: item.tags,
                            widescreen: widescreenResolution,
                            ultraWidescreen: ultraWidescreenResolution,
                            dualscreen: dualscreenResolution,
                            triplescreen: triplescreenResolution,
                            portrait: portraitResolution,
                            misc: miscResolution
                        )) &&
                        (!filterFavoritesLocally || favoriteIDs.contains(item.publishedFileId))
                })
                guard let nextCursor = batch.nextCursor,
                      !nextCursor.isEmpty,
                      nextCursor != cursor,
                      !batch.items.isEmpty else {
                    exhausted = true
                    break
                }
                cursor = nextCursor
            }
            let start = (requestedPage - 1) * requestedPageSize
            if start < matchingItems.count {
                let end = min(start + requestedPageSize, matchingItems.count)
                resultItems = Array(matchingItems[start..<end])
            } else {
                resultItems = []
            }
            resultTotal = exhausted ? matchingItems.count : max(matchingItems.count, requestedEnd + 1)
        } else {
            params["page"] = "\(requestedPage)"
            params["numperpage"] = "\(requestedPageSize)"
            let batch = try await fetchQueryBatch(params: params)
            resultItems = batch.items
            resultTotal = batch.total
        }

        let items = enrichCreatorProfiles ? await enrichCreators(in: resultItems) : resultItems
        return (items, resultTotal)
    }

    func queryDiscoverFiles(
        searchText: String,
        sortOrder: WorkshopSortOrder,
        requiredTags: [String],
        excludedTags: [String],
        page: Int,
        perPage: Int,
        trendDays: Int?
    ) async throws -> (items: [WorkshopItem], total: Int) {
        var params: [String: String] = [
            "key": apiKey,
            "query_type": "\(sortOrder.apiValue)",
            "appid": appId,
            "filetype": "18",
            "page": "\(max(1, page))",
            "numperpage": "\(max(1, perPage))",
            "return_tags": "true",
            "return_previews": "true",
            "return_metadata": "true",
            "strip_description_bbcode": "true",
            "match_all_tags": "true"
        ]
        let normalizedSearch = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !normalizedSearch.isEmpty {
            params["search_text"] = normalizedSearch
        }
        if sortOrder.usesTrendPeriod, let trendDays {
            params["days"] = "\(max(1, min(365, trendDays)))"
        }
        for (index, tag) in requiredTags.enumerated() {
            params["requiredtags[\(index)]"] = tag
        }
        for (index, tag) in excludedTags.enumerated() {
            params["excludedtags[\(index)]"] = tag
        }
        let batch = try await fetchQueryBatch(params: params)
        return (batch.items, batch.total)
    }

    private func fetchQueryBatch(
        params: [String: String]
    ) async throws -> (items: [WorkshopItem], total: Int, nextCursor: String?) {
        try await throttle()
        var components = URLComponents(string: baseURL + "IPublishedFileService/QueryFiles/v1/")!
        components.queryItems = params.map { URLQueryItem(name: $0.key, value: $0.value) }

        guard let url = components.url else {
            throw SteamAPIError.invalidURL
        }

        let (data, response) = try await session.data(from: url)

        guard let httpResponse = response as? HTTPURLResponse else {
            throw SteamAPIError.invalidResponse
        }

        guard httpResponse.statusCode == 200 else {
            throw SteamAPIError.httpError(httpResponse.statusCode)
        }

        let apiResponse = try decoder.decode(SteamAPIResponse.self, from: data)
        let decodedItems = apiResponse.response.publishedfiledetails?.map { $0.toWorkshopItem() } ?? []
        let items = decodedItems.filter { $0.fileSize > 0 }
        return (items, apiResponse.response.total ?? items.count, apiResponse.response.next_cursor)
    }

    // MARK: - Get File Details

    func getFileDetails(
        workshopIds: [String],
        enrichCreatorProfiles: Bool = true
    ) async throws -> [WorkshopItem] {
        try await throttle()

        let components = URLComponents(string: baseURL + "ISteamRemoteStorage/GetPublishedFileDetails/v1/")!

        var bodyParams = "itemcount=\(workshopIds.count)"
        for (index, id) in workshopIds.enumerated() {
            bodyParams += "&publishedfileids[\(index)]=\(id)"
        }

        guard let url = components.url else {
            throw SteamAPIError.invalidURL
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.httpBody = bodyParams.data(using: .utf8)
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")

        let (data, response) = try await session.data(for: request)

        guard let httpResponse = response as? HTTPURLResponse else {
            throw SteamAPIError.invalidResponse
        }
        guard httpResponse.statusCode == 200 else {
            throw SteamAPIError.httpError(httpResponse.statusCode)
        }

        let apiResponse = try decoder.decode(SteamAPIResponse.self, from: data)
        let items = (apiResponse.response.publishedfiledetails ?? [])
            .map { $0.toWorkshopItem() }
            .filter { $0.fileSize > 0 }
        return enrichCreatorProfiles ? await enrichCreators(in: items) : items
    }

    func getUserFiles(
        steamId: String,
        page: Int = 1,
        perPage: Int = 30,
        sortMethod: String? = nil,
        requiredTags: [String] = [],
        excludedTags: [String] = [],
        enrichCreatorProfiles: Bool = true
    ) async throws -> (items: [WorkshopItem], total: Int) {
        try await throttle()
        var components = URLComponents(string: baseURL + "IPublishedFileService/GetUserFiles/v1/")!
        var queryItems = [
            URLQueryItem(name: "key", value: apiKey),
            URLQueryItem(name: "steamid", value: steamId),
            URLQueryItem(name: "appid", value: appId),
            URLQueryItem(name: "numperpage", value: "\(perPage)"),
            URLQueryItem(name: "page", value: "\(page)"),
            URLQueryItem(name: "return_tags", value: "true"),
            URLQueryItem(name: "return_previews", value: "true"),
            URLQueryItem(name: "return_metadata", value: "true"),
            URLQueryItem(name: "strip_description_bbcode", value: "true"),
        ]
        if let sortMethod, !sortMethod.isEmpty {
            queryItems.append(URLQueryItem(name: "sortmethod", value: sortMethod))
        }
        for (index, tag) in requiredTags.enumerated() {
            queryItems.append(URLQueryItem(name: "requiredtags[\(index)]", value: tag))
        }
        for (index, tag) in excludedTags.enumerated() {
            queryItems.append(URLQueryItem(name: "excludedtags[\(index)]", value: tag))
        }
        components.queryItems = queryItems
        guard let url = components.url else { throw SteamAPIError.invalidURL }
        let (data, response) = try await session.data(from: url)
        guard let httpResponse = response as? HTTPURLResponse else { throw SteamAPIError.invalidResponse }
        guard httpResponse.statusCode == 200 else { throw SteamAPIError.httpError(httpResponse.statusCode) }
        let apiResponse = try decoder.decode(SteamAPIResponse.self, from: data)
        let decodedItems = apiResponse.response.publishedfiledetails?.map { $0.toWorkshopItem() } ?? []
        let visibleItems = decodedItems.filter { $0.fileSize > 0 }
        let items = enrichCreatorProfiles ? await enrichCreators(in: visibleItems) : visibleItems
        let total = apiResponse.response.total ?? items.count
        return (items, total)
    }

    func getCollectionItems(
        collectionId: String,
        page: Int = 1,
        perPage: Int = 12
    ) async throws -> (items: [WorkshopItem], total: Int) {
        try await throttle()
        let components = URLComponents(string: baseURL + "ISteamRemoteStorage/GetCollectionDetails/v1/")!
        guard let url = components.url else { throw SteamAPIError.invalidURL }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.httpBody = "collectioncount=1&publishedfileids[0]=\(collectionId)".data(using: .utf8)
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        let (data, response) = try await session.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse else { throw SteamAPIError.invalidResponse }
        guard httpResponse.statusCode == 200 else { throw SteamAPIError.httpError(httpResponse.statusCode) }
        let decoded = try decoder.decode(SteamCollectionResponse.self, from: data)
        guard let details = decoded.response.collectiondetails?.first,
              details.result == 1 else { return ([], 0) }
        let ids = (details.children ?? [])
            .sorted { ($0.sortorder ?? 0) < ($1.sortorder ?? 0) }
            .compactMap(\.publishedfileid)
        let start = max(0, (max(1, page) - 1) * max(1, perPage))
        guard start < ids.count else { return ([], ids.count) }
        let end = min(ids.count, start + max(1, perPage))
        let items = try await getFileDetails(
            workshopIds: Array(ids[start..<end]),
            enrichCreatorProfiles: false
        )
        let order = Dictionary(uniqueKeysWithValues: ids.enumerated().map { ($0.element, $0.offset) })
        return (items.sorted { order[$0.id, default: 0] < order[$1.id, default: 0] }, ids.count)
    }

    func creatorProfile(steamId: String) async -> WorkshopCreator? {
        let normalized = steamId.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalized.isEmpty else { return nil }
        if let cached = cachedCreator(steamId: normalized) {
            return cached
        }
        let profiles = try? await fetchCreatorProfiles(steamIds: [normalized])
        return profiles?[normalized]
    }

    // MARK: - Trending / Featured

    func fetchDiscover(
        category: WorkshopDiscoverCategory,
        period: WorkshopTrendPeriod,
        count: Int = 12
    ) async throws -> [WorkshopItem] {
        let sortOrder = category.sortOrder ?? .trending
        let result = try await queryFiles(
            tags: category.tag.map { [$0] } ?? [],
            sortOrder: sortOrder,
            page: 1,
            perPage: count,
            trendDays: category.usesTrendPeriod ? period.rawValue : nil,
            enrichCreatorProfiles: false
        )
        return result.items
    }

    func enrichCreatorDetails(in items: [WorkshopItem]) async -> [WorkshopItem] {
        await enrichCreators(in: items)
    }

    func fetchTrending(count: Int = 10, ageRating: WorkshopAgeRatingFilter = .all) async throws -> [WorkshopItem] {
        let result = try await queryFiles(sortOrder: .trending, ageRating: ageRating, page: 1, perPage: count)
        return result.items
    }

    func fetchMostSubscribed(count: Int = 10, ageRating: WorkshopAgeRatingFilter = .all) async throws -> [WorkshopItem] {
        let result = try await queryFiles(sortOrder: .mostSubscribed, ageRating: ageRating, page: 1, perPage: count)
        return result.items
    }

    func fetchTopRated(count: Int = 10, ageRating: WorkshopAgeRatingFilter = .all) async throws -> [WorkshopItem] {
        let result = try await queryFiles(sortOrder: .topRated, ageRating: ageRating, page: 1, perPage: count)
        return result.items
    }

    func fetchByTag(
        _ tag: String,
        sortOrder: WorkshopSortOrder = .trending,
        count: Int = 10,
        ageRating: WorkshopAgeRatingFilter = .all
    ) async throws -> [WorkshopItem] {
        let result = try await queryFiles(
            tags: [tag], sortOrder: sortOrder, ageRating: ageRating, page: 1, perPage: count)
        return result.items
    }

    // MARK: - Image Download

    func downloadPreviewImage(url: URL) async throws -> Data {
        let cacheKey = NSString(string: url.absoluteString)

        if let cached = imageCache.object(forKey: cacheKey),
           Date().timeIntervalSince(cached.date) < 300 {
            return cached.data
        }

        let diskPath = cacheDirectory.appending(path: url.absoluteString.hash.description + ".gif")
        if let diskData = try? Data(contentsOf: diskPath) {
            let entry = CacheEntry(data: diskData)
            imageCache.setObject(entry, forKey: cacheKey)
            return diskData
        }

        let (data, _) = try await session.data(from: url)

        let entry = CacheEntry(data: data)
        imageCache.setObject(entry, forKey: cacheKey)

        try? data.write(to: diskPath, options: .atomic)

        return data
    }

    // MARK: - Check Downloaded

    func isItemDownloaded(_ workshopId: String) -> Bool {
        downloadedItemURL(workshopId) != nil
    }

    func downloadedItemURL(_ workshopId: String) -> URL? {
        let dirs = [
            WallpaperLibrary.shared.steamWorkshopDirectory,
            WallpaperLibrary.shared.defaultSteamWorkshopDirectory,
            SteamServiceManager.shared.contentDirectory
        ]
        return dirs
            .map { $0.appending(path: workshopId) }
            .first { FileManager.default.fileExists(atPath: $0.appending(path: "project.json").path) }
    }

    // MARK: - Throttle

    private func throttle() async throws {
        try await requestThrottle.wait()
    }

    private func enrichCreators(in items: [WorkshopItem]) async -> [WorkshopItem] {
        let steamIds = Set(items.map(\.creatorSteamId).filter { !$0.isEmpty })
        guard !steamIds.isEmpty else { return items }

        var creators: [String: WorkshopCreator] = [:]
        var missingIds: [String] = []
        for steamId in steamIds {
            if let creator = cachedCreator(steamId: steamId) {
                creators[steamId] = creator
            } else {
                missingIds.append(steamId)
            }
        }

        if !missingIds.isEmpty,
           let fetched = try? await fetchCreatorProfiles(steamIds: missingIds) {
            creators.merge(fetched) { _, new in new }
        }

        return items.map { item in
            guard let creator = creators[item.creatorSteamId] else { return item }
            var enriched = item
            enriched.creatorName = creator.name
            enriched.creatorAvatarURL = creator.avatarURL
            enriched.creatorProfileURL = creator.profileURL
            return enriched
        }
    }

    private func cachedCreator(steamId: String) -> WorkshopCreator? {
        let key = NSString(string: steamId)
        guard let entry = creatorCache.object(forKey: key) else { return nil }
        if Date().timeIntervalSince(entry.date) > 3_600 {
            creatorCache.removeObject(forKey: key)
            return nil
        }
        return entry.creator
    }

    private func fetchCreatorProfiles(steamIds: [String]) async throws -> [String: WorkshopCreator] {
        guard !apiKey.isEmpty else { return [:] }
        var result: [String: WorkshopCreator] = [:]

        for start in stride(from: 0, to: steamIds.count, by: 100) {
            let end = min(start + 100, steamIds.count)
            let batch = Array(steamIds[start..<end])
            try await throttle()

            var components = URLComponents(string: baseURL + "ISteamUser/GetPlayerSummaries/v2/")!
            components.queryItems = [
                URLQueryItem(name: "key", value: apiKey),
                URLQueryItem(name: "steamids", value: batch.joined(separator: ","))
            ]
            guard let url = components.url else { throw SteamAPIError.invalidURL }

            let (data, response) = try await session.data(from: url)
            guard let httpResponse = response as? HTTPURLResponse else {
                throw SteamAPIError.invalidResponse
            }
            guard httpResponse.statusCode == 200 else {
                throw SteamAPIError.httpError(httpResponse.statusCode)
            }

            let apiResponse = try decoder.decode(SteamPlayerSummariesResponse.self, from: data)
            for player in apiResponse.response.players {
                let creator = WorkshopCreator(
                    steamId: player.steamid,
                    name: player.personaname,
                    avatarURL: player.avatarmedium.flatMap(URL.init(string:)),
                    profileURL: player.profileurl.flatMap(URL.init(string:))
                )
                result[player.steamid] = creator
                creatorCache.setObject(CreatorCacheEntry(creator: creator), forKey: NSString(string: player.steamid))
            }
        }

        return result
    }
}

private actor SteamRequestThrottle {
    private let interval: TimeInterval
    private var nextAllowed = Date.distantPast

    init(interval: TimeInterval) {
        self.interval = interval
    }

    func wait() async throws {
        try Task.checkCancellation()
        let now = Date()
        let scheduled = max(now, nextAllowed)
        nextAllowed = scheduled.addingTimeInterval(interval)
        let delay = scheduled.timeIntervalSince(now)
        if delay > 0 {
            try await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
        }
        try Task.checkCancellation()
    }
}

// MARK: - Errors

enum SteamAPIError: LocalizedError {
    case invalidURL
    case invalidResponse
    case httpError(Int)
    case decodingError(String)

    var errorDescription: String? {
        switch self {
        case .invalidURL: return L("无效的 API 地址")
        case .invalidResponse: return L("无效的服务器响应")
        case .httpError(401), .httpError(403): return L("Steam API Key 无效、权限不足或当前线路拒绝访问")
        case .httpError(429): return L("Steam Web API 请求过于频繁，请稍后重试或设置专属 API Key")
        case .httpError(let code): return L("HTTP 错误: %@", String(code))
        case .decodingError(let msg): return L("数据解析错误: %@", msg)
        }
    }
}
