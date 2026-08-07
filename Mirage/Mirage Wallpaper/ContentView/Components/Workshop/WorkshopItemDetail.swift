//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Combine

struct WorkshopItemDetail: View {
    var item: WorkshopItem?
    @ObservedObject var workshopViewModel: WorkshopViewModel
    var isEmbedded: Bool = false
    var embeddedCreatorSteamId: String?

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
                    .frame(width: 280, height: 280)
                    .background(Color.secondary.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                } else {
                    WorkshopImage(
                        url: item.previewImageURL,
                        contentMode: .fill,
                        isAnimating: true
                    )
                        .frame(width: 280, height: 280)
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

                if !item.creatorSteamId.isEmpty {
                    let isSelf = isEmbedded && embeddedCreatorSteamId.map({ !$0.isEmpty && item.creatorSteamId == $0 }) ?? false
                    Button {
                        if isSelf { return }
                        if let creator = WorkshopCreator(item: item) {
                            workshopViewModel.openCreatorProfile(creator)
                        }
                    } label: {
                        HStack(spacing: 10) {
                            creatorAvatar(for: item)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.creatorDisplayName)
                                    .font(.subheadline.weight(.semibold))
                                    .lineLimit(1)
                                Text(LocalizedStringKey(isSelf ? "当前作者" : "查看作者主页和作品"))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer(minLength: 0)
                            if !isSelf {
                                Image(systemName: "arrow.up.right")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .padding(8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 7))
                    }
                    .buttonStyle(.plain)
                    .disabled(isSelf)

                    if let creator = WorkshopCreator(item: item), creator.profileURL != nil {
                        Button {
                            workshopViewModel.openCreatorWorkshop(creator)
                        } label: {
                            Label(LocalizedStringKey("在 Steam 中查看作者"), systemImage: "safari")
                        }
                        .buttonStyle(.link)
                    }
                }

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
                    if let rating = item.ageRating {
                        let tint: Color = rating == .everyone
                            ? .secondary
                            : (rating == .mature ? .red : .orange)
                        Label(rating.displayName, systemImage: rating == .everyone ? "checkmark.seal" : "exclamationmark.triangle")
                            .foregroundStyle(tint)
                    }
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

        if !isEmbedded {
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
                Label(LocalizedStringKey(item.isPreset ? "预设已安装" : "已下载"), systemImage: "checkmark.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(.green)
            .disabled(true)

            if isEmbedded, let installed {
                Button {
                    workshopViewModel.openInstalledWallpaper(installed)
                } label: {
                    Label(LocalizedStringKey("设为壁纸"), systemImage: "play.rectangle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }
        } else if workshopViewModel.steamSetupState != .ready {
            VStack(spacing: 6) {
                Text(workshopViewModel.steamServiceStatus.workshopDownload.summary)
                    .font(.caption2)
                    .foregroundStyle(.orange)
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Label(
                        LocalizedStringKey(workshopViewModel.steamSetupState == .steamCMDMissing ? "安装 SteamCMD" : "登录全球 Steam"),
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
                    Text(percent.map { L("%d%% 下载中…", Int($0 * 100)) } ?? L("正在连接 Steam…"))
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
                Label(LocalizedStringKey(item.isPreset ? "下载预设" : "下载壁纸"), systemImage: "arrow.down.circle.fill")
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

    func sectionHeader(_ title: LocalizedStringKey) -> some View {
        HStack(spacing: 3) {
            Text(title)
            VStack {
                Divider().frame(height: 1).overlay(Color.accentColor)
            }
        }
    }

    @ViewBuilder
    func creatorAvatar(for item: WorkshopItem) -> some View {
        AsyncImage(url: item.creatorAvatarURL) { phase in
            if case .success(let image) = phase {
                image
                    .resizable()
                    .scaledToFill()
            } else {
                Image(systemName: "person.crop.circle.fill")
                    .resizable()
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: 34, height: 34)
        .clipShape(Circle())
    }
}

struct StatView: View {
    var icon: String
    var value: String
    var label: LocalizedStringKey

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

struct CreatorProfileView: View {
    let creator: WorkshopCreator
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @State private var selectedDetailItem: WorkshopItem?
    @State private var hoveredItemID: String?
    @AppStorage("ExplorerIconSize") private var iconSize: Double = 170
    @AppStorage("CreatorPerPage") private var creatorPageSize: Int = 10

    var body: some View {
        Group {
            if let detailItem = selectedDetailItem {
                creatorItemDetail(item: detailItem)
            } else {
                creatorGrid
            }
        }
        .onChange(of: creatorPageSize) {
            workshopViewModel.creatorItemsPage = 1
            workshopViewModel.loadCreatorItems(for: creator)
        }
    }

    private var creatorGrid: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                AsyncImage(url: creator.avatarURL) { phase in
                    if case .success(let image) = phase {
                        image.resizable().scaledToFill()
                    } else {
                        Image(systemName: "person.crop.circle.fill")
                            .resizable()
                            .foregroundStyle(.secondary)
                    }
                }
                .frame(width: 42, height: 42)
                .clipShape(Circle())

                VStack(alignment: .leading, spacing: 2) {
                    Text(creator.name)
                        .font(.headline)
                        .lineLimit(1)
                    Text(L("Steam ID：%@", creator.steamId))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                Spacer(minLength: 0)

                if let profileURL = creator.profileURL {
                    Button {
                        NSWorkspace.shared.open(profileURL)
                    } label: {
                        Image(systemName: "safari")
                    }
                    .buttonStyle(.plain)
                    .help(L("在 Steam 中查看作者"))
                }

                Button {
                    workshopViewModel.showCreatorProfile = false
                } label: {
                    Image(systemName: "xmark")
                }
                .buttonStyle(.plain)
                .help(L("关闭作者主页"))
            }
            .padding(12)

            Divider()

            if workshopViewModel.isLoadingCreatorItems && workshopViewModel.creatorItems.isEmpty {
                Spacer()
                VStack(spacing: 12) {
                    ProgressView()
                    Text(LocalizedStringKey("加载中..."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            } else if let error = workshopViewModel.creatorItemsError, workshopViewModel.creatorItems.isEmpty {
                Spacer()
                VStack(spacing: 12) {
                    Image(systemName: "exclamationmark.triangle")
                        .font(.system(size: 32))
                        .foregroundStyle(.secondary)
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal)
                    Button(LocalizedStringKey("重试")) {
                        workshopViewModel.loadCreatorItems(for: creator)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
                Spacer()
            } else if workshopViewModel.creatorItems.isEmpty && !workshopViewModel.isLoadingCreatorItems {
                Spacer()
                VStack(spacing: 12) {
                    Image(systemName: "rectangle.on.rectangle.slash")
                        .font(.system(size: 32))
                        .foregroundStyle(.secondary)
                    Text(L("该创作者暂未发布 Wallpaper Engine 作品"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal)
                }
                Spacer()
            } else {
                ScrollView {
                    LazyVGrid(
                        columns: [GridItem(.adaptive(minimum: iconSize), spacing: 10)],
                        spacing: 10
                    ) {
                        ForEach(workshopViewModel.creatorItems) { item in
                            WorkshopItemCard(
                                item: item,
                                isHovered: hoveredItemID == item.id,
                                isSelected: false,
                                isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                                presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                                downloadState: workshopViewModel.downloadState(for: item.publishedFileId)
                            )
                            .onHover { hovering in
                                hoveredItemID = hovering ? item.id : nil
                            }
                            .onTapGesture {
                                selectedDetailItem = item
                            }
                            .contextMenu {
                                Section {
                                    Button {
                                        workshopViewModel.downloadItem(item)
                                    } label: {
                                        Label(
                                            LocalizedStringKey(item.isPreset ? "下载预设" : "下载壁纸"),
                                            systemImage: "arrow.down.circle.fill"
                                        )
                                    }
                                    .disabled(workshopViewModel.downloadState(for: item.publishedFileId) != nil)

                                    Button {
                                        selectedDetailItem = item
                                    } label: {
                                        Label(LocalizedStringKey("查看壁纸详情"), systemImage: "info.circle")
                                    }
                                }

                                Section {
                                    Button {
                                        guard let url = URL(
                                            string: "https://steamcommunity.com/sharedfiles/filedetails/?id=\(item.publishedFileId)"
                                        ) else { return }
                                        NSWorkspace.shared.open(url)
                                    } label: {
                                        Label(LocalizedStringKey("在 Steam 中查看"), systemImage: "safari")
                                    }
                                }

                                Section {
                                    Picker(LocalizedStringKey("图标大小"), selection: $iconSize) {
                                        Text(LocalizedStringKey("小图标")).tag(Double(140))
                                        Text(LocalizedStringKey("中图标")).tag(Double(170))
                                        Text(LocalizedStringKey("大图标")).tag(Double(200))
                                    }
                                    .pickerStyle(.inline)

                                    Divider()

                                    Picker(LocalizedStringKey("每页数量"), selection: $creatorPageSize) {
                                        Text("10").tag(10)
                                        Text("25").tag(25)
                                        Text("50").tag(50)
                                    }
                                    .pickerStyle(.inline)
                                }
                            }
                        }
                    }
                    .padding(12)

                    if workshopViewModel.creatorTotalPages > 1 {
                        PageNavigator(
                            currentPage: workshopViewModel.creatorItemsPage,
                            pageCount: workshopViewModel.creatorTotalPages,
                            onSelect: workshopViewModel.goToCreatorPage
                        )
                        .padding(.bottom, 12)
                    }
                }
            }
        }
    }

    private func creatorItemDetail(item: WorkshopItem) -> some View {
        VStack(spacing: 0) {
            HStack {
                Button {
                    selectedDetailItem = nil
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                            .font(.body.weight(.medium))
                        Text(L("返回"))
                            .font(.body.weight(.medium))
                    }
                    .padding(.vertical, 8)
                    .padding(.horizontal, 12)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)

                Spacer()

                Text(item.title)
                    .font(.subheadline.weight(.medium))
                    .lineLimit(1)

                Spacer()

                Spacer().frame(width: 60)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            WorkshopItemDetail(
                item: item,
                workshopViewModel: workshopViewModel,
                isEmbedded: true,
                embeddedCreatorSteamId: creator.steamId
            )
        }
    }
}


