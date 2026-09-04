//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopTagBar: View {
    let browseStore: WorkshopBrowseStore

    private let displayTags: [WorkshopTag] = [
        .anime, .nature, .abstract, .landscape, .sciFi, .cartoon,
        .cyberpunk, .fantasy, .girl, .game, .animal, .music,
        .vehicle, .technology, .city, .space, .dark, .minimal, .relaxing
    ]

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                ForEach(displayTags) { tag in
                    let isSelected = browseStore.selectedTags.contains(tag.rawValue)
                    Button {
                        browseStore.applyTagFilter(tag.rawValue)
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: tag.sfSymbol)
                                .font(.caption2)
                            Text(tag.displayName)
                                .font(.caption)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(isSelected ? Color.accentColor : Color.secondary.opacity(0.12))
                        .foregroundStyle(isSelected ? .white : .primary)
                        .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                    .animation(.spring(response: 0.25, dampingFraction: 0.8), value: isSelected)
                }

                if !browseStore.selectedTags.isEmpty {
                    Button {
                        browseStore.clearTags()
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "xmark")
                                .font(.caption2)
                            Text("清除")
                                .font(.caption)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.red.opacity(0.15))
                        .foregroundStyle(.red)
                        .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.vertical, 2)
        }
    }
}
