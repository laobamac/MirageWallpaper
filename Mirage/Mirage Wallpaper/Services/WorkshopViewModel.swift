//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Combine
import AppKit

class WorkshopViewModel: ObservableObject {
    // MARK: - Browse State

    @Published var items: [WorkshopItem] = []
    @Published var searchText: String = ""
    @Published var selectedTags: Set<String> = []
    @Published var sortOrder: WorkshopSortOrder = .trending
    @Published var trendPeriod: WorkshopTrendPeriod = .week
    @Published var typeFilter: WorkshopTypeFilter = .all
    /// `@Published` + manual persistence rather than `@AppStorage`: SwiftUI does
    /// not route `@AppStorage` writes inside an `ObservableObject` through
    /// `objectWillChange`, which would leave the sidebar checkboxes stale.
    @Published var ageRatingFilter: WorkshopAgeRatingFilter = .default {
        didSet {
            guard ageRatingFilter != oldValue else { return }
            UserDefaults.standard.set(ageRatingFilter.rawValue, forKey: Self.ageRatingStorageKey)
        }
    }
    @Published var currentPage: Int = 1
    @Published var totalItems: Int = 0
    @Published var isLoading: Bool = false
    @Published var error: String?
    @Published var pageNavigationMessage: String?
    @Published private(set) var knownCreators: [WorkshopCreator] = []

    @Published var selectedItem: WorkshopItem?
    @Published var showCustomization: Bool = false
    @Published var selectedCreator: WorkshopCreator?
    @Published var showCreatorProfile: Bool = false

    @Published var creatorItems: [WorkshopItem] = []
    @Published var isLoadingCreatorItems = false
    @Published var creatorItemsError: String?
    @Published var creatorItemsPage = 1
    @Published var creatorItemsTotal = 0
    var creatorItemsPerPage: Int {
        let stored = UserDefaults.standard.integer(forKey: "CreatorPerPage")
        return stored > 0 ? stored : 10
    }

    var creatorTotalPages: Int {
        max(1, creatorItemsTotal / creatorItemsPerPage + (creatorItemsTotal % creatorItemsPerPage > 0 ? 1 : 0))
    }

    let itemsPerPage = 50
    let maximumPages = 1000

    // MARK: - Discover State

    @Published private(set) var discoverItems: [WorkshopDiscoverCategory: [WorkshopItem]] = [:]
    @Published var discoverTrendPeriod: WorkshopTrendPeriod = .week
    @Published var isDiscoverLoading: Bool = false

    var bannerItems: [WorkshopItem] {
        Array((discoverItems[.trending] ?? []).prefix(5))
    }

    // MARK: - Download State

    @Published var downloadQueue: [DownloadTask] = []
    @Published var downloadHistory: [DownloadTask] = []
    @Published var presetDependencyPrompt: PresetDependencyPrompt?

    // MARK: - Sync State
    // MARK: - Steam service state

    @Published var steamSetupState: SteamSetupState = .notConfigured
    @Published var steamServiceStatus = SteamServiceStatus()
    @Published var logoutResultMessage: String?
    @Published var isLoggingOut = false

    var totalPages: Int {
        maximumPages
    }

    var activeDownloadCount: Int {
        downloadQueue.filter {
            if case .downloading = $0.state { return true }
            if case .starting = $0.state { return true }
            if case .validating = $0.state { return true }
            return false
        }.count
    }

    var isTextRelevanceSearch: Bool {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        return !query.isEmpty && !Self.isPublishedFileId(query) && !Self.isSteamUserId(query)
    }

    private static let ageRatingStorageKey = "WorkshopAgeRatingFilter"

    private var searchDebounce: AnyCancellable?
    private var serviceStateCancellables = Set<AnyCancellable>()
    private var cancelledDownloadIDs: Set<String> = []
    private var pendingPresetApplication: (presetID: String, dependencyID: String, selectionGeneration: Int)?
    private var pendingCreatorPresetApplication: (presetID: String, dependencyID: String)?
    private var backgroundAutoApplyIDs: Set<String> = []
    private var searchTask: Task<Void, Never>?
    private var discoverTask: Task<Void, Never>?
    private var searchGeneration = 0
    private var discoverGeneration = 0
    private var selectionGeneration = 0
    private var loadedPage = 1

