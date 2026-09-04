//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Observation

struct SubscriptionDownloadPlan: Equatable {
    let subscriptionCount: Int
    let remainingCount: Int
    let items: [WorkshopItem]

    var downloadCount: Int { items.count }
}

@MainActor
@Observable
final class WorkshopSubscriptionStatus {
    let workshopID: String
    private(set) var state: WorkshopSubscriptionState = .unknown
    private(set) var isChecking = false
    private(set) var isChanging = false
    private(set) var actionError: String?

    init(workshopID: String) {
        self.workshopID = workshopID
    }

    fileprivate func beginChecking() {
        isChecking = true
    }

    fileprivate func finishChecking(state: WorkshopSubscriptionState?) {
        if let state {
            self.state = state
        }
        isChecking = false
    }

    fileprivate func beginChange() {
        isChanging = true
        actionError = nil
    }

    fileprivate func finishChange(
        state: WorkshopSubscriptionState? = nil,
        error: String? = nil
    ) {
        if let state {
            self.state = state
        }
        isChanging = false
        actionError = error
    }

    fileprivate func setState(_ state: WorkshopSubscriptionState) {
        self.state = state
    }

    fileprivate func setError(_ error: String?) {
        actionError = error
    }

    fileprivate func reset() {
        state = .unknown
        isChecking = false
        isChanging = false
        actionError = nil
    }
}

@MainActor
@Observable
final class SubscriptionStore {
    private(set) var records: [WorkshopSubscription] = []
    private(set) var catalogItems: [WorkshopItem] = []
    private(set) var items: [WorkshopItem] = []
    private(set) var total = 0
    private(set) var startIndex = 0
    private(set) var isLoading = false
    private(set) var error: String?

    var searchText = "" {
        didSet {
            guard searchText != oldValue else { return }
            scheduleFilterRefresh()
        }
    }
    var selectedTags: Set<String> = []
    var selectedTypeFilters: Set<WorkshopTypeFilter> = [.all]
    var showOnly: FRShowOnly = .none {
        didSet {
            guard showOnly != oldValue else { return }
            UserDefaults.standard.set(showOnly.rawValue, forKey: Self.showOnlyStorageKey)
        }
    }
    var ageRatingFilter: WorkshopAgeRatingFilter = .all
    var widescreenResolution = FRWidescreenResolution.all
    var ultraWidescreenResolution = FRUltraWidescreenResolution.all
    var dualscreenResolution = FRDualscreenResolution.all
    var triplescreenResolution = FRTriplescreenResolution.all
    var portraitResolution = FRPortraitScreenResolution.all
    var miscResolution = FRMiscResolution.all

    private(set) var isPreparingDownloads = false
    private(set) var downloadPlan: SubscriptionDownloadPlan?

    @ObservationIgnored private var statuses: [String: WorkshopSubscriptionStatus] = [:]
    @ObservationIgnored private var refreshTask: Task<Void, Never>?
    @ObservationIgnored private var filterTask: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var sessionGeneration = 0
    @ObservationIgnored private var isSteamReady: () -> Bool = { false }
    @ObservationIgnored private var selectedItemID: () -> String? = { nil }
    @ObservationIgnored private var isInstalled: (String) -> Bool = { _ in false }
    @ObservationIgnored private var pendingDownloadIDs: () -> Set<String> = { [] }
    @ObservationIgnored private var openSteamSetup: () -> Void = {}
    @ObservationIgnored private var downloadItem: (WorkshopItem, DownloadPurpose) -> Void = { _, _ in }
    @ObservationIgnored private var cancelDownload: (String) -> Void = { _ in }
    @ObservationIgnored private var onItemsLoaded: ([WorkshopItem]) -> Void = { _ in }
    @ObservationIgnored private var onLibraryChanged: () -> Void = {}

    private static let showOnlyStorageKey = "SubscriptionShowOnlyV2"

    init() {
        if let raw = UserDefaults.standard.object(forKey: Self.showOnlyStorageKey) as? Int {
            showOnly = FRShowOnly(rawValue: raw & FRShowOnly.all.rawValue)
        }
    }

