//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Observation

@MainActor
@Observable
final class WorkshopSelectionCoordinator {
    private(set) var selectedItem: WorkshopItem?
    private(set) var showCustomization = false
    private(set) var presetDependencyPrompt: PresetDependencyPrompt?

    private let libraryStore: WorkshopLibraryStore
    private let downloadStore: WorkshopDownloadStore
    private let subscriptionStore: SubscriptionStore
    private let interactionStore: WorkshopInteractionStore
    private let sessionStore: SteamSessionStore

    @ObservationIgnored private var pendingPresetApplication: (
        presetID: String,
        dependencyID: String,
        selectionGeneration: Int
    )?
    @ObservationIgnored private var pendingBackgroundPresetApplication: (
        presetID: String,
        dependencyID: String
    )?
    @ObservationIgnored private var backgroundAutoApplyIDs: Set<String> = []
    @ObservationIgnored private var completionTasks: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var selectionGeneration = 0
    @ObservationIgnored private var dismissCreatorProfile: () -> Void = {}
    @ObservationIgnored private var applyWallpaper: (WEWallpaper) -> Void = { _ in }

    init(
        libraryStore: WorkshopLibraryStore,
        downloadStore: WorkshopDownloadStore,
        subscriptionStore: SubscriptionStore,
        interactionStore: WorkshopInteractionStore,
        sessionStore: SteamSessionStore
    ) {
        self.libraryStore = libraryStore
        self.downloadStore = downloadStore
        self.subscriptionStore = subscriptionStore
        self.interactionStore = interactionStore
        self.sessionStore = sessionStore
    }

    func configure(
        dismissCreatorProfile: @escaping () -> Void,
        applyWallpaper: @escaping (WEWallpaper) -> Void
    ) {
        self.dismissCreatorProfile = dismissCreatorProfile
        self.applyWallpaper = applyWallpaper
    }

    func prepareRemoteSelection() -> Int {
        selectionGeneration += 1
        selectedItem = nil
        showCustomization = false
        dismissCreatorProfile()
        return selectionGeneration
    }

    func isSelectionCurrent(_ generation: Int) -> Bool {
        selectionGeneration == generation
    }

    func showLoadedDetail(_ item: WorkshopItem) {
        selectedItem = item
        prepareInteractions(for: item)
    }

    func showDetail(_ item: WorkshopItem) {
        selectionGeneration += 1
        selectedItem = item
        showCustomization = false
        dismissCreatorProfile()
        prepareInteractions(for: item)
    }

    func prepareForCreatorProfile() {
        selectionGeneration += 1
        showCustomization = false
        pendingPresetApplication = nil
        pendingBackgroundPresetApplication = nil
        presetDependencyPrompt = nil
    }

    private func prepareInteractions(for item: WorkshopItem) {
        subscriptionStore.refreshStates(for: [item])
        interactionStore.prepareComments(for: item)
    }

    func selectWorkshopItem(_ item: WorkshopItem) {
        selectionGeneration += 1
        dismissCreatorProfile()
        let installed = libraryStore.loadInstalledItem(id: item.publishedFileId)
        if let wallpaper = installed, wallpaper.needsPresetDependency {
            showCustomization = false
            selectedItem = item
            requestPresetDependency(for: wallpaper)
        } else if let wallpaper = installed, wallpaper.isValid {
            applyWallpaper(wallpaper)
            showCustomization = true
            selectedItem = item
        } else {
            showCustomization = false
            selectedItem = item
        }
        prepareInteractions(for: item)
    }

    func openInstalledWallpaper(_ wallpaper: WEWallpaper) {
        // Re-resolve first: cached `.missingDependency` state can be stale after
        // its base wallpaper finishes downloading.
        let fresh = WEWallpaper.load(from: wallpaper.wallpaperDirectory)
        if fresh.needsPresetDependency {
            dismissCreatorProfile()
            requestPresetDependency(for: fresh)
        } else if fresh.isValid {
            dismissCreatorProfile()
            applyWallpaper(fresh)
            showCustomization = true
            selectedItem = nil
        }
    }

    func cancelDownload(_ item: WorkshopItem) {
        backgroundAutoApplyIDs.remove(item.publishedFileId)
        downloadStore.cancel(item)
    }

