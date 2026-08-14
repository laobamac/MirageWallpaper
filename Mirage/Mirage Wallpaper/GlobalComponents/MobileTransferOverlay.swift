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
                        .transition(.opacity)
                }
            }
            .padding(.bottom, 22)
            .transition(.opacity)
            .animation(.easeOut(duration: 0.35), value: model.jobs.map(\.id))
            .environment(\.locale, localization.locale)
        }
    }

    private func transferCard(_ job: MobileTransferProgressModel.Job) -> some View {
        HStack(spacing: 14) {
            Image(systemName: iconName(for: job))
                .font(.system(size: 27, weight: .medium))
                .foregroundStyle(iconColor(for: job.phase))
                .frame(width: 44, height: 44)
                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))

            VStack(alignment: .leading, spacing: 5) {
                Text(titleText(for: job))
                    .font(.subheadline.weight(.medium))
                    .lineLimit(2)
                    .truncationMode(.middle)

                HStack(spacing: 8) {
                    Text(statusText(for: job))
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

    private func titleText(for job: MobileTransferProgressModel.Job) -> String {
        switch job.destination {
        case .device(let deviceName):
            return L("正在将“%@”传输至“%@”", job.wallpaperTitle, deviceName)
        case .file:
            return L("正在导出“%@”", job.wallpaperTitle)
        }
    }

    private func statusText(for job: MobileTransferProgressModel.Job) -> String {
        switch job.phase {
        case .preparing:
            return L("正在打包壁纸")
        case .converting:
            return L("正在转换场景壁纸")
        case .waitingForDevice:
            return L("正在连接移动设备")
        case .uploading:
            return L("正在上传至移动设备")
        case .completed:
            return L("已完成")
        case .failed(let message):
            switch job.destination {
            case .device:
                return L("传输失败：%@", message)
            case .file:
                return L("导出失败：%@", message)
            }
        }
    }

    private func iconName(for job: MobileTransferProgressModel.Job) -> String {
        switch job.phase {
        case .completed:
            return "checkmark.circle.fill"
        case .failed:
            return "exclamationmark.triangle.fill"
        case .preparing, .converting, .waitingForDevice, .uploading:
            switch job.destination {
            case .device:
                return "iphone.radiowaves.left.and.right"
            case .file:
                return "arrow.down.doc"
            }
        }
    }

    private func iconColor(for phase: MobileTransferProgressModel.Phase) -> Color {
        switch phase {
        case .completed:
            return .green
        case .failed:
            return .red
        case .preparing, .converting, .waitingForDevice, .uploading:
            return .accentColor
        }
    }

    private func statusColor(for phase: MobileTransferProgressModel.Phase) -> Color {
        switch phase {
        case .completed:
            return .green
        case .failed:
            return .red
        case .preparing, .converting, .waitingForDevice, .uploading:
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
        case .preparing, .converting, .waitingForDevice, .uploading:
            return true
        case .completed, .failed:
            return false
        }
    }

}
