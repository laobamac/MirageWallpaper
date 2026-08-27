//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

protocol SubviewOfContentView: View {
    var viewModel: ContentViewModel { get set }
}

enum MainSection: Int, CaseIterable, Hashable {
    case installed
    case discover
    case workshop
    case subscriptions
}

final class MainNavigationModel: ObservableObject {
    @Published var selection: MainSection

    init(selection: MainSection = .installed) {
        self.selection = selection
    }
}

private struct FilterSidebarLayout<Sidebar: View, Content: View>: View {
    private let isPresented: Bool
    private let sidebar: Sidebar
    private let content: Content

    init(
        isPresented: Bool,
        @ViewBuilder sidebar: () -> Sidebar,
        @ViewBuilder content: () -> Content
    ) {
        self.isPresented = isPresented
        self.sidebar = sidebar()
        self.content = content()
    }

    var body: some View {
        ZStack(alignment: .leading) {
            content
                .padding(.leading, isPresented ? 235 : 0)
                .animation(nil, value: isPresented)
            sidebar
                .frame(width: 225)
                .offset(x: isPresented ? 0 : -225)
                .opacity(isPresented ? 1 : 0)
                .allowsHitTesting(isPresented)
                .accessibilityHidden(!isPresented)
                .animation(.easeInOut(duration: 0.18), value: isPresented)
        }
        .clipped()
    }
}

struct ContentView: View {
    @EnvironmentObject var globalSettingsViewModel: GlobalSettingsViewModel
    @ObservedObject private var localization = MirageLocalization.shared

    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let workshopFeature: WorkshopFeature
    @ObservedObject var navigationModel: MainNavigationModel
    @ObservedObject private var shortcutManager = WallpaperShortcutManager.shared
    @ObservedObject private var dynamicLockScreenManager = DynamicLockScreenManager.shared
    @ObservedObject private var screenSaverDynamicLockScreenManager = ScreenSaverDynamicLockScreenManager.shared
    @StateObject private var steamSetupViewModel = SteamSetupViewModel()
    @State private var loadedSections: Set<MainSection>

