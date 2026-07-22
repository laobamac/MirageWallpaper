//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DiscoverSectionView: View {
    var title: String
    var icon: String
    var iconColor: Color
    var items: [WorkshopItem]
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @ObservedObject var contentViewModel: ContentViewModel
    var onSeeAll: () -> Void

    @State private var hoveredId: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: icon)
                    .foregroundStyle(iconColor)
                    .font(.title3)
                Text(title)
                    .font(.title3)
                    .bold()
                Spacer()
                Button {
                    onSeeAll()
                } label: {
                    HStack(spacing: 4) {
                        Text("查看全部")
                        Image(systemName: "chevron.right")
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }

            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(spacing: 14) {
                    ForEach(items) { item in
                        DiscoverCard(
                            item: item,
                            isHovered: hoveredId == item.id,
                            isSelected: workshopViewModel.selectedItem?.id == item.id,
                            isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                            presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                            downloadState: workshopViewModel.downloadState(for: item.publishedFileId)
                        )
                        .onHover { hovered in
                            withAnimation(.easeInOut(duration: 0.18)) {
                                hoveredId = hovered ? item.id : nil
                            }
                        }
                        .onTapGesture {
                            workshopViewModel.selectWorkshopItem(item)
                        }
                    }
                }
                .padding(.vertical, 6)
                .padding(.horizontal, 2)
            }
        }
    }
}

struct DiscoverCard: View {
    var item: WorkshopItem
    var isHovered: Bool
    var isSelected: Bool
    var isDownloaded: Bool
    var presetNeedsDependency: Bool
    var downloadState: DownloadState?

    // A fixed, WE-style tile. The preview always fills its box; text lives on a
    // solid footer below so it reads cleanly at small sizes.
    private let cardWidth: CGFloat = 236

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack(alignment: .topTrailing) {
                WorkshopImage(url: item.previewImageURL, contentMode: .fill)
                    .frame(width: cardWidth, height: cardWidth * 9 / 16)

                if isDownloaded {
                    Image(systemName: presetNeedsDependency ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                        .foregroundStyle(.white, presetNeedsDependency ? .orange : .green)
                        .symbolRenderingMode(.palette)
                        .font(.body)
                        .padding(7)
                } else if let state = downloadState {
                    downloadStateIndicator(state)
                        .padding(7)
                }
            }
            .overlay(alignment: .topLeading) {
                if item.isPreset {
                    Label("预设", systemImage: "slider.horizontal.3")
                        .font(.caption2.bold())
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(.purple, in: Capsule())
                        .foregroundStyle(.white)
                        .padding(7)
                }
            }

            VStack(alignment: .leading, spacing: 3) {
                Text(item.title)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)

                HStack(spacing: 10) {
                    Label(item.formattedSubscriptions, systemImage: "arrow.down.circle")
                    Label(item.displayTypeName, systemImage: "tag")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 9)
            .frame(width: cardWidth, alignment: .leading)
        }
        .background(.regularMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(isSelected ? Color.accentColor : Color.white.opacity(0.06),
                              lineWidth: isSelected ? 2 : 1)
        }
        .shadow(color: .black.opacity(isHovered ? 0.28 : 0.10),
                radius: isHovered ? 12 : 4, y: isHovered ? 6 : 2)
        .scaleEffect(isHovered ? 1.03 : 1.0)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: isHovered)
    }

    @ViewBuilder
    func downloadStateIndicator(_ state: DownloadState) -> some View {
        switch state {
        case .downloading(let percent):
            ZStack {
                Circle()
                    .fill(.ultraThinMaterial)
                    .frame(width: 22, height: 22)
                if let percent {
                    Circle()
                        .trim(from: 0, to: percent)
                        .stroke(Color.blue, lineWidth: 2)
                        .frame(width: 16, height: 16)
                        .rotationEffect(.degrees(-90))
                } else {
                    ProgressView()
                        .controlSize(.mini)
                }
            }
        case .queued, .starting:
            Image(systemName: "clock.fill")
                .foregroundStyle(.white, .orange)
                .symbolRenderingMode(.palette)
                .font(.body)
        case .validating:
            ProgressView()
                .scaleEffect(0.5)
        default:
            EmptyView()
        }
    }
}
