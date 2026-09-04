//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DownloadPopover: View {
    let downloadStore: WorkshopDownloadStore
    let onCancel: (WorkshopItem) -> Void
    let onRetry: (DownloadTask) -> Void
    @State private var revealError: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("下载管理")
                    .font(.headline)
                Spacer()
                if hasCompleted {
                    Button {
                        downloadStore.clearCompleted()
                    } label: {
                        Text("清除记录")
                            .font(.caption)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)

            Divider()

            if downloadStore.queue.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "arrow.down.doc")
                        .font(.system(size: 36))
                        .foregroundStyle(.tertiary)
                    Text("暂无下载任务")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Text("在创意工坊中浏览并下载壁纸")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 40)
            } else {
                ScrollView {
                    VStack(spacing: 1) {
                        ForEach(downloadStore.queue) { status in
                            DownloadRow(
                                downloadStatus: status,
                                onCancel: {
                                    guard let task = status.task else { return }
                                    onCancel(task.workshopItem)
                                },
                                onRetry: {
                                    guard let task = status.task else { return }
                                    onRetry(task)
                                },
                                onReveal: {
                                    guard let task = status.task else { return }
                                    revealInFinder(task)
                                }
                            )
                        }
                    }
                }
                .frame(maxHeight: 400)
            }

            Divider()

            HStack {
                Label(
                    "\(downloadStore.activeDownloadCount) 下载中",
                    systemImage: "arrow.down.circle.fill"
                )
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                Label(
                    "\(downloadStore.completedDownloadCount) 已完成",
                    systemImage: "checkmark.circle.fill"
                )
                    .font(.caption)
                    .foregroundStyle(.green)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
        }
        .frame(width: 420)
        .alert("无法打开下载目录", isPresented: Binding(
            get: { revealError != nil },
            set: { if !$0 { revealError = nil } }
        )) {
            Button("好", role: .cancel) { revealError = nil }
        } message: {
            Text(revealError ?? L("未知错误"))
        }
    }

    private var hasCompleted: Bool {
        downloadStore.clearableDownloadCount > 0
    }

    private func revealInFinder(_ task: DownloadTask) {
        guard let path = SteamServiceManager.shared.downloadedItemDirectory(
            workshopId: task.workshopItem.publishedFileId
        ) else {
            revealError = L("未找到该壁纸的本地下载目录。")
            return
        }
        if !NSWorkspace.shared.open(path) {
            revealError = L("Finder 无法打开该壁纸目录。")
        }
    }
}

struct DownloadRow: View {
    var downloadStatus: WorkshopDownloadStatus
    var onCancel: () -> Void
    var onRetry: () -> Void
    var onReveal: () -> Void

    @ViewBuilder
    var body: some View {
        if let task = downloadStatus.task {
            row(for: task)
        }
    }

    private func row(for task: DownloadTask) -> some View {
        HStack(spacing: 10) {
            WorkshopImage(url: task.workshopItem.previewImageURL, contentMode: .fill)
                .frame(width: 64, height: 64)
                .clipShape(RoundedRectangle(cornerRadius: 4))

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(task.workshopItem.title)
                        .font(.callout)
                        .lineLimit(1)
                    if task.workshopItem.isPreset {
                        Text("预设")
                            .font(.caption2.bold())
                            .foregroundStyle(.purple)
                    } else if task.purpose == .presetDependency {
                        Text("基础壁纸")
                            .font(.caption2.bold())
                            .foregroundStyle(.orange)
                    }
                }

                switch task.state {
                case .queued:
                    HStack(spacing: 6) {
                        Text("等待可用下载槽…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(task.workshopItem.formattedFileSize)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                case .resolving:
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text("正在解析创意工坊内容…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                case .downloading(let progress):
                    ProgressView(value: progress.fraction)
                        .animation(.linear, value: progress.fraction)
                    Text(progressSummary(progress))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                case .validating:
                    Text("验证中...")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                case .completed:
                    HStack {
                        Text("已完成 ✓")
                            .font(.caption)
                            .foregroundStyle(.green)
                        Spacer()
                        Text(task.workshopItem.formattedFileSize)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                case .failed(let msg):
                    Text("失败: \(msg)")
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(1)
                        .help(msg)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            switch task.state {
            case .downloading, .resolving, .queued:
                Button { onCancel() } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                        .font(.body)
                }
                .buttonStyle(.plain)
            case .failed:
                Button { onRetry() } label: {
                    Image(systemName: "arrow.clockwise.circle.fill")
                        .foregroundStyle(.blue)
                        .font(.body)
                }
                .buttonStyle(.plain)
            case .completed:
                Button { onReveal() } label: {
                    Image(systemName: "folder.fill")
                        .foregroundStyle(.secondary)
                        .font(.body)
                }
                .buttonStyle(.plain)
            case .validating:
                ProgressView()
                    .scaleEffect(0.7)
                    .frame(width: 20, height: 20)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(Color(nsColor: NSColor.controlBackgroundColor))
    }

    private func progressSummary(_ progress: DownloadProgress) -> String {
        let percent = Int(progress.fraction * 100)
        let received = ByteCountFormatter.string(fromByteCount: progress.receivedBytes, countStyle: .file)
        let total = progress.totalBytes > 0
            ? ByteCountFormatter.string(fromByteCount: progress.totalBytes, countStyle: .file)
            : L("未知大小")
        let speed = progress.bytesPerSecond > 0
            ? L("%@/秒", ByteCountFormatter.string(fromByteCount: Int64(progress.bytesPerSecond), countStyle: .file))
            : L("正在连接 Steam…")
        if let eta = progress.etaSeconds, eta.isFinite {
            return L("%d%% · %@ / %@ · %@ · 剩余 %@", percent, received, total, speed, etaText(eta))
        }
        return L("%d%% · %@ / %@ · %@", percent, received, total, speed)
    }

    private func etaText(_ seconds: Double) -> String {
        let value = max(0, Int(seconds.rounded()))
        if value < 60 { return L("%d 秒", value) }
        if value < 3600 { return L("%d 分 %d 秒", value / 60, value % 60) }
        return L("%d 小时 %d 分", value / 3600, value % 3600 / 60)
    }
}
