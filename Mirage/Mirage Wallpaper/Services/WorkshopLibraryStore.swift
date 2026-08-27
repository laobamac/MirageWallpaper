//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Combine
import Foundation
import Observation

private struct InstalledWallpaperCacheEntry: Equatable {
    let wallpaper: WEWallpaper

    static func == (
        lhs: InstalledWallpaperCacheEntry,
        rhs: InstalledWallpaperCacheEntry
    ) -> Bool {
        lhs.wallpaper.wallpaperDirectory == rhs.wallpaper.wallpaperDirectory
            && lhs.wallpaper.renderDirectory == rhs.wallpaper.renderDirectory
            && lhs.wallpaper.assetOverlayDirectories == rhs.wallpaper.assetOverlayDirectories
            && lhs.wallpaper.project == rhs.wallpaper.project
            && lhs.wallpaper.presetDependency == rhs.wallpaper.presetDependency
            && lhs.wallpaper.presetStatus == rhs.wallpaper.presetStatus
    }
}

@MainActor
@Observable
final class WorkshopLibraryItemStatus: Identifiable, Equatable {
    let id: String
    private(set) var isInstalled: Bool
    private(set) var needsPresetDependency: Bool

    init(
        workshopID: String,
        isInstalled: Bool = false,
        needsPresetDependency: Bool = false
    ) {
        id = workshopID
        self.isInstalled = isInstalled
        self.needsPresetDependency = needsPresetDependency
    }

    nonisolated static func == (
        lhs: WorkshopLibraryItemStatus,
        rhs: WorkshopLibraryItemStatus
    ) -> Bool {
        lhs === rhs
    }

    fileprivate func update(isInstalled: Bool, needsPresetDependency: Bool) {
        self.isInstalled = isInstalled
        self.needsPresetDependency = needsPresetDependency
    }
}

@MainActor
@Observable
final class WorkshopLibraryStore {
    private(set) var installedWorkshopIDs: Set<String> = []
    private(set) var presetsNeedingDependency: Set<String> = []
    private var installedWallpapers: [String: InstalledWallpaperCacheEntry] = [:]
    /// Steam metadata is completed lazily for local wallpapers that the user
    /// actually selects. It is separate from the filesystem snapshot.
    private(set) var installedWorkshopItems: [String: WorkshopItem] = [:]

