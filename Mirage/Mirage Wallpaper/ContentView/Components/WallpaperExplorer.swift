//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WallpaperExplorer: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let creatorStore: WorkshopCreatorStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let isActive: Bool
    let animatedPreviewMode: GSAnimatedPreviewPlayback

    init(contentViewModel viewModel: ContentViewModel,
         wallpaperViewModel: WallpaperViewModel,
         creatorStore: WorkshopCreatorStore,
         interactionStore: WorkshopInteractionStore,
         libraryStore: WorkshopLibraryStore,
         selectionCoordinator: WorkshopSelectionCoordinator,
         isActive: Bool = true,
         animatedPreviewMode: GSAnimatedPreviewPlayback = .hover) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.creatorStore = creatorStore
        self.interactionStore = interactionStore
        self.libraryStore = libraryStore
        self.selectionCoordinator = selectionCoordinator
        self.isActive = isActive
        self.animatedPreviewMode = animatedPreviewMode
    }

    var body: some View {
        let page = viewModel.wallpaperPage
        let selectedDirectory = wallpaperViewModel.currentWallpaper.wallpaperDirectory
        let currentPage = min(max(viewModel.currentPage, 1), page.pageCount)
        ScrollViewReader { proxy in
            ZStack(alignment: .bottom) {
                ScrollView {
                    Color.clear
                        .frame(height: 0)
                        .id("libraryTop")

                    if page.items.isEmpty {
                        HStack {
                            Spacer()
                            Text("""
                                没有找到匹配的壁纸。
                                请调整或重置左侧筛选条件，或更换搜索关键词。
                                也可以点击底部“导入壁纸”添加新壁纸。
                                """)
                            .font(.title)
                            .foregroundStyle(Color.secondary)
                            .multilineTextAlignment(.center)
                            .lineLimit(nil)
                            .lineSpacing(10)
                            Spacer()
                        }
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.top, 50)
                    } else {
                        LazyVGrid(
                            columns: [GridItem(.adaptive(
                                minimum: viewModel.explorerIconSize,
                                maximum: viewModel.explorerIconSize * 2
                            ), spacing: 14)],
                            alignment: .leading,
                            spacing: 14
                        ) {
                            ForEach(page.items) { wallpaper in
                                ExplorerItem(
                                    wallpaper: wallpaper,
                                    selectionCoordinator: selectionCoordinator,
                                    isSelected: wallpaper.wallpaperDirectory == selectedDirectory,
                                    isActive: isActive,
                                    animatedPreviewMode: animatedPreviewMode
                                )
                                .contextMenu {
                                    ExplorerItemMenu(
                                        contentViewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        creatorStore: creatorStore,
                                        interactionStore: interactionStore,
                                        libraryStore: libraryStore,
                                        selectionCoordinator: selectionCoordinator,
                                        current: wallpaper
                                    )
                                    ExplorerGlobalMenu(
                                        contentViewModel: viewModel,
                                        wallpaperViewModel: wallpaperViewModel
                                    )
                                }
                            }
                        }
                        .padding(.trailing)
                    }

                    if page.pageCount > 1 {
                        Color.clear.frame(height: 58)
                    }
                }

                if page.pageCount > 1 {
                    PageNavigator(currentPage: currentPage, pageCount: page.pageCount) { page in
                        viewModel.currentPage = page
                    }
                    .padding(.bottom, 12)
                }
            }
            .onChange(of: viewModel.currentPage) { _, _ in
                withAnimation(.easeOut(duration: 0.2)) {
                    proxy.scrollTo("libraryTop", anchor: .top)
                }
            }
        }
    }
}
