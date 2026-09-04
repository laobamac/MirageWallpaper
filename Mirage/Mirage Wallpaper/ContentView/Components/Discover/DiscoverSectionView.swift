//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DiscoverSectionView: View {
    var row: DiscoverRow
    let discoverStore: DiscoverStore
    let creatorStore: WorkshopCreatorStore
    let downloadStore: WorkshopDownloadStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let subscriptionStore: SubscriptionStore
    @ObservedObject var contentViewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let isActive: Bool
    let animatedPreviewMode: GSAnimatedPreviewPlayback
    let onSeeAll: () -> Void

    @State private var hoveredID: String?
    @State private var scrollIndex = 0

    private var cardWidth: CGFloat {
        min(190, max(118, contentViewModel.explorerIconSize))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(row.query.title)
                .font(.title3.weight(.medium))
                .lineLimit(1)

            if row.isLoading && row.items.isEmpty {
                loadingCards
            } else if let error = row.error, row.items.isEmpty {
                HStack(spacing: 10) {
                    Text(error)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                    Button("重试") {
                        discoverStore.loadRow(id: row.id)
                    }
                }
                .frame(height: cardWidth, alignment: .center)
            } else if row.items.isEmpty {
                Text("此推荐暂无可用壁纸")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .frame(height: cardWidth, alignment: .center)
            } else {
                cards
            }

            if !row.items.isEmpty {
                HStack(spacing: 8) {
                    Button {
                        onSeeAll()
                    } label: {
                        Text("查看更多")
                        .padding(.horizontal, 10)
                        .frame(height: 30)
                    }
                    .buttonStyle(.borderless)
                    .background(Color.primary.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 2))

                    Text(L("%d 项", row.total))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
            }
        }
        .onAppear {
            discoverStore.loadRow(id: row.id)
        }
    }

    private var loadingCards: some View {
        HStack(spacing: 8) {
            ForEach(0..<6, id: \.self) { _ in
                Rectangle()
                    .fill(Color.primary.opacity(0.07))
                    .frame(width: cardWidth, height: cardWidth)
                    .overlay { ProgressView().controlSize(.small) }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .clipped()
    }

    private var cards: some View {
        ScrollViewReader { proxy in
            ZStack {
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(spacing: 8) {
                        ForEach(row.items) { item in
                            DiscoverCard(
                                item: item,
                                isHovered: hoveredID == item.id,
                                isSelected: discoverStore.selectedItemID == item.id,
                                libraryStatus: libraryStore.status(
                                    for: item.publishedFileId
                                ),
                                downloadStatus: downloadStore.status(
                                    for: item.publishedFileId
                                ),
                                cardWidth: cardWidth,
                                isActive: isActive,
                                animatedPreviewMode: animatedPreviewMode
                            )
                            .id(item.id)
                            .onHover { hovered in
                                hoveredID = hovered ? item.id : nil
                            }
                            .onTapGesture {
                                discoverStore.select(item)
                            }
                            .contextMenu {
                                if let wallpaper = libraryStore.installedItem(
                                    id: item.publishedFileId
                                ) {
                                    ExplorerItemMenu(
                                        contentViewModel: contentViewModel,
                                        wallpaperViewModel: wallpaperViewModel,
                                        creatorStore: creatorStore,
                                        interactionStore: interactionStore,
                                        libraryStore: libraryStore,
                                        selectionCoordinator: selectionCoordinator,
                                        current: wallpaper
                                    )
                                    ExplorerGlobalMenu(
                                        contentViewModel: contentViewModel,
                                        wallpaperViewModel: wallpaperViewModel
                                    )
                                } else {
                                    WorkshopCardContextMenu(
                                        item: item,
                                        creatorStore: creatorStore,
                                        downloadStore: downloadStore,
                                        interactionStore: interactionStore,
                                        selectionCoordinator: selectionCoordinator,
                                        subscriptionStore: subscriptionStore
                                    )
                                    WallpaperGridViewMenu(viewModel: contentViewModel)
                                }
                            }
                        }
                    }
                    .background(HorizontalScrollWheelBridge { offset in
                        let extent = max(1, cardWidth + 8)
                        scrollIndex = min(max(0, row.items.count - 1), max(0, Int((offset / extent).rounded(.up))))
                    })
                }

                HStack {
                    if scrollIndex > 0 {
                        scrollButton(systemImage: "chevron.left") {
                            scrollIndex = max(0, scrollIndex - 5)
                            scroll(to: scrollIndex, proxy: proxy)
                        }
                    }
                    Spacer()
                    if scrollIndex < max(0, row.items.count - 1) {
                        scrollButton(systemImage: "chevron.right") {
                            scrollIndex = min(max(0, row.items.count - 1), scrollIndex + 5)
                            scroll(to: scrollIndex, proxy: proxy)
                        }
                    }
                }
                .padding(.horizontal, 4)
                .allowsHitTesting(true)
            }
            .frame(height: cardWidth)
            .onChange(of: row.items.map(\.id)) { _, _ in
                scrollIndex = 0
            }
        }
    }

    private func scroll(to index: Int, proxy: ScrollViewProxy) {
        guard row.items.indices.contains(index) else { return }
        withAnimation(.easeOut(duration: 0.2)) {
            proxy.scrollTo(row.items[index].id, anchor: .leading)
        }
    }

    private func scrollButton(systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.title2.weight(.semibold))
                .foregroundStyle(.white)
                .frame(width: 34, height: 72)
                .background(.black.opacity(0.55))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

struct DiscoverCard: View {
    var item: WorkshopItem
    var isHovered: Bool
    var isSelected: Bool
    var libraryStatus: WorkshopLibraryItemStatus
    var downloadStatus: WorkshopDownloadStatus
    var cardWidth: CGFloat
    var isActive: Bool
    var animatedPreviewMode: GSAnimatedPreviewPlayback

    var body: some View {
        ZStack(alignment: .topTrailing) {
            WorkshopImage(
                url: item.previewImageURL,
                contentMode: .fill,
                isAnimating: isActive && (isHovered || isSelected || animatedPreviewMode == .visible),
                isLoadingEnabled: isActive
            )
            .frame(width: cardWidth, height: cardWidth)
            .clipped()

            if libraryStatus.isInstalled {
                Image(systemName: libraryStatus.needsPresetDependency ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                    .foregroundStyle(.white, libraryStatus.needsPresetDependency ? .orange : .green)
                    .symbolRenderingMode(.palette)
                    .font(.body)
                    .padding(7)
            } else if let state = downloadStatus.state {
                downloadStateIndicator(state)
                    .padding(7)
            }
        }
        .overlay(alignment: .topLeading) {
            if item.isApproved {
                Image(systemName: "trophy.fill")
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.white)
                    .frame(width: 25, height: 25)
                    .background(Color.green.opacity(0.85))
            }
        }
        .overlay(alignment: .bottom) {
            Text(item.title)
                .font(.caption)
                .foregroundStyle(.white)
                .lineLimit(2)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 6)
                .padding(.vertical, 7)
                .background(.black.opacity(0.58))
        }
        .overlay {
            Rectangle()
                .strokeBorder(
                    isSelected ? Color.accentColor : Color.white.opacity(isHovered ? 0.55 : 0.10),
                    lineWidth: isSelected ? 3 : 1
                )
                .allowsHitTesting(false)
        }
        .brightness(isHovered ? 0.06 : 0)
        .animation(.easeOut(duration: 0.14), value: isHovered)
    }

    @ViewBuilder
    private func downloadStateIndicator(_ state: DownloadState) -> some View {
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
                .controlSize(.mini)
        default:
            EmptyView()
        }
    }
}
