//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import SwiftUI
import UniformTypeIdentifiers
import Combine
import CoreGraphics

struct ScreenSaverFeedback: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

enum SceneMobileExportDestination {
    case device(MobileDevice)
    case file
}

struct SceneMobileExportRequest: Identifiable {
    let id = UUID()
    let wallpaper: WEWallpaper
    let destination: SceneMobileExportDestination
}

class ContentViewModel: ObservableObject, DropDelegate {
    @AppStorage("SortingBy") var sortingBy: WEWallpaperSortingMethod = .name {
        didSet {
            currentPage = 1
            if sortingBy == .fileSize { prewarmWallpaperSizes() }
            if sortingBy == .recentlyAdded { sortingSequence = .decrease }
        }
    }
    @AppStorage("SortingSequence") var sortingSequence: WEWallpaperSortingSequence = .increase {
        didSet { currentPage = 1 }
    }

    @AppStorage("FRShowOnly") public var showOnly = FRShowOnly.none { didSet { currentPage = 1 } }
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
    @AppStorage("ExplorerIconSize") var explorerIconSize: Double = 170
    
    @Published var importAlertPresented = false
    @Published var isStaging = false
    
    @Published var wallpapers = [WEWallpaper]() {
        didSet { scheduleRecomputePage() }
    }
    
    /// The wallpaper the trust sheet is currently asking about, together with
    /// what to do once the user confirms. Carrying both here is what keeps the
    /// sheet honest: it used to read `nextCurrentWallpaper` at render time,
    /// which is written *after* the `willSet` that presents the sheet, so the
    /// dialog could name the previously selected wallpaper while authorizing
    /// this one. Per-screen requests also need the screen index preserved.
    struct PendingTrustRequest: Identifiable {
        enum Action {
            case applyToCurrent
            case applyOnDisplay(CGDirectDisplayID)
            case applyToAllDisplays
        }
        let id = UUID()
        let wallpaper: WEWallpaper
        let action: Action
    }

    @Published var pendingTrustRequest: PendingTrustRequest?

    @Published var hoveredWallpaper: WEWallpaper?
    
    @Published var isUnsubscribeConfirming = false

    @Published var screenSaverFeedback: ScreenSaverFeedback?

    @Published var pendingSceneMobileExport: SceneMobileExportRequest?

