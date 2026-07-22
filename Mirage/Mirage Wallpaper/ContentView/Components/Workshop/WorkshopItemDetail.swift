//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopItemDetail: View {
    var item: WorkshopItem?
    @ObservedObject var workshopViewModel: WorkshopViewModel

    @State private var isLoaded = false

    var body: some View {
        VStack {
            if let item {
                detailContent(for: item)
            } else {
                VStack(spacing: 12) {
                    Image(systemName: "sidebar.right")
                        .font(.system(size: 32))
                        .foregroundStyle(.tertiary)
                    Text("点击壁纸查看详情")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onChange(of: item?.id) { _ in
            isLoaded = false
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
                withAnimation(.easeInOut(duration: 0.2)) {
                    isLoaded = true
                }
            }
        }
        .onAppear {
            isLoaded = false
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
                withAnimation(.easeInOut(duration: 0.2)) {
                    isLoaded = true
                }
            }
        }
    }

    @ViewBuilder
    func detailContent(for item: WorkshopItem) -> some View {
        ScrollView {
            VStack(spacing: 16) {
                if !isLoaded {
                    VStack(spacing: 12) {
                        ProgressView()
                            .scaleEffect(1.2)
                        Text("正在加载壁纸详情...")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .frame(width: 280, height: 158)
                    .background(Color.secondary.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                } else {
                    WorkshopImage(url: item.previewImageURL, contentMode: .fill)
                        .frame(width: 280, height: 158)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .overlay(
                            RoundedRectangle(cornerRadius: 12)
                                .strokeBorder(Color.white.opacity(0.7), lineWidth: 3)
                        )
                }

                Text(item.title)
                    .font(.headline)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)

                if item.isPreset {
                    Label("创意工坊预设：需要对应的基础壁纸", systemImage: "slider.horizontal.3")
                        .font(.caption.bold())
                        .foregroundStyle(.purple)
                        .padding(8)
                        .frame(maxWidth: .infinity)
                        .background(Color.purple.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
                }

                HStack(spacing: 16) {
                    StatView(icon: "arrow.down.circle.fill", value: item.formattedSubscriptions, label: "订阅")
                    StatView(icon: "heart.fill", value: item.formattedFavorited, label: "收藏")
                    StatView(icon: "eye.fill", value: item.formattedViews, label: "浏览")
                }

                HStack(spacing: 12) {
                    Label(item.displayTypeName, systemImage: "tag.fill")
                    Label(item.formattedFileSize, systemImage: "doc.fill")
                }
                .font(.caption)
                .foregroundStyle(.secondary)

                sectionHeader("标签")
                tagList(for: item)

                sectionHeader("描述")
                if item.itemDescription.isEmpty {
                    Text("暂无描述")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                } else {
                    Text(item.itemDescription)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(8)
                }

                sectionHeader("操作")
                downloadSection(for: item)

                Button {
                    let urlStr = "https://steamcommunity.com/sharedfiles/filedetails/?id=\(item.publishedFileId)"
                    if let url = URL(string: urlStr) {
                        NSWorkspace.shared.open(url)
                    }
                } label: {
                    Label("在 Steam 中查看", systemImage: "safari")
                        .frame(maxWidth: .infinity)
                }

                sectionHeader("信息")
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text("Workshop ID")
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(item.publishedFileId)
                    }
                    HStack {
                        Text("更新时间")
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(item.timeUpdated, style: .date)
                    }
                }
                .font(.caption)
            }
            .padding([.horizontal, .top])
        }

        HStack {
            Spacer()
            Button {
                AppDelegate.shared.mainWindowController.close()
            } label: {
                Text("确定").frame(width: 50)
            }
            .buttonStyle(.borderedProminent)
            Button {
                AppDelegate.shared.mainWindowController.close()
            } label: {
                Text("取消").frame(width: 50)
            }
        }
        .padding()
    }

    @ViewBuilder
    func downloadSection(for item: WorkshopItem) -> some View {
        let hasDownloadTask = workshopViewModel.downloadState(for: item.publishedFileId) != nil
        let installed = workshopViewModel.installedItem(workshopId: item.publishedFileId)
        if let installed, installed.needsPresetDependency {
            VStack(spacing: 6) {
                Text("预设已下载，但缺少基础壁纸 \(installed.presetDependency?.rawValue ?? "")")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                Button {
                    workshopViewModel.requestPresetDependency(for: installed)
                } label: {
                    Label("下载基础壁纸", systemImage: "square.stack.3d.up.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }
        } else if !hasDownloadTask, installed?.isValid == true {
            Button { } label: {
                Label(item.isPreset ? "预设已安装" : "已下载", systemImage: "checkmark.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(.green)
            .disabled(true)
        } else if workshopViewModel.steamSetupState != .ready {
            VStack(spacing: 6) {
                Text(workshopViewModel.steamServiceStatus.workshopDownload.summary)
                    .font(.caption2)
                    .foregroundStyle(.orange)
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Label(
                        workshopViewModel.steamSetupState == .steamCMDMissing ? "安装 SteamCMD" : "登录全球 Steam",
                        systemImage: "person.crop.circle.badge.exclamationmark"
                    )
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }
        } else if let state = workshopViewModel.downloadState(for: item.publishedFileId) {
            switch state {
            case .downloading(let percent):
                VStack(spacing: 4) {
                    if let percent {
                        ProgressView(value: percent)
                            .animation(.linear, value: percent)
                    } else {
                        ProgressView(value: 0)
                    }
                    Text(percent.map { "\(Int($0 * 100))% 下载中…" } ?? "正在连接 Steam…")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Button {
                    workshopViewModel.cancelDownload(item)
                } label: {
                    Label("取消下载", systemImage: "xmark.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
            case .queued:
                Button { } label: {
                    Label("排队中...", systemImage: "clock")
                        .frame(maxWidth: .infinity)
                }
                .disabled(true)
            case .starting:
                HStack {
                    ProgressView()
                        .scaleEffect(0.7)
                    Text("正在启动 SteamCMD…")
                        .font(.caption)
                }
            case .validating:
                HStack {
                    ProgressView()
                        .scaleEffect(0.7)
                    Text("正在验证下载文件…")
                        .font(.caption)
                }
            case .failed(let msg):
                Text(msg)
                    .font(.caption2)
                    .foregroundStyle(.red)
                Button {
                    if let task = workshopViewModel.downloadQueue.first(where: { $0.id == item.publishedFileId }) {
                        workshopViewModel.retryDownload(task)
                    }
                } label: {
                    Label("重试", systemImage: "arrow.clockwise")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            case .completed:
                Button { } label: {
                    Label("已完成", systemImage: "checkmark.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(.green)
                .disabled(true)
            }
        } else {
            Button {
                workshopViewModel.downloadItem(item)
            } label: {
                Label(item.isPreset ? "下载预设" : "下载壁纸", systemImage: "arrow.down.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    @ViewBuilder
    func tagList(for item: WorkshopItem) -> some View {
        if item.tags.isEmpty {
            Text("暂无标签")
                .font(.caption)
                .foregroundStyle(.tertiary)
        } else {
            HStack {
                ForEach(item.tags.prefix(6), id: \.self) { tag in
                    Text(tag)
                        .font(.caption2)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background {
                            RoundedRectangle(cornerRadius: 12)
                                .colorInvert()
                                .foregroundStyle(Color.primary)
                            RoundedRectangle(cornerRadius: 12)
                                .stroke(Color.secondary, lineWidth: 1)
                        }
                }
                if item.tags.count > 6 {
                    Text("+\(item.tags.count - 6)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    func sectionHeader(_ title: String) -> some View {
        HStack(spacing: 3) {
            Text(title)
            VStack {
                Divider().frame(height: 1).overlay(Color.accentColor)
            }
        }
    }
}

struct StatView: View {
    var icon: String
    var value: String
    var label: String

    var body: some View {
        VStack(spacing: 2) {
            Image(systemName: icon)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.caption)
                .bold()
            Text(label)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
    }
}