    func configure(
        isSteamReady: @escaping () -> Bool,
        selectedItemID: @escaping () -> String?,
        isInstalled: @escaping (String) -> Bool,
        pendingDownloadIDs: @escaping () -> Set<String>,
        openSteamSetup: @escaping () -> Void,
        downloadItem: @escaping (WorkshopItem, DownloadPurpose) -> Void,
        cancelDownload: @escaping (String) -> Void,
        onItemsLoaded: @escaping ([WorkshopItem]) -> Void,
        onLibraryChanged: @escaping () -> Void
    ) {
        self.isSteamReady = isSteamReady
        self.selectedItemID = selectedItemID
        self.isInstalled = isInstalled
        self.pendingDownloadIDs = pendingDownloadIDs
        self.openSteamSetup = openSteamSetup
        self.downloadItem = downloadItem
        self.cancelDownload = cancelDownload
        self.onItemsLoaded = onItemsLoaded
        self.onLibraryChanged = onLibraryChanged
    }

    func status(for workshopID: String) -> WorkshopSubscriptionStatus {
        if let status = statuses[workshopID] {
            return status
        }
        let status = WorkshopSubscriptionStatus(workshopID: workshopID)
        statuses[workshopID] = status
        return status
    }

    var pageSize: Int {
        let value = UserDefaults.standard.integer(forKey: "WallpapersPerPage")
        return [10, 25, 50].contains(value) ? value : 50
    }

    var canLoadPrevious: Bool {
        startIndex > 0 && !isLoading
    }

    var canLoadNext: Bool {
        !isLoading && startIndex + pageSize < total
    }

    var pageCount: Int {
        max(1, (total + pageSize - 1) / pageSize)
    }

    var currentPage: Int {
        min(pageCount, startIndex / pageSize + 1)
    }

    var allResolutionsSelected: Bool {
        widescreenResolution == .all &&
            ultraWidescreenResolution == .all &&
            dualscreenResolution == .all &&
            triplescreenResolution == .all &&
            portraitResolution == .all &&
            miscResolution == .all
    }

    var allResolutionsCleared: Bool {
        widescreenResolution.isEmpty &&
            ultraWidescreenResolution.isEmpty &&
            dualscreenResolution.isEmpty &&
            triplescreenResolution.isEmpty &&
            portraitResolution.isEmpty &&
            miscResolution.isEmpty
    }

    var allTagsSelected: Bool {
        Set(WorkshopTag.allCases.map(\.rawValue)).isSubset(of: selectedTags)
    }

    var hasActiveFilters: Bool {
        !showOnly.isEmpty ||
            !searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
            (!selectedTags.isEmpty && !allTagsSelected) ||
            selectedTypeFilters.normalizedWorkshopTypes != [.all] ||
            (!ageRatingFilter.isEmpty && ageRatingFilter != .all) ||
            !allResolutionsSelected
    }

