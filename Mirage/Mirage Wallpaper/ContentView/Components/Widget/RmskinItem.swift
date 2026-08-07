//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

// One grid card for an installed rmskin theme.
struct RmskinItem: View {
    @ObservedObject var viewModel: RmskinViewModel
    let theme: RmskinTheme

    private var isSelected: Bool { viewModel.selectedTheme?.id == theme.id }
    private var isApplied: Bool { viewModel.isApplied(theme) }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color(nsColor: .windowBackgroundColor))
                preview
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                if isApplied {
                    VStack {
                        HStack {
                            Spacer()
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(.white, .green)
                                .padding(6)
                        }
                        Spacer()
                    }
                }
            }
            .frame(height: viewModel_iconSize * 0.62)
            .overlay(RoundedRectangle(cornerRadius: 6)
                .stroke(isSelected ? Color.accentColor : Color.clear, lineWidth: 3))

            Text(theme.name).font(.callout).bold().lineLimit(1)
            HStack(spacing: 6) {
                if !theme.loadType.isEmpty {
                    Text(theme.loadType).font(.caption2)
                        .padding(.horizontal, 5).padding(.vertical, 1)
                        .background(Color.blue.opacity(0.2), in: Capsule())
                }
                if !theme.version.isEmpty {
                    Text("v\(theme.version)").font(.caption2).foregroundStyle(.secondary)
                }
                Spacer()
            }
            if !theme.author.isEmpty {
                Text(theme.author).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(6)
        .contentShape(Rectangle())
        .onTapGesture { viewModel.selectedTheme = theme }
        .contextMenu {
            Button("应用") { viewModel.apply(theme) }
            if isApplied { Button("停止") { viewModel.stop(theme) } }
            Divider()
            Button("删除", role: .destructive) { viewModel.delete(theme) }
        }
    }

    private var viewModel_iconSize: CGFloat { 200 }

    @ViewBuilder private var preview: some View {
        if let url = theme.previewURL, let image = NSImage(contentsOf: url) {
            Image(nsImage: image)
                .resizable()
                .aspectRatio(contentMode: .fill)
        } else {
            Image(systemName: "square.grid.2x2")
                .font(.largeTitle)
                .foregroundStyle(.secondary)
        }
    }
}
