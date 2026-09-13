//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import SwiftUI
import Observation
import UniformTypeIdentifiers
import Combine
import CoreGraphics

enum ScreenSaverFeedbackAction: Equatable {
    case dismiss
    case openFullDiskAccess
}

struct ScreenSaverFeedback: Identifiable {
    let id = UUID()
    let title: String
    let message: String
    let action: ScreenSaverFeedbackAction

    init(title: String, message: String, action: ScreenSaverFeedbackAction = .dismiss) {
        self.title = title
        self.message = message
        self.action = action
    }
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

@Observable
class ContentViewModel: DropDelegate {
    var sortingBy = UIStoredValue.rawValue("SortingBy", fallback: WEWallpaperSortingMethod.name) {
        didSet {
            guard sortingBy != oldValue else { return }
            UserDefaults.standard.set(sortingBy.rawValue, forKey: "SortingBy")
            currentPage = 1
            if sortingBy == .fileSize { prewarmWallpaperSizes() }
            if sortingBy == .recentlyAdded { sortingSequence = .decrease }
        }
    }
    var sortingSequence = UIStoredValue.rawValue("SortingSequence", fallback: WEWallpaperSortingSequence.increase) {
        didSet {
            guard sortingSequence != oldValue else { return }
            UserDefaults.standard.set(sortingSequence.rawValue, forKey: "SortingSequence")
            currentPage = 1
        }
    }

    public var showOnly = UIStoredValue.rawValue("FRShowOnly", fallback: FRShowOnly.none) {
        didSet {
            guard showOnly != oldValue else { return }
            UserDefaults.standard.set(showOnly.rawValue, forKey: "FRShowOnly")
            currentPage = 1
        }
    }
    public var type = UIStoredValue.rawValue("FRType", fallback: FRType.all) {
        didSet {
            guard type != oldValue else { return }
            UserDefaults.standard.set(type.rawValue, forKey: "FRType")
            currentPage = 1
        }
    }
    public var ageRating = UIStoredValue.rawValue("FRAgeRating", fallback: FRAgeRating.all) {
        didSet {
            guard ageRating != oldValue else { return }
            UserDefaults.standard.set(ageRating.rawValue, forKey: "FRAgeRating")
            currentPage = 1
        }
    }
    public var widescreenResolution = UIStoredValue.rawValue("FRWidescreenResolution", fallback: FRWidescreenResolution.all) {
        didSet {
            guard widescreenResolution != oldValue else { return }
            UserDefaults.standard.set(widescreenResolution.rawValue, forKey: "FRWidescreenResolution")
            currentPage = 1
        }
    }
    public var ultraWidescreenResolution = UIStoredValue.rawValue("FRUltraWidescreenResolution", fallback: FRUltraWidescreenResolution.all) {
        didSet {
            guard ultraWidescreenResolution != oldValue else { return }
            UserDefaults.standard.set(ultraWidescreenResolution.rawValue, forKey: "FRUltraWidescreenResolution")
            currentPage = 1
        }
    }
    public var dualscreenResolution = UIStoredValue.rawValue("FRDualscreenResolution", fallback: FRDualscreenResolution.all) {
        didSet {
            guard dualscreenResolution != oldValue else { return }
            UserDefaults.standard.set(dualscreenResolution.rawValue, forKey: "FRDualscreenResolution")
            currentPage = 1
        }
    }
    public var triplescreenResolution = UIStoredValue.rawValue("FRTriplescreenResolution", fallback: FRTriplescreenResolution.all) {
        didSet {
            guard triplescreenResolution != oldValue else { return }
            UserDefaults.standard.set(triplescreenResolution.rawValue, forKey: "FRTriplescreenResolution")
            currentPage = 1
        }
    }
    public var potraitscreenResolution = UIStoredValue.rawValue("FRPortraitScreenResolution", fallback: FRPortraitScreenResolution.all) {
        didSet {
            guard potraitscreenResolution != oldValue else { return }
            UserDefaults.standard.set(potraitscreenResolution.rawValue, forKey: "FRPortraitScreenResolution")
            currentPage = 1
        }
    }
    public var miscResolution = UIStoredValue.rawValue("FRMiscResolution", fallback: FRMiscResolution.all) {
        didSet {
            guard miscResolution != oldValue else { return }
            UserDefaults.standard.set(miscResolution.rawValue, forKey: "FRMiscResolution")
            currentPage = 1
        }
    }
    public var source = UIStoredValue.rawValue("FRSource", fallback: FRSource.all) {
        didSet {
            guard source != oldValue else { return }
            UserDefaults.standard.set(source.rawValue, forKey: "FRSource")
            currentPage = 1
        }
    }
    public var tag = UIStoredValue.rawValue("FRTag", fallback: FRTag.all) {
        didSet {
            guard tag != oldValue else { return }
            UserDefaults.standard.set(tag.rawValue, forKey: "FRTag")
            currentPage = 1
        }
    }
    
