//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

protocol SubviewOfContentView: View {
    var viewModel: ContentViewModel { get set }
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
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @ObservedObject var rmskinViewModel: RmskinViewModel
    @ObservedObject private var shortcutManager = WallpaperShortcutManager.shared
    @StateObject private var steamSetupViewModel = SteamSetupViewModel()

    init(viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel, workshopViewModel: WorkshopViewModel = AppDelegate.shared.workshopViewModel, rmskinViewModel: RmskinViewModel = AppDelegate.shared.rmskinViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.workshopViewModel = workshopViewModel
        self.rmskinViewModel = rmskinViewModel
    }

    var body: some View {
        ZStack {
            HSplitView {
                if viewModel.isStaging {
                    VStack(spacing: 5) {
                        TopTabBar(contentViewModel: viewModel,
                                  wallpaperViewModel: wallpaperViewModel)
                        ProjectFeedbackBanner()
                        switch viewModel.topTabBarSelection {
                        case 0:
                            ExplorerTopBar(contentViewModel: viewModel)
                                .environmentObject(globalSettingsViewModel)
                            FilterSidebarLayout(isPresented: viewModel.isFilterReveal, sidebar: {
                                    FilterResults(viewModel: viewModel)
                            }, content: {
                                WallpaperExplorer(contentViewModel: viewModel,
                                                  wallpaperViewModel: wallpaperViewModel)
                                    .onDrop(of: [.fileURL], delegate: viewModel)
                                    .contextMenu {
                                        ExplorerGlobalMenu(contentViewModel: viewModel,
                                                           wallpaperViewModel: wallpaperViewModel)
                                    }
                            })
                        case 1:
                            DiscoverView(
                                workshopViewModel: workshopViewModel,
                                viewModel: viewModel,
                                wallpaperViewModel: wallpaperViewModel
                            )
                        case 3:
                            HStack(spacing: 0) {
                                HStack(alignment: .center) {
                                    Button {
                                        viewModel.toggleFilter()
                                    } label: {
                                        Label("筛选", systemImage: "line.3.horizontal.decrease.circle")
                                    }
                                    .buttonStyle(.plain)
                                    TextField("搜索小组件", text: $rmskinViewModel.searchText)
                                        .textFieldStyle(.roundedBorder)
                                    Spacer()
                                }
                                .padding(.bottom, 4)
                            }
                            HStack(spacing: 0) {
                                HStack(spacing: 0) {
                                    WidgetFilterSidebar(viewModel: rmskinViewModel)
                                }
                                .frame(width: viewModel.isFilterReveal ? 225 : 0)
                                .opacity(viewModel.isFilterReveal ? 1 : 0)

                                WidgetExplorer(viewModel: rmskinViewModel)
                                    .padding(.leading, viewModel.isFilterReveal ? 10 : 0)
                            }
                            .animation(.default, value: viewModel.isFilterReveal)
                        case 2:
                            FilterSidebarLayout(isPresented: viewModel.isFilterReveal, sidebar: {
                                WorkshopFilterSidebar(workshopViewModel: workshopViewModel)
                            }, content: {
                                WorkshopView(
                                    workshopViewModel: workshopViewModel,
                                    viewModel: viewModel,
                                    wallpaperViewModel: wallpaperViewModel
                                )
                            })
                        default:
                            EmptyView()
                        }
                        if viewModel.topTabBarSelection == 0 {
                            ExplorerBottomBar(contentViewModel: viewModel,
                                              wallpaperViewModel: wallpaperViewModel)
                        }
                    }
                }
                .padding()

                    if workshopViewModel.showCreatorProfile,
                       let creator = workshopViewModel.selectedCreator {
                        CreatorProfileView(
                            creator: creator,
                            workshopViewModel: workshopViewModel
                        )
                        .frame(maxWidth: 420)
                    } else if viewModel.topTabBarSelection == 0 {
                        WallpaperPreview(contentViewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        workshopViewModel: workshopViewModel)
                            .frame(maxWidth: 320)
                    } else if viewModel.topTabBarSelection == 3 {
                        WidgetPreview(viewModel: rmskinViewModel)
                            .frame(maxWidth: 320)
                    } else if workshopViewModel.showCustomization {
                        WallpaperPreview(contentViewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        workshopViewModel: workshopViewModel)
                            .frame(maxWidth: 320)
                    } else {
                        WorkshopItemDetail(
                            item: workshopViewModel.selectedItem,
                            workshopViewModel: workshopViewModel
                        )
                        .frame(maxWidth: 320)
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
        .alert(
            "需要基础壁纸",
            isPresented: Binding(
                get: { workshopViewModel.presetDependencyPrompt != nil },
                set: { if !$0 { workshopViewModel.dismissPresetDependencyPrompt() } }
            ),
            presenting: workshopViewModel.presetDependencyPrompt
        ) { prompt in
            Button("一起下载") {
                workshopViewModel.confirmPresetDependencyDownload(prompt)
            }
            Button("暂不", role: .cancel) {
                workshopViewModel.dismissPresetDependencyPrompt()
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
        .sheet(isPresented: $globalSettingsViewModel.isSettingsPresented, onDismiss: {
            // Match the previous window-close behavior: discard any in-flight
            // edits that were not committed via "好".
            globalSettingsViewModel.reset()
        }) {
            SettingsView()
                .environmentObject(globalSettingsViewModel)
        }
        // Applied outside the sheets so the card also floats over an open sheet,
        // and after them so it is the topmost layer in the window.
        .overlay(alignment: .bottomTrailing) {
            VideoTranscodeOverlay()
                .allowsHitTesting(false)
        }
        .environment(\.locale, localization.locale)
        .frame(minWidth: 1000, minHeight: 640)
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView(viewModel: .init(isStaging: true), wallpaperViewModel: .init())
            .environmentObject(GlobalSettingsViewModel())
    }
}
