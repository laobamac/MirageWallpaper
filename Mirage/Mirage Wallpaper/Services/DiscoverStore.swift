//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Observation

@MainActor
@Observable
final class DiscoverStore {
    private(set) var rows: [DiscoverRow] = []
    private(set) var browse: DiscoverBrowseState?
    var searchText = ""
    private(set) var isLoading = false
    private(set) var error: String?
    private(set) var selectedItemID: String?
    private(set) var isDetailLoading = false
    private(set) var detailError: String?

    @ObservationIgnored private var feedTask: Task<Void, Never>?
    @ObservationIgnored private var rowTasks: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var rowRequestIDs: [String: UUID] = [:]
    @ObservationIgnored private var browseTask: Task<Void, Never>?
    @ObservationIgnored private var browseRequestID: UUID?
    @ObservationIgnored private var detailTask: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var prepareSelection: () -> Int = { 0 }
    @ObservationIgnored private var isSelectionCurrent: (Int) -> Bool = { _ in false }
    @ObservationIgnored private var onItemsLoaded: ([WorkshopItem]) -> Void = { _ in }
    @ObservationIgnored private var onDetailLoaded: (WorkshopItem) -> Void = { _ in }

    func configure(
        prepareSelection: @escaping () -> Int,
        isSelectionCurrent: @escaping (Int) -> Bool,
        onItemsLoaded: @escaping ([WorkshopItem]) -> Void,
        onDetailLoaded: @escaping (WorkshopItem) -> Void
    ) {
        self.prepareSelection = prepareSelection
        self.isSelectionCurrent = isSelectionCurrent
        self.onItemsLoaded = onItemsLoaded
        self.onDetailLoaded = onDetailLoaded
    }

