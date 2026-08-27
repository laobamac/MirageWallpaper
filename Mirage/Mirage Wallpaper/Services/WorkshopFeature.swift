//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

/// The Workshop feature's composition root. Mutable UI state belongs to the
/// focused stores below; this type only creates them and connects cross-store
/// callbacks.
@MainActor
final class WorkshopFeature {
    let browseStore: WorkshopBrowseStore
    let creatorStore: WorkshopCreatorStore
    let discoverStore: DiscoverStore
    let downloadStore: WorkshopDownloadStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let sessionStore: SteamSessionStore
    let subscriptionStore: SubscriptionStore

    init() {
        let browseStore = WorkshopBrowseStore()
        let creatorStore = WorkshopCreatorStore()
        let discoverStore = DiscoverStore()
        let downloadStore = WorkshopDownloadStore()
        let interactionStore = WorkshopInteractionStore()
        let libraryStore = WorkshopLibraryStore()
        let sessionStore = SteamSessionStore()
        let subscriptionStore = SubscriptionStore()
        let selectionCoordinator = WorkshopSelectionCoordinator(
            libraryStore: libraryStore,
            downloadStore: downloadStore,
            subscriptionStore: subscriptionStore,
            interactionStore: interactionStore,
            sessionStore: sessionStore
        )

        self.browseStore = browseStore
        self.creatorStore = creatorStore
        self.discoverStore = discoverStore
        self.downloadStore = downloadStore
        self.interactionStore = interactionStore
        self.libraryStore = libraryStore
        self.selectionCoordinator = selectionCoordinator
        self.sessionStore = sessionStore
        self.subscriptionStore = subscriptionStore

        selectionCoordinator.configure(
            dismissCreatorProfile: { [weak creatorStore] in
                creatorStore?.close()
            },
            applyWallpaper: { wallpaper in
                AppDelegate.shared.wallpaperViewModel.requestApply(wallpaper)
            }
        )

        libraryStore.configure(
            onSnapshotChanged: { [weak downloadStore] installedWorkshopIDs in
                downloadStore?.reconcileCompletedDownloads(
                    installedWorkshopIDs: installedWorkshopIDs
                )
            },
            onMetadataLoaded: { [weak subscriptionStore] items in
                subscriptionStore?.refreshStates(for: items)
            }
        )

        creatorStore.configure(
            onOpen: { [weak selectionCoordinator] in
                selectionCoordinator?.prepareForCreatorProfile()
            },
            onItemsLoaded: { [weak browseStore, weak subscriptionStore] items in
                browseStore?.rememberCreators(in: items)
                subscriptionStore?.refreshStates(for: items)
            }
        )

        browseStore.configure(
            onItemsLoaded: { [weak subscriptionStore] items in
                subscriptionStore?.refreshStates(for: items)
            },
            onBrowsingAPIStateChange: { [weak sessionStore] state in
                sessionStore?.updateBrowsingAPIState(state)
            }
        )

        interactionStore.configure(
            isSteamReady: { [weak sessionStore] in
                sessionStore?.isReady ?? false
            },
            openSteamSetup: { [weak sessionStore] in
                sessionStore?.openSetupIfActionable()
            },
            onFavoritesChanged: { [weak browseStore, weak subscriptionStore] in
                browseStore?.favoritesDidChange()
                subscriptionStore?.favoritesDidChange()
            }
        )

        downloadStore.configure(
            canProcessDownloads: { [weak sessionStore] in
                sessionStore?.isReady ?? false
            },
            isInstalled: { [weak libraryStore] workshopID in
                libraryStore?.isInstalledOrOnDisk(workshopID) ?? false
            },
            onServiceStateChange: { [weak sessionStore] state in
                sessionStore?.handleDownloadState(state)
            },
            onCompletedDownload: { [weak selectionCoordinator] workshopID, purpose in
                selectionCoordinator?.handleDownloadCompletion(
                    workshopID: workshopID,
                    purpose: purpose
                )
            }
        )

        discoverStore.configure(
            prepareSelection: { [weak selectionCoordinator] in
                selectionCoordinator?.prepareRemoteSelection() ?? 0
            },
            isSelectionCurrent: { [weak selectionCoordinator] generation in
                selectionCoordinator?.isSelectionCurrent(generation) ?? false
            },
            onItemsLoaded: { [weak browseStore, weak subscriptionStore] items in
                browseStore?.rememberCreators(in: items)
                subscriptionStore?.refreshStates(for: items)
            },
            onDetailLoaded: { [weak selectionCoordinator] item in
                selectionCoordinator?.showLoadedDetail(item)
            }
        )

        subscriptionStore.configure(
            isSteamReady: { [weak sessionStore] in
                sessionStore?.isReady ?? false
            },
            selectedItemID: { [weak selectionCoordinator] in
                selectionCoordinator?.selectedItem?.publishedFileId
            },
            isInstalled: { [weak libraryStore] workshopID in
                libraryStore?.isInstalledOrOnDisk(workshopID) ?? false
            },
            pendingDownloadIDs: { [weak downloadStore] in
                downloadStore?.pendingWorkshopIDs ?? []
            },
            openSteamSetup: { [weak sessionStore] in
                sessionStore?.openSetupIfActionable()
            },
            downloadItem: { [weak downloadStore] item, purpose in
                downloadStore?.download(item, purpose: purpose)
            },
            cancelDownload: { [weak selectionCoordinator] workshopID in
                selectionCoordinator?.cancelDownloadForUnsubscribe(workshopID: workshopID)
            },
            onItemsLoaded: { [weak browseStore] items in
                browseStore?.rememberCreators(in: items)
            },
            onLibraryChanged: { [weak libraryStore] in
                libraryStore?.refreshInstalled()
            }
        )

        sessionStore.configure(
            onLoggedIn: {
                downloadStore.processQueue()
                if subscriptionStore.catalogItems.isEmpty && !subscriptionStore.isLoading {
                    subscriptionStore.refresh(startIndex: 0)
                }
                if let item = selectionCoordinator.selectedItem {
                    subscriptionStore.refreshStates(for: [item])
                    interactionStore.loadComments(for: item, startIndex: 0)
                }
            },
            onLoggedOut: {
                subscriptionStore.reset()
                interactionStore.reset()
            }
        )
    }
}

// MARK: - Notification Names

extension Notification.Name {
    static let workshopItemDownloaded = Notification.Name("workshopItemDownloaded")
    static let favoritesChanged = Notification.Name("favoritesChanged")
}
