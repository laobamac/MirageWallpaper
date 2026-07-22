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

    @State private var hoveredId: String?
    @State private var isDownloadPopoverPresented = false
    @State private var showAPIKeyReminder = false

    var body: some View {
        VStack(spacing: 8) {
            if !globalSettingsViewModel.settings.hasValidCustomSteamAPIKey {
                SteamAPIKeyReminderBanner()
            }

            HStack {
                WorkshopSearchBar(workshopViewModel: workshopViewModel)

                Spacer()

                Button {
                    isDownloadPopoverPresented.toggle()
                } label: {
                    ZStack(alignment: .topTrailing) {
                        Image(systemName: "arrow.down.circle")
                            .font(.title3)
                        if workshopViewModel.activeDownloadCount > 0 {
                            Text("\(workshopViewModel.activeDownloadCount)")
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
                    DownloadPopover(workshopViewModel: workshopViewModel)
                }

                steamAccountSection
            }

            if workshopViewModel.steamSetupState != .ready {
                steamSetupBanner
            }

            if workshopViewModel.isLoading && workshopViewModel.items.isEmpty {
                VStack(spacing: 16) {
                    ProgressView()
                        .scaleEffect(1.5)
                    Text("正在搜索创意工坊...")
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if workshopViewModel.items.isEmpty && !workshopViewModel.isLoading {
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
            } else {
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 240, maximum: 340), spacing: 14)],
                              alignment: .leading, spacing: 14) {
                        ForEach(workshopViewModel.items) { item in
                            // O(1) lookups against the cached installed-state set;
                            // no filesystem access during card rendering.
                            WorkshopItemCard(
                                item: item,
                                isHovered: hoveredId == item.id,
                                isDownloaded: workshopViewModel.isInstalled(item.publishedFileId),
                                presetNeedsDependency: workshopViewModel.presetNeedsDependency(item.publishedFileId),
                                downloadState: workshopViewModel.downloadState(for: item.publishedFileId)
                            )
                            .overlay {
                                if workshopViewModel.selectedItem?.id == item.id {
                                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                                        .stroke(Color.accentColor, lineWidth: 2)
                                }
                            }
                            .onHover { hovered in
                                hoveredId = hovered ? item.id : nil
                            }
                            .onTapGesture {
                                workshopViewModel.selectWorkshopItem(item)
                            }
                        }
                    }
                    .padding(.vertical, 4)

                    if workshopViewModel.isLoading {
                        ProgressView()
                            .padding()
                    }

                    pageControls
                        .padding(.vertical, 8)
                }
            }
        }
        .onAppear {
            presentAPIKeyReminderIfNeeded()
            workshopViewModel.checkSteamSetup()
            workshopViewModel.refreshInstalledState()
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
        .onChange(of: viewModel.topTabBarSelection) { _, _ in
            if viewModel.topTabBarSelection == 2 {
                workshopViewModel.checkSteamSetup()
            }
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
        if workshopViewModel.steamSetupState == .ready {
            HStack(spacing: 8) {
                HStack(spacing: 4) {
                    Image(systemName: "person.crop.circle.fill")
                        .foregroundStyle(.green)
                        .font(.caption)
                    Text(SteamCMDManager.shared.savedUsername)
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
        } else {
            Button {
                AppDelegate.shared.openSteamSetup()
            } label: {
                Label("设置 Steam", systemImage: "gear")
                    .font(.caption)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    var pageControls: some View {
        HStack(spacing: 8) {
            Spacer()
            Button {
                workshopViewModel.loadPreviousPage()
            } label: {
                Image(systemName: "chevron.left")
            }
            .disabled(workshopViewModel.currentPage <= 1)

            Text("\(workshopViewModel.currentPage) / \(workshopViewModel.totalPages)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(minWidth: 60)

            Button {
                workshopViewModel.loadNextPage()
            } label: {
                Image(systemName: "chevron.right")
            }
            .disabled(workshopViewModel.currentPage >= workshopViewModel.totalPages)
            Spacer()
        }
        .buttonStyle(.bordered)
    }

    var steamSetupBanner: some View {
        HStack(spacing: 12) {
            Image(systemName: "cloud.fill")
                .font(.title2)
                .foregroundStyle(.blue)
            VStack(alignment: .leading, spacing: 2) {
                Text("连接 Steam 以下载壁纸")
                    .font(.callout)
                    .bold()
                Text("设置 SteamCMD 后可直接从创意工坊下载壁纸到本地（需拥有 Wallpaper Engine）")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                AppDelegate.shared.openSteamSetup()
            } label: {
                Text("立即设置")
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
                Text("内置 Key 由所有用户共享，可能因请求过多影响浏览；专属 Key 不影响 SteamCMD 登录和下载。")
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
