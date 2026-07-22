//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DownloadPopover: View {
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @State private var revealError: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("下载管理")
                    .font(.headline)
                Spacer()
                if hasCompleted {
                    Button {
                        workshopViewModel.clearCompletedDownloads()
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

            if workshopViewModel.downloadQueue.isEmpty {
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
                        ForEach(workshopViewModel.downloadQueue) { task in
                            DownloadRow(
                                task: task,
                                onCancel: { workshopViewModel.cancelDownload(task.workshopItem) },
                                onRetry: { workshopViewModel.retryDownload(task) },
                                onReveal: { revealInFinder(task) }
                            )
                        }
                    }
                }
                .frame(maxHeight: 400)
            }

            Divider()

            HStack {
                let active = workshopViewModel.downloadQueue.filter {
                    if case .downloading = $0.state { return true }
                    if case .starting = $0.state { return true }
                    if case .validating = $0.state { return true }
                    return false
                }.count
                let completed = workshopViewModel.downloadQueue.filter {
                    if case .completed = $0.state { return true }
                    return false
                }.count

                Label("\(active) 下载中", systemImage: "arrow.down.circle.fill")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                Label("\(completed) 已完成", systemImage: "checkmark.circle.fill")
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
            Text(revealError ?? "未知错误")
        }
    }

    private var hasCompleted: Bool {
        workshopViewModel.downloadQueue.contains {
            if case .completed = $0.state { return true }
            if case .failed = $0.state { return true }
            return false
        }
    }

    private func revealInFinder(_ task: DownloadTask) {
        guard let path = SteamCMDManager.shared.downloadedItemDirectory(
            workshopId: task.workshopItem.publishedFileId
        ) else {
            revealError = "未找到该壁纸的本地下载目录。"
            return
        }
        if !NSWorkspace.shared.open(path) {
            revealError = "Finder 无法打开该壁纸目录。"
        }
    }
}

struct DownloadRow: View {
    var task: DownloadTask
    var onCancel: () -> Void
    var onRetry: () -> Void
    var onReveal: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            WorkshopImage(url: task.workshopItem.previewImageURL, contentMode: .fill)
                .frame(width: 64, height: 36)
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
                        Text("等待 SteamCMD 按顺序下载…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(task.workshopItem.formattedFileSize)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                case .starting:
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text("正在启动 SteamCMD…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                case .downloading(let percent):
                    if let percent {
                        ProgressView(value: percent)
                            .animation(.linear, value: percent)
                    } else {
                        ProgressView(value: 0)
                    }
                    HStack {
                        Text(percent.map { "\(Int($0 * 100))%" } ?? "正在连接 Steam…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(task.workshopItem.formattedFileSize)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
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
            case .downloading, .starting, .queued:
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
}
