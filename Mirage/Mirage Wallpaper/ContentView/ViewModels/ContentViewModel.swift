//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers
import Combine

struct ScreenSaverFeedback: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

class ContentViewModel: ObservableObject, DropDelegate {
    @AppStorage("SortingBy") var sortingBy: WEWallpaperSortingMethod = .name {
        didSet {
            currentPage = 1
            if sortingBy == .fileSize { prewarmWallpaperSizes() }
        }
    }
    @AppStorage("SortingSequence") var sortingSequence: WEWallpaperSortingSequence = .increase {
        didSet { currentPage = 1 }
    }

    @AppStorage("FRShowOnly") public var showOnly = FRShowOnly.all { didSet { currentPage = 1 } }
    @AppStorage("FRType") public var type = FRType.all { didSet { currentPage = 1 } }
    @AppStorage("FRAgeRating") public var ageRating = FRAgeRating.all { didSet { currentPage = 1 } }
    @AppStorage("FRWidescreenResolution") public var widescreenResolution = FRWidescreenResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRUltraWidescreenResolution") public var ultraWidescreenResolution = FRUltraWidescreenResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRDualscreenResolution") public var dualscreenResolution = FRDualscreenResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRTriplescreenResolution") public var triplescreenResolution = FRTriplescreenResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRPortraitScreenResolution") public var potraitscreenResolution = FRPortraitScreenResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRMiscResolution") public var miscResolution = FRMiscResolution.all { didSet { currentPage = 1 } }
    @AppStorage("FRSource") public var source = FRSource.all { didSet { currentPage = 1 } }
    @AppStorage("FRTag") public var tag = FRTag.all { didSet { currentPage = 1 } }
    
    @AppStorage("FilterReveal") var isFilterReveal = false
    @AppStorage("ExplorerIconSize") var explorerIconSize: Double = 200
    
    @Published var isDisplaySettingsReveal = false
    @Published var importAlertPresented = false
    @Published var isStaging = false
    
    @Published var topTabBarSelection: Int = 0
    @Published var topTabBarHoverSelection: Int = -1

    @Published var wallpapers = [WEWallpaper]() {
        didSet { scheduleRecomputePage() }
    }
    
    @Published var isUnsafeWallpaperWarningPresented = false
    
    @Published var hoveredWallpaper: WEWallpaper?
    
    @Published var isUnsubscribeConfirming = false

    @Published var screenSaverFeedback: ScreenSaverFeedback?

    @Published var searchText = "" { didSet { currentPage = 1 } }

    @Published var isSteamSetupPresented = false
    
    @AppStorage("WallpapersPerPage") var wallpapersPerPage: Int = 50 {
        didSet { currentPage = 1 }
    }
    
    var importAlertError: WPImportError? = nil

    private var downloadObserver: AnyCancellable?
    private var favoritesObserver: AnyCancellable?
    private var refreshWorkItem: DispatchWorkItem?
    private var refreshInFlight = false
    private var refreshAgain = false

    convenience init(isStaging: Bool, topTabBarSelection: Int = 0) {
        self.init()
        self.isStaging = isStaging
        self.topTabBarSelection = topTabBarSelection
        refresh()
    }

    init() {
        downloadObserver = NotificationCenter.default.publisher(for: .workshopItemDownloaded)
            .debounce(for: .seconds(1), scheduler: RunLoop.main)
            .sink { [weak self] _ in
                self?.refresh()
            }

        favoritesObserver = NotificationCenter.default.publisher(for: .favoritesChanged)
            .sink { [weak self] _ in
                self?.scheduleRecomputePage()
            }

        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            WallpaperLibrary.shared.startMonitoringWorkshopDirectory { [weak self] in
                self?.scheduleRefresh()
            }
        }
    }

    @Published public var currentPage: Int = 1 {
        didSet { scheduleRecomputePage() }
    }

    struct WallpaperPage {
        let items: [WEWallpaper]
        let pageCount: Int
    }

    // Cached, background-computed result of the search/filter/sort/paginate
    // pipeline. Recomputed only when a genuine input changes, never per frame
    // inside a view body.
    @Published private(set) var wallpaperPage = WallpaperPage(items: [], pageCount: 1)

    private var recomputeGeneration: UInt64 = 0
    private let pipelineQueue = DispatchQueue(label: "cn.laobamac.Mirage.library.pipeline", qos: .userInitiated)

    private var allWallpapers: [WEWallpaper] { wallpapers }

    func importWallpapers(urls: [URL]) {
        self.isStaging = true
        DispatchQueue.global(qos: .userInitiated).async {
            var lastError: WPImportError?
            for url in urls {
                do { try WallpaperLibrary.shared.importAny(at: url) }
                catch let e as WPImportError { lastError = e }
                catch { lastError = .unknown }
            }
            DispatchQueue.main.async {
                WEWallpaper.invalidateSizeCache()
                if let e = lastError { self.alertImportModal(which: e) }
                self.refresh()
            }
        }
    }
    
    // Immutable snapshot of every input the pipeline reads. Captured on the
    // main thread, then processed on a background queue so the search, filter,
    // sort and paginate work never runs inside a SwiftUI view body.
    private struct PipelineInput {
        let wallpapers: [WEWallpaper]
        let searchText: String
        let showOnly: FRShowOnly
        let type: FRType
        let ageRating: FRAgeRating
        let source: FRSource
        let tag: FRTag
        let sortingBy: WEWallpaperSortingMethod
        let sortingSequence: WEWallpaperSortingSequence
        let wallpapersPerPage: Int
        let currentPage: Int
        let favorites: Set<String>
        let importedPrefix: String
    }

    private func currentPipelineInput() -> PipelineInput {
        PipelineInput(
            wallpapers: wallpapers,
            searchText: searchText,
            showOnly: showOnly,
            type: type,
            ageRating: ageRating,
            source: source,
            tag: tag,
            sortingBy: sortingBy,
            sortingSequence: sortingSequence,
            wallpapersPerPage: wallpapersPerPage,
            currentPage: currentPage,
            favorites: FavoritesManager.shared.snapshot(),
            importedPrefix: WallpaperLibrary.shared.importedDirectory.path)
    }

    // Coalesce bursts of input changes (typing, rapid filter toggles) into a
    // single background pass, and drop stale results via a generation token.
    func scheduleRecomputePage() {
        recomputeGeneration &+= 1
        let generation = recomputeGeneration
        let input = currentPipelineInput()
        pipelineQueue.async { [weak self] in
            let page = Self.computePage(input)
            DispatchQueue.main.async {
                guard let self, self.recomputeGeneration == generation else { return }
                self.wallpaperPage = page
            }
        }
    }

    private static func computePage(_ input: PipelineInput) -> WallpaperPage {
        let searched = searched(input)
        let filtered = filtered(searched, input)
        let sorted = sorted(filtered, input)

        guard input.wallpapersPerPage > 0 else {
            return WallpaperPage(items: sorted, pageCount: 1)
        }
        let pageCount = max(1, Int(ceil(Double(sorted.count) / Double(input.wallpapersPerPage))))
        let page = min(max(input.currentPage, 1), pageCount)
        let startIndex = (page - 1) * input.wallpapersPerPage
        guard startIndex < sorted.count else {
            return WallpaperPage(items: [], pageCount: pageCount)
        }
        let endIndex = min(startIndex + input.wallpapersPerPage, sorted.count)
        return WallpaperPage(items: Array(sorted[startIndex..<endIndex]), pageCount: pageCount)
    }

    private static func searched(_ input: PipelineInput) -> [WEWallpaper] {
        let query = input.searchText.lowercased()
        guard !query.isEmpty else { return input.wallpapers }
        return input.wallpapers.filter { wallpaper in
            let project = wallpaper.project
            if project.title.lowercased().contains(query) { return true }
            if project.type.lowercased().contains(query) { return true }
            if let description = project.description?.lowercased(), description.contains(query) {
                return true
            }
            if let tags = project.tags,
               tags.contains(where: { $0.localizedCaseInsensitiveContains(query) }) {
                return true
            }
            if let workshopid = project.workshopid, workshopid.rawValue.contains(query) {
                return true
            }
            if wallpaper.wallpaperDirectory.lastPathComponent.lowercased().contains(query) {
                return true
            }
            return false
        }
    }

    private static func filtered(_ wallpapers: [WEWallpaper], _ input: PipelineInput) -> [WEWallpaper] {
        wallpapers.filter { wallpaper in
            // 仅显示：未选或全选时不筛选；否则按 AND 逻辑要求每项命中
            let activeShowOnly = input.showOnly
            if !activeShowOnly.isEmpty && activeShowOnly != FRShowOnly.all {
                if activeShowOnly.contains(.approved) {
                    guard wallpaper.project.approved == true else { return false }
                }
                if activeShowOnly.contains(.myFavourites) {
                    guard input.favorites.contains(wallpaper.id) else { return false }
                }
                if activeShowOnly.contains(.customizable) {
                    guard let props = wallpaper.project.general?.properties,
                          !props.items.isEmpty else { return false }
                }
                if activeShowOnly.contains(.mobileCompatible) {
                    let tags = (wallpaper.project.tags ?? []).map { $0.lowercased() }
                    guard tags.contains(where: { $0.contains("mobile") }) else { return false }
                }
                if activeShowOnly.contains(.audioResponsive) {
                    let tags = (wallpaper.project.tags ?? []).map { $0.lowercased() }
                    guard tags.contains(where: { $0.contains("audio") }) else { return false }
                }
            }

            var type = FRType.none
            if wallpaper.isPreset {
                type = .preset
            } else {
                switch wallpaper.project.type.lowercased() {
                case "video":
                    type = .video
                case "scene":
                    type = .scene
                case "web":
                    type = .web
                case "application":
                    type = .application
                default:
                    break
                }
            }
            let selectedTypes = input.type == .legacyAll ? FRType.all : input.type
            guard selectedTypes.contains(type) else { return false }

            var ageRating: FRAgeRating
            switch wallpaper.project.contentrating {
            case "Everyone":
                ageRating = .everyone
            case "Questionable":
                ageRating = .partialNudity
            case "Mature":
                ageRating = .mature
            default:
                ageRating = .none
            }
            guard input.ageRating.contains(ageRating) else { return false }

            var source = FRSource.none
            if wallpaper.wallpaperDirectory.path.hasPrefix(input.importedPrefix) {
                source = .myWallpapers
            } else {
                source = .workshop
            }
            guard input.source.contains(source) else { return false }

            if input.tag != FRTag.all {
                let wallpaperTags = FRTag.bits(from: wallpaper.project.tags ?? [])
                if wallpaperTags.isEmpty {
                    guard input.tag.contains(.unspecifiedGenre) else { return false }
                } else {
                    guard !input.tag.intersection(wallpaperTags).isEmpty else { return false }
                }
            }

            return true
        }
    }

    private static func sorted(_ wallpapers: [WEWallpaper], _ input: PipelineInput) -> [WEWallpaper] {
        wallpapers.sorted {
            let comparison: ComparisonResult
            switch input.sortingBy {
            case .name:
                comparison = $0.project.title.localizedStandardCompare($1.project.title)
            case .rating:
                comparison = ($0.project.contentrating ?? "0")
                    .localizedStandardCompare($1.project.contentrating ?? "0")
            case .fileSize:
                if $0.wallpaperSize == $1.wallpaperSize {
                    comparison = $0.project.title.localizedStandardCompare($1.project.title)
                } else {
                    comparison = $0.wallpaperSize < $1.wallpaperSize ? .orderedAscending : .orderedDescending
                }
            }
            return input.sortingSequence == .increase
                ? comparison == .orderedAscending
                : comparison == .orderedDescending
        }
    }

    public var autoRefreshWallpapers: [WEWallpaper] {
        wallpaperPage.items
    }

    var maxPage: Int {
        wallpaperPage.pageCount
    }

    func toggleFilter() {
        isFilterReveal.toggle()
    }
    
    func alertImportModal(which error: WPImportError) {
        self.importAlertError = error
        self.importAlertPresented = true
    }
    
    func warningUnsafeWallpaperModal(which wallpaper: WEWallpaper) {
        self.isUnsafeWallpaperWarningPresented = true
    }
    
    func dropUpdated(info: DropInfo) -> DropProposal? {
        let proposal = DropProposal(operation: .copy)
        return proposal
    }

    func performDrop(info: DropInfo) -> Bool {
        let providers = info.itemProviders(for: [UTType.fileURL])
        guard !providers.isEmpty else {
            alertImportModal(which: .unknown)
            return false
        }
        var urls: [URL] = []
        let group = DispatchGroup()
        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                defer { group.leave() }
                if let data = item as? Data, let url = URL(dataRepresentation: data, relativeTo: nil) {
                    urls.append(url)
                }
            }
        }
        group.notify(queue: .main) { [weak self] in
            self?.importWallpapers(urls: urls)
        }
        return true
    }
    
    public func refresh() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.refresh() }
            return
        }
        if refreshInFlight {
            refreshAgain = true
            return
        }
        refreshInFlight = true
        let shouldPrewarmSizes = sortingBy == .fileSize
        DispatchQueue.global(qos: .userInitiated).async {
            let loaded = WallpaperLibrary.shared.loadAll()
            if shouldPrewarmSizes {
                loaded.forEach { _ = $0.wallpaperSize }
            }
            DispatchQueue.main.async {
                self.wallpapers = loaded
                self.refreshInFlight = false
                if self.refreshAgain {
                    self.refreshAgain = false
                    self.refresh()
                }
            }
        }
    }

    private func scheduleRefresh() {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.refreshWorkItem?.cancel()
            let work = DispatchWorkItem { [weak self] in self?.refresh() }
            self.refreshWorkItem = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: work)
        }
    }

    private func prewarmWallpaperSizes() {
        let snapshot = wallpapers
        DispatchQueue.global(qos: .utility).async {
            snapshot.forEach { _ = $0.wallpaperSize }
            DispatchQueue.main.async { [weak self] in self?.scheduleRecomputePage() }
        }
    }
    
    public func reset() {
        self.showOnly = .none
        self.type = .all
        self.ageRating = .all
        self.widescreenResolution = .all
        self.ultraWidescreenResolution = .all
        self.dualscreenResolution = .all
        self.triplescreenResolution = .all
        self.potraitscreenResolution = .all
        self.miscResolution = .all
        self.source = .all
        self.tag = .all
    }
}

extension Array: RawRepresentable where Element: Codable {
    public init?(rawValue: String) {
        guard let data = rawValue.data(using: .utf8),
              let result = try? JSONDecoder().decode([Element].self, from: data)
        else {
            return nil
        }
        self = result
    }
    
    public var rawValue: String {
        guard let data = try? JSONEncoder().encode(self),
              let result = String(data: data, encoding: .utf8)
        else {
            return "[]"
        }
        return result
    }
}
