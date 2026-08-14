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
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let isActive: Bool
    let animatedPreviewMode: GSAnimatedPreviewPlayback
    var onSeeAll: () -> Void

    @State private var hoveredId: String?
    @State private var scrollIndex = 0

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

            ScrollViewReader { proxy in
                ZStack {
                    ScrollView(.horizontal, showsIndicators: false) {
                        LazyHStack(spacing: 14) {
                            ForEach(items) { item in
                                WorkshopItemDownloadStatus(
                                    workshopID: item.publishedFileId,
                                    downloadStore: workshopViewModel.downloadStore
                                ) { downloadState in
                                    DiscoverCard(
                                        item: item,
                                        isHovered: hoveredId == item.id,
                                        isSelected: workshopViewModel.selectedItem?.id == item.id,
                                        isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                                        presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                                        downloadState: downloadState,
                                        isFavorite: workshopViewModel.isWorkshopFavorite(item.publishedFileId),
                                        cardWidth: contentViewModel.explorerIconSize,
                                        isActive: isActive,
                                        animatedPreviewMode: animatedPreviewMode
                                    )
                                }
                                .id(item.id)
                                .onHover { hovered in
                                    hoveredId = hovered ? item.id : nil
                                }
                                .onTapGesture {
                                    workshopViewModel.selectWorkshopItem(item)
                                }
                                .contextMenu {
                                    if let wallpaper = workshopViewModel.installedItem(
                                        workshopId: item.publishedFileId
                                    ) {
                                        ExplorerItemMenu(
                                            contentViewModel: contentViewModel,
                                            wallpaperViewModel: wallpaperViewModel,
                                            workshopViewModel: workshopViewModel,
                                            current: wallpaper
                                        )
                                        ExplorerGlobalMenu(
                                            contentViewModel: contentViewModel,
                                            wallpaperViewModel: wallpaperViewModel
                                        )
                                    } else {
                                        WorkshopCardContextMenu(
                                            item: item,
                                            workshopViewModel: workshopViewModel
                                        )
                                        WallpaperGridViewMenu(viewModel: contentViewModel)
                                    }
                                }
                            }
                        }
                        .padding(.vertical, 6)
                        .padding(.horizontal, 2)
                        .background(HorizontalScrollWheelBridge { offset in
                            let extent = max(1, contentViewModel.explorerIconSize + 14)
                            let index = Int((offset / extent).rounded(.up))
                            scrollIndex = min(max(0, items.count - 1), max(0, index))
                        })
                    }

                    HStack {
                        scrollButton(systemImage: "chevron.left", disabled: scrollIndex == 0) {
                            scrollIndex = max(0, scrollIndex - 3)
                            scroll(to: scrollIndex, proxy: proxy)
                        }
                        Spacer()
                        scrollButton(
                            systemImage: "chevron.right",
                            disabled: scrollIndex >= max(0, items.count - 1)
                        ) {
                            scrollIndex = min(max(0, items.count - 1), scrollIndex + 3)
                            scroll(to: scrollIndex, proxy: proxy)
                        }
                    }
                    .padding(.horizontal, 6)
                }
                .onChange(of: items.map(\.id)) { _, _ in
                    scrollIndex = 0
                }
            }
        }
    }

    private func scroll(to index: Int, proxy: ScrollViewProxy) {
        guard items.indices.contains(index) else { return }
        withAnimation(.easeOut(duration: 0.25)) {
            proxy.scrollTo(items[index].id, anchor: .leading)
        }
    }

    private func scrollButton(systemImage: String, disabled: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.title3.weight(.semibold))
                .frame(width: 44, height: 44)
                .background(.regularMaterial, in: Circle())
                .shadow(color: .black.opacity(0.18), radius: 4, y: 2)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .disabled(disabled)
        .opacity(disabled ? 0.35 : 1)
    }
}

struct DiscoverCard: View {
    var item: WorkshopItem
    var isHovered: Bool
    var isSelected: Bool
    var isDownloaded: Bool
    var presetNeedsDependency: Bool
    var downloadState: DownloadState?
    var isFavorite: Bool
    var cardWidth: CGFloat
    var isActive: Bool
    var animatedPreviewMode: GSAnimatedPreviewPlayback

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack(alignment: .topTrailing) {
                WorkshopImage(
                    url: item.previewImageURL,
                    contentMode: .fill,
                    isAnimating: isActive && (isHovered || isSelected ||
                        animatedPreviewMode == .visible),
                    isLoadingEnabled: isActive
                )
                    .frame(width: cardWidth, height: cardWidth)

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
                HStack(spacing: 5) {
                    Text(item.title)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    if isFavorite {
                        Image(systemName: "heart.fill")
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                }

                HStack(spacing: 10) {
                    Label(item.formattedSubscriptions, systemImage: "arrow.down.circle")
                    Label(item.displayTypeName, systemImage: "tag")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 9)
            .frame(width: cardWidth, alignment: .leading)
        }
        .background(.regularMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(
                    isSelected ? Color.accentColor : Color.white.opacity(isHovered ? 0.32 : 0.06),
                    lineWidth: isSelected ? 2 : 1
                )
                .allowsHitTesting(false)
        }
        .shadow(color: .black.opacity(isHovered ? 0.28 : 0.10),
                radius: isHovered ? 12 : 4, y: isHovered ? 6 : 2)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: isHovered)
    }

    @ViewBuilder
    func downloadStateIndicator(_ state: DownloadState) -> some View {
        switch state {
        case .downloading(let progress):
            ZStack {
                Circle()
                    .fill(.ultraThinMaterial)
                    .frame(width: 22, height: 22)
                Circle()
                    .trim(from: 0, to: progress.fraction)
                    .stroke(Color.blue, lineWidth: 2)
                    .frame(width: 16, height: 16)
                    .rotationEffect(.degrees(-90))
            }
        case .queued, .resolving:
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
