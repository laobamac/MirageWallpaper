//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WallpaperExplorer: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel

    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
    }

    var body: some View {
        let page = viewModel.wallpaperPage
        let selectedDirectory = wallpaperViewModel.currentWallpaper.wallpaperDirectory
        ScrollView {
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
                LazyVGrid(columns: [GridItem(.adaptive(minimum: viewModel.explorerIconSize,
                                                       maximum: viewModel.explorerIconSize * 2)
                )], alignment: .leading) {
                    ForEach(page.items) { wallpaper in
                        ExplorerItem(wallpaper: wallpaper,
                                     isSelected: wallpaper.wallpaperDirectory == selectedDirectory)
                            .contextMenu {
                                ExplorerItemMenu(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel, current: wallpaper)
                                ExplorerGlobalMenu(contentViewModel: viewModel, wallpaperViewModel: wallpaperViewModel)
                            }
                    }
                }
                .padding(.trailing)
            }
        }
        .overlay {
            VStack {
                Spacer()
                HStack {
                    ForEach(0..<page.pageCount, id: \.self) { pageIndex in
                        Button("\(pageIndex + 1)") {
                            viewModel.currentPage = pageIndex + 1
                        }
                    }
                }
                .padding(.bottom)
            }
        }
    }
}