    func exportMobileMPKG(_ wallpaper: WEWallpaper, to outputURL: URL) {
        let progressModel = MobileTransferProgressModel.shared
        let progressID = progressModel.startExport(
            wallpaperTitle: wallpaper.project.title,
            initialPhase: wallpaper.kind == .scene ? .converting : .preparing
        )
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                switch wallpaper.kind {
                case .video:
                    try MobileMPKGExporter.export(wallpaper, to: outputURL) { completed, total in
                        progressModel.updatePreparation(
                            id: progressID,
                            completedBytes: completed,
                            totalBytes: total
                        )
                    }
                case .scene:
                    try SceneMobileMPKGExporter.export(wallpaper, to: outputURL) { fraction in
                        progressModel.updateConversion(id: progressID, fraction: fraction)
                    }
                case .web, .unsupported:
                    throw MobileMPKGExportError.unsupportedWallpaperType(wallpaper.kind)
                }
                progressModel.complete(id: progressID)
            } catch {
                progressModel.fail(id: progressID, message: error.localizedDescription)
                DispatchQueue.main.async {
                    self?.screenSaverFeedback = ScreenSaverFeedback(
                        title: L("导出 .mpkg 失败"),
                        message: error.localizedDescription
                    )
                }
            }
        }
    }

    func presentMobileMPKGSavePanel(for wallpaper: WEWallpaper) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "mpkg") ?? .data]
        panel.nameFieldStringValue = MobileMPKGExporter.suggestedFilename(for: wallpaper)
        panel.prompt = L("导出")

        let completion: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportMobileMPKG(wallpaper, to: url)
        }
        // Keep the save panel independent from the main window. Attaching an
        // NSSavePanel as a sheet can make SwiftUI/AppKit renegotiate the host
        // window's fitting size when the sheet is removed, which visibly
        // resizes the adaptive wallpaper grid.
        panel.begin(completionHandler: completion)
    }

    // Debounced: every keystroke used to kick off a full search + filter + sort
    // over the whole library. The pipeline already runs off the main thread, but
    // typing "landscape" still queued nine complete passes of which only the
    // last mattered. Matches the 500 ms debounce the Workshop search already had.
    @Published var searchText = "" {
        didSet {
            guard searchText != oldValue else { return }
            searchDebounceWorkItem?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self else { return }
                if self.currentPage != 1 {
                    self.currentPage = 1  // its didSet schedules the recompute
                } else {
                    self.scheduleRecomputePage()
                }
            }
            searchDebounceWorkItem = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
        }
    }

    private var searchDebounceWorkItem: DispatchWorkItem?

    @Published var isSteamSetupPresented = false
    
    @AppStorage("WallpapersPerPage") var wallpapersPerPage: Int = 50 {
        didSet { currentPage = 1 }
    }
    
    var importAlertError: WPImportError? = nil

    private var downloadObserver: AnyCancellable?
    private var favoritesObserver: AnyCancellable?
    private var workshopFavoritesObserver: AnyCancellable?
    private var refreshWorkItem: DispatchWorkItem?
    private var refreshInFlight = false
    private var refreshAgain = false
    @Published private(set) var isRefreshing = false

    convenience init(isStaging: Bool) {
        self.init()
        self.isStaging = isStaging
        refresh()
    }

    init() {
        let showOnlyMigrationKey = "FRShowOnlyMigrationV2"
        if !UserDefaults.standard.bool(forKey: showOnlyMigrationKey) {
            if let legacyRaw = UserDefaults.standard.object(forKey: "FRShowOnly") as? Int {
                showOnly = FRShowOnly.migratedLegacyRawValue(legacyRaw)
            } else {
                showOnly = .none
            }
            UserDefaults.standard.set(true, forKey: showOnlyMigrationKey)
        } else {
            showOnly = FRShowOnly(rawValue: showOnly.rawValue & FRShowOnly.all.rawValue)
        }
        switch explorerIconSize {
        case 100:
            explorerIconSize = 140
        case 125:
            explorerIconSize = 170
        case 150:
            explorerIconSize = 200
        case 140, 170, 200:
            break
        default:
            explorerIconSize = 170
        }
        let resolutionMigrationKey = "FRWidescreenResolutionMigrationV2"
        if !UserDefaults.standard.bool(forKey: resolutionMigrationKey) {
            let raw = widescreenResolution.rawValue
            if raw == FRWidescreenResolution.legacyAll.rawValue
                || raw == FRWidescreenResolution.interimAll.rawValue
                || raw == FRWidescreenResolution.all.rawValue {
                widescreenResolution = .all
            } else {
                var migratedRaw = 0
                if raw & (1 << 0) != 0 { migratedRaw |= 1 << 0 }
                if raw & (1 << 1) != 0 { migratedRaw |= 1 << 1 }
                if raw & (1 << 2) != 0 { migratedRaw |= 1 << 3 }
                if raw & (1 << 3) != 0 { migratedRaw |= 1 << 4 }
                if raw & (1 << 4) != 0 { migratedRaw |= 1 << 5 }
                if raw & (1 << 5) != 0 { migratedRaw |= 1 << 2 }
                if raw & (1 << 6) != 0 { migratedRaw |= 1 << 0 }
                widescreenResolution = FRWidescreenResolution(rawValue: migratedRaw)
            }
            UserDefaults.standard.set(true, forKey: resolutionMigrationKey)
        }
        downloadObserver = NotificationCenter.default.publisher(for: .workshopItemDownloaded)
            .debounce(for: .seconds(1), scheduler: RunLoop.main)
            .sink { [weak self] _ in
                self?.refresh()
            }

        favoritesObserver = NotificationCenter.default.publisher(for: .favoritesChanged)
            .sink { [weak self] _ in
                self?.scheduleRecomputePage()
            }

        workshopFavoritesObserver = SteamServiceManager.shared.$workshopFavoriteIDs
            .receive(on: RunLoop.main)
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
        let widescreenResolution: FRWidescreenResolution
        let ultraWidescreenResolution: FRUltraWidescreenResolution
        let dualscreenResolution: FRDualscreenResolution
        let triplescreenResolution: FRTriplescreenResolution
        let potraitscreenResolution: FRPortraitScreenResolution
        let miscResolution: FRMiscResolution
        let source: FRSource
        let tag: FRTag
        let sortingBy: WEWallpaperSortingMethod
        let sortingSequence: WEWallpaperSortingSequence
        let wallpapersPerPage: Int
        let currentPage: Int
        let favorites: Set<String>
        let workshopFavorites: Set<String>
        let importedPrefix: String
        let additionDates: [String: Date]
    }

    private func currentPipelineInput() -> PipelineInput {
        PipelineInput(
            wallpapers: wallpapers,
            searchText: searchText,
            showOnly: showOnly,
            type: type,
            ageRating: ageRating,
            widescreenResolution: widescreenResolution,
            ultraWidescreenResolution: ultraWidescreenResolution,
            dualscreenResolution: dualscreenResolution,
            triplescreenResolution: triplescreenResolution,
            potraitscreenResolution: potraitscreenResolution,
            miscResolution: miscResolution,
            source: source,
            tag: tag,
            sortingBy: sortingBy,
            sortingSequence: sortingSequence,
            wallpapersPerPage: wallpapersPerPage,
            currentPage: currentPage,
            favorites: FavoritesManager.shared.snapshot(),
            workshopFavorites: SteamServiceManager.shared.workshopFavoriteIDs,
            importedPrefix: WallpaperLibrary.shared.importedDirectory.path,
            additionDates: WallpaperLibrary.shared.additionDates(for: wallpapers))
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
            let activeShowOnly = input.showOnly
            if !activeShowOnly.isEmpty && !activeShowOnly.matches(
                wallpaper: wallpaper,
                localFavoriteIDs: input.favorites,
                workshopFavoriteIDs: input.workshopFavorites,
                importedDirectoryPrefix: input.importedPrefix
            ) {
                return false
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

            guard FRResolutionFilter.matches(
                wallpaper: wallpaper,
                widescreen: input.widescreenResolution,
                ultraWidescreen: input.ultraWidescreenResolution,
                dualscreen: input.dualscreenResolution,
                triplescreen: input.triplescreenResolution,
                portrait: input.potraitscreenResolution,
                misc: input.miscResolution
            ) else { return false }

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
            case .recentlyAdded:
                let left = input.additionDates[$0.id] ?? .distantPast
                let right = input.additionDates[$1.id] ?? .distantPast
                if left == right {
                    let title = $0.project.title.localizedStandardCompare($1.project.title)
                    comparison = title == .orderedSame
                        ? $0.wallpaperDirectory.path.localizedStandardCompare($1.wallpaperDirectory.path)
                        : title
                } else {
                    comparison = left < right ? .orderedAscending : .orderedDescending
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
    
    func warningUnsafeWallpaperModal(which wallpaper: WEWallpaper,
                                     action: PendingTrustRequest.Action = .applyToCurrent) {
        self.pendingTrustRequest = PendingTrustRequest(wallpaper: wallpaper, action: action)
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
        isRefreshing = true
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
                } else {
                    self.isRefreshing = false
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
