//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopView: View {
    @EnvironmentObject private var globalSettingsViewModel: GlobalSettingsViewModel
    let browseStore: WorkshopBrowseStore
    let creatorStore: WorkshopCreatorStore
    let downloadStore: WorkshopDownloadStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let sessionStore: SteamSessionStore
    let subscriptionStore: SubscriptionStore
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let isActive: Bool

    @State private var hoveredId: String?
    @State private var isDownloadPopoverPresented = false
    @State private var showAPIKeyReminder = false

    var body: some View {
        VStack(spacing: 8) {
            if !globalSettingsViewModel.settings.hasValidCustomSteamAPIKey {
                SteamAPIKeyReminderBanner()
            }

            HStack {
                Button {
                    viewModel.isFilterReveal.toggle()
                } label: {
                    Label("筛选", systemImage: "checklist.checked")
                }
                .buttonStyle(.borderedProminent)

                WorkshopSearchBar(browseStore: browseStore)

                Spacer()

                Button {
                    browseStore.refresh()
                } label: {
                    Group {
                        if browseStore.isLoading {
                            ProgressView()
                                .controlSize(.small)
                        } else {
                            Image(systemName: "arrow.triangle.2.circlepath")
                        }
                    }
                    .frame(width: 16, height: 16)
                }
                .disabled(browseStore.isLoading)
                .help("刷新创意工坊")

                WallpaperGridViewMenu(viewModel: viewModel)

                Button {
                    isDownloadPopoverPresented.toggle()
                } label: {
                    ZStack(alignment: .topTrailing) {
                        Image(systemName: "arrow.down.circle")
                            .font(.title3)
                        if downloadStore.activeDownloadCount > 0 {
                            Text("\(downloadStore.activeDownloadCount)")
                                .font(.system(size: 9))
                                .bold()
                                .foregroundStyle(.white)
                                .padding(3)
                                .background(Color.red)
                                .clipShape(Circle())
                                .offset(x: 6, y: -4)
                        }
                    }
                }
                .buttonStyle(.plain)
                .popover(isPresented: $isDownloadPopoverPresented) {
                    DownloadPopover(
                        downloadStore: downloadStore,
                        onCancel: selectionCoordinator.cancelDownload,
                        onRetry: downloadStore.retry
                    )
                }

                steamAccountSection
            }

            if sessionStore.setupState != .ready {
                steamSetupBanner
            }

            if let message = browseStore.pageNavigationMessage {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                    Text(message)
                        .font(.caption)
                    Spacer()
                    Button {
                        browseStore.pageNavigationMessage = nil
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .buttonStyle(.plain)
                    .help("关闭")
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(Color.orange.opacity(0.1))
            }

            if browseStore.isLoading && browseStore.items.isEmpty {
                ZStack(alignment: .bottom) {
                    VStack(spacing: 16) {
                        ProgressView()
                            .scaleEffect(1.5)
                        Text("正在搜索创意工坊...")
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if browseStore.totalPages > 1 {
                        PageNavigator(
                            currentPage: browseStore.currentPage,
                            pageCount: browseStore.totalPages,
                            onSelect: browseStore.goToPage
                        )
                        .padding(.bottom, 12)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if browseStore.items.isEmpty && !browseStore.isLoading {
                ZStack(alignment: .bottom) {
                    VStack(spacing: 12) {
                        Image(systemName: "magnifyingglass")
                            .font(.system(size: 40))
                            .foregroundStyle(.tertiary)
                        if let error = browseStore.error {
                            Text("加载失败")
                                .font(.title3)
                                .foregroundStyle(.secondary)
                            Text(error)
                                .font(.caption)
                                .foregroundStyle(.red)
                            Button("重试") { browseStore.search() }
                                .buttonStyle(.borderedProminent)
                        } else {
                            Text("没有找到壁纸")
                                .font(.title3)
                                .foregroundStyle(.secondary)
                            Text("试试调整搜索条件或筛选标签")
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if browseStore.totalPages > 1 {
                        PageNavigator(
                            currentPage: browseStore.currentPage,
                            pageCount: browseStore.totalPages,
                            onSelect: browseStore.goToPage
                        )
                        .padding(.bottom, 12)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    ZStack(alignment: .bottom) {
                        ScrollView {
                            Color.clear
                                .frame(height: 0)
                                .id("workshopTop")

                            LazyVGrid(
                                columns: [GridItem(.adaptive(
                                    minimum: viewModel.explorerIconSize,
                                    maximum: viewModel.explorerIconSize * 2
                                ), spacing: 14)],
                                alignment: .leading,
                                spacing: 14
                            ) {
                                ForEach(browseStore.items) { item in
                                    WorkshopItemCard(
                                        item: item,
                                        isHovered: hoveredId == item.id,
                                        isSelected: selectionCoordinator.selectedItem?.id == item.id,
                                        libraryStatus: libraryStore.status(
                                            for: item.publishedFileId
                                        ),
                                        downloadStatus: downloadStore.status(
                                            for: item.publishedFileId
                                        ),
                                        isFavorite: interactionStore.isFavorite(
                                            item.publishedFileId
                                        ),
                                        isActive: isActive,
                                        animatedPreviewMode: globalSettingsViewModel.settings.animatedPreviewPlaybackMode
                                    )
                                    .onHover { hovered in
                                        hoveredId = hovered ? item.id : nil
                                    }
                                    .onTapGesture {
                                        selectionCoordinator.selectWorkshopItem(item)
                                    }
                                    .contextMenu {
                                        if let wallpaper = libraryStore.installedItem(
                                            id: item.publishedFileId
                                        ) {
                                            ExplorerItemMenu(
                                                contentViewModel: viewModel,
                                                wallpaperViewModel: wallpaperViewModel,
                                                creatorStore: creatorStore,
                                                interactionStore: interactionStore,
                                                libraryStore: libraryStore,
                                                selectionCoordinator: selectionCoordinator,
                                                current: wallpaper
                                            )
                                            ExplorerGlobalMenu(
                                                contentViewModel: viewModel,
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
                                            WallpaperGridViewMenu(viewModel: viewModel)
                                        }
                                    }
                                }
                            }
                            #if arch(arm64)
                            .padding(.trailing)
                            #endif

                            if browseStore.isLoading {
                                ProgressView()
                                    .padding()
                            }

                            if browseStore.totalPages > 1 {
                                Color.clear.frame(height: 58)
                            }
                        }
                        .contextMenu {
                            WallpaperGridViewMenu(viewModel: viewModel)
                        }

                        if browseStore.totalPages > 1 {
                            PageNavigator(
                                currentPage: browseStore.currentPage,
                                pageCount: browseStore.totalPages,
                                onSelect: browseStore.goToPage
                            )
                            .padding(.bottom, 12)
                        }
                    }
                    .onChange(of: browseStore.currentPage) { _, _ in
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo("workshopTop", anchor: .top)
                        }
                    }
                }
            }
        }
        .onAppear {
            presentAPIKeyReminderIfNeeded()
            sessionStore.checkSetup()
            if browseStore.items.isEmpty {
                browseStore.search()
            }
        }
        .alert("建议设置专属 Steam API Key", isPresented: $showAPIKeyReminder) {
            Button("立即设置") { AppDelegate.shared.openSteamAPIKeySettings() }
            Button("暂时使用内置 Key", role: .cancel) { }
        } message: {
            Text("内置 Key 由所有 Mirage 用户共享，繁忙时可能导致创意工坊无法加载。设置您自己的免费 API Key 后将不再提醒。此 Key 只影响浏览，不影响登录和下载。")
        }
        .alert("Steam 登录", isPresented: Binding(
            get: { sessionStore.logoutResultMessage != nil },
            set: { if !$0 { sessionStore.logoutResultMessage = nil } }
        )) {
            Button("确定", role: .cancel) {
                sessionStore.logoutResultMessage = nil
            }
        } message: {
            Text(sessionStore.logoutResultMessage ?? "")
        }
    }

    // MARK: - Steam Account Section (#2)

    @ViewBuilder
    var steamAccountSection: some View {
        switch sessionStore.setupState {
        case .ready:
            HStack(spacing: 8) {
                HStack(spacing: 4) {
                    Image(systemName: "person.crop.circle.fill")
                        .foregroundStyle(.green)
                        .font(.caption)
                    Text(SteamServiceManager.shared.accountName)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                if sessionStore.isLoggingOut {
                    HStack(spacing: 5) {
                        ProgressView()
                            .controlSize(.small)
                        Text("正在退出…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Button {
                        sessionStore.logout()
                    } label: {
                        Image(systemName: "rectangle.portrait.and.arrow.right")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("退出 Steam")
                }
            }
        case .checking:
            HStack(spacing: 6) {
                ProgressView()
                    .controlSize(.small)
                Text(sessionStore.checkingMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        case .needsLogin, .serviceUnavailable:
            Button {
                AppDelegate.shared.openSteamSetup()
            } label: {
                Label("设置 Steam", systemImage: "gear")
                    .font(.caption)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    @ViewBuilder
    var steamSetupBanner: some View {
        switch sessionStore.setupState {
        case .checking:
            HStack(spacing: 12) {
                ProgressView()
                    .controlSize(.regular)
                VStack(alignment: .leading, spacing: 2) {
                    Text(sessionStore.checkingMessage)
                        .font(.callout)
                        .bold()
                    Text("正在确认 Steam 登录状态，请稍候。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(12)
            .background(Color.secondary.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 8))
        case .needsLogin, .serviceUnavailable:
            HStack(spacing: 12) {
                Image(systemName: "cloud.fill")
                    .font(.title2)
                    .foregroundStyle(.blue)
                VStack(alignment: .leading, spacing: 2) {
                    Text(sessionStore.setupState == .serviceUnavailable ? "Steam 服务不可用" : "连接 Steam 以下载壁纸")
                        .font(.callout)
                        .bold()
                    Text(sessionStore.setupState == .serviceUnavailable
                         ? "检查内置 Steam 服务后重试。"
                         : "登录 Steam 后可直接从创意工坊下载壁纸到本地（需拥有 Wallpaper Engine）")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Text(sessionStore.setupState == .serviceUnavailable ? "检查服务" : "立即设置")
                }
                .buttonStyle(.borderedProminent)
            }
            .padding(12)
            .background(Color.blue.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Color.blue.opacity(0.2), lineWidth: 1)
            )
        case .ready:
            EmptyView()
        }
    }

    private func presentAPIKeyReminderIfNeeded() {
        guard !globalSettingsViewModel.settings.hasValidCustomSteamAPIKey else { return }
        showAPIKeyReminder = SteamAPIKeyReminderPolicy.shouldPresent()
    }

}

enum SteamAPIKeyReminderPolicy {
    static func shouldPresent() -> Bool {
        let key = "SteamAPIKeyReminderLastShown"
        let now = Date().timeIntervalSince1970
        guard now - UserDefaults.standard.double(forKey: key) >= 24 * 60 * 60 else { return false }
        UserDefaults.standard.set(now, forKey: key)
        return true
    }
}

struct SteamAPIKeyReminderBanner: View {
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "exclamationmark.key.fill")
                .font(.title2)
                .foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 2) {
                Text("建议设置专属 Steam Web API Key")
                    .font(.callout)
                    .bold()
                Text("内置 Key 由所有用户共享，可能因请求过多影响浏览；专属 Key 不影响 Steam 登录和下载。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button("立即设置") {
                AppDelegate.shared.openSteamAPIKeySettings()
            }
            .buttonStyle(.borderedProminent)
            .tint(.orange)
        }
        .padding(12)
        .background(Color.orange.opacity(0.1))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.orange.opacity(0.3), lineWidth: 1))
    }
}
