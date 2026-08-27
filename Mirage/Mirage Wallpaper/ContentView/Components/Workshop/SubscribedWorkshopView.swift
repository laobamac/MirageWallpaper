//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct SubscribedWorkshopView: View {
    @EnvironmentObject private var globalSettingsViewModel: GlobalSettingsViewModel
    @Bindable var subscriptionStore: SubscriptionStore
    let creatorStore: WorkshopCreatorStore
    let downloadStore: WorkshopDownloadStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let sessionStore: SteamSessionStore
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject private var steamService = SteamServiceManager.shared
    let isActive: Bool

    @State private var hoveredItemID: String?

    var body: some View {
        VStack(spacing: 8) {
            toolbar

            if let error = subscriptionStore.error,
               !subscriptionStore.items.isEmpty {
                errorBanner(error)
            }

            content
        }
        .onAppear {
            sessionStore.checkSetup()
            if steamService.isLoggedIn &&
                subscriptionStore.catalogItems.isEmpty &&
                !subscriptionStore.isLoading {
                subscriptionStore.refresh(startIndex: 0)
            }
        }
        .onChange(of: steamService.isLoggedIn) { _, isLoggedIn in
            if isLoggedIn {
                subscriptionStore.refresh(startIndex: 0)
            }
        }
        .onChange(of: viewModel.wallpapersPerPage) { _, _ in
            subscriptionStore.pageSizeDidChange()
        }
        .alert(
            "下载全部已订阅壁纸",
            isPresented: Binding(
                get: { subscriptionStore.downloadPlan != nil },
                set: { if !$0 { subscriptionStore.dismissDownloadPlan() } }
            ),
            presenting: subscriptionStore.downloadPlan
        ) { plan in
            if plan.downloadCount > 0 {
                Button("开始下载") {
                    subscriptionStore.confirmDownloads()
                }
                Button("取消", role: .cancel) {
                    subscriptionStore.dismissDownloadPlan()
                }
            } else {
                Button("好", role: .cancel) {
                    subscriptionStore.dismissDownloadPlan()
                }
            }
        } message: { plan in
            if plan.downloadCount > 0 {
                Text(L(
                    "已订阅 %d 个壁纸，还剩 %d 个未下载，本次将下载 %d 个。",
                    plan.subscriptionCount,
                    plan.remainingCount,
                    plan.downloadCount
                ))
            } else if plan.remainingCount > 0 {
                Text(L(
                    "已订阅 %d 个壁纸，还剩 %d 个正在下载，本次不需要新增下载。",
                    plan.subscriptionCount,
                    plan.remainingCount
                ))
            } else {
                Text(L("已订阅 %d 个壁纸，已全部下载，不需要下载。", plan.subscriptionCount))
            }
        }
    }

    private var toolbar: some View {
        ViewThatFits(in: .horizontal) {
            fullToolbar
            compactToolbar
        }
    }

    private var fullToolbar: some View {
        HStack(spacing: 10) {
            toolbarLeading
            subscriptionSearchField(maxWidth: 260)

            Spacer(minLength: 8)

            downloadAllButton
            refreshButton
            WallpaperGridViewMenu(viewModel: viewModel, showsPageSize: true)
            accountStatus
        }
    }

    private var compactToolbar: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                toolbarLeading

                Spacer(minLength: 8)

                accountStatus
            }

            HStack(spacing: 8) {
                subscriptionSearchField(maxWidth: .infinity)
                refreshButton
                WallpaperGridViewMenu(viewModel: viewModel, showsPageSize: true)
                downloadAllButton
            }
        }
    }

    private var toolbarLeading: some View {
        HStack(spacing: 10) {
            Button {
                viewModel.isFilterReveal.toggle()
            } label: {
                Label("筛选", systemImage: "checklist.checked")
            }
            .buttonStyle(.borderedProminent)

            Label("已订阅", systemImage: "checkmark.circle.fill")
                .font(.headline)

            if !subscriptionStore.catalogItems.isEmpty {
                Text(L("共 %d 项", subscriptionStore.total))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func subscriptionSearchField(maxWidth: CGFloat) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            TextField("搜索已订阅壁纸...", text: $subscriptionStore.searchText)
                .textFieldStyle(.plain)
                .onSubmit {
                    subscriptionStore.refreshFilters()
                }
            if !subscriptionStore.searchText.isEmpty {
                Button {
                    subscriptionStore.searchText = ""
                    subscriptionStore.refreshFilters()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }
        }
        .padding(6)
        .frame(minWidth: 150, idealWidth: 220, maxWidth: maxWidth)
        .background(Color(nsColor: NSColor.controlBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
        )
    }

    private var downloadAllButton: some View {
        Button {
            subscriptionStore.prepareDownloadAll()
        } label: {
            if subscriptionStore.isPreparingDownloads {
                HStack(spacing: 6) {
                    ProgressView()
                        .controlSize(.small)
                    Text("正在准备下载…")
                }
            } else {
                Label("下载全部", systemImage: "arrow.down.circle.fill")
            }
        }
        .buttonStyle(.borderedProminent)
        .disabled(
            !steamService.isLoggedIn ||
            subscriptionStore.isPreparingDownloads ||
            subscriptionStore.catalogItems.isEmpty
        )
    }

    private var refreshButton: some View {
        Button {
            subscriptionStore.refresh()
        } label: {
            Image(systemName: "arrow.triangle.2.circlepath")
                .frame(width: 16, height: 16)
        }
        .disabled(!steamService.isLoggedIn || subscriptionStore.isLoading)
        .help(L("刷新已订阅壁纸"))
    }

    @ViewBuilder
    private var accountStatus: some View {
        if sessionStore.setupState == .checking {
            HStack(spacing: 6) {
                ProgressView()
                    .controlSize(.small)
                Text(sessionStore.checkingMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .frame(maxWidth: 120)
            }
        } else if steamService.isLoggedIn {
            Label(steamService.accountName, systemImage: "person.crop.circle.fill")
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .frame(maxWidth: 120)
        } else {
            Button {
                AppDelegate.shared.openSteamSetup()
            } label: {
                Label("登录 Steam", systemImage: "person.crop.circle.badge.exclamationmark")
            }
            .buttonStyle(.borderedProminent)
        }
    }

    @ViewBuilder
    private var content: some View {
        if sessionStore.setupState == .checking {
            centered {
                ProgressView()
                    .scaleEffect(1.3)
                Text(sessionStore.checkingMessage)
                    .font(.title3)
                Text("正在确认 Steam 登录状态，请稍候。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } else if !steamService.isLoggedIn {
            centered {
                Image(systemName: "person.crop.circle.badge.exclamationmark")
                    .font(.system(size: 40))
                    .foregroundStyle(.secondary)
                Text("登录 Steam 后即可查看已订阅壁纸")
                    .font(.title3)
                Button("登录 Steam") {
                    AppDelegate.shared.openSteamSetup()
                }
                .buttonStyle(.borderedProminent)
            }
        } else if subscriptionStore.isLoading && subscriptionStore.items.isEmpty {
            centered {
                ProgressView()
                    .scaleEffect(1.3)
                Text("正在获取已订阅壁纸…")
                    .foregroundStyle(.secondary)
            }
        } else if let error = subscriptionStore.error,
                  subscriptionStore.items.isEmpty {
            centered {
                Image(systemName: "exclamationmark.triangle")
                    .font(.system(size: 36))
                    .foregroundStyle(.orange)
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Button("重试") {
                    subscriptionStore.refresh()
                }
                .buttonStyle(.borderedProminent)
            }
        } else if subscriptionStore.catalogItems.isEmpty {
            centered {
                Image(systemName: "rectangle.stack.badge.minus")
                    .font(.system(size: 40))
                    .foregroundStyle(.tertiary)
                Text("尚未订阅任何壁纸")
                    .font(.title3)
                Text("在创意工坊中订阅后，壁纸会自动下载并出现在这里。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        } else if subscriptionStore.items.isEmpty {
            centered {
                Image(systemName: "line.3.horizontal.decrease.circle")
                    .font(.system(size: 40))
                    .foregroundStyle(.tertiary)
                Text("没有符合筛选条件的已订阅壁纸")
                    .font(.title3)
                Text("请调整搜索关键词或筛选条件。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Button("重置筛选") {
                    subscriptionStore.clearFilters()
                }
                .buttonStyle(.borderedProminent)
            }
        } else {
            subscriptionGrid
        }
    }

    private var subscriptionGrid: some View {
        ScrollViewReader { proxy in
            ZStack(alignment: .bottom) {
                ScrollView {
                    Color.clear
                        .frame(height: 0)
                        .id("subscriptionsTop")

                    LazyVGrid(
                        columns: [GridItem(.adaptive(
                            minimum: viewModel.explorerIconSize,
                            maximum: viewModel.explorerIconSize * 2
                        ), spacing: 14)],
                        alignment: .leading,
                        spacing: 14
                    ) {
                        ForEach(subscriptionStore.items) { item in
                            WorkshopItemCard(
                                item: item,
                                isHovered: hoveredItemID == item.id,
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
                            .onHover { hovering in
                                hoveredItemID = hovering ? item.id : nil
                            }
                            .onTapGesture {
                                selectionCoordinator.selectWorkshopItem(item)
                            }
                            .contextMenu {
                                WorkshopCardContextMenu(
                                    item: item,
                                    creatorStore: creatorStore,
                                    downloadStore: downloadStore,
                                    interactionStore: interactionStore,
                                    selectionCoordinator: selectionCoordinator,
                                    subscriptionStore: subscriptionStore
                                )
                                WallpaperGridViewMenu(viewModel: viewModel, showsPageSize: true)
                            }
                        }
                    }
                    .padding(.vertical, 2)
                    #if arch(arm64)
                    .padding(.trailing)
                    #endif

                    if subscriptionStore.pageCount > 1 {
                        Color.clear.frame(height: 58)
                    }
                }

                if subscriptionStore.pageCount > 1 {
                    PageNavigator(
                        currentPage: subscriptionStore.currentPage,
                        pageCount: subscriptionStore.pageCount,
                        onSelect: subscriptionStore.goToPage
                    )
                    .disabled(subscriptionStore.isLoading)
                    .padding(.bottom, 12)
                }
            }
            .onChange(of: subscriptionStore.startIndex) { _, _ in
                withAnimation(.easeOut(duration: 0.2)) {
                    proxy.scrollTo("subscriptionsTop", anchor: .top)
                }
            }
        }
    }

    private func centered<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        VStack(spacing: 12) {
            content()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(24)
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text(message)
                .font(.caption)
                .lineLimit(2)
            Spacer()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
        .background(Color.orange.opacity(0.1))
    }
}
