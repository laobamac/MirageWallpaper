//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ExplorerGlobalMenu: SubviewOfContentView {
    
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    
    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel) {
        self.wallpaperViewModel = wallpaperViewModel
        self.viewModel = viewModel
    }
    
    var body: some View {
        Section {
            WallpaperGridViewMenu(viewModel: viewModel, showsPageSize: true)
        }
        .labelStyle(.titleAndIcon)
    }
}
