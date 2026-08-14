//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopView: View {
    @EnvironmentObject private var globalSettingsViewModel: GlobalSettingsViewModel
    @ObservedObject var workshopViewModel: WorkshopViewModel
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

                WorkshopSearchBar(workshopViewModel: workshopViewModel)

                Spacer()

                Button {
                    workshopViewModel.refreshSearch()
                } label: {
                    Group {
                        if workshopViewModel.isLoading {
                            ProgressView()
                                .controlSize(.small)
                        } else {
                            Image(systemName: "arrow.triangle.2.circlepath")
                        }
                    }
                    .frame(width: 16, height: 16)
                }
                .disabled(workshopViewModel.isLoading)
                .help("刷新创意工坊")

                WallpaperGridViewMenu(viewModel: viewModel)

                Button {
                    isDownloadPopoverPresented.toggle()
                } label: {
                    WorkshopActiveDownloadCount(downloadStore: workshopViewModel.downloadStore) { count in
                        ZStack(alignment: .topTrailing) {
                            Image(systemName: "arrow.down.circle")
                                .font(.title3)
                            if count > 0 {
                                Text("\(count)")
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
                }
                .buttonStyle(.plain)
                .popover(isPresented: $isDownloadPopoverPresented) {
                    DownloadPopover(workshopViewModel: workshopViewModel)
                }

                steamAccountSection
            }

            if workshopViewModel.steamSetupState != .ready {
                steamSetupBanner
            }

            if let message = workshopViewModel.pageNavigationMessage {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                    Text(message)
                        .font(.caption)
                    Spacer()
                    Button {
                        workshopViewModel.pageNavigationMessage = nil
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

            if workshopViewModel.isLoading && workshopViewModel.items.isEmpty {
                ZStack(alignment: .bottom) {
                    VStack(spacing: 16) {
                        ProgressView()
                            .scaleEffect(1.5)
                        Text("正在搜索创意工坊...")
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if workshopViewModel.totalPages > 1 {
                        PageNavigator(
                            currentPage: workshopViewModel.currentPage,
                            pageCount: workshopViewModel.totalPages,
                            onSelect: workshopViewModel.goToPage
                        )
                        .padding(.bottom, 12)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if workshopViewModel.items.isEmpty && !workshopViewModel.isLoading {
                ZStack(alignment: .bottom) {
                    VStack(spacing: 12) {
                        Image(systemName: "magnifyingglass")
                            .font(.system(size: 40))
                            .foregroundStyle(.tertiary)
                        if let error = workshopViewModel.error {
                            Text("加载失败")
                                .font(.title3)
                                .foregroundStyle(.secondary)
                            Text(error)
                                .font(.caption)
                                .foregroundStyle(.red)
                            Button("重试") { workshopViewModel.search() }
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

                    if workshopViewModel.totalPages > 1 {
                        PageNavigator(
                            currentPage: workshopViewModel.currentPage,
                            pageCount: workshopViewModel.totalPages,
                            onSelect: workshopViewModel.goToPage
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
                                ForEach(workshopViewModel.items) { item in
                                    WorkshopItemDownloadStatus(
                                        workshopID: item.publishedFileId,
                                        downloadStore: workshopViewModel.downloadStore
                                    ) { downloadState in
                                        WorkshopItemCard(
                                            item: item,
                                            isHovered: hoveredId == item.id,
                                            isSelected: workshopViewModel.selectedItem?.id == item.id,
                                            isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                                            presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                                            downloadState: downloadState,
                                            isFavorite: workshopViewModel.isWorkshopFavorite(item.publishedFileId),
                                            isActive: isActive,
                                            animatedPreviewMode: globalSettingsViewModel.settings.animatedPreviewPlaybackMode
                                        )
                                    }
                                    .onHover { hovered in
                                        hoveredId = hovered ? item.id : nil
                                    }
                                    .onTapGesture {
                                        workshopViewModel.selectWorkshopItem(item)
                                    }
                                    .contextMenu {
                                        if let wallpaper = workshopViewModel.installedItem(
                                            workshopId: item.publishedFileId
                                        ) {
                                            ExplorerItemMenu(
                                                contentViewModel: viewModel,
                                                wallpaperViewModel: wallpaperViewModel,
                                                workshopViewModel: workshopViewModel,
                                                current: wallpaper
                                            )
                                            ExplorerGlobalMenu(
                                                contentViewModel: viewModel,
                                                wallpaperViewModel: wallpaperViewModel
                                            )
                                        } else {
                                            WorkshopCardContextMenu(
                                                item: item,
                                                workshopViewModel: workshopViewModel
                                            )
                                            WallpaperGridViewMenu(viewModel: viewModel)
                                        }
                                    }
                                }
                            }
                            #if arch(arm64)
                            .padding(.trailing)
                            #endif

                            if workshopViewModel.isLoading {
                                ProgressView()
                                    .padding()
                            }

                            if workshopViewModel.totalPages > 1 {
                                Color.clear.frame(height: 58)
                            }
                        }
                        .contextMenu {
                            WallpaperGridViewMenu(viewModel: viewModel)
                        }

                        if workshopViewModel.totalPages > 1 {
                            PageNavigator(
                                currentPage: workshopViewModel.currentPage,
                                pageCount: workshopViewModel.totalPages,
                                onSelect: workshopViewModel.goToPage
                            )
                            .padding(.bottom, 12)
                        }
                    }
                    .onChange(of: workshopViewModel.currentPage) { _, _ in
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo("workshopTop", anchor: .top)
                        }
                    }
                }
            }
        }
        .onAppear {
            presentAPIKeyReminderIfNeeded()
            workshopViewModel.checkSteamSetup()
            if workshopViewModel.items.isEmpty {
                workshopViewModel.search()
            }
        }
        .alert("建议设置专属 Steam API Key", isPresented: $showAPIKeyReminder) {
            Button("立即设置") { AppDelegate.shared.openSteamAPIKeySettings() }
            Button("暂时使用内置 Key", role: .cancel) { }
        } message: {
            Text("内置 Key 由所有 Mirage 用户共享，繁忙时可能导致创意工坊无法加载。设置您自己的免费 API Key 后将不再提醒。此 Key 只影响浏览，不影响登录和下载。")
        }
        .alert("Steam 登录", isPresented: Binding(
            get: { workshopViewModel.logoutResultMessage != nil },
            set: { if !$0 { workshopViewModel.logoutResultMessage = nil } }
        )) {
            Button("确定", role: .cancel) { workshopViewModel.logoutResultMessage = nil }
        } message: {
            Text(workshopViewModel.logoutResultMessage ?? "")
        }
    }

    // MARK: - Steam Account Section (#2)

    @ViewBuilder
    var steamAccountSection: some View {
        switch workshopViewModel.steamSetupState {
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

                if workshopViewModel.isLoggingOut {
                    HStack(spacing: 5) {
                        ProgressView()
                            .controlSize(.small)
                        Text("正在退出…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Button {
                        workshopViewModel.logout()
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
                Text(workshopViewModel.steamCheckingMessage)
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
        switch workshopViewModel.steamSetupState {
        case .checking:
            HStack(spacing: 12) {
                ProgressView()
                    .controlSize(.regular)
                VStack(alignment: .leading, spacing: 2) {
                    Text(workshopViewModel.steamCheckingMessage)
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
                    Text(workshopViewModel.steamSetupState == .serviceUnavailable ? "Steam 服务不可用" : "连接 Steam 以下载壁纸")
                        .font(.callout)
                        .bold()
                    Text(workshopViewModel.steamSetupState == .serviceUnavailable
                         ? "检查内置 Steam 服务后重试。"
                         : "登录 Steam 后可直接从创意工坊下载壁纸到本地（需拥有 Wallpaper Engine）")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    AppDelegate.shared.openSteamSetup()
                } label: {
                    Text(workshopViewModel.steamSetupState == .serviceUnavailable ? "检查服务" : "立即设置")
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
