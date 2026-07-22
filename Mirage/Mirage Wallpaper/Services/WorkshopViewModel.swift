//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Combine

class WorkshopViewModel: ObservableObject {
    // MARK: - Browse State

    @Published var items: [WorkshopItem] = []
    @Published var searchText: String = ""
    @Published var selectedTags: Set<String> = []
    @Published var sortOrder: WorkshopSortOrder = .trending
    @Published var typeFilter: WorkshopTypeFilter = .all
    @Published var currentPage: Int = 1
    @Published var totalItems: Int = 0
    @Published var isLoading: Bool = false
    @Published var error: String?

    @Published var selectedItem: WorkshopItem?
    @Published var showCustomization: Bool = false

    let itemsPerPage = 30

    // MARK: - Discover State

    @Published var trendingItems: [WorkshopItem] = []
    @Published var mostRecentItems: [WorkshopItem] = []
    @Published var mostSubscribedItems: [WorkshopItem] = []
    @Published var topRatedItems: [WorkshopItem] = []
    @Published var animeItems: [WorkshopItem] = []
    @Published var natureItems: [WorkshopItem] = []
    @Published var abstractItems: [WorkshopItem] = []
    @Published var landscapeItems: [WorkshopItem] = []
    @Published var isDiscoverLoading: Bool = false
    @Published var bannerItems: [WorkshopItem] = []

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
        max(1, Int(ceil(Double(totalItems) / Double(itemsPerPage))))
    }

    var activeDownloadCount: Int {
        downloadQueue.filter {
            if case .downloading = $0.state { return true }
            if case .starting = $0.state { return true }
            if case .validating = $0.state { return true }
            return false
        }.count
    }

    private var searchDebounce: AnyCancellable?
    private var serviceStateCancellables = Set<AnyCancellable>()
    private var cancelledDownloadIDs: Set<String> = []
    private var pendingPresetApplication: (presetID: String, dependencyID: String)?

    init() {
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

    private let installedScanQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.workshop.installed", qos: .utility)

    func refreshInstalledState() {
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
            }
        }
    }

    func isInstalled(_ workshopId: String) -> Bool {
        installedWorkshopIDs.contains(workshopId)
    }

    func presetNeedsDependency(_ workshopId: String) -> Bool {
        presetsNeedingDependency.contains(workshopId)
    }

    // MARK: - Setup Check

    func checkSteamSetup() {
        let cmdManager = SteamCMDManager.shared
        steamServiceStatus.steamCMD = .checking
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let path = cmdManager.detectSteamCMD()
            DispatchQueue.main.async {
                guard let self else { return }
                if let path {
                    self.steamServiceStatus.steamCMD = .available(path.path)
                    cmdManager.refreshSessionIfNeeded()
                } else {
                    self.steamServiceStatus.steamCMD = .unavailable("未安装 SteamCMD")
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
            steamServiceStatus.workshopDownload = .needsAction("需要先安装 SteamCMD")
        } else if !cmdManager.isLoggedIn {
            steamSetupState = .needsLogin
            if cmdManager.savedUsername.isEmpty {
                steamServiceStatus.authentication = .needsAction("需要登录 Steam")
            }
            steamServiceStatus.workshopDownload = .needsAction("需要有效的 Steam 会话")
        } else {
            steamSetupState = .ready
            steamServiceStatus.authentication = .available("会话已验证")
            if case .unknown = steamServiceStatus.workshopDownload {
                steamServiceStatus.workshopDownload = .needsAction("尚未开始下载")
            }
        }
    }

    // MARK: - Search

    func search() {
        guard !isLoading else { return }
        isLoading = true
        error = nil
        steamServiceStatus.browsingAPI = .checking

        Task { @MainActor in
            do {
                let result = try await SteamWebAPI.shared.queryFiles(
                    searchText: searchText,
                    tags: Array(selectedTags),
                    sortOrder: sortOrder,
                    typeFilter: typeFilter,
                    page: currentPage,
                    perPage: itemsPerPage
                )

                // 创意工坊在线数据不含 approved/mobile/audio/customizable 标志，
                // "仅显示"分区仅为与「已安装」保持 UI 一致，不收窄结果，避免空列表。
                self.items = result.items
                self.totalItems = result.total
                self.isLoading = false
                self.steamServiceStatus.browsingAPI = .available("Steam Web API 可用")
            } catch {
                self.error = error.localizedDescription
                self.isLoading = false
                self.steamServiceStatus.browsingAPI = .unavailable(error.localizedDescription)
            }
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

    func clearFilters() {
        selectedTags.removeAll()
        searchText = ""
        typeFilter = .all
        sortOrder = .trending
        currentPage = 1
        search()
    }

    // MARK: - Discover

    func loadDiscover() {
        guard !isDiscoverLoading else { return }
        isDiscoverLoading = true

        Task { @MainActor in
            async let trending = SteamWebAPI.shared.fetchTrending(count: 15)
            async let recent = SteamWebAPI.shared.fetchMostRecent(count: 10)
            async let subscribed = SteamWebAPI.shared.fetchMostSubscribed(count: 10)
            async let rated = SteamWebAPI.shared.fetchTopRated(count: 10)
            async let anime = SteamWebAPI.shared.fetchByTag("Anime", count: 10)
            async let nature = SteamWebAPI.shared.fetchByTag("Nature", count: 10)
            async let abstract = SteamWebAPI.shared.fetchByTag("Abstract", count: 10)
            async let landscape = SteamWebAPI.shared.fetchByTag("Landscape", count: 10)

            do {
                self.trendingItems = try await trending
                self.mostRecentItems = try await recent
                self.mostSubscribedItems = try await subscribed
                self.topRatedItems = try await rated
                self.animeItems = try await anime
                self.natureItems = try await nature
                self.abstractItems = try await abstract
                self.landscapeItems = try await landscape

                self.bannerItems = Array(self.trendingItems.prefix(5))
            } catch {
                NSLog("[Mirage] 加载发现页失败: \(error.localizedDescription)")
            }
            self.isDiscoverLoading = false
        }
    }

    // MARK: - Download

    func downloadItem(_ item: WorkshopItem, purpose: DownloadPurpose = .wallpaper) {
        if let existing = downloadQueue.first(where: { $0.id == item.publishedFileId }) {
            switch existing.state {
            case .failed, .completed:
                downloadQueue.removeAll { $0.id == item.publishedFileId }
            case .queued, .starting, .downloading, .validating:
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
        let installed = installedItem(workshopId: item.publishedFileId)
        if let wallpaper = installed, wallpaper.needsPresetDependency {
            showCustomization = false
            selectedItem = item
            requestPresetDependency(for: wallpaper)
        } else if let wallpaper = installed, wallpaper.isValid {
            AppDelegate.shared.wallpaperViewModel.nextCurrentWallpaper = wallpaper
            showCustomization = true
            selectedItem = nil
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
                self.steamServiceStatus.workshopDownload = .available("最近一次下载已验证")
                self.downloadQueue[idx].completedAt = Date()
                self.processDownloadQueue()
                NotificationCenter.default.post(name: .workshopItemDownloaded, object: workshopId)
                self.handleCompletedDownload(workshopId: workshopId)
            } else if case .failed = state {
                self.steamServiceStatus.workshopDownload = .unavailable("最近一次下载失败")
                if SteamCMDManager.shared.isLoggedIn {
                    self.processDownloadQueue()
                }
            } else if case .starting = state {
                self.steamServiceStatus.workshopDownload = .checking
            }
        }
    }

    // MARK: - Navigate to Workshop with filter

    func navigateToWorkshopWithTag(_ tag: String) {
        selectedTags = [tag]
        searchText = ""
        typeFilter = .all
        sortOrder = .trending
        showCustomization = false
        currentPage = 1
        search()
    }

    func navigateToWorkshopWithSort(_ sort: WorkshopSortOrder) {
        selectedTags.removeAll()
        searchText = ""
        typeFilter = .all
        sortOrder = sort
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
                self.steamServiceStatus.authentication = .needsAction("已退出登录")
                self.logoutResultMessage = "已退出 Mirage 专用 SteamCMD 会话。"
            case .failure(let error):
                self.steamServiceStatus.authentication = .needsAction(error.localizedDescription)
                self.logoutResultMessage = error.localizedDescription
            }
            self.refreshSetupState()
        }
    }

    // MARK: - Auto Apply

    func openInstalledWallpaper(_ wallpaper: WEWallpaper) {
        if wallpaper.needsPresetDependency {
            requestPresetDependency(for: wallpaper)
        } else if wallpaper.isValid {
            AppDelegate.shared.wallpaperViewModel.nextCurrentWallpaper = wallpaper
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
        pendingPresetApplication = (prompt.presetID, prompt.dependencyID)

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
    }

    private func handleCompletedDownload(workshopId: String) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self else { return }

            if let pending = self.pendingPresetApplication,
               pending.dependencyID == workshopId,
               let preset = self.installedItem(workshopId: pending.presetID),
               preset.isValid {
                self.pendingPresetApplication = nil
                self.openInstalledWallpaper(preset)
                return
            }

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
