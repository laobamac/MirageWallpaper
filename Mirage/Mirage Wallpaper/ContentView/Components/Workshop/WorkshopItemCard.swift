//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopItemCard: View {
    var item: WorkshopItem
    var isHovered: Bool
    var isSelected: Bool
    var libraryStatus: WorkshopLibraryItemStatus
    var downloadStatus: WorkshopDownloadStatus
    var isFavorite: Bool = false
    var isActive: Bool = true
    var animatedPreviewMode: GSAnimatedPreviewPlayback = .hover

    var body: some View {
        ZStack(alignment: .bottom) {
            WorkshopImage(
                url: item.previewImageURL,
                contentMode: .fill,
                isAnimating: isActive && (isHovered || isSelected ||
                    animatedPreviewMode == .visible),
                isLoadingEnabled: isActive
            )

            captionStrip
        }
        .aspectRatio(1.0, contentMode: .fit)
        .frame(maxWidth: .infinity)
        .overlay {
            GeometryReader { proxy in
                topBadges
                    .frame(width: max(0, proxy.size.width - 16), alignment: .leading)
                    .padding(8)
            }
            .allowsHitTesting(false)
        }
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(
                    isSelected ? Color.accentColor : Color.white.opacity(isHovered ? 0.32 : 0.06),
                    lineWidth: isSelected ? 2 : 1
                )
                .allowsHitTesting(false)
        )
        .shadow(color: .black.opacity(isHovered ? 0.30 : 0.12),
                radius: isHovered ? 12 : 4, y: isHovered ? 6 : 2)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: isHovered)
    }

    private var captionStrip: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 5) {
                Text(item.title)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                    .foregroundStyle(.white)
                if isFavorite {
                    Image(systemName: "heart.fill")
                        .font(.caption)
                        .foregroundStyle(.red)
                }
            }

            HStack(spacing: 10) {
                Label(item.formattedSubscriptions, systemImage: "arrow.down.circle.fill")
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
                Label(item.formattedViews, systemImage: "eye.fill")
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
                Spacer(minLength: 0)
                Text(item.displayTypeName)
                    .lineLimit(1)
                    .fixedSize(horizontal: true, vertical: false)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(kindColor.opacity(0.9), in: Capsule())
                    .foregroundStyle(.white)
            }
            .font(.caption2)
            .foregroundStyle(.white.opacity(0.9))
        }
        .padding(.horizontal, 10)
        .padding(.top, 22)
        .padding(.bottom, 9)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            LinearGradient(
                colors: [.clear, .black.opacity(0.55), .black.opacity(0.78)],
                startPoint: .top,
                endPoint: .bottom
            )
        )
    }

    private var topBadges: some View {
        VStack {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .top, spacing: 6) {
                    leadingBadges
                    Spacer(minLength: 6)
                    statusBadge
                        .fixedSize(horizontal: true, vertical: false)
                }
                VStack(alignment: .leading, spacing: 5) {
                    leadingBadges
                    statusBadge
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            Spacer(minLength: 0)
        }
    }

    private var leadingBadges: some View {
        VStack(alignment: .leading, spacing: 5) {
            if item.isPreset {
                Label("预设", systemImage: "slider.horizontal.3")
                    .font(.caption2.bold())
                    .lineLimit(1)
                    .fixedSize()
                    .padding(.horizontal, 7)
                    .padding(.vertical, 3)
                    .background(.purple, in: Capsule())
                    .foregroundStyle(.white)
            }
            ageRatingBadge
        }
    }

    /// Only restricted ratings are called out; badging every all-ages wallpaper
    /// would mark almost the whole grid.
    @ViewBuilder
    private var ageRatingBadge: some View {
        if let rating = item.ageRating, rating != .everyone {
            Text(rating.displayName)
                .font(.caption2.bold())
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(rating == .mature ? .red : .orange, in: Capsule())
                .foregroundStyle(.white)
        }
    }

    @ViewBuilder
    private var statusBadge: some View {
        if let state = downloadStatus.state,
           !(libraryStatus.needsPresetDependency && state == .completed) {
            downloadBadge(state)
                .fixedSize()
        } else if libraryStatus.isInstalled {
            HStack(spacing: 3) {
                Image(systemName: libraryStatus.needsPresetDependency ? "exclamationmark.triangle.fill" : "checkmark")
                    .font(.caption2).bold()
                Text(LocalizedStringKey(libraryStatus.needsPresetDependency ? "缺少基础壁纸" : "已下载"))
                    .font(.caption2)
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(libraryStatus.needsPresetDependency ? .orange : .green, in: Capsule())
            .foregroundStyle(.white)
            .lineLimit(2)
            .minimumScaleFactor(0.75)
        }
    }

    private var kindColor: Color {
        switch item.kind {
        case .scene: return .purple
        case .web: return .orange
        case .video: return .blue
        case .unsupported: return .gray
        }
    }

    @ViewBuilder
    private func downloadBadge(_ state: DownloadState) -> some View {
        switch state {
        case .downloading(let progress):
            HStack(spacing: 4) {
                ZStack {
                    Circle()
                        .stroke(Color.white.opacity(0.3), lineWidth: 2)
                        .frame(width: 14, height: 14)
                    Circle()
                        .trim(from: 0, to: progress.fraction)
                        .stroke(Color.white, lineWidth: 2)
                        .frame(width: 14, height: 14)
                        .rotationEffect(.degrees(-90))
                }
                Text("\(Int(progress.fraction * 100))%")
                    .font(.caption2)
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(.blue, in: Capsule())
            .foregroundStyle(.white)
        case .queued:
            badge("排队中", systemImage: "clock", color: .orange)
        case .resolving, .validating:
            HStack(spacing: 3) {
                ProgressView().scaleEffect(0.5).frame(width: 12, height: 12)
                Text("处理中").font(.caption2)
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(.blue, in: Capsule())
            .foregroundStyle(.white)
        case .failed:
            badge("失败", systemImage: "exclamationmark.triangle", color: .red)
        case .completed:
            badge("已下载", systemImage: "checkmark", color: .green)
        }
    }

    private func badge(_ text: LocalizedStringKey, systemImage: String, color: Color) -> some View {
        HStack(spacing: 3) {
            Image(systemName: systemImage).font(.caption2)
            Text(text).font(.caption2)
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(color, in: Capsule())
        .foregroundStyle(.white)
    }
}
