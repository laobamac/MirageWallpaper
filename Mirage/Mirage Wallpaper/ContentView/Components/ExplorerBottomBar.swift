//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ExplorerBottomBar: View {
    @ObservedObject var contentViewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject private var manager = PlaylistManager.shared

    @State private var targetScreen = 0
    @State private var selectedItemID: String?
    @State private var isOpenPresented = false
    @State private var isSavePresented = false
    @State private var isSettingsPresented = false
    @State private var isClearConfirming = false

    private var screenCount: Int { NSScreen.screens.count }

    private var itemCount: Int {
        manager.current(on: targetScreen).items.count
    }

    var body: some View {
        VStack(spacing: 8) {
            header
            PlaylistStrip(manager: manager,
                          wallpaperViewModel: wallpaperViewModel,
                          screen: targetScreen,
                          selectedItemID: $selectedItemID)
            footer
        }
        .onAppear { manager.ensureScreen(targetScreen) }
        .onChange(of: targetScreen) { _ in
            manager.ensureScreen(targetScreen)
            selectedItemID = nil
        }
        .sheet(isPresented: $isOpenPresented) {
            PlaylistOpenSheet(manager: manager, screen: targetScreen, isPresented: $isOpenPresented)
        }
        .sheet(isPresented: $isSavePresented) {
            PlaylistSaveSheet(manager: manager, screen: targetScreen, isPresented: $isSavePresented)
        }
        .sheet(isPresented: $isSettingsPresented) {
            PlaylistSettingsSheet(manager: manager, screen: targetScreen, isPresented: $isSettingsPresented)
        }
        .confirmationDialog(L("清空播放列表"),
                            isPresented: $isClearConfirming) {
            Button(L("清空"), role: .destructive) {
                manager.clear(screen: targetScreen)
                selectedItemID = nil
            }
            Button(L("取消"), role: .cancel) {}
        } message: {
            Text(L("确定要清空当前播放列表中的全部壁纸吗？"))
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text(itemCount > 0 ? L("播放列表 (%d)", itemCount) : L("播放列表"))
                .font(.title2.bold())
                .lineLimit(1)

            if screenCount > 1 {
                Picker("", selection: $targetScreen) {
                    ForEach(0..<screenCount, id: \.self) { idx in
                        Text(L("屏幕 %d", idx + 1)).tag(idx)
                    }
                }
                .labelsHidden()
                .frame(width: 110)
            }

            Spacer()

            Button {
                isOpenPresented = true
            } label: {
                Label(L("载入"), systemImage: "folder.fill")
            }
            .disabled(manager.saved.isEmpty)

            Button {
                isSavePresented = true
            } label: {
                Label(L("保存"), systemImage: "square.and.arrow.down.fill")
            }
            .disabled(itemCount == 0)

            Button {
                isSettingsPresented = true
            } label: {
                Label(L("配置"), systemImage: "gearshape.2.fill")
            }

            Button {
                addCurrentSelection()
            } label: {
                Label(L("添加壁纸"), systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
        }
    }

    private var footer: some View {
        HStack(spacing: 8) {
            Button {
                AppDelegate.shared.openImportFromFolderPanel()
            } label: {
                Label(L("导入壁纸"), systemImage: "arrow.up.bin.fill")
                    .frame(width: 160)
            }
            .buttonStyle(.borderedProminent)

            Spacer()

            Button {
                guard let id = selectedItemID else { return }
                manager.remove(itemID: id, from: targetScreen)
                selectedItemID = nil
            } label: {
                Label(L("移除壁纸"), systemImage: "minus")
            }
            .disabled(selectedItemID == nil)

            Button(role: .destructive) {
                isClearConfirming = true
            } label: {
                Label(L("清理"), systemImage: "trash")
            }
            .disabled(itemCount == 0)
        }
    }

    private func addCurrentSelection() {
        let current = wallpaperViewModel.currentWallpaper
        if current.isValid {
            manager.add(current, to: targetScreen)
            selectedItemID = current.id
        }
    }
}
