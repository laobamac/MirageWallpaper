//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopSearchBar: View {
    @Bindable var browseStore: WorkshopBrowseStore

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                TextField("搜索作品、作者或作品 ID...", text: $browseStore.searchText)
                    .textFieldStyle(.plain)
                    .onSubmit {
                        browseStore.submitSearch()
                    }
                if !browseStore.searchText.isEmpty {
                    Button {
                        browseStore.clearSearch()
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(6)
            .background(Color(nsColor: NSColor.controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
            )

            HStack(spacing: 6) {
                Text("排序")
                Menu {
                    sortOption(.topRated)
                    sortOption(.trending, period: .year)
                    sortOption(.trending, period: .month)
                    sortOption(.trending, period: .week)
                    sortOption(.trending, period: .day)
                    sortOption(.lastUpdated)
                    sortOption(.recentlyReleased)
                    sortOption(.mostSubscribed)
                    if !browseStore.searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                        Divider()
                        sortOption(.textRelevance)
                    }
                } label: {
                    Text(browseStore.sortOrder.workshopLabel(
                        period: browseStore.trendPeriod
                    ))
                    .lineLimit(1)
                }
                .fixedSize()
            }
        }
    }

    private func sortOption(
        _ order: WorkshopSortOrder,
        period: WorkshopTrendPeriod? = nil
    ) -> some View {
        let activePeriod = period ?? browseStore.trendPeriod
        let isSelected = browseStore.sortOrder == order
            && (period == nil || browseStore.trendPeriod == period)
        return Button {
            browseStore.selectSort(order, period: period)
        } label: {
            HStack {
                Text(order.workshopLabel(period: activePeriod))
                if isSelected {
                    Image(systemName: "checkmark")
                }
            }
        }
    }
}
