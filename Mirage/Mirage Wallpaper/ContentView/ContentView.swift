//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

protocol SubviewOfContentView: View {
    var viewModel: ContentViewModel { get set }
}

struct ContentView: View {
    @EnvironmentObject var globalSettingsViewModel: GlobalSettingsViewModel

    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject var workshopViewModel: WorkshopViewModel

    init(viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel, workshopViewModel: WorkshopViewModel = AppDelegate.shared.workshopViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.workshopViewModel = workshopViewModel
    }

    var body: some View {
        ZStack {
            HSplitView {
                if viewModel.isStaging {
                    VStack(spacing: 5) {
                        TopTabBar(contentViewModel: viewModel)
                        ProjectFeedbackBanner()
                        switch viewModel.topTabBarSelection {
                        case 0:
                            ExplorerTopBar(contentViewModel: viewModel)
                                .environmentObject(globalSettingsViewModel)
                            HStack(spacing: 0) {
                                HStack(spacing: 0) {
                                    FilterResults(viewModel: viewModel)
                                }
                                .frame(width: viewModel.isFilterReveal ? 225 : 0)
                                .opacity(viewModel.isFilterReveal ? 1 : 0)
                                .animation(.spring(), value: viewModel.isFilterReveal)

                                WallpaperExplorer(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                                    .onDrop(of: [.fileURL], delegate: viewModel)
                                    .contextMenu {
                                        ExplorerGlobalMenu(contentViewModel: viewModel,
                                                           wallpaperViewModel: wallpaperViewModel)
                                    }
                                    .padding(.leading, viewModel.isFilterReveal ? 10 : 0)
                            }
                            .animation(.default, value: viewModel.isFilterReveal)
                        case 1:
                            DiscoverView(
                                workshopViewModel: workshopViewModel,
                                viewModel: viewModel
                            )
                        case 2:
                            ExplorerTopBar(contentViewModel: viewModel)
                                .environmentObject(globalSettingsViewModel)
                            HStack(spacing: 0) {
                                WorkshopFilterSidebar(workshopViewModel: workshopViewModel)
                                    .frame(width: viewModel.isFilterReveal ? 225 : 0)
                                    .opacity(viewModel.isFilterReveal ? 1 : 0)
                                    .animation(.spring(), value: viewModel.isFilterReveal)

                                WorkshopView(
                                    workshopViewModel: workshopViewModel,
                                    viewModel: viewModel
                                )
                                .padding(.leading, viewModel.isFilterReveal ? 10 : 0)
                            }
                            .animation(.default, value: viewModel.isFilterReveal)
                        default:
                            fatalError()
                        }
                        ExplorerBottomBar()
                    }
                    .padding()

                    if viewModel.topTabBarSelection == 0 {
                        WallpaperPreview(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                            .frame(maxWidth: 320)
                    } else if workshopViewModel.showCustomization {
                        WallpaperPreview(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                            .frame(maxWidth: 320)
                    } else {
                        WorkshopItemDetail(
                            item: workshopViewModel.selectedItem,
                            workshopViewModel: workshopViewModel
                        )
                        .frame(maxWidth: 320)
                    }
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
            if let url = viewModel.hoveredWallpaper?.wallpaperDirectory {
                Button("立即删除", role: .destructive) {
                    WEWallpaper.invalidateSizeCache()
                    try? FileManager.default.removeItem(at: url)
                    if url == wallpaperViewModel.currentWallpaper.wallpaperDirectory {
                        wallpaperViewModel.currentWallpaper = WallpaperViewModel.invalidWallpaper
                    }
                    viewModel.hoveredWallpaper = nil
                    viewModel.refresh()
                }
                Button("移到废纸篓") {
                    WEWallpaper.invalidateSizeCache()
                    try? FileManager.default.trashItem(at: url, resultingItemURL: nil)
                    if url == wallpaperViewModel.currentWallpaper.wallpaperDirectory {
                        wallpaperViewModel.currentWallpaper = WallpaperViewModel.invalidWallpaper
                    }
                    viewModel.hoveredWallpaper = nil
                    viewModel.refresh()
                }
            }
            Button("取消", role: .cancel) {
                viewModel.hoveredWallpaper = nil
            }
        } message: {
            Text("确定要删除“\(viewModel.hoveredWallpaper?.project.title ?? "该壁纸")”吗？")
        }
        .alert(isPresented: $viewModel.importAlertPresented, error: viewModel.importAlertError) { }
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
        .sheet(isPresented: $viewModel.isDisplaySettingsReveal) {
            DisplaySettings(viewModel: viewModel)
                .padding()
                .frame(width: 520, height: 450)
        }
        .sheet(isPresented: $viewModel.isUnsafeWallpaperWarningPresented) {
            UnsafeWallpaper(wallpaper: wallpaperViewModel.nextCurrentWallpaper)
                .frame(width: 600, height: 300)
        }
        .sheet(isPresented: $viewModel.isSteamSetupPresented) {
            SteamSetupView(viewModel: SteamSetupViewModel())
                .frame(width: 560, height: 640)
        }
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView(viewModel: .init(isStaging: true), wallpaperViewModel: .init())
            .environmentObject(GlobalSettingsViewModel())
    }
}