    func refresh(startIndex requestedStartIndex: Int? = nil) {
        guard SteamServiceManager.shared.isLoggedIn else {
            resetCatalog(error: L("需要登录 Steam"))
            return
        }
        refreshTask?.cancel()
        generation += 1
        let requestGeneration = generation
        let requestedStart = max(0, requestedStartIndex ?? startIndex)
        isLoading = true
        error = nil

        refreshTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                var records: [WorkshopSubscription] = []
                var seen = Set<String>()
                var serviceStart = 0
                var serviceTotal = Int.max
                while serviceStart < serviceTotal {
                    let page = try await self.subscriptionPage(startIndex: serviceStart)
                    guard !Task.isCancelled, requestGeneration == self.generation else { return }
                    serviceTotal = page.total
                    for record in page.subscriptions where seen.insert(record.publishedFileId).inserted {
                        records.append(record)
                    }
                    guard !page.subscriptions.isEmpty else { break }
                    let nextStart = page.startIndex + page.subscriptions.count
                    guard nextStart > serviceStart else { break }
                    serviceStart = nextStart
                }

                let loaded: [WorkshopItem]
                do {
                    loaded = try await self.loadItems(for: records)
                } catch {
                    loaded = records.map { WorkshopItem.unavailableSubscription(id: $0.publishedFileId) }
                    self.error = error.localizedDescription
                }
                guard !Task.isCancelled, requestGeneration == self.generation else { return }
                let subscribedIDs = Set(records.map(\.publishedFileId))
                let removedIDs = Set(self.records.map(\.publishedFileId)).subtracting(subscribedIDs)
                self.records = records
                self.catalogItems = loaded
                for id in removedIDs {
                    self.status(for: id).setState(.unsubscribed)
                }
                for record in records {
                    self.status(for: record.publishedFileId).setState(.subscribed)
                }
                self.onItemsLoaded(loaded)
                self.rebuildPage(startIndex: requestedStart)
                self.isLoading = false
            } catch {
                guard !Task.isCancelled, requestGeneration == self.generation else { return }
                self.error = error.localizedDescription
                self.isLoading = false
            }
        }
    }

    func loadPrevious() {
        guard canLoadPrevious else { return }
        goToPage(currentPage - 1)
    }

    func loadNext() {
        guard canLoadNext else { return }
        goToPage(currentPage + 1)
    }

    func goToPage(_ page: Int) {
        let target = min(max(page, 1), pageCount)
        guard !isLoading, target != currentPage else { return }
        rebuildPage(startIndex: (target - 1) * pageSize)
    }

    func pageSizeDidChange() {
        rebuildPage(startIndex: 0)
    }

    func refreshFilters() {
        filterTask?.cancel()
        rebuildPage(startIndex: 0)
    }

    func favoritesDidChange() {
        if showOnly.contains(.myFavourites) {
            refreshFilters()
        }
    }

    func setShowOnly(_ option: FRShowOnly, isOn: Bool) {
        if isOn {
            showOnly.insert(option)
        } else {
            showOnly.remove(option)
        }
        if showOnly.contains(.myFavourites) {
            SteamServiceManager.shared.refreshWorkshopFavorites()
        }
        refreshFilters()
    }

    func setTypeFilter(_ filter: WorkshopTypeFilter, isOn: Bool) {
        let updated = Self.updatedTypeSelection(selectedTypeFilters, filter: filter, isOn: isOn)
        guard updated != selectedTypeFilters else { return }
        selectedTypeFilters = updated
        refreshFilters()
    }

    func applyAgeRatingFilter(_ rating: WorkshopAgeRating, isOn: Bool) {
        var updated = ageRatingFilter
        let option = WorkshopAgeRatingFilter.bit(for: rating)
        if isOn {
            updated.insert(option)
        } else {
            updated.remove(option)
        }
        guard updated != ageRatingFilter else { return }
        ageRatingFilter = updated
        refreshFilters()
    }

    func setResolutionOption<Filter: FilterResultsModel>(
        _ keyPath: ReferenceWritableKeyPath<SubscriptionStore, Filter>,
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
        refreshFilters()
    }

    func selectAllResolutions() {
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        portraitResolution = .all
        miscResolution = .all
        refreshFilters()
    }

    func clearResolutions() {
        widescreenResolution = .none
        ultraWidescreenResolution = .none
        dualscreenResolution = .none
        triplescreenResolution = .none
        portraitResolution = .none
        miscResolution = .none
        refreshFilters()
    }

    func selectAllTags() {
        selectedTags = Set(WorkshopTag.allCases.map(\.rawValue))
        refreshFilters()
    }

    func clearTags() {
        selectedTags.removeAll()
        refreshFilters()
    }

    func applyTagFilter(_ tag: String) {
        if selectedTags.contains(tag) {
            selectedTags.remove(tag)
        } else {
            selectedTags.insert(tag)
        }
        refreshFilters()
    }

    func clearFilters() {
        searchText = ""
        selectedTags.removeAll()
        selectedTypeFilters = [.all]
        showOnly = .none
        ageRatingFilter = .all
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        portraitResolution = .all
        miscResolution = .all
        refreshFilters()
    }

    func prepareDownloadAll() {
        guard SteamServiceManager.shared.isLoggedIn, !isPreparingDownloads else {
            if !SteamServiceManager.shared.isLoggedIn { openSteamSetup() }
            return
        }
        isPreparingDownloads = true
        downloadPlan = nil
        error = nil
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.isPreparingDownloads = false }
            do {
                let loaded: [WorkshopItem]
                let subscriptionCount: Int
                if !self.catalogItems.isEmpty {
                    loaded = self.catalogItems
                    subscriptionCount = self.records.count
                } else {
                    var allRecords: [WorkshopSubscription] = []
                    var startIndex = 0
                    var total = Int.max
                    while startIndex < total {
                        let page = try await self.subscriptionPage(startIndex: startIndex)
                        total = page.total
                        allRecords.append(contentsOf: page.subscriptions)
                        guard !page.subscriptions.isEmpty else { break }
                        startIndex = page.startIndex + page.subscriptions.count
                    }
                    loaded = try await self.loadItems(for: allRecords)
                    subscriptionCount = total == Int.max ? allRecords.count : total
                }
                let activeIDs = self.pendingDownloadIDs()
                var seen = Set<String>()
                let remaining = loaded.filter {
                    seen.insert($0.publishedFileId).inserted &&
                        $0.kind != .unsupported &&
                        !self.isInstalled($0.publishedFileId)
                }
                let pending = remaining.filter { !activeIDs.contains($0.publishedFileId) }
                self.downloadPlan = SubscriptionDownloadPlan(
                    subscriptionCount: subscriptionCount,
                    remainingCount: remaining.count,
                    items: pending
                )
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    func confirmDownloads() {
        guard let plan = downloadPlan else { return }
        downloadPlan = nil
        for item in plan.items {
            downloadItem(item, .subscription)
        }
    }

    func dismissDownloadPlan() {
        downloadPlan = nil
    }

    func refreshStates(for items: [WorkshopItem]) {
        guard SteamServiceManager.shared.isLoggedIn else { return }
        let requestSession = sessionGeneration
        let ids = Set(items.map(\.publishedFileId)).filter { id in
            guard !id.isEmpty else { return false }
            let status = status(for: id)
            return !status.isChecking && !status.isChanging
        }
        guard !ids.isEmpty else { return }
        if let selectedID = selectedItemID(), ids.contains(selectedID) {
            status(for: selectedID).setError(nil)
        }
        for id in ids {
            status(for: id).beginChecking()
        }
        SteamServiceManager.shared.fetchSubscriptionStates(workshopIds: Array(ids)) { [weak self] result in
            guard let self, requestSession == self.sessionGeneration else { return }
            switch result {
            case .success(let states):
                for id in ids {
                    self.status(for: id).finishChecking(
                        state: states[id] == true ? .subscribed : .unsubscribed
                    )
                }
            case .failure(let error):
                for id in ids {
                    self.status(for: id).finishChecking(state: nil)
                }
                if let selectedID = self.selectedItemID(), ids.contains(selectedID) {
                    self.status(for: selectedID).setError(error.localizedDescription)
                }
            }
        }
    }

    func subscribe(_ item: WorkshopItem) {
        guard isSteamReady() else {
            openSteamSetup()
            return
        }
        let id = item.publishedFileId
        let status = status(for: id)
        guard !status.isChanging else { return }
        let requestSession = sessionGeneration
        status.beginChange()
        SteamServiceManager.shared.subscribe(workshopId: id) { [weak self] result in
            guard let self, requestSession == self.sessionGeneration else { return }
            switch result {
            case .success:
                status.finishChange(state: .subscribed)
                if !self.isInstalled(id) {
                    self.downloadItem(item, .subscription)
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                    self?.refresh(startIndex: 0)
                }
            case .failure(let error):
                status.finishChange(error: error.localizedDescription)
            }
        }
    }

    func unsubscribe(_ item: WorkshopItem) {
        let id = item.publishedFileId
        let status = status(for: id)
        guard isSteamReady(), !status.isChanging else {
            if !isSteamReady() { openSteamSetup() }
            return
        }
        let requestSession = sessionGeneration
        status.beginChange()
        SteamServiceManager.shared.unsubscribe(workshopId: id) { [weak self] result in
            guard let self, requestSession == self.sessionGeneration else { return }
            switch result {
            case .success:
                status.finishChange(state: .unsubscribed)
                self.cancelDownload(id)
                self.records.removeAll { $0.publishedFileId == id }
                self.catalogItems.removeAll { $0.publishedFileId == id }
                self.rebuildPage(startIndex: self.startIndex)
                do {
                    try WallpaperLibrary.shared.removeManagedWorkshopItem(workshopId: id)
                    self.onLibraryChanged()
                } catch {
                    status.setError(L(
                        "已取消订阅，但无法删除 Mirage 下载副本：%@",
                        error.localizedDescription
                    ))
                }
            case .failure(let error):
                status.finishChange(error: error.localizedDescription)
            }
        }
    }

    func reset() {
        refreshTask?.cancel()
        filterTask?.cancel()
        generation += 1
        sessionGeneration += 1
        statuses.values.forEach { $0.reset() }
        statuses.removeAll()
        records = []
        catalogItems = []
        items = []
        total = 0
        startIndex = 0
        searchText = ""
        selectedTags = []
        selectedTypeFilters = [.all]
        ageRatingFilter = .all
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        portraitResolution = .all
        miscResolution = .all
        isLoading = false
        error = nil
        isPreparingDownloads = false
        downloadPlan = nil
        filterTask?.cancel()
        filterTask = nil
    }

    private func resetCatalog(error: String?) {
        records = []
        catalogItems = []
        items = []
        total = 0
        startIndex = 0
        self.error = error
        isLoading = false
    }

    private func scheduleFilterRefresh() {
        filterTask?.cancel()
        filterTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            self?.rebuildPage(startIndex: 0)
        }
    }

    private func subscriptionPage(startIndex: Int) async throws -> WorkshopSubscriptionPage {
        try await withCheckedThrowingContinuation { continuation in
            SteamServiceManager.shared.fetchSubscriptions(startIndex: startIndex) {
                continuation.resume(with: $0)
            }
        }
    }

    private func loadItems(for records: [WorkshopSubscription]) async throws -> [WorkshopItem] {
        var details: [WorkshopItem] = []
        let ids = records.map(\.publishedFileId)
        for start in stride(from: 0, to: ids.count, by: 100) {
            let end = min(start + 100, ids.count)
            details.append(contentsOf: try await SteamWebAPI.shared.getFileDetails(
                workshopIds: Array(ids[start..<end])
            ))
        }
        let byID = Dictionary(
            details.map { ($0.publishedFileId, $0) },
            uniquingKeysWith: { first, _ in first }
        )
        return records.map {
            byID[$0.publishedFileId] ?? .unavailableSubscription(id: $0.publishedFileId)
        }
    }

    private func rebuildPage(startIndex: Int) {
        let filtered = catalogItems.filter(matchesFilters)
        let maximumStart = filtered.isEmpty ? 0 : (filtered.count - 1) / pageSize * pageSize
        let clampedStart = min(max(0, startIndex), maximumStart)
        total = filtered.count
        self.startIndex = clampedStart
        items = Array(filtered.dropFirst(clampedStart).prefix(pageSize))
    }

    private func matchesFilters(_ item: WorkshopItem) -> Bool {
        if !showOnly.matches(
            workshopItem: item,
            favoriteIDs: SteamServiceManager.shared.workshopFavoriteIDs
        ) {
            return false
        }

        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !query.isEmpty {
            let searchableValues = [
                item.title,
                item.itemDescription,
                item.creatorDisplayName,
                item.creatorSteamId,
                item.publishedFileId
            ] + item.tags
            guard searchableValues.contains(where: {
                $0.localizedCaseInsensitiveContains(query)
            }) else { return false }
        }

        guard selectedTypeFilters.matches(item) else { return false }

        if !ageRatingFilter.isEmpty,
           ageRatingFilter != .all,
           !ageRatingFilter.contains(item.ageRating ?? .everyone) {
            return false
        }

        let selectableTags = Set(WorkshopTag.allCases.map(\.rawValue))
        if !selectedTags.isEmpty, !selectableTags.isSubset(of: selectedTags) {
            let itemTags = Set(item.tags.map { $0.lowercased() })
            guard selectedTags.contains(where: { itemTags.contains($0.lowercased()) }) else {
                return false
            }
        }

        return FRResolutionFilter.matches(
            tags: item.tags,
            widescreen: widescreenResolution,
            ultraWidescreen: ultraWidescreenResolution,
            dualscreen: dualscreenResolution,
            triplescreen: triplescreenResolution,
            portrait: portraitResolution,
            misc: miscResolution
        )
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
}