    func cancelDownloadForUnsubscribe(workshopID: String) {
        backgroundAutoApplyIDs.remove(workshopID)
        downloadStore.cancelForUnsubscribe(workshopID: workshopID)
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
        let requestSelectionGeneration = selectionGeneration
        Task { @MainActor [weak self] in
            guard let self else { return }
            let dependencyItem: WorkshopItem
            do {
                dependencyItem = try await SteamWebAPI.shared.getFileDetails(
                    workshopIds: [dependencyID]
                ).first(where: { $0.publishedFileId == dependencyID })
                    ?? .dependencyPlaceholder(id: dependencyID)
            } catch {
                dependencyItem = .dependencyPlaceholder(id: dependencyID)
            }
            guard self.selectionGeneration == requestSelectionGeneration,
                  self.selectedItem?.publishedFileId == presetID else { return }
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
        if let pending = pendingBackgroundPresetApplication,
           pending.presetID == prompt.presetID,
           pending.dependencyID == prompt.dependencyID {
            downloadStore.download(prompt.dependencyItem, purpose: .presetDependency)
            if !sessionStore.isReady {
                sessionStore.openSetupIfActionable()
            }
            return
        }
        guard selectedItem?.publishedFileId == prompt.presetID else { return }
        pendingPresetApplication = (
            prompt.presetID,
            prompt.dependencyID,
            selectionGeneration
        )

        if let preset = libraryStore.loadInstalledItem(id: prompt.presetID), preset.isValid {
            pendingPresetApplication = nil
            openInstalledWallpaper(preset)
            return
        }

        downloadStore.download(prompt.dependencyItem, purpose: .presetDependency)
        if !sessionStore.isReady {
            sessionStore.openSetupIfActionable()
        }
    }

    func dismissPresetDependencyPrompt() {
        presetDependencyPrompt = nil
        pendingBackgroundPresetApplication = nil
    }

    func handleDownloadCompletion(
        workshopID: String,
        purpose: DownloadPurpose
    ) {
        let selectedItemID = selectedItem?.publishedFileId
        let requestSelectionGeneration = selectionGeneration
        completionTasks[workshopID]?.cancel()
        completionTasks[workshopID] = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            guard !Task.isCancelled, let self else { return }
            self.completionTasks[workshopID] = nil
            self.handleCompletedDownload(
                workshopID: workshopID,
                purpose: purpose,
                selectedItemID: selectedItemID,
                selectionGeneration: requestSelectionGeneration
            )
        }
    }

    private func handleCompletedDownload(
        workshopID: String,
        purpose: DownloadPurpose,
        selectedItemID: String?,
        selectionGeneration: Int
    ) {
        if purpose == .subscription,
           let wallpaper = libraryStore.loadInstalledItem(id: workshopID),
           wallpaper.needsPresetDependency,
           let dependencyID = wallpaper.presetDependency?.rawValue {
            Task { @MainActor [weak self] in
                guard let self else { return }
                let dependencyItem = (try? await SteamWebAPI.shared.getFileDetails(
                    workshopIds: [dependencyID]
                ).first(where: { $0.publishedFileId == dependencyID }))
                    ?? .dependencyPlaceholder(id: dependencyID)
                guard dependencyItem.kind != .unsupported,
                      !self.libraryStore.isInstalledOrOnDisk(dependencyID) else { return }
                self.downloadStore.download(dependencyItem, purpose: .subscription)
            }
            return
        }

        if purpose == .presetDependency {
            if let pending = pendingBackgroundPresetApplication,
               pending.dependencyID == workshopID,
               let preset = libraryStore.loadInstalledItem(id: pending.presetID),
               preset.isValid {
                pendingBackgroundPresetApplication = nil
                applyWallpaper(preset)
                return
            }
            guard let pending = pendingPresetApplication,
                  pending.dependencyID == workshopID,
                  pending.selectionGeneration == selectionGeneration,
                  selectedItemID == pending.presetID,
                  self.selectionGeneration == selectionGeneration,
                  selectedItem?.publishedFileId == pending.presetID,
                  let preset = libraryStore.loadInstalledItem(id: pending.presetID),
                  preset.isValid else { return }
            pendingPresetApplication = nil
            openInstalledWallpaper(preset)
            return
        }

        if backgroundAutoApplyIDs.remove(workshopID) != nil,
           let wallpaper = libraryStore.loadInstalledItem(id: workshopID) {
            if wallpaper.needsPresetDependency,
               let dependencyID = wallpaper.presetDependency?.rawValue {
                pendingBackgroundPresetApplication = (workshopID, dependencyID)
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    let dependencyItem = (try? await SteamWebAPI.shared.getFileDetails(
                        workshopIds: [dependencyID]
                    ).first(where: { $0.publishedFileId == dependencyID }))
                        ?? .dependencyPlaceholder(id: dependencyID)
                    guard self.pendingBackgroundPresetApplication?.presetID == workshopID else {
                        return
                    }
                    self.presetDependencyPrompt = PresetDependencyPrompt(
                        presetID: workshopID,
                        presetTitle: wallpaper.project.title,
                        dependencyID: dependencyID,
                        dependencyItem: dependencyItem
                    )
                }
            } else if wallpaper.isValid {
                applyWallpaper(wallpaper)
            }
            return
        }

        guard selectedItemID == workshopID,
              self.selectionGeneration == selectionGeneration,
              selectedItem?.publishedFileId == workshopID,
              let wallpaper = libraryStore.loadInstalledItem(id: workshopID) else { return }
        if wallpaper.needsPresetDependency {
            requestPresetDependency(for: wallpaper)
        } else if wallpaper.isValid {
            openInstalledWallpaper(wallpaper)
        }
    }
}
