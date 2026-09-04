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
    var isActive: Bool = true
    @State private var isConfirmingUnsubscribe = false

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
        .task(id: item?.publishedFileId) {
            guard let item else { return }
            workshopViewModel.prepareWorkshopInteractions(for: item)
        }
        .confirmationDialog(
            "取消订阅",
            isPresented: $isConfirmingUnsubscribe,
            presenting: item
        ) { item in
            Button("取消订阅", role: .destructive) {
                workshopViewModel.unsubscribe(item)
            }
            Button("取消", role: .cancel) { }
        } message: { _ in
            Text("取消订阅后，Mirage 会停止下载并删除 Mirage 下载目录中的副本。Steam 内容目录中的文件不会被删除。")
        }
    }

    @ViewBuilder
    func detailContent(for item: WorkshopItem) -> some View {
        ScrollView {
            LazyVStack(spacing: 16) {
                WorkshopImage(
                    url: item.previewImageURL,
                    contentMode: .fill,
                    isAnimating: isActive,
                    isLoadingEnabled: isActive
                )
                    .frame(width: 280, height: 280)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                    .overlay(
                        RoundedRectangle(cornerRadius: 12)
                            .strokeBorder(Color.white.opacity(0.7), lineWidth: 3)
                    )

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

                sectionHeader("操作")
                favoriteSection(for: item)
                subscriptionSection(for: item)
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

                sectionHeader("评论")
                commentsSection(for: item)

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
    func favoriteSection(for item: WorkshopItem) -> some View {
        let id = item.publishedFileId
        let isFavorite = workshopViewModel.isWorkshopFavorite(id)
        let isChanging = workshopViewModel.changingFavoriteIDs.contains(id)

        VStack(spacing: 6) {
            if workshopViewModel.steamSetupState == .checking {
                HStack(spacing: 8) {
                    ProgressView()
                        .controlSize(.small)
                    Text(workshopViewModel.steamCheckingMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 5)
            } else if workshopViewModel.steamSetupState != .ready {
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Label("登录 Steam 以收藏", systemImage: "person.crop.circle.badge.exclamationmark")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            } else if isChanging {
                HStack(spacing: 8) {
                    ProgressView()
                        .controlSize(.small)
                    Text("正在同步收藏状态…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 5)
            } else {
                Button {
                    workshopViewModel.toggleFavorite(item)
                } label: {
                    Label(
                        LocalizedStringKey(isFavorite ? "取消收藏" : "加入收藏"),
                        systemImage: isFavorite ? "heart.slash.fill" : "heart.fill"
                    )
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(isFavorite ? .red : .accentColor)
            }

            if let error = workshopViewModel.favoriteActionError(for: id) {
                Text(error)
                    .font(.caption2)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
            }
        }
    }

    @ViewBuilder
    func subscriptionSection(for item: WorkshopItem) -> some View {
        let id = item.publishedFileId
        let state = workshopViewModel.subscriptionState(for: id)
        let isChecking = workshopViewModel.checkingSubscriptionIDs.contains(id)
        let isChanging = workshopViewModel.changingSubscriptionIDs.contains(id)

        VStack(spacing: 6) {
            if workshopViewModel.steamSetupState == .checking {
                HStack(spacing: 8) {
                    ProgressView()
                        .controlSize(.small)
                    Text(workshopViewModel.steamCheckingMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 5)
            } else if workshopViewModel.steamSetupState != .ready {
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Label("登录 Steam 以订阅", systemImage: "person.crop.circle.badge.exclamationmark")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            } else if isChecking || isChanging {
                HStack(spacing: 8) {
                    ProgressView()
                        .controlSize(.small)
                    Text(LocalizedStringKey(isChanging ? "正在同步订阅状态…" : "正在检查订阅状态…"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 5)
            } else if state == .unknown {
                Button {
                    workshopViewModel.refreshSubscriptionStates(for: [item])
                } label: {
                    Label("重新检查订阅状态", systemImage: "arrow.clockwise")
                        .frame(maxWidth: .infinity)
                }
            } else if state == .subscribed {
                Button {
                    isConfirmingUnsubscribe = true
                } label: {
                    Label("取消订阅", systemImage: "xmark.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
            } else {
                Button {
                    workshopViewModel.subscribe(item)
                } label: {
                    Label("订阅并下载", systemImage: "plus.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }

            if let error = workshopViewModel.subscriptionActionError(for: id) {
                Text(error)
                    .font(.caption2)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
            }
        }
    }

    @ViewBuilder
    func commentsSection(for item: WorkshopItem) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(L("%d 条评论", workshopViewModel.commentsTotal))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button {
                    workshopViewModel.refreshComments(for: item)
                } label: {
                    Image(systemName: "arrow.triangle.2.circlepath")
                }
                .buttonStyle(.plain)
                .disabled(workshopViewModel.isLoadingComments)
                .help(L("刷新评论"))
            }

            if workshopViewModel.commentsItemID != item.publishedFileId ||
                workshopViewModel.isLoadingComments && workshopViewModel.comments.isEmpty {
                HStack {
                    Spacer()
                    ProgressView()
                        .controlSize(.small)
                    Spacer()
                }
                .padding(.vertical, 8)
            } else if let error = workshopViewModel.commentsError,
                      workshopViewModel.comments.isEmpty {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
            } else if workshopViewModel.comments.isEmpty {
                Text("暂无评论")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
            } else {
                VStack(spacing: 8) {
                    ForEach(workshopViewModel.comments) { comment in
                        commentRow(comment)
                    }
                }
            }

            if workshopViewModel.commentsStartIndex > 0 ||
                workshopViewModel.commentsNextStartIndex < workshopViewModel.commentsTotal {
                HStack {
                    Button {
                        workshopViewModel.loadPreviousComments(for: item)
                    } label: {
                        Image(systemName: "chevron.left")
                    }
                    .disabled(workshopViewModel.commentsStartIndex == 0 || workshopViewModel.isLoadingComments)

                    Spacer()

                    Text(L(
                        "%d–%d / %d",
                        workshopViewModel.comments.isEmpty ? 0 : workshopViewModel.commentsStartIndex + 1,
                        workshopViewModel.commentsStartIndex + workshopViewModel.comments.count,
                        workshopViewModel.commentsTotal
                    ))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()

                    Spacer()

                    Button {
                        workshopViewModel.loadNextComments(for: item)
                    } label: {
                        Image(systemName: "chevron.right")
                    }
                    .disabled(
                        workshopViewModel.commentsNextStartIndex <= workshopViewModel.commentsStartIndex ||
                        workshopViewModel.commentsNextStartIndex >= workshopViewModel.commentsTotal ||
                        workshopViewModel.isLoadingComments
                    )
                }
            }

            if workshopViewModel.commentsCanPost {
                TextField("发表评论", text: $workshopViewModel.commentDraft, axis: .vertical)
                    .lineLimit(2...5)
                HStack {
                    Spacer()
                    Button {
                        workshopViewModel.postComment(for: item)
                    } label: {
                        if workshopViewModel.isPostingComment {
                            ProgressView()
                                .controlSize(.small)
                                .frame(width: 16, height: 16)
                        } else {
                            Label("发布评论", systemImage: "paperplane.fill")
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(
                        workshopViewModel.commentDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
                        workshopViewModel.isPostingComment
                    )
                }
            } else if SteamServiceManager.shared.isLoggedIn && workshopViewModel.commentsError == nil {
                Text("该作品当前不允许发表评论")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if let error = workshopViewModel.commentsError,
               !workshopViewModel.comments.isEmpty {
                Text(error)
                    .font(.caption2)
                    .foregroundStyle(.red)
            }
        }
    }

    func commentRow(_ comment: WorkshopComment) -> some View {
        let creator = workshopViewModel.commentAuthors[comment.authorSteamId]
        let isCurrentUser = comment.authorSteamId == SteamServiceManager.shared.steamId
        let creatorName = creator?.name.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let displayName = !creatorName.isEmpty
            ? creatorName
            : (isCurrentUser ? L("我") : comment.authorSteamId)
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 5) {
                AsyncImage(url: creator?.avatarURL) { phase in
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
                .frame(width: 24, height: 24)
                .clipShape(Circle())
                Text(displayName)
                    .font(.caption.bold())
                    .lineLimit(1)
                Spacer()
                Text(comment.createdAt, format: .dateTime.year().month().day().hour().minute())
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }

            Text(comment.isHidden ? L("该评论已隐藏") : comment.text)
                .font(.caption)
                .foregroundStyle(comment.isHidden ? .tertiary : .primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)

            if comment.upvotes > 0 {
                Label("\(comment.upvotes)", systemImage: "hand.thumbsup.fill")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(8)
        .background(Color.secondary.opacity(0.07), in: RoundedRectangle(cornerRadius: 7))
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
        } else if workshopViewModel.steamSetupState == .checking {
            HStack(spacing: 8) {
                ProgressView()
                    .controlSize(.small)
                Text(workshopViewModel.steamCheckingMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 6)
        } else if workshopViewModel.steamSetupState != .ready {
            VStack(spacing: 6) {
                Text(workshopViewModel.steamServiceStatus.workshopDownload.summary)
                    .font(.caption2)
                    .foregroundStyle(.orange)
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Label(
                        LocalizedStringKey(workshopViewModel.steamSetupState == .serviceUnavailable ? "检查 Steam 服务" : "登录全球 Steam"),
                        systemImage: "person.crop.circle.badge.exclamationmark"
                    )
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }
        } else if let state = workshopViewModel.downloadState(for: item.publishedFileId) {
            switch state {
            case .downloading(let progress):
                VStack(spacing: 4) {
                    ProgressView(value: progress.fraction)
                        .animation(.linear, value: progress.fraction)
                    Text(L("%d%% 下载中…", Int(progress.fraction * 100)))
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
            case .resolving:
                HStack {
                    ProgressView()
                        .scaleEffect(0.7)
                    Text("正在解析创意工坊内容…")
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

struct CreatorGridViewMenu: View {
    @Binding var iconSize: Double
    @Binding var pageSize: Int

    var body: some View {
        Menu("视图") {
            Picker(LocalizedStringKey("图标大小"), selection: $iconSize) {
                Text(LocalizedStringKey("小图标")).tag(CreatorGridMetrics.small)
                Text(LocalizedStringKey("中图标")).tag(CreatorGridMetrics.medium)
                Text(LocalizedStringKey("大图标")).tag(CreatorGridMetrics.large)
            }
            .pickerStyle(.inline)

            Divider()

            Picker(LocalizedStringKey("每页数量"), selection: $pageSize) {
                Text("10").tag(10)
                Text("25").tag(25)
                Text("50").tag(50)
            }
            .pickerStyle(.inline)
        }
    }
}

enum CreatorGridMetrics {
    static let small: Double = 115
    static let medium: Double = 170
    static let large: Double = 260

    static func normalized(_ value: Double) -> Double {
        [small, medium, large].contains(value) ? value : medium
    }
}

struct CreatorProfileView: View {
    let creator: WorkshopCreator
    @ObservedObject var workshopViewModel: WorkshopViewModel
    let animatedPreviewMode: GSAnimatedPreviewPlayback
    @State private var selectedDetailItem: WorkshopItem?
    @State private var hoveredItemID: String?
    @AppStorage("CreatorIconSize") private var iconSize: Double = CreatorGridMetrics.medium
    @AppStorage("CreatorPerPage") private var creatorPageSize: Int = 10

    var body: some View {
        Group {
            if let detailItem = selectedDetailItem {
                creatorItemDetail(item: detailItem)
            } else {
                creatorGrid
            }
        }
        .onAppear {
            iconSize = CreatorGridMetrics.normalized(iconSize)
        }
        .onChange(of: creatorPageSize) {
            workshopViewModel.creatorItemsPage = 1
            workshopViewModel.loadCreatorItems(for: creator)
        }
    }

    private var gridColumns: [GridItem] {
        [GridItem(.adaptive(minimum: iconSize, maximum: iconSize * 1.6), spacing: 10)]
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

            creatorContent
                .contextMenu {
                    CreatorGridViewMenu(iconSize: $iconSize, pageSize: $creatorPageSize)
                }
        }
    }

    @ViewBuilder
    private var creatorContent: some View {
        if workshopViewModel.isLoadingCreatorItems && workshopViewModel.creatorItems.isEmpty {
            centeredContent {
                ProgressView()
                Text(LocalizedStringKey("加载中..."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } else if let error = workshopViewModel.creatorItemsError, workshopViewModel.creatorItems.isEmpty {
            centeredContent {
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
        } else if workshopViewModel.creatorItems.isEmpty && !workshopViewModel.isLoadingCreatorItems {
            centeredContent {
                Image(systemName: "rectangle.on.rectangle.slash")
                    .font(.system(size: 32))
                    .foregroundStyle(.secondary)
                Text(L("该创作者暂未发布 Wallpaper Engine 作品"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
            }
        } else {
            creatorScrollView
        }
    }

    private func centeredContent<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        VStack(spacing: 12) {
            content()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .contentShape(Rectangle())
    }

    private var creatorScrollView: some View {
        ScrollViewReader { proxy in
            ScrollView {
                Color.clear
                    .frame(height: 0)
                    .id("creatorTop")

                LazyVGrid(columns: gridColumns, spacing: 10) {
                    ForEach(workshopViewModel.creatorItems) { item in
                        WorkshopItemCard(
                            item: item,
                            isHovered: hoveredItemID == item.id,
                            isSelected: false,
                            isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                            presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                            downloadState: workshopViewModel.downloadState(for: item.publishedFileId),
                            isFavorite: workshopViewModel.isWorkshopFavorite(item.publishedFileId),
                            isActive: selectedDetailItem == nil,
                            animatedPreviewMode: animatedPreviewMode
                        )
                        .onHover { hovering in
                            hoveredItemID = hovering ? item.id : nil
                        }
                        .onTapGesture {
                            selectedDetailItem = item
                        }
                        .contextMenu {
                            Section {
                                if workshopViewModel.changingFavoriteIDs.contains(item.publishedFileId) {
                                    Label("正在同步收藏状态…", systemImage: "arrow.triangle.2.circlepath")
                                } else {
                                    Button {
                                        workshopViewModel.toggleFavorite(item)
                                    } label: {
                                        Label(
                                            LocalizedStringKey(workshopViewModel.isWorkshopFavorite(item.publishedFileId) ? "取消收藏" : "加入收藏"),
                                            systemImage: workshopViewModel.isWorkshopFavorite(item.publishedFileId) ? "heart.slash.fill" : "heart.fill"
                                        )
                                    }
                                }

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

                            CreatorGridViewMenu(iconSize: $iconSize, pageSize: $creatorPageSize)
                        }
                    }
                }
                .padding(12)
                .frame(maxWidth: .infinity)
                .contentShape(Rectangle())

                if workshopViewModel.creatorTotalPages > 1 {
                    PageNavigator(
                        currentPage: workshopViewModel.creatorItemsPage,
                        pageCount: workshopViewModel.creatorTotalPages,
                        onSelect: workshopViewModel.goToCreatorPage
                    )
                    .padding(.bottom, 12)
                }
            }
            .onChange(of: workshopViewModel.creatorItemsPage) { _, _ in
                withAnimation(.easeOut(duration: 0.2)) {
                    proxy.scrollTo("creatorTop", anchor: .top)
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
