//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

// A Wallpaper-Engine-style browse card: a fixed 16:9 preview that always fills
// the frame, a gradient caption strip with title + stats, and status/preset
// badges. Downloaded state is passed in as plain values so the card never
// touches the filesystem while rendering.
struct WorkshopItemCard: View {
    var item: WorkshopItem
    var isHovered: Bool
    var isDownloaded: Bool
    var presetNeedsDependency: Bool
    var downloadState: DownloadState?

    var body: some View {
        ZStack(alignment: .bottom) {
            WorkshopImage(url: item.previewImageURL, contentMode: .fill)

            captionStrip

            topBadges
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .frame(maxWidth: .infinity)
        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.white.opacity(isHovered ? 0.18 : 0.06),
                              lineWidth: 1)
        )
        .shadow(color: .black.opacity(isHovered ? 0.30 : 0.12),
                radius: isHovered ? 12 : 4, y: isHovered ? 6 : 2)
        .scaleEffect(isHovered ? 1.035 : 1.0)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: isHovered)
    }

    private var captionStrip: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(item.title)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
                .foregroundStyle(.white)

            HStack(spacing: 10) {
                Label(item.formattedSubscriptions, systemImage: "arrow.down.circle.fill")
                Label(item.formattedViews, systemImage: "eye.fill")
                Spacer(minLength: 0)
                Text(item.displayTypeName)
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
            HStack(alignment: .top) {
                if item.isPreset {
                    Label("预设", systemImage: "slider.horizontal.3")
                        .font(.caption2.bold())
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(.purple, in: Capsule())
                        .foregroundStyle(.white)
                }
                Spacer(minLength: 0)
                statusBadge
            }
            Spacer(minLength: 0)
        }
        .padding(8)
    }

    @ViewBuilder
    private var statusBadge: some View {
        if let state = downloadState, !(presetNeedsDependency && state == .completed) {
            downloadBadge(state)
        } else if isDownloaded {
            HStack(spacing: 3) {
                Image(systemName: presetNeedsDependency ? "exclamationmark.triangle.fill" : "checkmark")
                    .font(.caption2).bold()
                Text(presetNeedsDependency ? "缺少基础壁纸" : "已下载")
                    .font(.caption2)
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(presetNeedsDependency ? .orange : .green, in: Capsule())
            .foregroundStyle(.white)
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
        case .downloading(let percent):
            HStack(spacing: 4) {
                if let percent {
                    ZStack {
                        Circle()
                            .stroke(Color.white.opacity(0.3), lineWidth: 2)
                            .frame(width: 14, height: 14)
                        Circle()
                            .trim(from: 0, to: percent)
                            .stroke(Color.white, lineWidth: 2)
                            .frame(width: 14, height: 14)
                            .rotationEffect(.degrees(-90))
                    }
                    Text("\(Int(percent * 100))%")
                        .font(.caption2)
                } else {
                    Image(systemName: "arrow.down")
                        .font(.caption2)
                    Text("连接中")
                        .font(.caption2)
                }
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(.blue, in: Capsule())
            .foregroundStyle(.white)
        case .queued:
            badge("排队中", systemImage: "clock", color: .orange)
        case .starting, .validating:
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

    private func badge(_ text: String, systemImage: String, color: Color) -> some View {
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
