//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers

struct PlaylistStrip: View {
    @ObservedObject var manager: PlaylistManager
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let screen: Int
    @Binding var selectedItemID: String?

    private var items: [PlaylistItem] {
        manager.current(on: screen).items
    }

    private var libraryByID: [String: WEWallpaper] {
        Dictionary(uniqueKeysWithValues: AppDelegate.shared.contentViewModel.wallpapers.map { ($0.id, $0) })
    }

    private var playingID: String? {
        wallpaperViewModel.currentByScreen[screen]?.id
    }

    var body: some View {
        Group {
            if items.isEmpty {
                emptyState
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(spacing: 10) {
                        ForEach(Array(items.enumerated()), id: \.element.wallpaperID) { idx, item in
                            PlaylistThumb(
                                item: item,
                                index: idx,
                                wallpaper: libraryByID[item.wallpaperID],
                                isPlaying: playingID == item.wallpaperID,
                                isSelected: selectedItemID == item.wallpaperID,
                                onTap: { tap(item) },
                                onRemove: { manager.remove(itemID: item.wallpaperID, from: screen) }
                            )
                            .onDrag { NSItemProvider(object: item.wallpaperID as NSString) }
                            .onDrop(of: [.text], delegate: PlaylistDropDelegate(
                                targetIndex: idx,
                                items: items,
                                onMove: { from, to in
                                    manager.move(from: from, to: to, on: screen)
                                }
                            ))
                        }
                    }
                    .padding(.horizontal, 4)
                    .padding(.vertical, 6)
                }
            }
        }
        .frame(height: 92)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(Color.primary.opacity(0.05))
        )
    }

    private func tap(_ item: PlaylistItem) {
        selectedItemID = item.wallpaperID
        guard let wallpaper = libraryByID[item.wallpaperID], wallpaper.isValid else { return }
        if screen == 0 {
            wallpaperViewModel.currentWallpaper = wallpaper
        } else {
            wallpaperViewModel.applyOnScreen(wallpaper, screen: screen)
        }
        manager.kickRotator(on: screen)
    }

    private var emptyState: some View {
        HStack {
            Spacer()
            VStack(spacing: 6) {
                Image(systemName: "rectangle.stack.badge.plus")
                    .font(.title2)
                    .foregroundStyle(.secondary)
                Text(L("尚未添加壁纸，右键壁纸选择“加入播放列表”，或点击右上角“添加壁纸”。"))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            Spacer()
        }
    }
}

private struct PlaylistThumb: View {
    let item: PlaylistItem
    let index: Int
    let wallpaper: WEWallpaper?
    let isPlaying: Bool
    let isSelected: Bool
    let onTap: () -> Void
    let onRemove: () -> Void

    @State private var hovering = false

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            thumbnail
                .frame(width: 128, height: 72)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .strokeBorder(borderColor, lineWidth: borderWidth)
                )
                .shadow(color: .black.opacity(hovering ? 0.25 : 0), radius: hovering ? 6 : 0, y: 2)

            HStack(spacing: 3) {
                if isPlaying {
                    Image(systemName: "play.fill")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 2)
                        .background(Color.accentColor, in: Capsule())
                }
                Text(wallpaper?.project.title ?? item.wallpaperID)
                    .font(.system(size: 10))
                    .lineLimit(1)
                    .foregroundStyle(.white)
                    .padding(.horizontal, 4)
                    .padding(.vertical, 2)
                    .background(Color.black.opacity(0.55), in: Capsule())
            }
            .padding(4)
        }
        .overlay(alignment: .topTrailing) {
            if hovering {
                Button(action: onRemove) {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 14))
                        .foregroundStyle(.white, .black.opacity(0.65))
                        .symbolRenderingMode(.palette)
                }
                .buttonStyle(.plain)
                .padding(4)
            }
        }
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(perform: onTap)
        .help(wallpaper?.project.title ?? item.wallpaperID)
    }

    private var borderColor: Color {
        if isPlaying { return .accentColor }
        if isSelected { return .accentColor.opacity(0.6) }
        return hovering ? .accentColor.opacity(0.3) : .clear
    }

    private var borderWidth: CGFloat {
        if isPlaying { return 2 }
        if isSelected { return 2 }
        return hovering ? 1 : 0
    }

    @ViewBuilder
    private var thumbnail: some View {
        if let wallpaper, !wallpaper.project.preview.isEmpty {
            GifImage(contentsOf: wallpaper.previewURL, animates: hovering || isPlaying)
                .resizable()
                .aspectRatio(contentMode: .fill)
        } else {
            ZStack {
                Color.gray.opacity(0.3)
                Image(systemName: "photo")
                    .font(.title2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct PlaylistDropDelegate: DropDelegate {
    let targetIndex: Int
    let items: [PlaylistItem]
    let onMove: (Int, Int) -> Void

    func performDrop(info: DropInfo) -> Bool {
        guard let provider = info.itemProviders(for: [.text]).first else { return false }
        provider.loadObject(ofClass: NSString.self) { data, _ in
            guard let idString = data as? String,
                  let source = items.firstIndex(where: { $0.wallpaperID == idString }) else { return }
            let target = targetIndex
            guard source != target else { return }
            DispatchQueue.main.async {
                onMove(source, target)
            }
        }
        return true
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }
}
