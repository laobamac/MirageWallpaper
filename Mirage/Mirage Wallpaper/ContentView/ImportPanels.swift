//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa

extension AppDelegate {
    @objc func openImportFromFolderPanel() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.folder, .movie, .mpeg4Movie, .quickTimeMovie]
        panel.prompt = L("导入")
        panel.message = L("选择壁纸文件夹（含 project.json）或视频文件")
        panel.begin { [weak self] response in
            guard response == .OK, let self else { return }
            self.contentViewModel.importWallpapers(urls: panel.urls)
        }
    }

    @objc func openImportFromFoldersPanel() {
        openImportFromFolderPanel()
    }
}