    @ObservationIgnored
    private let statuses = NSMapTable<NSString, WorkshopLibraryItemStatus>(
        keyOptions: .strongMemory,
        valueOptions: .weakMemory
    )
    @ObservationIgnored private let scanQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.workshop.installed",
        qos: .utility
    )
    @ObservationIgnored private var statusInstalledIDs: Set<String> = []
    @ObservationIgnored private var statusPresetDependencyIDs: Set<String> = []
    @ObservationIgnored private var requestedMetadataIDs: Set<String> = []
    @ObservationIgnored private var cancellables = Set<AnyCancellable>()
    @ObservationIgnored private var onSnapshotChanged: (Set<String>) -> Void = { _ in }
    @ObservationIgnored private var onMetadataLoaded: ([WorkshopItem]) -> Void = { _ in }

    init() {
        NotificationCenter.default.publisher(for: .workshopItemDownloaded)
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.refreshInstalled()
            }
            .store(in: &cancellables)

        NotificationCenter.default.publisher(for: .wallpaperLibraryChanged)
            .receive(on: RunLoop.main)
            .sink { [weak self] notification in
                self?.handleLibraryChange(notification)
            }
            .store(in: &cancellables)

        refreshInstalled()
    }

    func configure(
        onSnapshotChanged: @escaping (Set<String>) -> Void,
        onMetadataLoaded: @escaping ([WorkshopItem]) -> Void
    ) {
        self.onSnapshotChanged = onSnapshotChanged
        self.onMetadataLoaded = onMetadataLoaded
    }

    /// Returns a stable, per-item observable so one installation change does
    /// not invalidate every card that displays the library snapshot.
    func status(for workshopID: String) -> WorkshopLibraryItemStatus {
        let key = workshopID as NSString
        if let status = statuses.object(forKey: key) {
            return status
        }
        let status = WorkshopLibraryItemStatus(
            workshopID: workshopID,
            isInstalled: statusInstalledIDs.contains(workshopID),
            needsPresetDependency: statusPresetDependencyIDs.contains(workshopID)
        )
        statuses.setObject(status, forKey: key)
        return status
    }

    func isInstalled(_ id: String) -> Bool {
        installedWorkshopIDs.contains(id)
    }

    func presetNeedsDependency(_ id: String) -> Bool {
        presetsNeedingDependency.contains(id)
    }

    /// Download and subscription actions may run before the initial background
    /// snapshot finishes. Their non-rendering path gets a filesystem fallback.
    func isInstalledOrOnDisk(_ id: String) -> Bool {
        isInstalled(id) || WallpaperLibrary.shared.workshopItemDirectory(for: id) != nil
    }

    func installedItem(id: String) -> WEWallpaper? {
        installedWallpapers[id]?.wallpaper
    }

    /// Action paths use a fresh read so a just-installed preset can immediately
    /// resolve a dependency before the background snapshot has caught up.
    func loadInstalledItem(id: String) -> WEWallpaper? {
        let installed = WallpaperLibrary.shared.workshopItemDirectories(for: id)
            .map { WEWallpaper.load(from: $0) }
        return installed.first(where: \.isValid)
            ?? installed.first(where: \.isPreset)
            ?? installed.first
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
    /// Prefer verified Steam metadata, then the local author and finally Steam ID.
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
              !requestedMetadataIDs.contains(id) else { return }
        requestedMetadataIDs.insert(id)

        Task { @MainActor [weak self] in
            guard let self else { return }
            let details = (try? await SteamWebAPI.shared.getFileDetails(workshopIds: [id])) ?? []
            let matchingItems = details.filter {
                $0.publishedFileId == id && $0.consumerAppId == 431960
            }
            for item in matchingItems {
                self.installedWorkshopItems[item.publishedFileId] = item
            }
            if matchingItems.isEmpty {
                // A transient API error must be retryable on the next selection.
                self.requestedMetadataIDs.remove(id)
            } else {
                self.onMetadataLoaded(matchingItems)
            }
        }
    }

    func refreshInstalled() {
        scanQueue.async { [weak self] in
            guard let self else { return }
            let directories = WallpaperLibrary.shared.allWorkshopIDDirectories()
            var installed = Set<String>()
            var needsDependency = Set<String>()
            var wallpapers: [String: InstalledWallpaperCacheEntry] = [:]
            installed.reserveCapacity(directories.count)
            wallpapers.reserveCapacity(directories.count)
            for (workshopID, url) in directories {
                installed.insert(workshopID)
                let candidates = WallpaperLibrary.shared
                    .workshopItemDirectories(for: workshopID)
                    .map { WEWallpaper.load(from: $0) }
                let wallpaper = candidates.first(where: \.isValid)
                    ?? candidates.first(where: \.isPreset)
                    ?? candidates.first
                    ?? WEWallpaper.load(from: url)
                wallpapers[workshopID] = InstalledWallpaperCacheEntry(wallpaper: wallpaper)
                if wallpaper.needsPresetDependency {
                    needsDependency.insert(workshopID)
                }
            }
            DispatchQueue.main.async {
                self.applySnapshot(
                    installed: installed,
                    presetsNeedingDependency: needsDependency,
                    wallpapers: wallpapers
                )
            }
        }
    }

    private func handleLibraryChange(_ notification: Notification) {
        if let url = notification.object as? URL {
            let workshopID = url.lastPathComponent
            if WallpaperLibrary.shared.workshopItemDirectory(for: workshopID) == nil {
                installedWorkshopIDs.remove(workshopID)
                presetsNeedingDependency.remove(workshopID)
                installedWallpapers.removeValue(forKey: workshopID)
                statusInstalledIDs.remove(workshopID)
                statusPresetDependencyIDs.remove(workshopID)
                installedWorkshopItems.removeValue(forKey: workshopID)
                requestedMetadataIDs.remove(workshopID)
                statuses.object(forKey: workshopID as NSString)?.update(
                    isInstalled: false,
                    needsPresetDependency: false
                )
            }
        }
        refreshInstalled()
    }

    private func applySnapshot(
        installed: Set<String>,
        presetsNeedingDependency: Set<String>,
        wallpapers: [String: InstalledWallpaperCacheEntry]
    ) {
        let affectedIDs = installedWorkshopIDs
            .union(installed)
            .union(self.presetsNeedingDependency)
            .union(presetsNeedingDependency)

        if installedWorkshopIDs != installed {
            installedWorkshopIDs = installed
        }
        if self.presetsNeedingDependency != presetsNeedingDependency {
            self.presetsNeedingDependency = presetsNeedingDependency
        }
        if installedWallpapers != wallpapers {
            installedWallpapers = wallpapers
        }
        statusInstalledIDs = installed
        statusPresetDependencyIDs = presetsNeedingDependency

        for workshopID in affectedIDs {
            statuses.object(forKey: workshopID as NSString)?.update(
                isInstalled: installed.contains(workshopID),
                needsPresetDependency: presetsNeedingDependency.contains(workshopID)
            )
        }
        onSnapshotChanged(installed)
    }
}