    func load(force: Bool = false) {
        if isLoading && !force { return }
        feedTask?.cancel()
        rowTasks.values.forEach { $0.cancel() }
        rowTasks.removeAll()
        rowRequestIDs.removeAll()
        generation += 1
        let requestGeneration = generation
        isLoading = true
        error = nil

        feedTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let definitions = try await WallpaperEngineExploreAPI.shared.fetch(force: force)
                guard !Task.isCancelled, requestGeneration == self.generation else { return }
                self.rows = DiscoverFeedBuilder.build(definitions: definitions)
            } catch {
                guard !Task.isCancelled, requestGeneration == self.generation else { return }
                self.rows = []
                self.error = error.localizedDescription
            }
            if requestGeneration == self.generation {
                self.isLoading = false
            }
        }
    }

    func refresh() {
        if let browse {
            loadBrowsePage(browse.page)
            return
        }
        load(force: true)
    }

    func performSearch() {
        let search = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !search.isEmpty else { return }
        open(query: DiscoverFeedBuilder.searchRow(text: search).query)
    }

    func clearSearch() {
        searchText = ""
    }

    func loadRow(id: String) {
        requestRow(id: id, page: 1, replacing: true)
    }

    func openRow(id: String) {
        guard let query = rows.first(where: { $0.id == id })?.query else { return }
        open(query: query)
    }

    func closeBrowse() {
        browseTask?.cancel()
        browseTask = nil
        browseRequestID = nil
        browse = nil
    }

    func goToBrowsePage(_ page: Int) {
        guard let browse else { return }
        let clamped = max(1, min(page, browse.totalPages))
        guard clamped != browse.page else { return }
        loadBrowsePage(clamped)
    }

    func select(_ item: WorkshopItem) {
        let selectionGeneration = prepareSelection()
        detailTask?.cancel()
        selectedItemID = item.publishedFileId
        detailError = nil
        isDetailLoading = true

        detailTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let details = try await SteamWebAPI.shared.getFileDetails(
                    workshopIds: [item.publishedFileId]
                )
                guard !Task.isCancelled,
                      self.isSelectionCurrent(selectionGeneration),
                      self.selectedItemID == item.publishedFileId else { return }
                guard let detail = details.first else {
                    throw SteamAPIError.invalidResponse
                }
                self.isDetailLoading = false
                self.onItemsLoaded([detail])
                self.onDetailLoaded(detail)
            } catch {
                guard !Task.isCancelled,
                      self.isSelectionCurrent(selectionGeneration),
                      self.selectedItemID == item.publishedFileId else { return }
                self.isDetailLoading = false
                self.detailError = error.localizedDescription
            }
        }
    }

    private func open(query: DiscoverQuery) {
        browseTask?.cancel()
        browse = DiscoverBrowseState(query: query)
        loadBrowsePage(1)
    }

    private func loadBrowsePage(_ page: Int) {
        guard var browse else { return }
        let clamped = max(1, min(page, browse.totalPages))
        let requestID = UUID()
        browseTask?.cancel()
        browseRequestID = requestID
        browse.page = clamped
        browse.items = []
        browse.isLoading = true
        browse.error = nil
        self.browse = browse

        browseTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result = try await self.fetchResults(
                    query: browse.query,
                    page: clamped,
                    perPage: 50
                )
                guard !Task.isCancelled,
                      self.browseRequestID == requestID,
                      self.browse?.query.id == browse.query.id else { return }
                var updated = self.browse ?? browse
                updated.items = result.items
                updated.total = result.total
                updated.page = min(clamped, updated.totalPages)
                updated.isLoading = false
                self.browse = updated
                self.onItemsLoaded(result.items)
            } catch {
                guard !Task.isCancelled,
                      self.browseRequestID == requestID,
                      self.browse?.query.id == browse.query.id else { return }
                var updated = self.browse ?? browse
                updated.isLoading = false
                updated.error = error.localizedDescription
                self.browse = updated
            }
        }
    }

    private func requestRow(id: String, page: Int, replacing: Bool) {
        guard rowTasks[id] == nil,
              let index = rows.firstIndex(where: { $0.id == id }) else { return }
        if replacing && !rows[index].items.isEmpty { return }
        let query = rows[index].query
        let requestGeneration = generation
        let requestID = UUID()
        rowRequestIDs[id] = requestID
        rows[index].isLoading = true
        rows[index].error = nil

        rowTasks[id] = Task { @MainActor [weak self] in
            guard let self else { return }
            defer {
                if self.rowRequestIDs[id] == requestID {
                    self.rowTasks[id] = nil
                    self.rowRequestIDs[id] = nil
                }
            }
            do {
                let result = try await self.fetchResults(query: query, page: page, perPage: 12)
                guard !Task.isCancelled,
                      requestGeneration == self.generation,
                      let currentIndex = self.rows.firstIndex(where: { $0.id == id }) else {
                    return
                }
                let visibleItems = result.items
                if replacing {
                    self.rows[currentIndex].items = visibleItems
                } else {
                    let existing = Set(self.rows[currentIndex].items.map(\.id))
                    self.rows[currentIndex].items.append(contentsOf: visibleItems.filter {
                        !existing.contains($0.id)
                    })
                }
                self.rows[currentIndex].total = result.total
                self.rows[currentIndex].page = page
                self.rows[currentIndex].isLoading = false
                if case .creator(_, let sortMethod) = query.kind,
                   let creatorName = visibleItems.first?.creatorName,
                   !creatorName.isEmpty {
                    self.rows[currentIndex].query.title = sortMethod == "newestfirst"
                        ? L("来自 %@ 的最新壁纸", creatorName)
                        : L("来自 %@ 的热门壁纸", creatorName)
                }
                self.onItemsLoaded(visibleItems)
            } catch {
                guard !Task.isCancelled,
                      requestGeneration == self.generation,
                      let currentIndex = self.rows.firstIndex(where: { $0.id == id }) else {
                    return
                }
                self.rows[currentIndex].isLoading = false
                self.rows[currentIndex].error = error.localizedDescription
            }
        }
    }

    private func fetchResults(
        query: DiscoverQuery,
        page: Int,
        perPage: Int
    ) async throws -> (items: [WorkshopItem], total: Int) {
        let result: (items: [WorkshopItem], total: Int)
        switch query.kind {
        case .workshop:
            result = try await SteamWebAPI.shared.queryDiscoverFiles(
                searchText: query.searchText,
                sortOrder: query.sortOrder,
                requiredTags: query.requiredTags,
                excludedTags: query.excludedTags,
                page: page,
                perPage: perPage,
                trendDays: query.trendDays
            )
        case .creator(let steamID, let sortMethod):
            result = try await SteamWebAPI.shared.getUserFiles(
                steamId: steamID,
                page: page,
                perPage: perPage,
                sortMethod: sortMethod,
                requiredTags: query.requiredTags,
                excludedTags: query.excludedTags,
                enrichCreatorProfiles: true
            )
        case .collection(let collectionID):
            result = try await SteamWebAPI.shared.getCollectionItems(
                collectionId: collectionID,
                page: page,
                perPage: perPage
            )
        }
        guard query.exact else { return result }
        let items = result.items.filter { item in
            item.title.localizedCaseInsensitiveContains(query.searchText) ||
                item.itemDescription.localizedCaseInsensitiveContains(query.searchText)
        }
        return (items, result.total)
    }
}
