//
//  Mirage Wallpaper
//
//  Non-modal transfer cards shown at the bottom of the main window.
//

import SwiftUI

struct MobileTransferOverlay: View {
    @ObservedObject private var model = MobileTransferProgressModel.shared
    @ObservedObject private var localization = MirageLocalization.shared

    var body: some View {
        if !model.jobs.isEmpty {
            VStack(spacing: 10) {
                ForEach(model.jobs) { job in
                    transferCard(job)
                }
            }
            .padding(.bottom, 22)
            .transition(.move(edge: .bottom).combined(with: .opacity))
            .animation(.easeInOut(duration: 0.2), value: model.jobs.map(\.id))
            .environment(\.locale, localization.locale)
        }
    }

    private func transferCard(_ job: MobileTransferProgressModel.Job) -> some View {
        HStack(spacing: 14) {
            Image(systemName: iconName(for: job.phase))
                .font(.system(size: 27, weight: .medium))
                .foregroundStyle(iconColor(for: job.phase))
                .frame(width: 44, height: 44)
                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))

            VStack(alignment: .leading, spacing: 5) {
                Text(L("正在将“%@”传输至“%@”", job.wallpaperTitle, job.deviceName))
                    .font(.subheadline.weight(.medium))
                    .lineLimit(2)
                    .truncationMode(.middle)

                HStack(spacing: 8) {
                    Text(statusText(for: job.phase))
                        .font(.caption)
                        .foregroundStyle(statusColor(for: job.phase))
                        .lineLimit(1)
                        .truncationMode(.tail)

                    Spacer(minLength: 8)

                    if isActive(job.phase) {
                        Text("\(Int((job.progress * 100).rounded()))%")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }

                ProgressView(value: job.progress)
                    .progressViewStyle(.linear)
                    .tint(progressColor(for: job.phase))
            }

            Button {
                // Hides only the progress card; the socket transfer continues.
                model.dismiss(id: job.id)
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 24, height: 24)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("关闭")
        }
        .padding(.horizontal, 15)
        .padding(.vertical, 12)
        .frame(width: 440)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.primary.opacity(0.1))
        )
        .shadow(color: .black.opacity(0.24), radius: 18, y: 8)
    }

    private func statusText(for phase: MobileTransferProgressModel.Phase) -> String {
        switch phase {
        case .preparing:
            return L("正在打包壁纸")
        case .uploading:
            return L("正在上传至移动设备")
        case .completed:
            return L("已完成")
        case .failed(let message):
            return L("传输失败：%@", message)
        }
    }

    private func iconName(for phase: MobileTransferProgressModel.Phase) -> String {
        switch phase {
        case .completed:
            return "checkmark.circle.fill"
        case .failed:
            return "exclamationmark.triangle.fill"
        case .preparing, .uploading:
            return "iphone.radiowaves.left.and.right"
        }
    }

    private func iconColor(for phase: MobileTransferProgressModel.Phase) -> Color {
        switch phase {
        case .completed:
            return .green
        case .failed:
            return .red
        case .preparing, .uploading:
            return .accentColor
        }
    }

    private func statusColor(for phase: MobileTransferProgressModel.Phase) -> Color {
        switch phase {
        case .completed:
            return .green
        case .failed:
            return .red
        case .preparing, .uploading:
            return .accentColor
        }
    }

    private func progressColor(for phase: MobileTransferProgressModel.Phase) -> Color {
        if case .failed = phase { return .red }
        if case .completed = phase { return .green }
        return .accentColor
    }

    private func isActive(_ phase: MobileTransferProgressModel.Phase) -> Bool {
        switch phase {
        case .preparing, .uploading:
            return true
        case .completed, .failed:
            return false
        }
    }

}
