//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopSearchBar: View {
    @ObservedObject var workshopViewModel: WorkshopViewModel

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                TextField("搜索作品、作者或作品 ID...", text: $workshopViewModel.searchText)
                    .textFieldStyle(.plain)
                    .onSubmit {
                        workshopViewModel.submitSearch()
                    }
                if !workshopViewModel.searchText.isEmpty {
                    Button {
                        workshopViewModel.searchText = ""
                        workshopViewModel.currentPage = 1
                        workshopViewModel.search()
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
                    sortOption(.trending, period: .week)
                    sortOption(.trending, period: .day)
                    sortOption(.mostSubscribed)
                } label: {
                    Text(workshopViewModel.isTextRelevanceSearch
                        ? WorkshopSortOrder.textRelevance.label
                        : workshopViewModel.sortOrder.workshopLabel(
                            period: workshopViewModel.trendPeriod
                        ))
                    .lineLimit(1)
                }
                .fixedSize()
                .disabled(workshopViewModel.isTextRelevanceSearch)
            }
        }
    }

    private func sortOption(
        _ order: WorkshopSortOrder,
        period: WorkshopTrendPeriod? = nil
    ) -> some View {
        let activePeriod = period ?? workshopViewModel.trendPeriod
        let isSelected = workshopViewModel.sortOrder == order
            && (period == nil || workshopViewModel.trendPeriod == period)
        return Button {
            workshopViewModel.selectWorkshopSort(order, period: period)
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