    init() {
        if let stored = UserDefaults.standard.object(forKey: Self.ageRatingStorageKey) as? Int {
            ageRatingFilter = WorkshopAgeRatingFilter(rawValue: stored)
        }

        searchDebounce = $searchText
            .debounce(for: .milliseconds(500), scheduler: RunLoop.main)
            .removeDuplicates()
            .sink { [weak self] _ in
                self?.currentPage = 1
                self?.search()
            }

        SteamCMDManager.shared.$isLoggedIn
            .receive(on: RunLoop.main)
            .sink { [weak self] isLoggedIn in
                self?.refreshSetupState()
                if isLoggedIn { self?.processDownloadQueue() }
            }
            .store(in: &serviceStateCancellables)

        SteamCMDManager.shared.$authenticationState
            .receive(on: RunLoop.main)
            .sink { [weak self] state in
                self?.steamServiceStatus.authentication = state
                self?.refreshSetupState()
            }
            .store(in: &serviceStateCancellables)

        // Keep the "already installed" lookup off the card render path: a newly
        // finished download refreshes the cached id set once, instead of every
        // card touching the filesystem on every rebuild.
        NotificationCenter.default.publisher(for: .workshopItemDownloaded)
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.refreshInstalledState() }
            .store(in: &serviceStateCancellables)

        NotificationCenter.default.publisher(for: .wallpaperLibraryChanged)
            .receive(on: RunLoop.main)
            .sink { [weak self] notification in self?.handleLibraryChange(notification) }
            .store(in: &serviceStateCancellables)

