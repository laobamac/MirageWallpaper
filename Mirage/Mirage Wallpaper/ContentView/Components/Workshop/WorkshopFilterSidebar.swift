//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WorkshopFilterSidebar: View {
    @Bindable var browseStore: WorkshopBrowseStore

    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 30) {
                    Button {
                        browseStore.clearFilters()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)

                    ShowOnlyFilterSection(
                        id: "workshop.showOnly",
                        selection: browseStore.showOnly,
                        onChange: browseStore.setShowOnly
                    )

                    FilterSection("类型", id: "workshop.type", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopTypeFilter.allCases) { filter in
                                Toggle(filter.label, isOn: Binding(
                                    get: { browseStore.selectedTypeFilters.contains(filter) },
                                    set: { browseStore.setTypeFilter(filter, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }
                        }
                    }

                    FilterSection("分级", id: "workshop.ageRating", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopAgeRating.allCases) { rating in
                                Toggle(rating.displayName, isOn: Binding(
                                    get: { browseStore.ageRatingFilter.contains(rating) },
                                    set: { browseStore.applyAgeRatingFilter(rating, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }

                            if browseStore.ageRatingFilter.isEmpty {
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
                                Button("全选") { browseStore.selectAllResolutions() }
                                    .disabled(browseStore.allResolutionsSelected)
                                Button("清空") { browseStore.clearResolutions() }
                                    .disabled(browseStore.allResolutionsCleared)
                            }
                            .buttonStyle(.link)
                            WorkshopResolutionFilterGroup(
                                "其他", selection: $browseStore.miscResolution,
                                options: FRMiscResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.miscResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "宽屏", selection: $browseStore.widescreenResolution,
                                options: FRWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.widescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "超宽屏", selection: $browseStore.ultraWidescreenResolution,
                                options: FRUltraWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.ultraWidescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "双显示器", selection: $browseStore.dualscreenResolution,
                                options: FRDualscreenResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.dualscreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "三显示器", selection: $browseStore.triplescreenResolution,
                                options: FRTriplescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.triplescreenResolution, option: option, isOn: isOn)
                                })
                            WorkshopResolutionFilterGroup(
                                "纵向监视器/手机", selection: $browseStore.portraitResolution,
                                options: FRPortraitScreenResolution.allOptions,
                                onChange: { option, isOn in
                                    browseStore.setResolutionOption(\.portraitResolution, option: option, isOn: isOn)
                                })
                        }
                    }

                    FilterSection("标签", id: "workshop.tags", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Button("全选") {
                                    browseStore.selectAllTags()
                                }
                                Button("清空") {
                                    browseStore.clearTags()
                                }
                            }
                            .buttonStyle(.link)

                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(WorkshopTag.allCases) { tag in
                                    Toggle(tag.displayName, isOn: Binding(
                                        get: { browseStore.selectedTags.contains(tag.rawValue) },
                                        set: { _ in browseStore.applyTagFilter(tag.rawValue) }
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
