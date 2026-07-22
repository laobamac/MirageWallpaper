//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ExplorerItemMenu: SubviewOfContentView {
    
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    
    var hoveredWallpaper: WEWallpaper
    
    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel, current hoveredWallpaper: WEWallpaper) {
        self.wallpaperViewModel = wallpaperViewModel
        self.viewModel = viewModel
        self.hoveredWallpaper = hoveredWallpaper
    }
    
    var body: some View {
        Group {
            Section {
                Button(action: setAsScreenSaver) {
                    Label("设为屏保", systemImage: "sparkles.tv")
                }
                .disabled(!hoveredWallpaper.isValid || hoveredWallpaper.kind == .unsupported)
            }

            Section {
                Button {
                    
                } label: {
                    Label("加入播放列表", systemImage: "plus")
                }.disabled(true)
                Button {
                    viewModel.hoveredWallpaper = hoveredWallpaper
                    viewModel.isUnsubscribeConfirming = true
                } label: {
                    Label("删除壁纸", systemImage: "xmark")
                }
                Button {
                    
                } label: {
                    Label("加入收藏", systemImage: "heart.fill")
                }.disabled(true)
            }
            
            Section {
                Button {
                    
                } label: {
                    Label("在创意工坊中打开", systemImage: "cloud.fill")
                }.disabled(true)
                Menu("相关壁纸") {
                    Link(destination: URL(string: "https://github.com/laobamac/MirageWallpaper")!) {
                        Label("浏览该作者全部", systemImage: "person.fill")
                    }
                    Link(destination: URL(string: "https://github.com/laobamac/MirageWallpaper")!) {
                        Label("浏览预设", systemImage: "cloud.fill")
                    }
                }.disabled(true)
                Menu("举报与屏蔽") {
                    Button(role: .destructive) {
                        
                    } label: {
                        Label("举报", systemImage: "exclamationmark.triangle.fill")
                    }
                    Button {
                        
                    } label: {
                        Label("管理屏蔽列表", systemImage: "hand.raised.fill")
                    }
                }.disabled(true)
            }
            
            Section {
                Button {
                    
                } label: {
                    Label("设置快捷键", systemImage: "command.square")
                }.disabled(true)
                Button {
                    NSWorkspace.shared.selectFile(nil,
                                                  inFileViewerRootedAtPath: hoveredWallpaper.wallpaperDirectory.path(percentEncoded: false))
                } label: {
                    Label("在访达中显示", systemImage: "folder.badge.gearshape")
                }
            }
        }
        .labelStyle(.titleAndIcon)
    }

    private func setAsScreenSaver() {
        let wallpaper = hoveredWallpaper
        let runtime = wallpaperViewModel.loadRuntime(for: wallpaper)
        let properties = wallpaperViewModel.effectiveProperties(for: wallpaper, runtime: runtime)
        let fps = Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
        let manager = ScreenSaverManager.shared
        let needsInstallation = !manager.isInstalled

        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result {
                if needsInstallation { try manager.install() }
                try manager.configure(
                    with: wallpaper,
                    runtime: runtime,
                    properties: properties,
                    fps: fps
                )
            }
            DispatchQueue.main.async {
                switch result {
                case .success:
                    viewModel.screenSaverFeedback = ScreenSaverFeedback(
                        title: "已设为屏保",
                        message: "“\(wallpaper.project.title)”将在下次启动屏保时显示。"
                    )
                case .failure(let error):
                    viewModel.screenSaverFeedback = ScreenSaverFeedback(
                        title: "设置屏保失败",
                        message: error.localizedDescription
                    )
                }
            }
        }
    }
}
