//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers

// Grid of installed rmskin themes (小组件 tab), mirroring WallpaperExplorer.
struct WidgetExplorer: View {
    @ObservedObject var viewModel: RmskinViewModel

    var body: some View {
        ScrollView {
            if viewModel.filteredThemes.isEmpty {
                HStack {
                    Spacer()
                    Text("""
                        没有找到小组件主题。
                        点击底部“导入小组件”添加 .rmskin 文件，
                        或调整左侧的加载类型 / 版本筛选。
                        """)
                    .font(.title2)
                    .foregroundStyle(Color.secondary)
                    .multilineTextAlignment(.center)
                    .lineSpacing(10)
                    Spacer()
                }
                .padding(.top, 50)
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 200, maximum: 320))],
                          alignment: .leading) {
                    ForEach(viewModel.filteredThemes) { theme in
                        RmskinItem(viewModel: viewModel, theme: theme)
                    }
                }
                .padding(.trailing)
            }
        }
        .onDrop(of: [.fileURL], delegate: viewModel)
    }
}
