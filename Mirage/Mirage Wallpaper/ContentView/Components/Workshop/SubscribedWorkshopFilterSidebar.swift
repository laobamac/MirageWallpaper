//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct SubscribedWorkshopFilterSidebar: View {
    @Bindable var workshopViewModel: WorkshopViewModel

    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 30) {
                    Button {
                        workshopViewModel.clearSubscriptionFilters()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!workshopViewModel.hasActiveSubscriptionFilters)

                    ShowOnlyFilterSection(
                        id: "subscriptions.showOnly",
                        selection: workshopViewModel.subscriptionShowOnly,
                        onChange: workshopViewModel.setSubscriptionShowOnly
                    )

                    FilterSection("类型", id: "subscriptions.type", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopTypeFilter.allCases) { filter in
                                Toggle(filter.label, isOn: Binding(
                                    get: { workshopViewModel.subscriptionSelectedTypeFilters.contains(filter) },
                                    set: { workshopViewModel.setSubscriptionTypeFilter(filter, isOn: $0) }
                                ))
                                .toggleStyle(.checkbox)
                            }
                        }
                    }

                    FilterSection("分级", id: "subscriptions.ageRating", alignment: .leading) {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(WorkshopAgeRating.allCases) { rating in
                                Toggle(rating.displayName, isOn: Binding(
                                    get: { workshopViewModel.subscriptionAgeRatingFilter.contains(rating) },
                                    set: {
                                        workshopViewModel.applySubscriptionAgeRatingFilter(rating, isOn: $0)
                                    }
                                ))
                                .toggleStyle(.checkbox)
                            }

                            if workshopViewModel.subscriptionAgeRatingFilter.isEmpty {
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
                                Button("全选") { workshopViewModel.selectAllSubscriptionResolutions() }
                                    .disabled(workshopViewModel.allSubscriptionResolutionsSelected)
                                Button("清空") { workshopViewModel.clearSubscriptionResolutions() }
                                    .disabled(workshopViewModel.allSubscriptionResolutionsCleared)
                            }
                            .buttonStyle(.link)

                            SubscribedResolutionFilterGroup(
                                "其他",
                                selection: $workshopViewModel.subscriptionMiscResolution,
                                options: FRMiscResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionMiscResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "宽屏",
                                selection: $workshopViewModel.subscriptionWidescreenResolution,
                                options: FRWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionWidescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "超宽屏",
                                selection: $workshopViewModel.subscriptionUltraWidescreenResolution,
                                options: FRUltraWidescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionUltraWidescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "双显示器",
                                selection: $workshopViewModel.subscriptionDualscreenResolution,
                                options: FRDualscreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionDualscreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "三显示器",
                                selection: $workshopViewModel.subscriptionTriplescreenResolution,
                                options: FRTriplescreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionTriplescreenResolution,
                                        option: option,
                                        isOn: isOn
                                    )
                                }
                            )
                            SubscribedResolutionFilterGroup(
                                "纵向监视器/手机",
                                selection: $workshopViewModel.subscriptionPortraitResolution,
                                options: FRPortraitScreenResolution.allOptions,
                                onChange: { option, isOn in
                                    workshopViewModel.setSubscriptionResolutionOption(
                                        \.subscriptionPortraitResolution,
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
                                Button("全选") { workshopViewModel.selectAllSubscriptionTags() }
                                    .disabled(workshopViewModel.allSubscriptionTagsSelected)
                                Button("清空") { workshopViewModel.clearSubscriptionTags() }
                                    .disabled(workshopViewModel.subscriptionSelectedTags.isEmpty)
                            }
                            .buttonStyle(.link)

                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(WorkshopTag.allCases) { tag in
                                    Toggle(tag.displayName, isOn: Binding(
                                        get: {
                                            workshopViewModel.subscriptionSelectedTags.contains(tag.rawValue)
                                        },
                                        set: { _ in
                                            workshopViewModel.applySubscriptionTagFilter(tag.rawValue)
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
