//
//  LibraryActionBar.swift
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct LibraryActionBar: View {
    @ObservedObject private var editor = EditorManager.shared

    var body: some View {
        HStack(spacing: 8) {
            Button { AppDelegate.shared.openImportFromFolderPanel() } label: {
                Label(L("导入壁纸"), systemImage: "arrow.up.bin.fill").frame(width: 160)
            }
            .buttonStyle(.borderedProminent)
            Button { editor.open() } label: {
                Label(L("编辑器"), systemImage: "pencil.and.outline")
            }
            .contextMenu {
                Button("管理编辑器…") { editor.manage() }
            }
            if editor.busy {
                ProgressView().controlSize(.small)
                Button("查看进度") { editor.manage() }.buttonStyle(.link)
            }
            Spacer()
        }
        .padding(.top, 4)
    }
}