        refreshInstalledState()
    }

    // MARK: - Installed-state cache
    //
    // Card views must never hit the filesystem while rendering. We keep a
    // snapshot of which workshop ids are installed (and which installed presets
    // still need their base wallpaper), rebuilt on a background queue whenever
    // the library could have changed.

    @Published private(set) var installedWorkshopIDs: Set<String> = []
    @Published private(set) var presetsNeedingDependency: Set<String> = []
    /// Workshop metadata is loaded lazily for the selected local wallpaper.
    /// Keeping it here lets the library and Workshop views share one cache
    /// without scanning or requesting metadata for the whole library.
    @Published private(set) var installedWorkshopItems: [String: WorkshopItem] = [:]

    private let installedScanQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.workshop.installed", qos: .utility)
    private var requestedInstalledMetadataIDs: Set<String> = []

    func refreshInstalledState(reconcileDownloads: Bool = false) {
        installedScanQueue.async { [weak self] in
            guard let self else { return }
            let directories = WallpaperLibrary.shared.allWorkshopIDDirectories()
            var installed = Set<String>()
            var needsDependency = Set<String>()
            installed.reserveCapacity(directories.count)
            for (workshopID, url) in directories {
                installed.insert(workshopID)
                let wallpaper = WEWallpaper.load(from: url)
                if wallpaper.needsPresetDependency {
                    needsDependency.insert(workshopID)
                }
            }
            DispatchQueue.main.async {
                if self.installedWorkshopIDs != installed {
                    self.installedWorkshopIDs = installed
                }
                if self.presetsNeedingDependency != needsDependency {
                    self.presetsNeedingDependency = needsDependency
                }
                if reconcileDownloads {
                    self.downloadQueue.removeAll { task in
                        guard !installed.contains(task.id) else { return false }
                        if case .completed = task.state { return true }
                        return false
                    }
                }
            }
        }
    }

    private func handleLibraryChange(_ notification: Notification) {
        if let url = notification.object as? URL {
            let workshopID = url.lastPathComponent
            if WallpaperLibrary.shared.workshopItemDirectory(for: workshopID) == nil {
                installedWorkshopIDs.remove(workshopID)
                presetsNeedingDependency.remove(workshopID)
                downloadQueue.removeAll { task in
                    guard task.id == workshopID else { return false }
                    if case .completed = task.state { return true }
                    return false
                }
            }
        }
        refreshInstalledState(reconcileDownloads: true)
    }

    func isInstalled(_ workshopId: String) -> Bool {
        installedWorkshopIDs.contains(workshopId)
    }

    func presetNeedsDependency(_ workshopId: String) -> Bool {
        presetsNeedingDependency.contains(workshopId)
    }

    func installedWorkshopItem(for wallpaper: WEWallpaper) -> WorkshopItem? {
        guard let id = wallpaper.verifiedWorkshopID() else { return nil }
        return installedWorkshopItems[id]
    }

    func installedCreator(for wallpaper: WEWallpaper) -> WorkshopCreator? {
        guard let item = installedWorkshopItem(for: wallpaper) else { return nil }
        return WorkshopCreator(item: item)
    }

    /// Local manifests do not consistently contain a human-readable author.
    /// Prefer the verified Steam profile name, then the manifest's author text,
    /// and finally the Steam ID when that is all the API made available.
    func installedAuthorName(for wallpaper: WEWallpaper) -> String? {
        if let item = installedWorkshopItem(for: wallpaper),
           let name = item.creatorName?.trimmingCharacters(in: .whitespacesAndNewlines),
           !name.isEmpty {
            return name
        }
        if let local = wallpaper.project.resolvedAuthor {
            return local
        }
        if let item = installedWorkshopItem(for: wallpaper), !item.creatorSteamId.isEmpty {
            return item.creatorSteamId
        }
        return nil
    }

    func loadInstalledMetadata(for wallpaper: WEWallpaper) {
        guard let id = wallpaper.verifiedWorkshopID(),
              installedWorkshopItems[id] == nil,
              !requestedInstalledMetadataIDs.contains(id) else { return }
        requestedInstalledMetadataIDs.insert(id)

        Task { @MainActor [weak self] in
            guard let self else { return }
            let details = (try? await SteamWebAPI.shared.getFileDetails(workshopIds: [id])) ?? []
            for item in details where item.consumerAppId == 431960 {
                self.installedWorkshopItems[item.publishedFileId] = item
            }
            // A transient API error must be retryable on the next selection.
            if details.contains(where: {
                $0.publishedFileId == id && $0.consumerAppId == 431960
            }) == false {
                self.requestedInstalledMetadataIDs.remove(id)
            }
        }
    }

    // MARK: - Setup Check

    func checkSteamSetup() {
        let cmdManager = SteamCMDManager.shared
        steamServiceStatus.steamCMD = .checking
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let path = cmdManager.detectSteamCMD()
            DispatchQueue.main.async {
                guard let self else { return }
                switch path {
                case .found(let path):
                    self.steamServiceStatus.steamCMD = .available(path.path)
                    cmdManager.refreshSessionIfNeeded()
                case .rosettaRequired:
                    self.steamServiceStatus.steamCMD = .needsAction(L("需要安装 Rosetta 2"))
                case .notFound:
                    self.steamServiceStatus.steamCMD = .unavailable(L("未安装 SteamCMD"))
                }
                self.steamServiceStatus.authentication = cmdManager.authenticationState
                self.refreshSetupState()
            }
        }
    }

    private func refreshSetupState() {
        let cmdManager = SteamCMDManager.shared
        if cmdManager.steamCMDPath == nil {
            steamSetupState = .steamCMDMissing
            steamServiceStatus.workshopDownload = .needsAction(L("需要先安装 SteamCMD"))
        } else if !cmdManager.isLoggedIn {
            steamSetupState = .needsLogin
            if cmdManager.savedUsername.isEmpty {
                steamServiceStatus.authentication = .needsAction(L("需要登录 Steam"))
            }
            steamServiceStatus.workshopDownload = .needsAction(L("需要有效的 Steam 会话"))
        } else {
            steamSetupState = .ready
            steamServiceStatus.authentication = .available(L("会话已验证"))
            if case .unknown = steamServiceStatus.workshopDownload {
                steamServiceStatus.workshopDownload = .needsAction(L("尚未开始下载"))
            }
        }
    }

    // MARK: - Search

    func search() {
        searchTask?.cancel()
        searchGeneration += 1
        let generation = searchGeneration
        let requestSearchText = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        let requestTags = Array(selectedTags)
        let requestSortOrder = sortOrder
        let requestTypeFilter = typeFilter
        let requestAgeRating = ageRatingFilter
        let requestTrendPeriod = trendPeriod
        let requestPage = currentPage
        isLoading = true
        error = nil
        pageNavigationMessage = nil
        steamServiceStatus.browsingAPI = .checking

        searchTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result: (items: [WorkshopItem], total: Int)
                var matchedCreator: WorkshopCreator?
                if Self.isPublishedFileId(requestSearchText) {
                    let details = try await SteamWebAPI.shared.getFileDetails(workshopIds: [requestSearchText])
                    let items = details.filter {
                        $0.publishedFileId == requestSearchText && $0.consumerAppId == 431960
                    }
                    result = (items, items.count)
                } else if Self.isSteamUserId(requestSearchText) {
                    matchedCreator = await SteamWebAPI.shared.creatorProfile(steamId: requestSearchText)
                    result = ([], 0)
                } else {
                    result = try await SteamWebAPI.shared.queryFiles(
                        searchText: requestSearchText,
                        tags: requestTags,
                        sortOrder: requestSortOrder,
                        typeFilter: requestTypeFilter,
                        ageRating: requestAgeRating,
                        page: requestPage,
                        perPage: self.itemsPerPage,
                        trendDays: requestSortOrder.usesTrendPeriod ? requestTrendPeriod.rawValue : nil
                    )
                }

                guard !Task.isCancelled, generation == self.searchGeneration else { return }
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
                    self.steamServiceStatus.browsingAPI = .available(L("Steam Web API 可用"))
                    return
                }
                self.items = result.items
                self.totalItems = result.total
                self.loadedPage = requestPage
                self.rememberCreators(in: result.items)
                if let matchedCreator {
                    self.rememberCreator(matchedCreator)
                }
                self.isLoading = false
                self.steamServiceStatus.browsingAPI = .available(L("Steam Web API 可用"))
            } catch {
                guard !Task.isCancelled, generation == self.searchGeneration else { return }
                self.error = error.localizedDescription
                self.isLoading = false
                self.steamServiceStatus.browsingAPI = .unavailable(error.localizedDescription)
            }
        }
    }

    func refreshSearch() {
        search()
    }

    func selectWorkshopSort(
        _ order: WorkshopSortOrder,
        period: WorkshopTrendPeriod? = nil
    ) {
        sortOrder = order
        if let period {
            trendPeriod = period
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
        if Self.isSteamUserId(query) {
            openCreatorWorkshop(WorkshopCreator(steamId: query, name: query))
            return
        }
        currentPage = 1
        search()
    }

    func openCreatorWorkshop(_ creator: WorkshopCreator) {
        guard let url = creator.workshopURL else { return }
        NSWorkspace.shared.open(url)
    }

    func openCreatorProfile(_ creator: WorkshopCreator) {
        selectedCreator = creator
        showCreatorProfile = true
        showCustomization = false
        creatorItems = []
        creatorItemsPage = 1
        creatorItemsTotal = 0
        creatorItemsError = nil
        loadCreatorItems(for: creator)
    }

    func openCreatorWorkshop(for item: WorkshopItem) {
        guard let creator = WorkshopCreator(item: item) else { return }
        openCreatorWorkshop(creator)
    }

    func loadCreatorItems(for creator: WorkshopCreator) {
        guard !isLoadingCreatorItems else { return }
        isLoadingCreatorItems = true
        creatorItemsError = nil
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result = try await SteamWebAPI.shared.getUserFiles(
                    steamId: creator.steamId,
                    page: self.creatorItemsPage,
                    perPage: self.creatorItemsPerPage
                )
                self.creatorItems = result.items
                self.creatorItemsTotal = result.total
            } catch {
                self.creatorItemsError = error.localizedDescription
            }
            self.isLoadingCreatorItems = false
        }
    }

    func goToCreatorPage(_ page: Int) {
        let clamped = max(1, min(page, creatorTotalPages))
        guard clamped != creatorItemsPage else { return }
        creatorItemsPage = clamped
        guard let creator = selectedCreator else { return }
        loadCreatorItems(for: creator)
    }

    func downloadWorkshopID(_ workshopID: String, completion: ((Bool) -> Void)? = nil) {
        guard workshopID.allSatisfy(\.isNumber), UInt64(workshopID) ?? 0 > 0 else {
            completion?(false)
            return
        }
        Task { @MainActor [weak self] in
            guard let self else { return }
            guard let item = try? await SteamWebAPI.shared.getFileDetails(workshopIds: [workshopID])
                .first(where: { $0.publishedFileId == workshopID && $0.consumerAppId == 431960 }) else {
                completion?(false)
                return
            }
            self.backgroundAutoApplyIDs.insert(workshopID)
            self.downloadItem(item)
            completion?(true)
        }
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

    func applyTagFilter(_ tag: String) {
        if selectedTags.contains(tag) {
            selectedTags.remove(tag)
        } else {
            selectedTags.insert(tag)
        }
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

    func clearFilters() {
        selectedTags.removeAll()
        searchText = ""
        typeFilter = .all
        sortOrder = .trending
        trendPeriod = .week
        ageRatingFilter = .default
        currentPage = 1
        search()
    }

    // MARK: - Discover

    func loadDiscover(force: Bool = false) {
        if isDiscoverLoading && !force { return }
        discoverTask?.cancel()
        discoverGeneration += 1
        let generation = discoverGeneration
        let rating = ageRatingFilter
        let period = discoverTrendPeriod
        isDiscoverLoading = true

        discoverTask = Task { @MainActor [weak self] in
            guard let self else { return }
            let loaded = await withTaskGroup(
                of: (WorkshopDiscoverCategory, [WorkshopItem])?.self,
                returning: [WorkshopDiscoverCategory: [WorkshopItem]].self
            ) { group in
                for category in WorkshopDiscoverCategory.allCases {
                    group.addTask {
                        guard !Task.isCancelled else { return nil }
                        let items = try? await SteamWebAPI.shared.fetchDiscover(
                            category: category,
                            period: period,
                            count: category == .trending ? 15 : 12,
                            ageRating: rating
                        )
                        guard let items else { return nil }
                        return (category, items)
                    }
                }
                var sections: [WorkshopDiscoverCategory: [WorkshopItem]] = [:]
                for await result in group {
                    if let (category, items) = result {
                        sections[category] = items
                    }
                }
                return sections
            }

            guard !Task.isCancelled, generation == self.discoverGeneration else { return }
            let enriched = await SteamWebAPI.shared.enrichCreatorDetails(
                in: loaded.values.flatMap { $0 }
            )
            guard !Task.isCancelled, generation == self.discoverGeneration else { return }
            let byID = Dictionary(enriched.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
            self.discoverItems = loaded.mapValues { items in
                items.map { byID[$0.id] ?? $0 }
            }
            self.rememberCreators(in: enriched)
            if generation == self.discoverGeneration {
                self.isDiscoverLoading = false
            }
        }
    }

    func refreshDiscover() {
        loadDiscover(force: true)
    }

    private func rememberCreators(in items: [WorkshopItem]) {
        var creators = Dictionary(uniqueKeysWithValues: knownCreators.map { ($0.id, $0) })
        for item in items {
            guard let creator = WorkshopCreator(item: item) else { continue }
            creators[creator.id] = creator
        }
        knownCreators = creators.values.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private func rememberCreator(_ creator: WorkshopCreator) {
        var creators = Dictionary(uniqueKeysWithValues: knownCreators.map { ($0.id, $0) })
        creators[creator.id] = creator
        knownCreators = creators.values.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private static func isSteamUserId(_ value: String) -> Bool {
        value.count == 17 && value.hasPrefix("7656119") && value.allSatisfy(\.isNumber)
    }

    private static func isPublishedFileId(_ value: String) -> Bool {
        guard !value.isEmpty, !isSteamUserId(value), value.allSatisfy(\.isNumber),
              let number = UInt64(value) else { return false }
        return number > 0
    }

    // MARK: - Download

    func downloadItem(_ item: WorkshopItem, purpose: DownloadPurpose = .wallpaper) {
        if let existingIndex = downloadQueue.firstIndex(where: { $0.id == item.publishedFileId }) {
            switch downloadQueue[existingIndex].state {
            case .failed, .completed:
                downloadQueue.removeAll { $0.id == item.publishedFileId }
            case .queued, .starting, .downloading, .validating:
                if purpose == .presetDependency {
                    downloadQueue[existingIndex].purpose = purpose
                }
                return
            }
        }

        let task = DownloadTask(
            workshopItem: item,
            state: .queued,
            startedAt: nil,
            completedAt: nil,
            purpose: purpose
        )
        downloadQueue.append(task)
        processDownloadQueue()
    }

    func cancelDownload(_ item: WorkshopItem) {
        guard let index = downloadQueue.firstIndex(where: { $0.id == item.publishedFileId }) else { return }
        backgroundAutoApplyIDs.remove(item.publishedFileId)
        if case .queued = downloadQueue[index].state {
            downloadQueue.remove(at: index)
            processDownloadQueue()
            return
        }
        cancelledDownloadIDs.insert(item.publishedFileId)
        SteamCMDManager.shared.cancelDownload(workshopId: item.publishedFileId)
    }

    func retryDownload(_ task: DownloadTask) {
        downloadQueue.removeAll { $0.id == task.id }
        downloadItem(task.workshopItem, purpose: task.purpose)
    }

    func clearCompletedDownloads() {
        downloadQueue.removeAll {
            if case .completed = $0.state { return true }
            if case .failed = $0.state { return true }
            return false
        }
    }

    func downloadState(for workshopId: String) -> DownloadState? {
        downloadQueue.first(where: { $0.id == workshopId })?.state
    }

    func selectWorkshopItem(_ item: WorkshopItem) {
        selectionGeneration += 1
        showCreatorProfile = false
        selectedCreator = nil
        let installed = installedItem(workshopId: item.publishedFileId)
        if let wallpaper = installed, wallpaper.needsPresetDependency {
            showCustomization = false
            selectedItem = item
            requestPresetDependency(for: wallpaper)
        } else if let wallpaper = installed, wallpaper.isValid {
            AppDelegate.shared.wallpaperViewModel.requestApply(wallpaper)
            showCustomization = true
            selectedItem = item
        } else {
            showCustomization = false
            selectedItem = item
        }
    }

    private func processDownloadQueue() {
        guard steamSetupState == .ready else { return }
        let maxConcurrent = 1
        let currentActive = downloadQueue.filter {
            if case .downloading = $0.state { return true }
            if case .starting = $0.state { return true }
            if case .validating = $0.state { return true }
            return false
        }.count

        guard currentActive < maxConcurrent else { return }

        guard let nextIndex = downloadQueue.firstIndex(where: {
            if case .queued = $0.state { return true }
            return false
        }) else { return }

        let workshopId = downloadQueue[nextIndex].workshopItem.publishedFileId
        downloadQueue[nextIndex].state = .starting
        downloadQueue[nextIndex].startedAt = Date()

        SteamCMDManager.shared.downloadItem(
            workshopId: workshopId,
            expectedFileSize: self.downloadQueue[nextIndex].workshopItem.fileSize
        ) { [weak self] state in
            guard let self else { return }
            guard let idx = self.downloadQueue.firstIndex(where: { $0.id == workshopId }) else { return }

            self.downloadQueue[idx].state = state

            if self.cancelledDownloadIDs.contains(workshopId), case .failed = state {
                self.cancelledDownloadIDs.remove(workshopId)
                self.downloadQueue.removeAll { $0.id == workshopId }
                self.processDownloadQueue()
                return
            }

            if case .completed = state {
                let purpose = self.downloadQueue[idx].purpose
                let selectedItemID = self.selectedItem?.publishedFileId
                let selectionGeneration = self.selectionGeneration
                self.steamServiceStatus.workshopDownload = .available(L("最近一次下载已验证"))
                self.downloadQueue[idx].completedAt = Date()
                self.processDownloadQueue()
                NotificationCenter.default.post(name: .workshopItemDownloaded, object: workshopId)
                self.handleCompletedDownload(
                    workshopId: workshopId,
                    purpose: purpose,
                    selectedItemID: selectedItemID,
                    selectionGeneration: selectionGeneration
                )
            } else if case .failed = state {
                self.steamServiceStatus.workshopDownload = .unavailable(L("最近一次下载失败"))
                if SteamCMDManager.shared.isLoggedIn {
                    self.processDownloadQueue()
                }
            } else if case .starting = state {
                self.steamServiceStatus.workshopDownload = .checking
            }
        }
    }

    // MARK: - Navigate to Workshop with filter

    func navigateToWorkshopWithTag(
        _ tag: String,
        trendPeriod: WorkshopTrendPeriod = .week
    ) {
        selectedTags = [tag]
        searchText = ""
        typeFilter = .all
        sortOrder = .trending
        self.trendPeriod = trendPeriod
        showCustomization = false
        currentPage = 1
        search()
    }

    func navigateToWorkshopWithSort(
        _ sort: WorkshopSortOrder,
        trendPeriod: WorkshopTrendPeriod = .week
    ) {
        selectedTags.removeAll()
        searchText = ""
        typeFilter = .all
        sortOrder = sort
        self.trendPeriod = trendPeriod
        showCustomization = false
        currentPage = 1
        search()
    }

    func logout() {
        guard !isLoggingOut else { return }
        isLoggingOut = true
        steamServiceStatus.authentication = .checking
        SteamCMDManager.shared.logout { [weak self] result in
            guard let self else { return }
            self.isLoggingOut = false
            switch result {
            case .success:
                self.steamServiceStatus.authentication = .needsAction(L("已退出登录"))
                self.logoutResultMessage = L("已退出 Mirage 专用 SteamCMD 会话。")
            case .failure(let error):
                self.steamServiceStatus.authentication = .needsAction(error.localizedDescription)
                self.logoutResultMessage = error.localizedDescription
            }
            self.refreshSetupState()
        }
    }

    // MARK: - Auto Apply

    func openInstalledWallpaper(_ wallpaper: WEWallpaper) {
        // Re-resolve first: a stale `.missingDependency` would otherwise send
        // the user to the "download the base wallpaper" prompt for a base that
        // is already installed, leaving the preset permanently unclickable.
        let fresh = WEWallpaper.load(from: wallpaper.wallpaperDirectory)
        if fresh.needsPresetDependency {
            showCreatorProfile = false
            selectedCreator = nil
            requestPresetDependency(for: fresh)
        } else if fresh.isValid {
            showCreatorProfile = false
            selectedCreator = nil
            AppDelegate.shared.wallpaperViewModel.requestApply(fresh)
            showCustomization = true
            selectedItem = nil
        }
    }

    func installedItem(workshopId: String) -> WEWallpaper? {
        let installed = WallpaperLibrary.shared.workshopItemDirectories(for: workshopId)
            .map { WEWallpaper.load(from: $0) }
        return installed.first(where: \.isValid)
            ?? installed.first(where: \.isPreset)
            ?? installed.first
    }

    func requestPresetDependency(for wallpaper: WEWallpaper) {
        guard wallpaper.isPreset, let dependency = wallpaper.presetDependency else { return }
        let dependencyID = dependency.rawValue

        if WallpaperLibrary.shared.workshopItemDirectory(for: dependencyID) != nil {
            let refreshed = WEWallpaper.load(from: wallpaper.wallpaperDirectory)
            if refreshed.isValid {
                openInstalledWallpaper(refreshed)
                return
            }
        }

        let presetID = wallpaper.wallpaperDirectory.lastPathComponent
        let presetTitle = wallpaper.project.title
        Task { @MainActor in
            let dependencyItem: WorkshopItem
            do {
                dependencyItem = try await SteamWebAPI.shared.getFileDetails(workshopIds: [dependencyID])
                    .first(where: { $0.publishedFileId == dependencyID })
                    ?? .dependencyPlaceholder(id: dependencyID)
            } catch {
                dependencyItem = .dependencyPlaceholder(id: dependencyID)
            }
            guard self.selectedItem?.publishedFileId == presetID else { return }
            self.presetDependencyPrompt = PresetDependencyPrompt(
                presetID: presetID,
                presetTitle: presetTitle,
                dependencyID: dependencyID,
                dependencyItem: dependencyItem
            )
        }
    }

    func confirmPresetDependencyDownload(_ prompt: PresetDependencyPrompt) {
        presetDependencyPrompt = nil
        if let pending = pendingCreatorPresetApplication,
           pending.presetID == prompt.presetID,
           pending.dependencyID == prompt.dependencyID {
            downloadItem(prompt.dependencyItem, purpose: .presetDependency)
            if steamSetupState != .ready {
                AppDelegate.shared.openSteamSetup()
            }
            return
        }
        guard selectedItem?.publishedFileId == prompt.presetID else { return }
        pendingPresetApplication = (prompt.presetID, prompt.dependencyID, selectionGeneration)

        if let preset = installedItem(workshopId: prompt.presetID), preset.isValid {
            pendingPresetApplication = nil
            openInstalledWallpaper(preset)
            return
        }

        downloadItem(prompt.dependencyItem, purpose: .presetDependency)
        if steamSetupState != .ready {
            AppDelegate.shared.openSteamSetup()
        }
    }

    func dismissPresetDependencyPrompt() {
        presetDependencyPrompt = nil
        pendingCreatorPresetApplication = nil
    }

    private func handleCompletedDownload(
        workshopId: String,
        purpose: DownloadPurpose,
        selectedItemID: String?,
        selectionGeneration: Int
    ) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self else { return }

            if purpose == .presetDependency {
                if let pending = self.pendingCreatorPresetApplication,
                   pending.dependencyID == workshopId,
                   let preset = self.installedItem(workshopId: pending.presetID),
                   preset.isValid {
                    self.pendingCreatorPresetApplication = nil
                    AppDelegate.shared.wallpaperViewModel.requestApply(preset)
                    return
                }
                guard let pending = self.pendingPresetApplication,
                      pending.dependencyID == workshopId,
                      pending.selectionGeneration == selectionGeneration,
                      selectedItemID == pending.presetID,
                      self.selectionGeneration == selectionGeneration,
                      self.selectedItem?.publishedFileId == pending.presetID,
                      let preset = self.installedItem(workshopId: pending.presetID),
                      preset.isValid else { return }
                self.pendingPresetApplication = nil
                self.openInstalledWallpaper(preset)
                return
            }

            if self.backgroundAutoApplyIDs.remove(workshopId) != nil,
               let wallpaper = self.installedItem(workshopId: workshopId) {
                if wallpaper.needsPresetDependency,
                   let dependencyID = wallpaper.presetDependency?.rawValue {
                    self.pendingCreatorPresetApplication = (workshopId, dependencyID)
                    Task { @MainActor in
                        let dependencyItem = (try? await SteamWebAPI.shared.getFileDetails(
                            workshopIds: [dependencyID]
                        ).first(where: { $0.publishedFileId == dependencyID }))
                            ?? .dependencyPlaceholder(id: dependencyID)
                        guard self.pendingCreatorPresetApplication?.presetID == workshopId else {
                            return
                        }
                        self.presetDependencyPrompt = PresetDependencyPrompt(
                            presetID: workshopId,
                            presetTitle: wallpaper.project.title,
                            dependencyID: dependencyID,
                            dependencyItem: dependencyItem
                        )
                    }
                } else if wallpaper.isValid {
                    AppDelegate.shared.wallpaperViewModel.requestApply(wallpaper)
                }
                return
            }

            guard selectedItemID == workshopId,
                  self.selectionGeneration == selectionGeneration,
                  self.selectedItem?.publishedFileId == workshopId else { return }
            guard let wallpaper = self.installedItem(workshopId: workshopId) else { return }
            if wallpaper.needsPresetDependency {
                self.requestPresetDependency(for: wallpaper)
            } else if wallpaper.isValid {
                self.openInstalledWallpaper(wallpaper)
            }
        }
    }
}

// MARK: - Notification Names

extension Notification.Name {
    static let workshopItemDownloaded = Notification.Name("workshopItemDownloaded")
    static let favoritesChanged = Notification.Name("favoritesChanged")
}
