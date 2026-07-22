//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ExplorerItem: View {
    let wallpaper: WEWallpaper
    // Selection is passed in as a plain value so the cell no longer observes the
    // shared view models. Hovering or selecting one cell then cannot force every
    // other cell in the grid to rebuild.
    let isSelected: Bool

    @State private var hovering = false

    var body: some View {
        ZStack(alignment: .bottom) {
            GifImage(contentsOf: wallpaper.project.preview.isEmpty
                ? Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")!
                : wallpaper.previewURL,
                     // Only the hovered cell or the active wallpaper animates its
                     // preview; every other cell stays a cheap static thumbnail.
                     animates: hovering || isSelected)
            .resizable()
            .scaleEffect(hovering ? 1.03 : 1.0)
            .aspectRatio(1.0, contentMode: .fit)
            .clipped()

            Text(wallpaper.project.title)
                .lineLimit(2)
                .frame(maxWidth: .infinity, minHeight: 30)
                .padding(4)
                .background(Color(white: 0, opacity: hovering ? 0.4 : 0.2))
                .font(.footnote)
                .multilineTextAlignment(.center)
                .foregroundStyle(Color(white: hovering ? 0.9 : 0.7))
        }
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.accentColor,
                              lineWidth: isSelected ? 3 : (hovering ? 1 : 0))
        )
        .shadow(color: .black.opacity(hovering ? 0.25 : 0), radius: hovering ? 8 : 0, y: 2)
        .overlay(alignment: .topLeading) {
            if wallpaper.isPreset {
                VStack(alignment: .leading, spacing: 3) {
                    Label("预设", systemImage: "slider.horizontal.3")
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(.purple, in: RoundedRectangle(cornerRadius: 4))
                    if let status = wallpaper.presetStatusDescription {
                        Label(status, systemImage: "exclamationmark.triangle.fill")
                            .padding(.horizontal, 6)
                            .padding(.vertical, 3)
                            .background(.orange, in: RoundedRectangle(cornerRadius: 4))
                    }
                }
                .font(.caption2.bold())
                .foregroundStyle(.white)
                .padding(6)
            }
        }
        .animation(.easeOut(duration: 0.15), value: hovering)
        .onTapGesture {
            AppDelegate.shared.workshopViewModel.openInstalledWallpaper(wallpaper)
        }
        .onHover { hovering = $0 }
    }
}