    var isFilterReveal = UIStoredValue.value("FilterReveal", fallback: false) {
        didSet {
            guard isFilterReveal != oldValue else { return }
            UserDefaults.standard.set(isFilterReveal, forKey: "FilterReveal")
        }
    }
    var explorerIconSize = UIStoredValue.value("ExplorerIconSize", fallback: 170.0) {
        didSet {
            guard explorerIconSize != oldValue else { return }
            UserDefaults.standard.set(explorerIconSize, forKey: "ExplorerIconSize")
        }
    }
    
    var importAlertPresented = false
    var isStaging = false
    
    var wallpapers = [WEWallpaper]() {
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

    var pendingTrustRequest: PendingTrustRequest?

    var hoveredWallpaper: WEWallpaper?
    
    var isUnsubscribeConfirming = false

    var screenSaverFeedback: ScreenSaverFeedback?

    var pendingSceneMobileExport: SceneMobileExportRequest?

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
    var searchText = "" {
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

    var isSteamSetupPresented = false
    
    var wallpapersPerPage = UIStoredValue.value("WallpapersPerPage", fallback: 50) {
        didSet {
            guard wallpapersPerPage != oldValue else { return }
            UserDefaults.standard.set(wallpapersPerPage, forKey: "WallpapersPerPage")
            currentPage = 1
        }
    }
    
    var importAlertError: WPImportError? = nil

    private var downloadObserver: AnyCancellable?
    private var favoritesObserver: AnyCancellable?
    private var workshopFavoritesObserver: AnyCancellable?
    private var refreshWorkItem: DispatchWorkItem?
    private var refreshInFlight = false
    private var refreshAgain = false
    private(set) var isRefreshing = false

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

    public var currentPage: Int = 1 {
        didSet { scheduleRecomputePage() }
    }

    struct WallpaperPage {
        let items: [WEWallpaper]
        let pageCount: Int
    }

    // Cached, background-computed result of the search/filter/sort/paginate
    // pipeline. Recomputed only when a genuine input changes, never per frame
    // inside a view body.
    private(set) var wallpaperPage = WallpaperPage(items: [], pageCount: 1)

    private var recomputeScheduled = false
    private let pagePipeline = LatestValueWorker<PipelineInput, WallpaperPage>(
        label: "cn.laobamac.Mirage.library.pipeline", process: ContentViewModel.computePage)

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
        var importedPrefix: String
        var additionDates: [String: Date]
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
            importedPrefix: "",
            additionDates: [:])
    }

    // Coalesce bursts of input changes (typing, rapid filter toggles) into a
    // single background pass, and drop stale results via a generation token.
    func scheduleRecomputePage() {
        pagePipeline.cancel()
        guard !recomputeScheduled else { return }
        recomputeScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.recomputeScheduled = false
            self.pagePipeline.submit(self.currentPipelineInput()) { [weak self] page in
                guard let self else { return }
                let sameItems = self.wallpaperPage.items.count == page.items.count &&
                    zip(self.wallpaperPage.items, page.items).allSatisfy { $0.hasSamePresentation(as: $1) }
                if !sameItems || self.wallpaperPage.pageCount != page.pageCount {
                    self.wallpaperPage = page
                }
            }
        }
    }

    private static func computePage(_ snapshot: PipelineInput) -> WallpaperPage {
        var input = snapshot
        input.importedPrefix = WallpaperLibrary.shared.importedDirectory.path
        if input.sortingBy == .recentlyAdded {
            input.additionDates = WallpaperLibrary.shared.additionDates(for: input.wallpapers)
        }
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
