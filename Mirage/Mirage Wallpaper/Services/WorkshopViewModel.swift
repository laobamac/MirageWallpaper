//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Combine
import AppKit

final class WorkshopDownloadStore: ObservableObject {
    @Published var queue: [DownloadTask] = []

    func state(for workshopID: String) -> DownloadState? {
        queue.first(where: { $0.id == workshopID })?.state
    }

    var activeCount: Int {
        queue.filter {
            if case .downloading = $0.state { return true }
            if case .resolving = $0.state { return true }
            if case .validating = $0.state { return true }
            return false
        }.count
    }
}

class WorkshopViewModel: ObservableObject {
    struct SubscriptionDownloadPlan {
        let subscriptionCount: Int
        let remainingCount: Int
        let items: [WorkshopItem]

        var downloadCount: Int { items.count }
    }

    // MARK: - Browse State

    @Published var items: [WorkshopItem] = []
    @Published var searchText: String = ""
    @Published var selectedTags: Set<String> = []
    @Published var sortOrder: WorkshopSortOrder = .trending
    @Published var trendPeriod: WorkshopTrendPeriod = .week
    @Published var typeFilter: WorkshopTypeFilter = .all
    @Published var workshopShowOnly: FRShowOnly = .none {
        didSet {
            guard workshopShowOnly != oldValue else { return }
            UserDefaults.standard.set(workshopShowOnly.rawValue, forKey: Self.workshopShowOnlyStorageKey)
        }
    }
    /// `@Published` + manual persistence rather than `@AppStorage`: SwiftUI does
    /// not route `@AppStorage` writes inside an `ObservableObject` through
    /// `objectWillChange`, which would leave the sidebar checkboxes stale.
    @Published var ageRatingFilter: WorkshopAgeRatingFilter = .default {
        didSet {
            guard ageRatingFilter != oldValue else { return }
            UserDefaults.standard.set(ageRatingFilter.rawValue, forKey: Self.ageRatingStorageKey)
        }
    }
    @Published var widescreenResolution = FRWidescreenResolution.all {
        didSet { UserDefaults.standard.set(widescreenResolution.rawValue, forKey: "WorkshopWidescreenResolution") }
    }
    @Published var ultraWidescreenResolution = FRUltraWidescreenResolution.all {
        didSet { UserDefaults.standard.set(ultraWidescreenResolution.rawValue, forKey: "WorkshopUltraWidescreenResolution") }
    }
    @Published var dualscreenResolution = FRDualscreenResolution.all {
        didSet { UserDefaults.standard.set(dualscreenResolution.rawValue, forKey: "WorkshopDualscreenResolution") }
    }
    @Published var triplescreenResolution = FRTriplescreenResolution.all {
        didSet { UserDefaults.standard.set(triplescreenResolution.rawValue, forKey: "WorkshopTriplescreenResolution") }
    }
    @Published var portraitResolution = FRPortraitScreenResolution.all {
        didSet { UserDefaults.standard.set(portraitResolution.rawValue, forKey: "WorkshopPortraitResolution") }
    }
    @Published var miscResolution = FRMiscResolution.all {
        didSet { UserDefaults.standard.set(miscResolution.rawValue, forKey: "WorkshopMiscResolution") }
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

    /// Download progress has its own observable channel. Publishing byte-level
    /// progress through the main workshop view model invalidated every browse
    /// grid and any native context menu currently attached to a card.
    let downloadStore = WorkshopDownloadStore()
    var downloadQueue: [DownloadTask] {
        get { downloadStore.queue }
        set { downloadStore.queue = newValue }
    }
    @Published var downloadHistory: [DownloadTask] = []
    @Published var presetDependencyPrompt: PresetDependencyPrompt?

    @Published private(set) var subscriptionRecords: [WorkshopSubscription] = []
    @Published private(set) var subscriptionCatalogItems: [WorkshopItem] = []
    @Published private(set) var subscriptionItems: [WorkshopItem] = []
    @Published private(set) var subscriptionTotal = 0
    @Published private(set) var subscriptionStartIndex = 0
    @Published private(set) var isLoadingSubscriptions = false
    @Published private(set) var subscriptionsError: String?
    @Published var subscriptionSearchText = ""
    @Published var subscriptionSelectedTags: Set<String> = []
    @Published var subscriptionTypeFilter: WorkshopTypeFilter = .all
    @Published var subscriptionShowOnly: FRShowOnly = .none {
        didSet {
            guard subscriptionShowOnly != oldValue else { return }
            UserDefaults.standard.set(subscriptionShowOnly.rawValue, forKey: Self.subscriptionShowOnlyStorageKey)
        }
    }
    @Published var subscriptionAgeRatingFilter: WorkshopAgeRatingFilter = .all
    @Published var subscriptionWidescreenResolution = FRWidescreenResolution.all
    @Published var subscriptionUltraWidescreenResolution = FRUltraWidescreenResolution.all
    @Published var subscriptionDualscreenResolution = FRDualscreenResolution.all
    @Published var subscriptionTriplescreenResolution = FRTriplescreenResolution.all
    @Published var subscriptionPortraitResolution = FRPortraitScreenResolution.all
    @Published var subscriptionMiscResolution = FRMiscResolution.all
    @Published private(set) var subscriptionStates: [String: WorkshopSubscriptionState] = [:]
    @Published private(set) var checkingSubscriptionIDs: Set<String> = []
    @Published private(set) var changingSubscriptionIDs: Set<String> = []
    @Published private(set) var subscriptionActionError: String?
    @Published private(set) var subscriptionActionErrorItemID: String?
    @Published private(set) var workshopFavoriteIDs: Set<String> = []
    @Published private(set) var changingFavoriteIDs: Set<String> = []
    @Published private(set) var favoriteActionError: String?
    @Published private(set) var favoriteActionErrorItemID: String?
    @Published private(set) var isPreparingSubscriptionDownloads = false
    @Published private(set) var subscriptionDownloadPlan: SubscriptionDownloadPlan?

    @Published private(set) var comments: [WorkshopComment] = []
    @Published private(set) var commentsTotal = 0
    @Published private(set) var commentsStartIndex = 0
    @Published private(set) var commentsNextStartIndex = 0
    @Published private(set) var commentsCanPost = false
    @Published private(set) var commentsItemID: String?
    @Published private(set) var isLoadingComments = false
    @Published private(set) var commentsError: String?
    @Published private(set) var commentAuthors: [String: WorkshopCreator] = [:]
    @Published var commentDraft = ""
    @Published private(set) var isPostingComment = false

    // MARK: - Sync State
    // MARK: - Steam service state

    @Published var steamSetupState: SteamSetupState = .checking
    @Published var steamServiceStatus = SteamServiceStatus()
    @Published var logoutResultMessage: String?
    @Published var isLoggingOut = false

    var steamCheckingMessage: String {
        SteamServiceManager.shared.savedUsername.isEmpty
            ? L("正在连接 Steam…")
            : L("正在恢复 Steam 会话…")
    }

    var totalPages: Int {
        min(maximumPages, max(1, Int(ceil(Double(totalItems) / Double(itemsPerPage)))))
    }

    var activeDownloadCount: Int {
        downloadStore.activeCount
    }

    var canLoadPreviousSubscriptions: Bool {
        subscriptionStartIndex > 0 && !isLoadingSubscriptions
    }

    var canLoadNextSubscriptions: Bool {
        !isLoadingSubscriptions && subscriptionStartIndex + subscriptionPageSize < subscriptionTotal
    }

    var subscriptionPageCount: Int {
        max(1, (subscriptionTotal + subscriptionPageSize - 1) / subscriptionPageSize)
    }

    var subscriptionCurrentPage: Int {
        min(subscriptionPageCount, subscriptionStartIndex / subscriptionPageSize + 1)
    }

    var allSubscriptionResolutionsSelected: Bool {
        subscriptionWidescreenResolution == .all &&
            subscriptionUltraWidescreenResolution == .all &&
            subscriptionDualscreenResolution == .all &&
            subscriptionTriplescreenResolution == .all &&
            subscriptionPortraitResolution == .all &&
            subscriptionMiscResolution == .all
    }

    var allSubscriptionResolutionsCleared: Bool {
        subscriptionWidescreenResolution.isEmpty &&
            subscriptionUltraWidescreenResolution.isEmpty &&
            subscriptionDualscreenResolution.isEmpty &&
            subscriptionTriplescreenResolution.isEmpty &&
            subscriptionPortraitResolution.isEmpty &&
            subscriptionMiscResolution.isEmpty
    }

    var allSubscriptionTagsSelected: Bool {
        Set(WorkshopTag.allCases.map(\.rawValue)).isSubset(of: subscriptionSelectedTags)
    }

    var hasActiveSubscriptionFilters: Bool {
        !subscriptionShowOnly.isEmpty ||
            !subscriptionSearchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
            (!subscriptionSelectedTags.isEmpty && !allSubscriptionTagsSelected) ||
            subscriptionTypeFilter != .all ||
            (!subscriptionAgeRatingFilter.isEmpty && subscriptionAgeRatingFilter != .all) ||
            !allSubscriptionResolutionsSelected
    }

    var isTextRelevanceSearch: Bool {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        return !query.isEmpty && !Self.isPublishedFileId(query) && !Self.isSteamUserId(query)
    }

    private static let ageRatingStorageKey = "WorkshopAgeRatingFilter"
    private static let workshopShowOnlyStorageKey = "WorkshopShowOnlyV2"
    private static let subscriptionShowOnlyStorageKey = "SubscriptionShowOnlyV2"

    private var searchDebounce: AnyCancellable?
    private var subscriptionSearchDebounce: AnyCancellable?
    private var serviceStateCancellables = Set<AnyCancellable>()
    private var cancelledDownloadIDs: Set<String> = []
    private var pendingPresetApplication: (presetID: String, dependencyID: String, selectionGeneration: Int)?
    private var pendingCreatorPresetApplication: (presetID: String, dependencyID: String)?
    private var backgroundAutoApplyIDs: Set<String> = []
    private var searchTask: Task<Void, Never>?
    private var discoverTask: Task<Void, Never>?
    private var searchGeneration = 0
    private var discoverGeneration = 0
    private var subscriptionGeneration = 0
    private var commentGeneration = 0
    private var commentAuthorTask: Task<Void, Never>?
    private var selectionGeneration = 0
    private var loadedPage = 1
    private let commentsPageSize = 20
    private var subscriptionPageSize: Int {
        let value = UserDefaults.standard.integer(forKey: "WallpapersPerPage")
        return [10, 25, 50].contains(value) ? value : 50
    }

    init() {
        workshopShowOnly = Self.storedShowOnly(forKey: Self.workshopShowOnlyStorageKey)
        subscriptionShowOnly = Self.storedShowOnly(forKey: Self.subscriptionShowOnlyStorageKey)
        if let stored = UserDefaults.standard.object(forKey: Self.ageRatingStorageKey) as? Int {
            ageRatingFilter = WorkshopAgeRatingFilter(rawValue: stored)
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

        searchDebounce = $searchText
            .debounce(for: .milliseconds(500), scheduler: RunLoop.main)
            .removeDuplicates()
            .sink { [weak self] _ in
                self?.currentPage = 1
                self?.search()
            }

        subscriptionSearchDebounce = $subscriptionSearchText
            .debounce(for: .milliseconds(300), scheduler: RunLoop.main)
            .removeDuplicates()
            .sink { [weak self] _ in
                self?.refreshSubscriptionFilters()
            }

        SteamServiceManager.shared.$isLoggedIn
            .receive(on: RunLoop.main)
            .sink { [weak self] isLoggedIn in
                guard let self else { return }
                self.refreshSetupState()
                guard isLoggedIn else { return }
                self.processDownloadQueue()
                if self.subscriptionCatalogItems.isEmpty && !self.isLoadingSubscriptions {
                    self.refreshSubscriptions(startIndex: 0)
                }
                if let item = self.selectedItem {
                    self.refreshSubscriptionStates(for: [item])
                    self.loadComments(for: item, startIndex: 0)
                }
            }
            .store(in: &serviceStateCancellables)

        SteamServiceManager.shared.$workshopFavoriteIDs
            .receive(on: RunLoop.main)
            .sink { [weak self] favoriteIDs in
                guard let self else { return }
                self.workshopFavoriteIDs = favoriteIDs
                if self.workshopShowOnly.contains(.myFavourites) {
                    self.currentPage = 1
                    self.search()
                }
                if self.subscriptionShowOnly.contains(.myFavourites) {
                    self.refreshSubscriptionFilters()
                }
            }
            .store(in: &serviceStateCancellables)

        SteamServiceManager.shared.$authenticationState
            .receive(on: RunLoop.main)
            .sink { [weak self] state in
                self?.steamServiceStatus.authentication = state
                self?.refreshSetupState()
            }
            .store(in: &serviceStateCancellables)

        SteamServiceManager.shared.$isAvailable
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
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

    private static func storedShowOnly(forKey key: String) -> FRShowOnly {
        guard let raw = UserDefaults.standard.object(forKey: key) as? Int else { return .none }
        return FRShowOnly(rawValue: raw & FRShowOnly.all.rawValue)
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
                self.refreshSubscriptionStates(for: [item])
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
        let manager = SteamServiceManager.shared
        steamServiceStatus.client = manager.isAvailable
            ? .available(L("内置 Steam 服务可用"))
            : .checking
        steamServiceStatus.authentication = manager.authenticationState
        manager.start()
        refreshSetupState()
    }

    static func resolveSteamSetupState(
        isAvailable: Bool,
        isLoggedIn: Bool,
        authenticationState: SteamServiceState
    ) -> SteamSetupState {
        if isAvailable && isLoggedIn {
            return .ready
        }
        switch authenticationState {
        case .unknown, .checking:
            return .checking
        case .available, .needsAction, .unavailable:
            return isAvailable ? .needsLogin : .serviceUnavailable
        }
    }

    private func refreshSetupState() {
        let manager = SteamServiceManager.shared
        steamSetupState = Self.resolveSteamSetupState(
            isAvailable: manager.isAvailable,
            isLoggedIn: manager.isLoggedIn,
            authenticationState: manager.authenticationState
        )
        steamServiceStatus.authentication = manager.authenticationState
        switch steamSetupState {
        case .checking:
            steamServiceStatus.client = manager.isAvailable
                ? .available(L("内置 Steam 服务可用"))
                : .checking
            steamServiceStatus.workshopDownload = .checking
        case .serviceUnavailable:
            steamServiceStatus.client = .unavailable(L("Steam 服务组件不可用"))
            steamServiceStatus.workshopDownload = .needsAction(L("Steam 服务尚未就绪"))
        case .needsLogin:
            steamServiceStatus.client = .available(L("内置 Steam 服务可用"))
            if manager.savedUsername.isEmpty {
                steamServiceStatus.authentication = .needsAction(L("需要登录 Steam"))
            }
            steamServiceStatus.workshopDownload = .needsAction(L("需要有效的 Steam 会话"))
        case .ready:
            steamServiceStatus.client = .available(L("内置 Steam 服务可用"))
            if steamServiceStatus.workshopDownload == .unknown ||
                steamServiceStatus.workshopDownload == .checking {
                steamServiceStatus.workshopDownload = .needsAction(L("尚未开始下载"))
            }
        }
    }

    private func openSteamSetupIfActionable() {
        if steamSetupState == .needsLogin || steamSetupState == .serviceUnavailable {
            AppDelegate.shared.openSteamSetup()
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
        let requestShowOnly = workshopShowOnly
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
        steamServiceStatus.browsingAPI = .checking

        searchTask = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let result: (items: [WorkshopItem], total: Int)
                var matchedCreator: WorkshopCreator?
                if Self.isPublishedFileId(requestSearchText) {
                    let details = try await SteamWebAPI.shared.getFileDetails(workshopIds: [requestSearchText])
                    let items = details.filter {
                        $0.publishedFileId == requestSearchText &&
                            $0.consumerAppId == 431960 &&
                            requestShowOnly.matches(
                                workshopItem: $0,
                                favoriteIDs: requestFavoriteIDs
                            )
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
                self.refreshSubscriptionStates(for: result.items)
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
                self.rememberCreators(in: result.items)
                self.refreshSubscriptionStates(for: result.items)
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

    func setResolutionOption<Filter: FilterResultsModel>(
        _ keyPath: ReferenceWritableKeyPath<WorkshopViewModel, Filter>,
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

    func clearFilters() {
        selectedTags.removeAll()
        searchText = ""
        typeFilter = .all
        workshopShowOnly = .none
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

    // MARK: - Discover

    func loadDiscover(force: Bool = false) {
        if isDiscoverLoading && !force { return }
        discoverTask?.cancel()
        discoverGeneration += 1
        let generation = discoverGeneration
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
                            count: category == .trending ? 15 : 12
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
            self.refreshSubscriptionStates(for: enriched)
            if generation == self.discoverGeneration {
                self.isDiscoverLoading = false
            }
        }
    }

    func refreshDiscover() {
        loadDiscover(force: true)
    }

    func setWorkshopShowOnly(_ option: FRShowOnly, isOn: Bool) {
        if isOn {
            workshopShowOnly.insert(option)
        } else {
            workshopShowOnly.remove(option)
        }
        currentPage = 1
        if workshopShowOnly.contains(.myFavourites) {
            SteamServiceManager.shared.refreshWorkshopFavorites()
        }
        search()
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

    func refreshSubscriptions(startIndex: Int? = nil) {
        guard SteamServiceManager.shared.isLoggedIn else {
            subscriptionRecords = []
            subscriptionCatalogItems = []
            subscriptionItems = []
            subscriptionTotal = 0
            subscriptionStartIndex = 0
            subscriptionsError = L("需要登录 Steam")
            isLoadingSubscriptions = false
            return
        }
        subscriptionGeneration += 1
        let generation = subscriptionGeneration
        let requestedStart = max(0, startIndex ?? subscriptionStartIndex)
        isLoadingSubscriptions = true
        subscriptionsError = nil
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                var records: [WorkshopSubscription] = []
                var seen = Set<String>()
                var serviceStart = 0
                var total = Int.max
                while serviceStart < total {
                    let page = try await self.subscriptionPage(startIndex: serviceStart)
                    guard generation == self.subscriptionGeneration else { return }
                    total = page.total
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
                    loaded = try await self.loadSubscriptionItems(for: records)
                } catch {
                    loaded = records.map { WorkshopItem.unavailableSubscription(id: $0.publishedFileId) }
                    self.subscriptionsError = error.localizedDescription
                }
                guard generation == self.subscriptionGeneration else { return }
                let subscribedIDs = Set(records.map(\.publishedFileId))
                let removedIDs = Set(self.subscriptionRecords.map(\.publishedFileId)).subtracting(subscribedIDs)
                self.subscriptionRecords = records
                self.subscriptionCatalogItems = loaded
                for id in removedIDs {
                    self.subscriptionStates[id] = .unsubscribed
                }
                for record in records {
                    self.subscriptionStates[record.publishedFileId] = .subscribed
                }
                self.rememberCreators(in: loaded)
                self.rebuildSubscriptionPage(startIndex: requestedStart)
                self.isLoadingSubscriptions = false
            } catch {
                guard generation == self.subscriptionGeneration else { return }
                self.subscriptionsError = error.localizedDescription
                self.isLoadingSubscriptions = false
            }
        }
    }

    func loadPreviousSubscriptions() {
        guard canLoadPreviousSubscriptions else { return }
        goToSubscriptionPage(subscriptionCurrentPage - 1)
    }

    func loadNextSubscriptions() {
        guard canLoadNextSubscriptions else { return }
        goToSubscriptionPage(subscriptionCurrentPage + 1)
    }

    func goToSubscriptionPage(_ page: Int) {
        let target = min(max(page, 1), subscriptionPageCount)
        guard !isLoadingSubscriptions, target != subscriptionCurrentPage else { return }
        rebuildSubscriptionPage(startIndex: (target - 1) * subscriptionPageSize)
    }

    func subscriptionPageSizeDidChange() {
        rebuildSubscriptionPage(startIndex: 0)
    }

    func refreshSubscriptionFilters() {
        rebuildSubscriptionPage(startIndex: 0)
    }

    func setSubscriptionShowOnly(_ option: FRShowOnly, isOn: Bool) {
        if isOn {
            subscriptionShowOnly.insert(option)
        } else {
            subscriptionShowOnly.remove(option)
        }
        if subscriptionShowOnly.contains(.myFavourites) {
            SteamServiceManager.shared.refreshWorkshopFavorites()
        }
        refreshSubscriptionFilters()
    }

    func setSubscriptionTypeFilter(_ filter: WorkshopTypeFilter) {
        subscriptionTypeFilter = filter
        refreshSubscriptionFilters()
    }

    func applySubscriptionAgeRatingFilter(_ rating: WorkshopAgeRating, isOn: Bool) {
        var updated = subscriptionAgeRatingFilter
        let option = WorkshopAgeRatingFilter.bit(for: rating)
        if isOn {
            updated.insert(option)
        } else {
            updated.remove(option)
        }
        guard updated != subscriptionAgeRatingFilter else { return }
        subscriptionAgeRatingFilter = updated
        refreshSubscriptionFilters()
    }

    func setSubscriptionResolutionOption<Filter: FilterResultsModel>(
        _ keyPath: ReferenceWritableKeyPath<WorkshopViewModel, Filter>,
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
        refreshSubscriptionFilters()
    }

    func selectAllSubscriptionResolutions() {
        subscriptionWidescreenResolution = .all
        subscriptionUltraWidescreenResolution = .all
        subscriptionDualscreenResolution = .all
        subscriptionTriplescreenResolution = .all
        subscriptionPortraitResolution = .all
        subscriptionMiscResolution = .all
        refreshSubscriptionFilters()
    }

    func clearSubscriptionResolutions() {
        subscriptionWidescreenResolution = .none
        subscriptionUltraWidescreenResolution = .none
        subscriptionDualscreenResolution = .none
        subscriptionTriplescreenResolution = .none
        subscriptionPortraitResolution = .none
        subscriptionMiscResolution = .none
        refreshSubscriptionFilters()
    }

    func selectAllSubscriptionTags() {
        subscriptionSelectedTags = Set(WorkshopTag.allCases.map(\.rawValue))
        refreshSubscriptionFilters()
    }

    func clearSubscriptionTags() {
        subscriptionSelectedTags.removeAll()
        refreshSubscriptionFilters()
    }

    func applySubscriptionTagFilter(_ tag: String) {
        if subscriptionSelectedTags.contains(tag) {
            subscriptionSelectedTags.remove(tag)
        } else {
            subscriptionSelectedTags.insert(tag)
        }
        refreshSubscriptionFilters()
    }

    func clearSubscriptionFilters() {
        subscriptionSearchText = ""
        subscriptionSelectedTags.removeAll()
        subscriptionTypeFilter = .all
        subscriptionShowOnly = .none
        subscriptionAgeRatingFilter = .all
        subscriptionWidescreenResolution = .all
        subscriptionUltraWidescreenResolution = .all
        subscriptionDualscreenResolution = .all
        subscriptionTriplescreenResolution = .all
        subscriptionPortraitResolution = .all
        subscriptionMiscResolution = .all
        refreshSubscriptionFilters()
    }

    func downloadAllSubscriptions() {
        guard SteamServiceManager.shared.isLoggedIn, !isPreparingSubscriptionDownloads else {
            if !SteamServiceManager.shared.isLoggedIn { openSteamSetupIfActionable() }
            return
        }
        isPreparingSubscriptionDownloads = true
        subscriptionDownloadPlan = nil
        subscriptionsError = nil
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.isPreparingSubscriptionDownloads = false }
            do {
                let loaded: [WorkshopItem]
                let subscriptionCount: Int
                if !self.subscriptionCatalogItems.isEmpty {
                    loaded = self.subscriptionCatalogItems
                    subscriptionCount = self.subscriptionRecords.count
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
                    loaded = try await self.loadSubscriptionItems(for: allRecords)
                    subscriptionCount = total == Int.max ? allRecords.count : total
                }
                let activeIDs = Set(self.downloadQueue.compactMap { task -> String? in
                    switch task.state {
                    case .queued, .resolving, .downloading, .validating:
                        return task.id
                    case .completed, .failed:
                        return nil
                    }
                })
                var seen = Set<String>()
                let remaining = loaded.filter {
                    seen.insert($0.publishedFileId).inserted &&
                        $0.kind != .unsupported &&
                        !self.isWorkshopItemInstalled($0.publishedFileId)
                }
                let pending = remaining.filter { !activeIDs.contains($0.publishedFileId) }
                self.subscriptionDownloadPlan = SubscriptionDownloadPlan(
                    subscriptionCount: subscriptionCount,
                    remainingCount: remaining.count,
                    items: pending
                )
            } catch {
                self.subscriptionsError = error.localizedDescription
            }
        }
    }

    func confirmSubscriptionDownloads() {
        guard let plan = subscriptionDownloadPlan else { return }
        subscriptionDownloadPlan = nil
        for item in plan.items {
            downloadItem(item, purpose: .subscription)
        }
    }

    func dismissSubscriptionDownloadPlan() {
        subscriptionDownloadPlan = nil
    }

    func subscriptionState(for workshopId: String) -> WorkshopSubscriptionState {
        subscriptionStates[workshopId] ?? .unknown
    }

    func subscriptionActionError(for workshopId: String) -> String? {
        subscriptionActionErrorItemID == workshopId ? subscriptionActionError : nil
    }

    func isWorkshopFavorite(_ workshopId: String) -> Bool {
        workshopFavoriteIDs.contains(workshopId)
    }

    func favoriteActionError(for workshopId: String) -> String? {
        favoriteActionErrorItemID == workshopId ? favoriteActionError : nil
    }

    func dismissFavoriteActionError() {
        favoriteActionError = nil
        favoriteActionErrorItemID = nil
    }

    func toggleFavorite(_ item: WorkshopItem) {
        toggleWorkshopFavorite(workshopId: item.publishedFileId)
    }

    func toggleWorkshopFavorite(workshopId: String) {
        guard steamSetupState == .ready else {
            openSteamSetupIfActionable()
            return
        }
        guard !workshopId.isEmpty, !changingFavoriteIDs.contains(workshopId) else { return }
        let favorited = !isWorkshopFavorite(workshopId)
        changingFavoriteIDs.insert(workshopId)
        favoriteActionError = nil
        favoriteActionErrorItemID = workshopId
        SteamServiceManager.shared.setWorkshopFavorite(
            workshopId: workshopId,
            favorited: favorited
        ) { [weak self] result in
            guard let self else { return }
            self.changingFavoriteIDs.remove(workshopId)
            switch result {
            case .success:
                self.favoriteActionError = nil
                self.favoriteActionErrorItemID = nil
            case .failure(let error):
                self.favoriteActionError = error.localizedDescription
                self.favoriteActionErrorItemID = workshopId
            }
        }
    }

    func refreshSubscriptionStates(for items: [WorkshopItem]) {
        guard SteamServiceManager.shared.isLoggedIn else { return }
        let ids = Set(items.map(\.publishedFileId)).filter {
            !$0.isEmpty && !checkingSubscriptionIDs.contains($0) && !changingSubscriptionIDs.contains($0)
        }
        guard !ids.isEmpty else { return }
        if let selectedID = selectedItem?.publishedFileId, ids.contains(selectedID) {
            subscriptionActionError = nil
            subscriptionActionErrorItemID = selectedID
        }
        checkingSubscriptionIDs.formUnion(ids)
        SteamServiceManager.shared.fetchSubscriptionStates(workshopIds: Array(ids)) { [weak self] result in
            guard let self else { return }
            self.checkingSubscriptionIDs.subtract(ids)
            switch result {
            case .success(let states):
                for id in ids {
                    self.subscriptionStates[id] = states[id] == true ? .subscribed : .unsubscribed
                }
                if let errorID = self.subscriptionActionErrorItemID, ids.contains(errorID) {
                    self.subscriptionActionError = nil
                }
            case .failure(let error):
                if ids.contains(self.selectedItem?.publishedFileId ?? "") {
                    self.subscriptionActionError = error.localizedDescription
                    self.subscriptionActionErrorItemID = self.selectedItem?.publishedFileId
                }
            }
        }
    }

    func subscribe(_ item: WorkshopItem) {
        guard steamSetupState == .ready else {
            openSteamSetupIfActionable()
            return
        }
        let id = item.publishedFileId
        guard !changingSubscriptionIDs.contains(id) else { return }
        changingSubscriptionIDs.insert(id)
        subscriptionActionError = nil
        subscriptionActionErrorItemID = id
        SteamServiceManager.shared.subscribe(workshopId: id) { [weak self] result in
            guard let self else { return }
            self.changingSubscriptionIDs.remove(id)
            switch result {
            case .success:
                self.subscriptionStates[id] = .subscribed
                if !self.isInstalled(id) {
                    self.downloadItem(item, purpose: .subscription)
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                    self.refreshSubscriptions(startIndex: 0)
                }
            case .failure(let error):
                self.subscriptionActionError = error.localizedDescription
                self.subscriptionActionErrorItemID = id
            }
        }
    }

    func unsubscribe(_ item: WorkshopItem) {
        let id = item.publishedFileId
        guard steamSetupState == .ready, !changingSubscriptionIDs.contains(id) else {
            if steamSetupState != .ready { openSteamSetupIfActionable() }
            return
        }
        changingSubscriptionIDs.insert(id)
        subscriptionActionError = nil
        subscriptionActionErrorItemID = id
        SteamServiceManager.shared.unsubscribe(workshopId: id) { [weak self] result in
            guard let self else { return }
            self.changingSubscriptionIDs.remove(id)
            switch result {
            case .success:
                self.subscriptionStates[id] = .unsubscribed
                self.cancelDownloadForUnsubscribe(workshopId: id)
                self.subscriptionRecords.removeAll { $0.publishedFileId == id }
                self.subscriptionCatalogItems.removeAll { $0.publishedFileId == id }
                self.rebuildSubscriptionPage(startIndex: self.subscriptionStartIndex)
                do {
                    try WallpaperLibrary.shared.removeManagedWorkshopItem(workshopId: id)
                    self.refreshInstalledState(reconcileDownloads: true)
                } catch {
                    self.subscriptionActionError = L("已取消订阅，但无法删除 Mirage 下载副本：%@", error.localizedDescription)
                    self.subscriptionActionErrorItemID = id
                }
            case .failure(let error):
                self.subscriptionActionError = error.localizedDescription
                self.subscriptionActionErrorItemID = id
            }
        }
    }

    func prepareWorkshopInteractions(for item: WorkshopItem) {
        refreshSubscriptionStates(for: [item])
        if commentsItemID != item.publishedFileId {
            loadComments(for: item, startIndex: 0)
        }
    }

    func loadComments(for item: WorkshopItem, startIndex: Int = 0) {
        commentAuthorTask?.cancel()
        commentGeneration += 1
        guard SteamServiceManager.shared.isLoggedIn else {
            comments = []
            commentsTotal = 0
            commentsStartIndex = 0
            commentsNextStartIndex = 0
            commentsCanPost = false
            commentsItemID = item.publishedFileId
            commentsError = L("需要登录 Steam")
            commentAuthors = [:]
            isLoadingComments = false
            return
        }
        guard !item.creatorSteamId.isEmpty else {
            comments = []
            commentsTotal = 0
            commentsStartIndex = 0
            commentsNextStartIndex = 0
            commentsCanPost = false
            commentsItemID = item.publishedFileId
            commentsError = L("该作品缺少可用的作者信息，无法加载评论")
            commentAuthors = [:]
            isLoadingComments = false
            return
        }
        let generation = commentGeneration
        let requestedStart = max(0, startIndex)
        if commentsItemID != item.publishedFileId {
            comments = []
            commentsTotal = 0
            commentsStartIndex = 0
            commentsNextStartIndex = 0
            commentDraft = ""
            commentsCanPost = false
            commentAuthors = [:]
        }
        commentsItemID = item.publishedFileId
        commentsError = nil
        isLoadingComments = true
        SteamServiceManager.shared.fetchComments(
            item: item,
            startIndex: requestedStart,
            count: commentsPageSize
        ) { [weak self] result in
            guard let self,
                  generation == self.commentGeneration,
                  self.commentsItemID == item.publishedFileId else { return }
            switch result {
            case .success(let page):
                self.comments = page.comments
                self.commentsTotal = page.total
                self.commentsStartIndex = page.startIndex
                self.commentsNextStartIndex = page.nextStartIndex
                self.commentsCanPost = page.canPost
                self.loadCommentAuthors(
                    for: page.comments,
                    itemID: item.publishedFileId,
                    generation: generation
                )
            case .failure(let error):
                self.commentsError = error.localizedDescription
            }
            self.isLoadingComments = false
        }
    }

    private func loadCommentAuthors(
        for comments: [WorkshopComment],
        itemID: String,
        generation: Int
    ) {
        let ids = Array(Set(comments.map(\.authorSteamId).filter { !$0.isEmpty })).sorted()
        guard !ids.isEmpty else {
            commentAuthors = [:]
            return
        }
        commentAuthorTask?.cancel()
        commentAuthorTask = Task { [weak self] in
            var profiles: [String: WorkshopCreator] = [:]
            for id in ids {
                guard !Task.isCancelled else { return }
                if let creator = await SteamWebAPI.shared.creatorProfile(steamId: id) {
                    profiles[id] = creator
                }
            }
            guard !Task.isCancelled else { return }
            DispatchQueue.main.async {
                guard let self,
                      self.commentGeneration == generation,
                      self.commentsItemID == itemID else { return }
                self.commentAuthors.merge(profiles) { _, new in new }
            }
        }
    }

    func refreshComments(for item: WorkshopItem) {
        loadComments(for: item, startIndex: commentsStartIndex)
    }

    func loadPreviousComments(for item: WorkshopItem) {
        guard commentsStartIndex > 0, !isLoadingComments else { return }
        loadComments(for: item, startIndex: max(0, commentsStartIndex - commentsPageSize))
    }

    func loadNextComments(for item: WorkshopItem) {
        guard commentsNextStartIndex > commentsStartIndex,
              commentsNextStartIndex < commentsTotal,
              !isLoadingComments else { return }
        loadComments(for: item, startIndex: commentsNextStartIndex)
    }

    func postComment(for item: WorkshopItem) {
        let text = commentDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, commentsCanPost, !isPostingComment else { return }
        isPostingComment = true
        commentsError = nil
        SteamServiceManager.shared.postComment(item: item, text: text) { [weak self] result in
            guard let self else { return }
            self.isPostingComment = false
            guard self.commentsItemID == item.publishedFileId else { return }
            switch result {
            case .success:
                self.commentDraft = ""
                self.loadComments(for: item, startIndex: 0)
            case .failure(let error):
                self.commentsError = error.localizedDescription
            }
        }
    }

    private func subscriptionPage(startIndex: Int) async throws -> WorkshopSubscriptionPage {
        try await withCheckedThrowingContinuation { continuation in
            SteamServiceManager.shared.fetchSubscriptions(startIndex: startIndex) {
                continuation.resume(with: $0)
            }
        }
    }

    private func loadSubscriptionItems(for records: [WorkshopSubscription]) async throws -> [WorkshopItem] {
        var details: [WorkshopItem] = []
        let ids = records.map(\.publishedFileId)
        for start in stride(from: 0, to: ids.count, by: 100) {
            let end = min(start + 100, ids.count)
            details.append(contentsOf: try await SteamWebAPI.shared.getFileDetails(
                workshopIds: Array(ids[start..<end])
            ))
        }
        let byID = Dictionary(details.map { ($0.publishedFileId, $0) }, uniquingKeysWith: { first, _ in first })
        return records.map { byID[$0.publishedFileId] ?? .unavailableSubscription(id: $0.publishedFileId) }
    }

    private func rebuildSubscriptionPage(startIndex: Int) {
        let filtered = subscriptionCatalogItems.filter(matchesSubscriptionFilters)
        let maximumStart = filtered.isEmpty ? 0 : (filtered.count - 1) / subscriptionPageSize * subscriptionPageSize
        let clampedStart = min(max(0, startIndex), maximumStart)
        subscriptionTotal = filtered.count
        subscriptionStartIndex = clampedStart
        subscriptionItems = Array(filtered.dropFirst(clampedStart).prefix(subscriptionPageSize))
    }

    private func matchesSubscriptionFilters(_ item: WorkshopItem) -> Bool {
        if !subscriptionShowOnly.matches(
            workshopItem: item,
            favoriteIDs: SteamServiceManager.shared.workshopFavoriteIDs
        ) {
            return false
        }

        let query = subscriptionSearchText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !query.isEmpty {
            let searchableValues = [
                item.title,
                item.itemDescription,
                item.creatorDisplayName,
                item.creatorSteamId,
                item.publishedFileId
            ] + item.tags
            guard searchableValues.contains(where: { $0.localizedCaseInsensitiveContains(query) }) else {
                return false
            }
        }

        switch subscriptionTypeFilter {
        case .all:
            break
        case .scene:
            guard item.kind == .scene else { return false }
        case .web:
            guard item.kind == .web else { return false }
        case .video:
            guard item.kind == .video else { return false }
        case .preset:
            guard item.isPreset else { return false }
        }

        if !subscriptionAgeRatingFilter.isEmpty,
           subscriptionAgeRatingFilter != .all,
           !subscriptionAgeRatingFilter.contains(item.ageRating ?? .everyone) {
            return false
        }

        let selectableTags = Set(WorkshopTag.allCases.map(\.rawValue))
        if !subscriptionSelectedTags.isEmpty,
           !selectableTags.isSubset(of: subscriptionSelectedTags) {
            let itemTags = Set(item.tags.map { $0.lowercased() })
            guard subscriptionSelectedTags.allSatisfy({ itemTags.contains($0.lowercased()) }) else {
                return false
            }
        }

        return FRResolutionFilter.matches(
            tags: item.tags,
            widescreen: subscriptionWidescreenResolution,
            ultraWidescreen: subscriptionUltraWidescreenResolution,
            dualscreen: subscriptionDualscreenResolution,
            triplescreen: subscriptionTriplescreenResolution,
            portrait: subscriptionPortraitResolution,
            misc: subscriptionMiscResolution
        )
    }

    private func cancelDownloadForUnsubscribe(workshopId: String) {
        guard let task = downloadQueue.first(where: { $0.id == workshopId }) else { return }
        if let attemptID = task.attemptID {
            cancelledDownloadIDs.insert(attemptID)
            SteamServiceManager.shared.cancelDownload(taskId: attemptID)
        }
        backgroundAutoApplyIDs.remove(workshopId)
        downloadQueue.removeAll { $0.id == workshopId }
        processDownloadQueue()
    }

    // MARK: - Download

    func downloadItem(_ item: WorkshopItem, purpose: DownloadPurpose = .wallpaper) {
        guard !isWorkshopItemInstalled(item.publishedFileId) else { return }
        if let existingIndex = downloadQueue.firstIndex(where: { $0.id == item.publishedFileId }) {
            switch downloadQueue[existingIndex].state {
            case .failed, .completed:
                downloadQueue.removeAll { $0.id == item.publishedFileId }
            case .queued, .resolving, .downloading, .validating:
                if purpose == .presetDependency {
                    downloadQueue[existingIndex].purpose = purpose
                }
                return
            }
        }

        let task = DownloadTask(
            workshopItem: item,
            attemptID: nil,
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
        guard let attemptID = downloadQueue[index].attemptID else { return }
        cancelledDownloadIDs.insert(attemptID)
        SteamServiceManager.shared.cancelDownload(taskId: attemptID)
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
        downloadStore.state(for: workshopId)
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
        prepareWorkshopInteractions(for: item)
    }

    private func processDownloadQueue() {
        guard steamSetupState == .ready else { return }
        let maxConcurrent = 3
        var currentActive = downloadQueue.filter {
            if case .downloading = $0.state { return true }
            if case .resolving = $0.state { return true }
            if case .validating = $0.state { return true }
            return false
        }.count

        while currentActive < maxConcurrent,
              let nextIndex = downloadQueue.firstIndex(where: {
                  if case .queued = $0.state { return true }
                  return false
              }) {
            let workshopId = downloadQueue[nextIndex].workshopItem.publishedFileId
            let attemptID = UUID().uuidString
            downloadQueue[nextIndex].attemptID = attemptID
            downloadQueue[nextIndex].state = .resolving
            downloadQueue[nextIndex].startedAt = Date()
            currentActive += 1

            SteamServiceManager.shared.downloadItem(
                workshopId: workshopId,
                taskId: attemptID
            ) { [weak self] state in
                guard let self else { return }
                guard let idx = self.downloadQueue.firstIndex(where: {
                    $0.id == workshopId && $0.attemptID == attemptID
                }) else { return }

                self.downloadQueue[idx].state = state

                if self.cancelledDownloadIDs.contains(attemptID), case .failed = state {
                    self.cancelledDownloadIDs.remove(attemptID)
                    self.downloadQueue.removeAll { $0.id == workshopId && $0.attemptID == attemptID }
                    self.processDownloadQueue()
                    return
                }

                if case .completed = state {
                    self.cancelledDownloadIDs.remove(attemptID)
                    if let directory = SteamServiceManager.shared.downloadedItemDirectory(workshopId: workshopId) {
                        WallpaperLibrary.shared.recordAdded(at: directory, workshopID: workshopId)
                    }
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
                    if SteamServiceManager.shared.isLoggedIn {
                        self.processDownloadQueue()
                    }
                } else if case .resolving = state {
                    self.steamServiceStatus.workshopDownload = .checking
                }
            }
        }
    }

    private func isWorkshopItemInstalled(_ workshopId: String) -> Bool {
        installedWorkshopIDs.contains(workshopId) ||
            WallpaperLibrary.shared.workshopItemDirectory(for: workshopId) != nil
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
        SteamServiceManager.shared.logout { [weak self] result in
            guard let self else { return }
            self.isLoggingOut = false
            switch result {
            case .success:
                self.steamServiceStatus.authentication = .needsAction(L("已退出登录"))
                self.logoutResultMessage = L("已退出 Mirage 的 Steam 会话。")
                self.subscriptionRecords = []
                self.subscriptionCatalogItems = []
                self.subscriptionItems = []
                self.subscriptionTotal = 0
                self.subscriptionStartIndex = 0
                self.subscriptionSearchText = ""
                self.subscriptionSelectedTags = []
                self.subscriptionTypeFilter = .all
                self.subscriptionAgeRatingFilter = .all
                self.subscriptionWidescreenResolution = .all
                self.subscriptionUltraWidescreenResolution = .all
                self.subscriptionDualscreenResolution = .all
                self.subscriptionTriplescreenResolution = .all
                self.subscriptionPortraitResolution = .all
                self.subscriptionMiscResolution = .all
                self.subscriptionStates = [:]
                self.checkingSubscriptionIDs = []
                self.changingSubscriptionIDs = []
                self.subscriptionActionError = nil
                self.subscriptionActionErrorItemID = nil
                self.workshopFavoriteIDs = []
                self.changingFavoriteIDs = []
                self.favoriteActionError = nil
                self.favoriteActionErrorItemID = nil
                self.isPreparingSubscriptionDownloads = false
                self.subscriptionDownloadPlan = nil
                self.comments = []
                self.commentsTotal = 0
                self.commentsStartIndex = 0
                self.commentsNextStartIndex = 0
                self.commentsCanPost = false
                self.commentsItemID = nil
                self.commentAuthors = [:]
                self.commentDraft = ""
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
                openSteamSetupIfActionable()
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
            openSteamSetupIfActionable()
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

            if purpose == .subscription,
               let wallpaper = self.installedItem(workshopId: workshopId),
               wallpaper.needsPresetDependency,
               let dependencyID = wallpaper.presetDependency?.rawValue {
                Task { @MainActor in
                    let dependencyItem = (try? await SteamWebAPI.shared.getFileDetails(
                        workshopIds: [dependencyID]
                    ).first(where: { $0.publishedFileId == dependencyID }))
                        ?? .dependencyPlaceholder(id: dependencyID)
                    guard dependencyItem.kind != .unsupported,
                          !self.isInstalled(dependencyID) else { return }
                    self.downloadItem(dependencyItem, purpose: .subscription)
                }
                return
            }

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
