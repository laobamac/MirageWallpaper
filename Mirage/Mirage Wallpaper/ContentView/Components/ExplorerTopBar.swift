//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ExplorerTopBar: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    
    @EnvironmentObject var globalSettingsViewModel: GlobalSettingsViewModel
    
    init(contentViewModel viewModel: ContentViewModel) {
        self.viewModel = viewModel
    }
    
    var body: some View {
        HStack {
            TextField("搜索", text: $viewModel.searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
            Button {
                viewModel.isFilterReveal.toggle()
            } label: {
                Label("筛选", systemImage: "checklist.checked")
            }
            .buttonStyle(.borderedProminent)
            Button {
                viewModel.refresh()
            } label: {
                Group {
                    if viewModel.isRefreshing {
                        ProgressView()
                            .controlSize(.small)
                    } else {
                        Image(systemName: "arrow.triangle.2.circlepath")
                    }
                }
                .frame(width: 16, height: 16)
            }
            .disabled(viewModel.isRefreshing)
            .help("刷新壁纸库")
            WallpaperGridViewMenu(viewModel: viewModel, showsPageSize: true)
            Spacer()
            Button { 
                if viewModel.sortingSequence == .decrease {
                    viewModel.sortingSequence = .increase
                } else {
                    viewModel.sortingSequence = .decrease
                }
            } label: {
                Image(systemName: viewModel.sortingSequence == .increase ?
                      "arrowtriangle.down.fill" : "arrowtriangle.up.fill")
            }
            .buttonStyle(.plain)
            Picker("排序", selection: $viewModel.sortingBy) {
                ForEach(WEWallpaperSortingMethod.allCases) { method in
                    Text(LocalizedStringKey(method.rawValue)).tag(method)
                }
            }
            .labelsHidden()
            .frame(width: 120)
        }
    }
}
