//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DiscoverView: View {
    @Environment(GlobalSettingsViewModel.self) private var globalSettingsViewModel
    @Bindable var workshopViewModel: WorkshopViewModel
    @Bindable var viewModel: ContentViewModel
    @Bindable var wallpaperViewModel: WallpaperViewModel
    let navigationModel: MainNavigationModel
    let isActive: Bool

    @State private var discoverReturnRowID: String?

    var body: some View {
        @Bindable var globalSettingsViewModel = globalSettingsViewModel
        VStack(spacing: 0) {
            if let browse = workshopViewModel.discoverBrowse {
                browseToolbar(browse)
                Divider()
                browseContent(browse)
            } else {
                feedToolbar
                Divider()
                feedContent
            }
        }
        .contextMenu {
            WallpaperGridViewMenu(viewModel: viewModel)
        }
        .onAppear {
            if workshopViewModel.discoverRows.isEmpty {
                workshopViewModel.loadDiscover()
            }
        }
    }

    private var feedToolbar: some View {
        HStack(spacing: 10) {
            HStack(spacing: 7) {
                Image(systemName: "magnifyingglass")
                    .font(.title3)
                TextField("查找壁纸", text: $workshopViewModel.discoverSearchText)
                    .textFieldStyle(.plain)
                    .onSubmit {
                        workshopViewModel.performDiscoverSearch()
                    }
                if !workshopViewModel.discoverSearchText.isEmpty {
                    Button {
                        workshopViewModel.clearDiscoverSearch()
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("清除搜索")
                }
            }
            .padding(.horizontal, 10)
            .frame(width: 250, height: 36)
            .background(Color.primary.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 3))

            Button {
                workshopViewModel.performDiscoverSearch()
            } label: {
                Label("查找", systemImage: "magnifyingglass")
                    .font(.headline)
                    .frame(height: 36)
                    .padding(.horizontal, 9)
            }
            .buttonStyle(.borderless)
            .disabled(workshopViewModel.discoverSearchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

            Spacer()

            if workshopViewModel.isDiscoverDetailLoading {
                ProgressView()
                    .controlSize(.small)
                    .help("正在加载壁纸详情")
            }

            Button {
                workshopViewModel.refreshDiscover()
            } label: {
                Image(systemName: "arrow.clockwise")
                    .frame(width: 18, height: 18)
            }
            .buttonStyle(.borderless)
            .disabled(workshopViewModel.isDiscoverLoading)
            .help("刷新发现")
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
    }

    private func browseToolbar(_ browse: DiscoverBrowseState) -> some View {
        HStack(spacing: 10) {
            Button {
                workshopViewModel.closeDiscoverBrowse()
            } label: {
                Image(systemName: "arrow.left")
                    .font(.headline)
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            .help("返回发现")

            Text(browse.query.title)
                .font(.title3.weight(.semibold))
                .lineLimit(1)

            if browse.total > 0 {
                Text(L("共 %d 项", browse.total))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if workshopViewModel.isDiscoverDetailLoading {
                ProgressView()
                    .controlSize(.small)
                    .help("正在加载壁纸详情")
            }

            Button {
                workshopViewModel.refreshDiscover()
            } label: {
                Image(systemName: "arrow.clockwise")
                    .frame(width: 18, height: 18)
            }
            .buttonStyle(.borderless)
            .disabled(browse.isLoading)
            .help("刷新")
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
    }

    private var feedContent: some View {
        ScrollViewReader { proxy in
            ScrollView {
                Group {
                    if workshopViewModel.isDiscoverLoading && workshopViewModel.discoverRows.isEmpty {
                        loadingState
                    } else if let error = workshopViewModel.discoverError,
                              workshopViewModel.discoverRows.isEmpty {
                        errorState(error)
                    } else if workshopViewModel.discoverRows.isEmpty {
                        emptyState
                    } else {
                        LazyVStack(alignment: .leading, spacing: 34) {
                            ForEach(workshopViewModel.discoverRows) { row in
                                DiscoverSectionView(
                                    row: row,
                                    workshopViewModel: workshopViewModel,
                                    contentViewModel: viewModel,
                                    wallpaperViewModel: wallpaperViewModel,
                                    isActive: isActive,
                                    animatedPreviewMode: globalSettingsViewModel.animatedPreviewPlaybackMode,
                                    onSeeAll: {
                                        discoverReturnRowID = row.id
                                        workshopViewModel.openDiscoverRow(id: row.id)
                                    }
                                )
                                .id(row.id)
                            }
                            Spacer(minLength: 24)
                        }
                        .padding(.horizontal, 18)
                        .padding(.top, 20)
                    }
                }
                .frame(maxWidth: .infinity)
            }
            .onAppear {
                guard let rowID = discoverReturnRowID else { return }
                DispatchQueue.main.async {
                    proxy.scrollTo(rowID, anchor: .top)
                    discoverReturnRowID = nil
                }
            }
        }
    }

    private func browseContent(_ browse: DiscoverBrowseState) -> some View {
        ScrollViewReader { proxy in
            ZStack(alignment: .bottom) {
                if browse.isLoading && browse.items.isEmpty {
                    loadingState
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let error = browse.error, browse.items.isEmpty {
                    errorState(error)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if browse.items.isEmpty {
                    emptyState
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    ScrollView {
                        Color.clear
                            .frame(height: 0)
                            .id("discoverBrowseTop")

                        LazyVGrid(
                            columns: [GridItem(.adaptive(
                                minimum: viewModel.explorerIconSize,
                                maximum: viewModel.explorerIconSize * 2
                            ), spacing: 14)],
                            alignment: .leading,
                            spacing: 14
                        ) {
                            ForEach(browse.items) { item in
                                WorkshopItemCard(
                                    item: item,

                                    isSelected: workshopViewModel.discoverSelectedItemID == item.id,
                                    isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                                    presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                                    downloadTask: workshopViewModel.downloadTask(for: item.publishedFileId),
                                    isFavorite: workshopViewModel.isWorkshopFavorite(item.publishedFileId),
                                    isActive: isActive,
                                    animatedPreviewMode: globalSettingsViewModel.animatedPreviewPlaybackMode
                                )
                                .onTapGesture {
                                    workshopViewModel.selectDiscoverItem(item)
                                }
                                .contextMenu {
                                    if let wallpaper = workshopViewModel.cachedInstalledWallpapers[item.publishedFileId] {
                                        ExplorerItemMenu(
                                            contentViewModel: viewModel,
                                            wallpaperViewModel: wallpaperViewModel,
                                            workshopViewModel: workshopViewModel,
                                            current: wallpaper
                                        )
                                        ExplorerGlobalMenu(
                                            contentViewModel: viewModel,
                                            wallpaperViewModel: wallpaperViewModel
                                        )
                                    } else {
                                        WorkshopCardContextMenu(
                                            item: item,
                                            workshopViewModel: workshopViewModel
                                        )
                                        WallpaperGridViewMenu(viewModel: viewModel)
                                    }
                                }
                            }
                        }
                        .padding(18)

                        if browse.totalPages > 1 {
                            Color.clear.frame(height: 58)
                        }
                    }
                }

                if browse.totalPages > 1 {
                    PageNavigator(
                        currentPage: browse.page,
                        pageCount: browse.totalPages,
                        onSelect: workshopViewModel.goToDiscoverBrowsePage
                    )
                    .padding(.bottom, 12)
                }
            }
            .onChange(of: browse.page) { _, _ in
                withAnimation(.easeOut(duration: 0.2)) {
                    proxy.scrollTo("discoverBrowseTop", anchor: .top)
                }
            }
        }
    }

    private var loadingState: some View {
        VStack(spacing: 16) {
            ProgressView()
                .controlSize(.large)
            Text("正在加载推荐内容...")
                .foregroundStyle(.secondary)
        }
        .padding(.top, 100)
    }

    private func errorState(_ error: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 34))
                .foregroundStyle(.secondary)
            Text(error)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button("重试") {
                workshopViewModel.refreshDiscover()
            }
        }
        .padding(.horizontal, 30)
        .padding(.top, 100)
    }

    private var emptyState: some View {
        VStack(spacing: 12) {
            Image(systemName: "rectangle.stack")
                .font(.system(size: 36))
                .foregroundStyle(.tertiary)
            Text("没有找到壁纸")
                .font(.title3)
                .foregroundStyle(.secondary)
        }
        .padding(.top, 100)
    }
}
