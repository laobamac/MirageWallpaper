//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation
import Observation

@MainActor
@Observable
final class WorkshopBrowseStore {
    private(set) var items: [WorkshopItem] = []
    var searchText = "" {
        didSet {
            guard searchText != oldValue else { return }
            scheduleSearch()
        }
    }
    var selectedTags: Set<String> = []
    var sortOrder: WorkshopSortOrder = .trending
    var trendPeriod: WorkshopTrendPeriod = .week
    var selectedTypeFilters: Set<WorkshopTypeFilter> = [.all]
    var showOnly: FRShowOnly = .none {
        didSet {
            guard showOnly != oldValue else { return }
            UserDefaults.standard.set(showOnly.rawValue, forKey: Self.showOnlyStorageKey)
        }
    }
    var ageRatingFilter: WorkshopAgeRatingFilter = .default {
        didSet {
            guard ageRatingFilter != oldValue else { return }
            UserDefaults.standard.set(ageRatingFilter.rawValue, forKey: Self.ageRatingStorageKey)
        }
    }
    var widescreenResolution = FRWidescreenResolution.all {
        didSet { UserDefaults.standard.set(widescreenResolution.rawValue, forKey: "WorkshopWidescreenResolution") }
    }
    var ultraWidescreenResolution = FRUltraWidescreenResolution.all {
        didSet { UserDefaults.standard.set(ultraWidescreenResolution.rawValue, forKey: "WorkshopUltraWidescreenResolution") }
    }
    var dualscreenResolution = FRDualscreenResolution.all {
        didSet { UserDefaults.standard.set(dualscreenResolution.rawValue, forKey: "WorkshopDualscreenResolution") }
    }
    var triplescreenResolution = FRTriplescreenResolution.all {
        didSet { UserDefaults.standard.set(triplescreenResolution.rawValue, forKey: "WorkshopTriplescreenResolution") }
    }
    var portraitResolution = FRPortraitScreenResolution.all {
        didSet { UserDefaults.standard.set(portraitResolution.rawValue, forKey: "WorkshopPortraitResolution") }
    }
    var miscResolution = FRMiscResolution.all {
        didSet { UserDefaults.standard.set(miscResolution.rawValue, forKey: "WorkshopMiscResolution") }
    }
    private(set) var currentPage = 1
    private(set) var totalItems = 0
    private(set) var isLoading = false
    private(set) var error: String?
    var pageNavigationMessage: String?
    private(set) var knownCreators: [WorkshopCreator] = []

    let itemsPerPage = 50
    let maximumPages = 1000

    @ObservationIgnored private var searchTask: Task<Void, Never>?
    @ObservationIgnored private var debounceTask: Task<Void, Never>?
    @ObservationIgnored private var searchGeneration = 0
    @ObservationIgnored private var loadedPage = 1
    @ObservationIgnored private var onItemsLoaded: ([WorkshopItem]) -> Void = { _ in }
    @ObservationIgnored private var onBrowsingAPIStateChange: (SteamServiceState) -> Void = { _ in }

    private static let ageRatingStorageKey = "WorkshopAgeRatingFilter"
    private static let showOnlyStorageKey = "WorkshopShowOnlyV2"

