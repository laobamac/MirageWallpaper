//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Observation

@MainActor
@Observable
final class WorkshopCreatorStore {
    private(set) var selectedCreator: WorkshopCreator?
    private(set) var items: [WorkshopItem] = []
    private(set) var isLoading = false
    private(set) var error: String?
    private(set) var currentPage = 1
    private(set) var totalItems = 0

    var itemsPerPage: Int {
        let stored = UserDefaults.standard.integer(forKey: "CreatorPerPage")
        return stored > 0 ? stored : 10
    }

    var totalPages: Int {
        max(1, (totalItems + itemsPerPage - 1) / itemsPerPage)
    }

    @ObservationIgnored private var loadTask: Task<Void, Never>?
    @ObservationIgnored private var loadGeneration = 0
    @ObservationIgnored private var onOpen: () -> Void = {}
    @ObservationIgnored private var onItemsLoaded: ([WorkshopItem]) -> Void = { _ in }

    func configure(
        onOpen: @escaping () -> Void,
        onItemsLoaded: @escaping ([WorkshopItem]) -> Void
    ) {
        self.onOpen = onOpen
        self.onItemsLoaded = onItemsLoaded
    }

    func open(_ creator: WorkshopCreator) {
        onOpen()
        loadTask?.cancel()
        loadGeneration += 1
        selectedCreator = creator
        items = []
        currentPage = 1
        totalItems = 0
        error = nil
        isLoading = false
        loadItems()
    }

    func close() {
        loadTask?.cancel()
        loadTask = nil
        loadGeneration += 1
        selectedCreator = nil
        isLoading = false
    }

    func reloadFromFirstPage() {
        guard selectedCreator != nil else { return }
        currentPage = 1
        loadItems()
    }

    func loadItems() {
        guard let creator = selectedCreator else { return }

        loadTask?.cancel()
        loadGeneration += 1
        let requestGeneration = loadGeneration
        let requestPage = currentPage
        let requestPageSize = itemsPerPage
        isLoading = true
        error = nil

        loadTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result = try await SteamWebAPI.shared.getUserFiles(
                    steamId: creator.steamId,
                    page: requestPage,
                    perPage: requestPageSize
                )
                guard !Task.isCancelled,
                      self.loadGeneration == requestGeneration,
                      self.selectedCreator?.id == creator.id,
                      self.currentPage == requestPage else { return }
                self.items = result.items
                self.totalItems = result.total
                self.isLoading = false
                self.onItemsLoaded(result.items)
            } catch is CancellationError {
                return
            } catch {
                guard !Task.isCancelled,
                      self.loadGeneration == requestGeneration,
                      self.selectedCreator?.id == creator.id,
                      self.currentPage == requestPage else { return }
                self.error = error.localizedDescription
                self.isLoading = false
            }
        }
    }

    func goToPage(_ page: Int) {
        let clamped = max(1, min(page, totalPages))
        guard clamped != currentPage else { return }
        currentPage = clamped
        loadItems()
    }
}
