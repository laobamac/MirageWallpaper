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
    @ObservedObject private var localization = MirageLocalization.shared

    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @ObservedObject var rmskinViewModel: RmskinViewModel

    init(viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel, workshopViewModel: WorkshopViewModel = AppDelegate.shared.workshopViewModel, rmskinViewModel: RmskinViewModel = AppDelegate.shared.rmskinViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.workshopViewModel = workshopViewModel
        self.rmskinViewModel = rmskinViewModel
    }

    var body: some View {
        ZStack {
            HSplitView {
                VStack(spacing: 5) {
                    if viewModel.isStaging {
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
                            ExplorerTopBar(contentViewModel: viewModel)
                                .environmentObject(globalSettingsViewModel)
                            HStack(spacing: 0) {
                                WorkshopFilterSidebar(workshopViewModel: workshopViewModel)
                                    .frame(width: viewModel.isFilterReveal ? 225 : 0)
                                    .opacity(viewModel.isFilterReveal ? 1 : 0)

                                WorkshopView(
                                    workshopViewModel: workshopViewModel,
                                    viewModel: viewModel
                                )
                                .padding(.leading, viewModel.isFilterReveal ? 10 : 0)
                            }
                            .animation(.default, value: viewModel.isFilterReveal)
                        default:
                            EmptyView()
                        }
                        ExplorerBottomBar()
                    }
                }
                .padding()

                if viewModel.isStaging {
                    if viewModel.topTabBarSelection == 0 {
                        WallpaperPreview(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                            .frame(maxWidth: 320)
                    } else if viewModel.topTabBarSelection == 3 {
                        WidgetPreview(viewModel: rmskinViewModel)
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
        .sheet(isPresented: $globalSettingsViewModel.isSettingsPresented, onDismiss: {
            // Match the previous window-close behavior: discard any in-flight
            // edits that were not committed via "好".
            globalSettingsViewModel.reset()
        }) {
            SettingsView()
                .environmentObject(globalSettingsViewModel)
        }
        .environment(\.locale, localization.locale)
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView(viewModel: .init(isStaging: true), wallpaperViewModel: .init())
            .environmentObject(GlobalSettingsViewModel())
    }
}