    init(
        viewModel: ContentViewModel,
        wallpaperViewModel: WallpaperViewModel,
        workshopFeature: WorkshopFeature? = nil,
        navigationModel: MainNavigationModel = AppDelegate.shared.navigationModel
    ) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.workshopFeature = workshopFeature ?? AppDelegate.shared.workshopFeature
        self.navigationModel = navigationModel
        _loadedSections = State(initialValue: [navigationModel.selection])
    }

    var body: some View {
        ZStack {
            HSplitView {
                if viewModel.isStaging {
                    VStack(spacing: 5) {
                        TopTabBar(
                            navigationModel: navigationModel,
                            wallpaperViewModel: wallpaperViewModel,
                            downloadStore: workshopFeature.downloadStore
                        )
                        ProjectFeedbackBanner()
                        ZStack {
                            if loadedSections.contains(.installed) {
                                VStack(spacing: 5) {
                                    ExplorerTopBar(contentViewModel: viewModel)
                                        .environmentObject(globalSettingsViewModel)
                                    FilterSidebarLayout(isPresented: viewModel.isFilterReveal, sidebar: {
                                        FilterResults(viewModel: viewModel)
                                    }, content: {
                                        WallpaperExplorer(
                                            contentViewModel: viewModel,
                                            wallpaperViewModel: wallpaperViewModel,
                                            creatorStore: workshopFeature.creatorStore,
                                            interactionStore: workshopFeature.interactionStore,
                                            libraryStore: workshopFeature.libraryStore,
                                            selectionCoordinator: workshopFeature.selectionCoordinator,
                                            isActive: navigationModel.selection == .installed,
                                            animatedPreviewMode: globalSettingsViewModel.settings.animatedPreviewPlaybackMode
                                        )
                                        .onDrop(of: [.fileURL], delegate: viewModel)
                                        .contextMenu {
                                            ExplorerGlobalMenu(
                                                contentViewModel: viewModel,
                                                wallpaperViewModel: wallpaperViewModel
                                            )
                                        }
                                    })
                                    ExplorerBottomBar(contentViewModel: viewModel,
                                                      wallpaperViewModel: wallpaperViewModel)
                                }
                                .sectionVisibility(navigationModel.selection == .installed)
                            }

                            if loadedSections.contains(.discover) {
                                DiscoverView(
                                    discoverStore: workshopFeature.discoverStore,
                                    creatorStore: workshopFeature.creatorStore,
                                    downloadStore: workshopFeature.downloadStore,
                                    interactionStore: workshopFeature.interactionStore,
                                    libraryStore: workshopFeature.libraryStore,
                                    selectionCoordinator: workshopFeature.selectionCoordinator,
                                    subscriptionStore: workshopFeature.subscriptionStore,
                                    viewModel: viewModel,
                                    wallpaperViewModel: wallpaperViewModel,
                                    navigationModel: navigationModel,
                                    isActive: navigationModel.selection == .discover
                                )
                                .sectionVisibility(navigationModel.selection == .discover)
                            }

                            if loadedSections.contains(.workshop) {
                                FilterSidebarLayout(isPresented: viewModel.isFilterReveal, sidebar: {
                                    WorkshopFilterSidebar(
                                        browseStore: workshopFeature.browseStore
                                    )
                                }, content: {
                                    WorkshopView(
                                        browseStore: workshopFeature.browseStore,
                                        creatorStore: workshopFeature.creatorStore,
                                        downloadStore: workshopFeature.downloadStore,
                                        interactionStore: workshopFeature.interactionStore,
                                        libraryStore: workshopFeature.libraryStore,
                                        selectionCoordinator: workshopFeature.selectionCoordinator,
                                        sessionStore: workshopFeature.sessionStore,
                                        subscriptionStore: workshopFeature.subscriptionStore,
                                        viewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        isActive: navigationModel.selection == .workshop
                                    )
                                })
                                .sectionVisibility(navigationModel.selection == .workshop)
                            }

                            if loadedSections.contains(.subscriptions) {
                                FilterSidebarLayout(isPresented: viewModel.isFilterReveal, sidebar: {
                                    SubscribedWorkshopFilterSidebar(
                                        subscriptionStore: workshopFeature.subscriptionStore
                                    )
                                }, content: {
                                    SubscribedWorkshopView(
                                        subscriptionStore: workshopFeature.subscriptionStore,
                                        creatorStore: workshopFeature.creatorStore,
                                        downloadStore: workshopFeature.downloadStore,
                                        interactionStore: workshopFeature.interactionStore,
                                        libraryStore: workshopFeature.libraryStore,
                                        selectionCoordinator: workshopFeature.selectionCoordinator,
                                        sessionStore: workshopFeature.sessionStore,
                                        viewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        isActive: navigationModel.selection == .subscriptions
                                    )
                                })
                                .sectionVisibility(navigationModel.selection == .subscriptions)
                            }
                        }
                        .isolatedFromParentLayout()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .padding()
                    .frame(minWidth: 640)

                    ZStack {
                        WallpaperPreview(contentViewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        creatorStore: workshopFeature.creatorStore,
                                        interactionStore: workshopFeature.interactionStore,
                                        libraryStore: workshopFeature.libraryStore,
                                        selectionCoordinator: workshopFeature.selectionCoordinator,
                                        sessionStore: workshopFeature.sessionStore,
                                        subscriptionStore: workshopFeature.subscriptionStore,
                                        isActive: navigationModel.selection == .installed || workshopFeature.selectionCoordinator.showCustomization)
                            .frame(maxWidth: 320)
                            .sectionVisibility(
                                workshopFeature.creatorStore.selectedCreator == nil &&
                                    (navigationModel.selection == .installed || workshopFeature.selectionCoordinator.showCustomization)
                            )

                        WorkshopItemDetail(
                            item: workshopFeature.selectionCoordinator.selectedItem,
                            browseStore: workshopFeature.browseStore,
                            creatorStore: workshopFeature.creatorStore,
                            downloadStore: workshopFeature.downloadStore,
                            interactionStore: workshopFeature.interactionStore,
                            libraryStore: workshopFeature.libraryStore,
                            selectionCoordinator: workshopFeature.selectionCoordinator,
                            sessionStore: workshopFeature.sessionStore,
                            subscriptionStore: workshopFeature.subscriptionStore,
                            isActive: navigationModel.selection != .installed && workshopFeature.selectionCoordinator.showCustomization == false
                        )
                            .frame(maxWidth: 320)
                            .sectionVisibility(
                                    workshopFeature.creatorStore.selectedCreator == nil &&
                                    navigationModel.selection != .installed &&
                                    workshopFeature.selectionCoordinator.showCustomization == false
                            )

                        if let creator = workshopFeature.creatorStore.selectedCreator {
                            CreatorProfileView(
                                creator: creator,
                                browseStore: workshopFeature.browseStore,
                                creatorStore: workshopFeature.creatorStore,
                                downloadStore: workshopFeature.downloadStore,
                                interactionStore: workshopFeature.interactionStore,
                                libraryStore: workshopFeature.libraryStore,
                                selectionCoordinator: workshopFeature.selectionCoordinator,
                                sessionStore: workshopFeature.sessionStore,
                                subscriptionStore: workshopFeature.subscriptionStore,
                                animatedPreviewMode: globalSettingsViewModel.settings.animatedPreviewPlaybackMode
                            )
                            .frame(maxWidth: 420)
                        }
                    }
                    .frame(
                        minWidth: workshopFeature.creatorStore.selectedCreator != nil ? 360 : 320,
                        idealWidth: workshopFeature.creatorStore.selectedCreator != nil ? 420 : 320,
                        maxWidth: workshopFeature.creatorStore.selectedCreator != nil ? 420 : 360
                    )
                    .layoutPriority(1)
                }
            }
            .opacity(viewModel.isStaging ? 1 : 0)
            .blur(radius: viewModel.isStaging ? 0 : 2.0)

            if !viewModel.isStaging {
                HStack(spacing: 20) {
                    Text("省电模式，休眠中…")
                        .font(.largeTitle)
                }
            }
        }
        .confirmationDialog("删除壁纸",
                            isPresented: $viewModel.isUnsubscribeConfirming) {
            if let wallpaper = viewModel.hoveredWallpaper {
                let url = wallpaper.wallpaperDirectory
                Button("立即删除", role: .destructive) {
                    WEWallpaper.invalidateSizeCache()
                    try? WallpaperLibrary.shared.delete(wallpaper)
                    wallpaperViewModel.removeWallpaper(at: url)
                    viewModel.hoveredWallpaper = nil
                    viewModel.refresh()
                }
                Button("移到废纸篓") {
                    WEWallpaper.invalidateSizeCache()
                    try? WallpaperLibrary.shared.trash(wallpaper)
                    wallpaperViewModel.removeWallpaper(at: url)
                    viewModel.hoveredWallpaper = nil
                    viewModel.refresh()
                }
            }
            Button("取消", role: .cancel) {
                viewModel.hoveredWallpaper = nil
            }
        } message: {
            Text(L("确定要删除“%@”吗？", viewModel.hoveredWallpaper?.project.title ?? L("该壁纸")))
        }
        .alert(isPresented: $viewModel.importAlertPresented, error: viewModel.importAlertError) { }
        .alert(item: $viewModel.screenSaverFeedback) { feedback in
            Alert(
                title: Text(feedback.title),
                message: Text(feedback.message),
                dismissButton: .default(Text("好"))
            )
        }
        .sheet(isPresented: $dynamicLockScreenManager.isConfirmationPresented) {
            DynamicLockScreenConfirmationSheet(manager: dynamicLockScreenManager)
        }
        .sheet(isPresented: $screenSaverDynamicLockScreenManager.isConfirmationPresented) {
            ScreenSaverDynamicLockScreenConfirmationSheet(manager: screenSaverDynamicLockScreenManager)
        }
        .alert(
            "Steam 收藏",
            isPresented: Binding(
                get: { workshopFeature.interactionStore.favoriteActionError != nil },
                set: { if !$0 { workshopFeature.interactionStore.dismissFavoriteError() } }
            )
        ) {
            Button("确定", role: .cancel) {
                workshopFeature.interactionStore.dismissFavoriteError()
            }
        } message: {
            Text(workshopFeature.interactionStore.favoriteActionError ?? "")
        }
        .alert(
            "需要基础壁纸",
            isPresented: Binding(
                get: { workshopFeature.selectionCoordinator.presetDependencyPrompt != nil },
                set: { if !$0 { workshopFeature.selectionCoordinator.dismissPresetDependencyPrompt() } }
            ),
            presenting: workshopFeature.selectionCoordinator.presetDependencyPrompt
        ) { prompt in
            Button("一起下载") {
                workshopFeature.selectionCoordinator.confirmPresetDependencyDownload(prompt)
            }
            Button("暂不", role: .cancel) {
                workshopFeature.selectionCoordinator.dismissPresetDependencyPrompt()
            }
        } message: { prompt in
            Text(prompt.message)
        }
        .sheet(isPresented: $globalSettingsViewModel.isFirstLaunch) {
            FirstLaunchView()
                .environmentObject(globalSettingsViewModel)
        }
        .sheet(item: $shortcutManager.recordingWallpaper, onDismiss: {
            shortcutManager.cancelRecording()
        }) { wallpaper in
            WallpaperShortcutRecorderSheet(
                wallpaper: wallpaper,
                manager: shortcutManager
            )
        }
        .sheet(item: $viewModel.pendingTrustRequest) { request in
            UnsafeWallpaper(request: request)
                .frame(width: 600, height: 300)
        }
        .sheet(isPresented: $viewModel.isSteamSetupPresented, onDismiss: {
            steamSetupViewModel.reset()
        }) {
            SteamSetupView(viewModel: steamSetupViewModel)
                .frame(width: 560, height: 640)
        }
        // Applied outside the sheets so the card also floats over an open sheet,
        // and after them so it is the topmost layer in the window.
        .overlay(alignment: .bottomTrailing) {
            VideoTranscodeOverlay()
                .allowsHitTesting(false)
        }
        .environment(\.locale, localization.locale)
        .frame(minWidth: 1100, minHeight: 640)
        .onChange(of: navigationModel.selection) { _, section in
            loadedSections.insert(section)
        }
        .task {
            for section in MainSection.allCases where !loadedSections.contains(section) {
                await Task.yield()
                loadedSections.insert(section)
            }
        }
    }
}

private extension View {
    func isolatedFromParentLayout() -> some View {
        Color.clear
            .overlay { self }
            .clipped()
    }

    func sectionVisibility(_ visible: Bool) -> some View {
        opacity(visible ? 1 : 0)
            .allowsHitTesting(visible)
            .accessibilityHidden(!visible)
            .zIndex(visible ? 1 : 0)
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView(
            viewModel: .init(isStaging: true),
            wallpaperViewModel: .init(),
            navigationModel: MainNavigationModel()
        )
            .environmentObject(GlobalSettingsViewModel())
    }
}