    init() {
        if let raw = UserDefaults.standard.object(forKey: Self.showOnlyStorageKey) as? Int {
            showOnly = FRShowOnly(rawValue: raw & FRShowOnly.all.rawValue)
        }
        if let raw = UserDefaults.standard.object(forKey: Self.ageRatingStorageKey) as? Int {
            ageRatingFilter = WorkshopAgeRatingFilter(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopWidescreenResolution") as? Int {
            widescreenResolution = FRWidescreenResolution(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopUltraWidescreenResolution") as? Int {
            ultraWidescreenResolution = FRUltraWidescreenResolution(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopDualscreenResolution") as? Int {
            dualscreenResolution = FRDualscreenResolution(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopTriplescreenResolution") as? Int {
            triplescreenResolution = FRTriplescreenResolution(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopPortraitResolution") as? Int {
            portraitResolution = FRPortraitScreenResolution(rawValue: raw)
        }
        if let raw = UserDefaults.standard.object(forKey: "WorkshopMiscResolution") as? Int {
            miscResolution = FRMiscResolution(rawValue: raw)
        }
    }

    func configure(
        onItemsLoaded: @escaping ([WorkshopItem]) -> Void,
        onBrowsingAPIStateChange: @escaping (SteamServiceState) -> Void
    ) {
        self.onItemsLoaded = onItemsLoaded
        self.onBrowsingAPIStateChange = onBrowsingAPIStateChange
    }

    var totalPages: Int {
        min(maximumPages, max(1, Int(ceil(Double(totalItems) / Double(itemsPerPage)))))
    }

    var isTextRelevanceSearch: Bool {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        return !query.isEmpty && !Self.isPublishedFileID(query) && !Self.isSteamUserID(query)
    }

    var allResolutionsSelected: Bool {
        widescreenResolution == .all && ultraWidescreenResolution == .all &&
            dualscreenResolution == .all && triplescreenResolution == .all &&
            portraitResolution == .all && miscResolution == .all
    }

    var allResolutionsCleared: Bool {
        widescreenResolution.isEmpty && ultraWidescreenResolution.isEmpty &&
            dualscreenResolution.isEmpty && triplescreenResolution.isEmpty &&
            portraitResolution.isEmpty && miscResolution.isEmpty
    }

    func search() {
        debounceTask?.cancel()
        debounceTask = nil
        searchTask?.cancel()
        searchGeneration += 1
        let requestGeneration = searchGeneration
        let requestSearchText = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        let requestTags = Array(selectedTags)
        let requestSortOrder = sortOrder
        let requestTypeFilters = selectedTypeFilters.normalizedWorkshopTypes
        let requestShowOnly = showOnly
        let requestFavoriteIDs = SteamServiceManager.shared.workshopFavoriteIDs
        let requestAgeRating = ageRatingFilter
        let requestWidescreenResolution = widescreenResolution
        let requestUltraWidescreenResolution = ultraWidescreenResolution
        let requestDualscreenResolution = dualscreenResolution
        let requestTriplescreenResolution = triplescreenResolution
        let requestPortraitResolution = portraitResolution
        let requestMiscResolution = miscResolution
        let requestTrendPeriod = trendPeriod
        let requestPage = currentPage

        if requestShowOnly.contains(.myFavourites), !SteamServiceManager.shared.isLoggedIn {
            items = []
            totalItems = 0
            isLoading = false
            error = L("需要登录 Steam")
            return
        }

        isLoading = true
        error = nil
        pageNavigationMessage = nil
        onBrowsingAPIStateChange(.checking)

        searchTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result: (items: [WorkshopItem], total: Int)
                var matchedCreator: WorkshopCreator?
                if Self.isPublishedFileID(requestSearchText) {
                    let details = try await SteamWebAPI.shared.getFileDetails(
                        workshopIds: [requestSearchText]
                    )
                    let items = details.filter {
                        $0.publishedFileId == requestSearchText &&
                            $0.consumerAppId == 431960 &&
                            requestShowOnly.matches(
                                workshopItem: $0,
                                favoriteIDs: requestFavoriteIDs
                            )
                    }
                    result = (items, items.count)
                } else if Self.isSteamUserID(requestSearchText) {
                    matchedCreator = await SteamWebAPI.shared.creatorProfile(
                        steamId: requestSearchText
                    )
                    result = ([], 0)
                } else {
                    result = try await SteamWebAPI.shared.queryFiles(
                        searchText: requestSearchText,
                        tags: requestTags,
                        sortOrder: requestSortOrder,
                        typeFilters: requestTypeFilters,
                        ageRating: requestAgeRating,
                        widescreenResolution: requestWidescreenResolution,
                        ultraWidescreenResolution: requestUltraWidescreenResolution,
                        dualscreenResolution: requestDualscreenResolution,
                        triplescreenResolution: requestTriplescreenResolution,
                        portraitResolution: requestPortraitResolution,
                        miscResolution: requestMiscResolution,
                        showOnly: requestShowOnly,
                        favoriteIDs: requestFavoriteIDs,
                        page: requestPage,
                        perPage: self.itemsPerPage,
                        trendDays: requestSortOrder.usesTrendPeriod
                            ? requestTrendPeriod.rawValue
                            : nil
                    )
                }

                guard !Task.isCancelled, requestGeneration == self.searchGeneration else { return }
                if requestPage > 1, result.items.isEmpty, !self.items.isEmpty {
                    let retainedPage = self.loadedPage
                    if result.total > 0 {
                        self.totalItems = result.total
                    }
                    self.currentPage = retainedPage
                    self.pageNavigationMessage = L(
                        "Steam 没有返回第 %d 页，已保留第 %d 页。",
                        requestPage,
                        retainedPage
                    )
                    self.isLoading = false
                    self.onBrowsingAPIStateChange(.available(L("Steam Web API 可用")))
                    return
                }
                self.items = result.items
                self.totalItems = result.total
                self.loadedPage = requestPage
                self.rememberCreators(in: result.items)
                self.onItemsLoaded(result.items)
                if let matchedCreator {
                    self.rememberCreator(matchedCreator)
                }
                self.isLoading = false
                self.onBrowsingAPIStateChange(.available(L("Steam Web API 可用")))
            } catch {
                guard !Task.isCancelled, requestGeneration == self.searchGeneration else { return }
                self.error = error.localizedDescription
                self.isLoading = false
                self.onBrowsingAPIStateChange(.unavailable(error.localizedDescription))
            }
        }
    }

    func refresh() {
        search()
    }

    func reloadFromFirstPage(clearingItems: Bool = false) {
        if clearingItems {
            items = []
        }
        currentPage = 1
        search()
    }

    func submitSearch() {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        if let creator = knownCreators.first(where: {
            $0.steamId == query || $0.name.caseInsensitiveCompare(query) == .orderedSame
        }) {
            openCreatorWorkshop(creator)
            return
        }
        if Self.isSteamUserID(query) {
            openCreatorWorkshop(WorkshopCreator(steamId: query, name: query))
            return
        }
        currentPage = 1
        search()
    }

    func clearSearch() {
        searchText = ""
        currentPage = 1
        search()
    }

    func selectSort(_ order: WorkshopSortOrder, period: WorkshopTrendPeriod? = nil) {
        sortOrder = order
        if let period {
            trendPeriod = period
        }
        currentPage = 1
        search()
    }

    func loadNextPage() {
        guard currentPage < totalPages else { return }
        currentPage += 1
        search()
    }

    func loadPreviousPage() {
        guard currentPage > 1 else { return }
        currentPage -= 1
        search()
    }

    func goToPage(_ page: Int) {
        let clamped = max(1, min(page, totalPages))
        guard clamped != currentPage else { return }
        currentPage = clamped
        search()
    }

    func setTypeFilter(_ filter: WorkshopTypeFilter, isOn: Bool) {
        let updated = Self.updatedTypeSelection(
            selectedTypeFilters,
            filter: filter,
            isOn: isOn
        )
        guard updated != selectedTypeFilters else { return }
        selectedTypeFilters = updated
        currentPage = 1
        search()
    }

    func applyTagFilter(_ tag: String) {
        if selectedTags.contains(tag) {
            selectedTags.remove(tag)
        } else {
            selectedTags.insert(tag)
        }
        currentPage = 1
        search()
    }

    func selectAllTags() {
        selectedTags = Set(WorkshopTag.allCases.map(\.rawValue))
        currentPage = 1
        search()
    }

    func clearTags() {
        selectedTags.removeAll()
        currentPage = 1
        search()
    }

    func applyAgeRatingFilter(_ rating: WorkshopAgeRating, isOn: Bool) {
        let bit = WorkshopAgeRatingFilter.bit(for: rating)
        var updated = ageRatingFilter
        if isOn {
            updated.insert(bit)
        } else {
            updated.remove(bit)
        }
        guard updated != ageRatingFilter else { return }
        ageRatingFilter = updated
        currentPage = 1
        search()
    }

    func setResolutionOption<Filter: FilterResultsModel>(
        _ keyPath: ReferenceWritableKeyPath<WorkshopBrowseStore, Filter>,
        option: Filter,
        isOn: Bool
    ) {
        var value = self[keyPath: keyPath]
        if isOn {
            value.insert(option)
        } else {
            value.remove(option)
        }
        self[keyPath: keyPath] = value
        currentPage = 1
        search()
    }

    func selectAllResolutions() {
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        portraitResolution = .all
        miscResolution = .all
        currentPage = 1
        search()
    }

    func clearResolutions() {
        widescreenResolution = .none
        ultraWidescreenResolution = .none
        dualscreenResolution = .none
        triplescreenResolution = .none
        portraitResolution = .none
        miscResolution = .none
        currentPage = 1
        search()
    }

    func clearFilters() {
        selectedTags.removeAll()
        searchText = ""
        selectedTypeFilters = [.all]
        showOnly = .none
        sortOrder = .trending
        trendPeriod = .week
        ageRatingFilter = .default
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        portraitResolution = .all
        miscResolution = .all
        currentPage = 1
        search()
    }

    func setShowOnly(_ option: FRShowOnly, isOn: Bool) {
        if isOn {
            showOnly.insert(option)
        } else {
            showOnly.remove(option)
        }
        currentPage = 1
        if showOnly.contains(.myFavourites) {
            SteamServiceManager.shared.refreshWorkshopFavorites()
        }
        search()
    }

    func favoritesDidChange() {
        if showOnly.contains(.myFavourites) {
            currentPage = 1
            search()
        }
    }

    func navigateToTag(_ tag: String, trendPeriod: WorkshopTrendPeriod = .week) {
        selectedTags = [tag]
        searchText = ""
        selectedTypeFilters = [.all]
        sortOrder = .trending
        self.trendPeriod = trendPeriod
        currentPage = 1
        search()
    }

    func navigateToSort(
        _ sort: WorkshopSortOrder,
        trendPeriod: WorkshopTrendPeriod = .week
    ) {
        selectedTags.removeAll()
        searchText = ""
        selectedTypeFilters = [.all]
        sortOrder = sort
        self.trendPeriod = trendPeriod
        currentPage = 1
        search()
    }

    func rememberCreators(in items: [WorkshopItem]) {
        var creators = Dictionary(uniqueKeysWithValues: knownCreators.map { ($0.id, $0) })
        for item in items {
            guard let creator = WorkshopCreator(item: item) else { continue }
            creators[creator.id] = creator
        }
        let updated = creators.values.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
        if knownCreators != updated {
            knownCreators = updated
        }
    }

    func rememberCreator(_ creator: WorkshopCreator) {
        var creators = Dictionary(uniqueKeysWithValues: knownCreators.map { ($0.id, $0) })
        creators[creator.id] = creator
        let updated = creators.values.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
        if knownCreators != updated {
            knownCreators = updated
        }
    }

    func openCreatorWorkshop(_ creator: WorkshopCreator) {
        guard let url = creator.workshopURL else { return }
        NSWorkspace.shared.open(url)
    }

    func openCreatorWorkshop(for item: WorkshopItem) {
        guard let creator = WorkshopCreator(item: item) else { return }
        openCreatorWorkshop(creator)
    }

    private func scheduleSearch() {
        debounceTask?.cancel()
        debounceTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(500))
            guard !Task.isCancelled else { return }
            self?.currentPage = 1
            self?.search()
        }
    }

    private static func updatedTypeSelection(
        _ selection: Set<WorkshopTypeFilter>,
        filter: WorkshopTypeFilter,
        isOn: Bool
    ) -> Set<WorkshopTypeFilter> {
        var updated = selection.normalizedWorkshopTypes
        if filter == .all {
            return isOn ? [.all] : updated
        }
        if isOn {
            updated.remove(.all)
            updated.insert(filter)
        } else {
            updated.remove(filter)
            if updated.isEmpty {
                updated = [.all]
            }
        }
        return updated
    }

    private static func isSteamUserID(_ value: String) -> Bool {
        value.count == 17 && value.hasPrefix("7656119") && value.allSatisfy(\.isNumber)
    }

    private static func isPublishedFileID(_ value: String) -> Bool {
        guard !value.isEmpty,
              !isSteamUserID(value),
              value.allSatisfy(\.isNumber),
              let number = UInt64(value) else { return false }
        return number > 0
    }
}
