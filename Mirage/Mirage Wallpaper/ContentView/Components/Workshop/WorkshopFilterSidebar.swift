//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopFilterSidebar: View {
    @Bindable var workshopViewModel: WorkshopViewModel

    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 30) {
                    Button {
                        workshopViewModel.clearFilters()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)

                    ShowOnlyFilterSection(
                        id: "workshop.showOnly",
                        selection: workshopViewModel.workshopShowOnly,
                        onChange: workshopViewModel.setWorkshopShowOnly
                    )

                    FilterSection("类型", id: "workshop.type", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopTypeFilter.allCases) { filter in
                                Toggle(filter.label, isOn: Binding(
                                    get: { workshopViewModel.selectedTypeFilters.contains(filter) },
                                    set: { workshopViewModel.setWorkshopTypeFilter(filter, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }
                        }
                    }

                    FilterSection("分级", id: "workshop.ageRating", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopAgeRating.allCases) { rating in
                                Toggle(rating.displayName, isOn: Binding(
                                    get: { workshopViewModel.ageRatingFilter.contains(rating) },
                                    set: { workshopViewModel.applyAgeRatingFilter(rating, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }

                            if workshopViewModel.ageRatingFilter.isEmpty {
                                Text("未选择任何分级，当前显示全部分级")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }

                    FilterSection("分辨率", id: "workshop.resolution", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 16) {
                            HStack {
                                Button("全选") { workshopViewModel.selectAllResolutions() }
                                    .disabled(workshopViewModel.allResolutionsSelected)
                                Button("清空") { workshopViewModel.clearResolutions() }
                                    .disabled(workshopViewModel.allResolutionsCleared)
                            }
                            .buttonStyle(.link)
                            WorkshopResolutionFilterGroup(
                                "其他", selection: $workshopViewModel.miscResolution,
                                options: FRMiscResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.miscResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "宽屏", selection: $workshopViewModel.widescreenResolution,
                                options: FRWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.widescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "超宽屏", selection: $workshopViewModel.ultraWidescreenResolution,
                                options: FRUltraWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.ultraWidescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "双显示器", selection: $workshopViewModel.dualscreenResolution,
                                options: FRDualscreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.dualscreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "三显示器", selection: $workshopViewModel.triplescreenResolution,
                                options: FRTriplescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.triplescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "纵向监视器/手机", selection: $workshopViewModel.portraitResolution,
                                options: FRPortraitScreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setResolutionOption(\.portraitResolution, option: option, isOn: isOn)
                                })
                        }
                    }

                    FilterSection("标签", id: "workshop.tags", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Button("全选") {
                                    workshopViewModel.selectedTags = Set(WorkshopTag.allCases.map { $0.rawValue })
                                    workshopViewModel.currentPage = 1
                                    workshopViewModel.search()
                                }
                                Button("清空") {
                                    workshopViewModel.selectedTags.removeAll()
                                    workshopViewModel.currentPage = 1
                                    workshopViewModel.search()
                                }
                            }
                            .buttonStyle(.link)

                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(WorkshopTag.allCases) { tag in
                                    Toggle(tag.displayName, isOn: Binding(
                                        get: { workshopViewModel.selectedTags.contains(tag.rawValue) },
                                        set: { _ in workshopViewModel.applyTagFilter(tag.rawValue) }
                                    ))
                                    .toggleStyle(.checkbox)
                                }
                            }
                        }
                    }
                }
                .padding(.trailing)
            }
            Divider()
        }
    }
}


private struct WorkshopResolutionFilterGroup<Filter>: View where Filter: FilterResultsModel {
    let title: LocalizedStringKey
    @Binding var selection: Filter
    let options: [String]
    let onChange: (Filter, Bool) -> Void

    init(
        _ title: LocalizedStringKey,
        selection: Binding<Filter>,
        options: [String],
        onChange: @escaping (Filter, Bool) -> Void
    ) {
        self.title = title
        _selection = selection
        self.options = options
        self.onChange = onChange
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            ForEach(options.indices, id: \.self) { index in
                let option = Filter.option(at: index)
                Toggle(LocalizedStringKey(options[index]), isOn: Binding(
                    get: { selection.contains(option) },
                    set: { onChange(option, $0) }
                ))
                .toggleStyle(.checkbox)
            }
        }
    }
}
