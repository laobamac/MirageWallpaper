//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DiscoverView: View {
    @EnvironmentObject private var globalSettingsViewModel: GlobalSettingsViewModel
    @ObservedObject var workshopViewModel: WorkshopViewModel
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel

    @State private var showAPIKeyReminder = false

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Text("趋势范围")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Picker("趋势范围", selection: $workshopViewModel.discoverTrendPeriod) {
                    ForEach(WorkshopTrendPeriod.allCases) { period in
                        Text(period.label).tag(period)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(width: 110)
                .onChange(of: workshopViewModel.discoverTrendPeriod) { _, _ in
                    workshopViewModel.loadDiscover(force: true)
                }

                Spacer()

                Button {
                    workshopViewModel.refreshDiscover()
                } label: {
                    Group {
                        if workshopViewModel.isDiscoverLoading {
                            ProgressView()
                                .controlSize(.small)
                        } else {
                            Image(systemName: "arrow.triangle.2.circlepath")
                        }
                    }
                    .frame(width: 16, height: 16)
                }
                .disabled(workshopViewModel.isDiscoverLoading)
                .help("刷新发现")
                WallpaperGridViewMenu(viewModel: viewModel)
            }
            .padding(.horizontal)

            ScrollView {
                if !globalSettingsViewModel.settings.hasValidCustomSteamAPIKey {
                    SteamAPIKeyReminderBanner()
                        .padding(.horizontal)
                }

                if workshopViewModel.isDiscoverLoading && workshopViewModel.discoverItems.isEmpty {
                    VStack(spacing: 20) {
                        ProgressView()
                            .scaleEffect(1.5)
                        Text("正在加载推荐内容...")
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding(.top, 100)
                } else {
                    VStack(spacing: 24) {
                        ForEach(WorkshopDiscoverCategory.allCases) { category in
                            if let items = workshopViewModel.discoverItems[category], !items.isEmpty {
                                DiscoverSectionView(
                                    title: category.title(period: workshopViewModel.discoverTrendPeriod),
                                    icon: category.icon,
                                    iconColor: category.iconColor,
                                    items: items,
                                    workshopViewModel: workshopViewModel,
                                    contentViewModel: viewModel,
                                    wallpaperViewModel: wallpaperViewModel,
                                    onSeeAll: { showAll(category) }
                                )
                            }
                        }

                        Spacer(minLength: 20)
                    }
                    .padding(.horizontal)
                }
            }
        }
        .contextMenu {
            WallpaperGridViewMenu(viewModel: viewModel)
        }
        .onAppear {
            if !globalSettingsViewModel.settings.hasValidCustomSteamAPIKey {
                showAPIKeyReminder = SteamAPIKeyReminderPolicy.shouldPresent()
            }
            if workshopViewModel.discoverItems.isEmpty {
                workshopViewModel.loadDiscover()
            }
            workshopViewModel.refreshInstalledState()
        }
        .alert("建议设置专属 Steam API Key", isPresented: $showAPIKeyReminder) {
            Button("立即设置") { AppDelegate.shared.openSteamAPIKeySettings() }
            Button("暂时使用内置 Key", role: .cancel) { }
        } message: {
            Text("内置 Key 由所有 Mirage 用户共享，繁忙时可能导致创意工坊无法加载。设置您自己的免费 API Key 后将不再提醒。此 Key 只影响浏览，不影响登录和下载。")
        }
    }

    private func showAll(_ category: WorkshopDiscoverCategory) {
        if let tag = category.tag {
            workshopViewModel.navigateToWorkshopWithTag(
                tag,
                trendPeriod: workshopViewModel.discoverTrendPeriod
            )
        } else if let sortOrder = category.sortOrder {
            workshopViewModel.navigateToWorkshopWithSort(
                sortOrder,
                trendPeriod: workshopViewModel.discoverTrendPeriod
            )
        }
        viewModel.topTabBarSelection = 2
    }
}

private extension WorkshopDiscoverCategory {
    func title(period: WorkshopTrendPeriod) -> String {
        switch self {
        case .trending: return L("%@最热门", period.label)
        case .mostSubscribed: return L("订阅最多")
        case .topRated: return L("评分最高")
        case .lastUpdated: return L("最近更新")
        case .playtimeTrend: return L("%@播放时长最多", period.label)
        case .averagePlaytimeTrend: return L("%@平均播放时长最长", period.label)
        case .sessionsTrend: return L("%@播放次数最多", period.label)
        case .totalPlaytime: return L("总播放时长最多")
        case .lifetimeAveragePlaytime: return L("终身平均播放时长")
        case .lifetimeSessions: return L("总播放次数最多")
        case .anime: return L("动漫精选")
        case .nature: return L("自然风光")
        case .abstract: return L("抽象艺术")
        case .landscape: return L("风景壁纸")
        }
    }

    var icon: String {
        switch self {
        case .trending: return "flame.fill"
        case .mostSubscribed: return "arrow.down.circle.fill"
        case .topRated: return "star.fill"
        case .lastUpdated: return "clock.arrow.circlepath"
        case .playtimeTrend, .totalPlaytime: return "hourglass"
        case .averagePlaytimeTrend, .lifetimeAveragePlaytime: return "timer"
        case .sessionsTrend, .lifetimeSessions: return "repeat.circle.fill"
        case .anime: return "sparkles"
        case .nature: return "leaf.fill"
        case .abstract: return "circle.hexagongrid.fill"
        case .landscape: return "mountain.2.fill"
        }
    }

    var iconColor: Color {
        switch self {
        case .trending: return .orange
        case .mostSubscribed: return .green
        case .topRated: return .yellow
        case .lastUpdated: return .indigo
        case .playtimeTrend: return .mint
        case .averagePlaytimeTrend: return .cyan
        case .sessionsTrend: return .teal
        case .totalPlaytime: return .purple
        case .lifetimeAveragePlaytime: return .blue
        case .lifetimeSessions: return .green
        case .anime: return .purple
        case .nature: return .green
        case .abstract: return .cyan
        case .landscape: return .teal
        }
    }
}
