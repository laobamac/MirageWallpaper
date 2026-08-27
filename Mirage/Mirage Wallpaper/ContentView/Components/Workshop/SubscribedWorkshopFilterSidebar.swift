//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct SubscribedWorkshopFilterSidebar: View {
    @Bindable var subscriptionStore: SubscriptionStore

    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 30) {
                    Button {
                        subscriptionStore.clearFilters()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!subscriptionStore.hasActiveFilters)

                    ShowOnlyFilterSection(
                        id: "subscriptions.showOnly",
                        selection: subscriptionStore.showOnly,
                        onChange: subscriptionStore.setShowOnly
                    )

                    FilterSection("类型", id: "subscriptions.type", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopTypeFilter.allCases) { filter in
                                Toggle(filter.label, isOn: Binding(
                                    get: { subscriptionStore.selectedTypeFilters.contains(filter) },
                                    set: { subscriptionStore.setTypeFilter(filter, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }
                        }
                    }

                    FilterSection("分级", id: "subscriptions.ageRating", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopAgeRating.allCases) { rating in
                                Toggle(rating.displayName, isOn: Binding(
                                    get: { subscriptionStore.ageRatingFilter.contains(rating) },
                                    set: {
                                        subscriptionStore.applyAgeRatingFilter(rating, isOn: $0)
                                    }
                                ))
                                .toggleStyle(.checkbox)
                            }

                            if subscriptionStore.ageRatingFilter.isEmpty {
                                Text("未选择任何分级，当前显示全部分级")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }

                    FilterSection("分辨率", id: "subscriptions.resolution", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 16) {
                            HStack {
                                Button("全选") { subscriptionStore.selectAllResolutions() }
                                    .disabled(subscriptionStore.allResolutionsSelected)
                                Button("清空") { subscriptionStore.clearResolutions() }
                                    .disabled(subscriptionStore.allResolutionsCleared)
                            }
                            .buttonStyle(.link)

                            SubscribedResolutionFilterGroup(
                                "其他",
                                selection: $subscriptionStore.miscResolution,
                                options: FRMiscResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.miscResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "宽屏",
                                selection: $subscriptionStore.widescreenResolution,
                                options: FRWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.widescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "超宽屏",
                                selection: $subscriptionStore.ultraWidescreenResolution,
                                options: FRUltraWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.ultraWidescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "双显示器",
                                selection: $subscriptionStore.dualscreenResolution,
                                options: FRDualscreenResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.dualscreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "三显示器",
                                selection: $subscriptionStore.triplescreenResolution,
                                options: FRTriplescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.triplescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "纵向监视器/手机",
                                selection: $subscriptionStore.portraitResolution,
                                options: FRPortraitScreenResolution.allOptions,
                                onChange: { option, isOn in
                                    subscriptionStore.setResolutionOption(
                                        \.portraitResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                        }
                    }

                    FilterSection("标签", id: "subscriptions.tags", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Button("全选") { subscriptionStore.selectAllTags() }
                                    .disabled(subscriptionStore.allTagsSelected)
                                Button("清空") { subscriptionStore.clearTags() }
                                    .disabled(subscriptionStore.selectedTags.isEmpty)
                            }
                            .buttonStyle(.link)

                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(WorkshopTag.allCases) { tag in
                                    Toggle(tag.displayName, isOn: Binding(
                                        get: {
                                            subscriptionStore.selectedTags.contains(tag.rawValue)
                                        },
                                        set: { _ in
                                            subscriptionStore.applyTagFilter(tag.rawValue)
                                        }
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

private struct SubscribedResolutionFilterGroup<Filter>: View where Filter: FilterResultsModel {
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
