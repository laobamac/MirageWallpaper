//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

// Left filter sidebar for the 小组件 tab: filter installed rmskin themes by
// LoadType and Version (facets derived from the installed set).
struct WidgetFilterSidebar: View {
    @ObservedObject var viewModel: RmskinViewModel

    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 24) {
                    Button {
                        viewModel.resetFilters()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)

                    FilterSection("加载类型", alignment: .leading) {
                        if viewModel.availableLoadTypes.isEmpty {
                            Text("暂无").foregroundStyle(.secondary)
                        }
                        ForEach(viewModel.availableLoadTypes, id: \.self) { loadType in
                            Toggle(loadType, isOn: Binding<Bool>(
                                get: { viewModel.selectedLoadTypes.contains(loadType) },
                                set: { on in
                                    if on { viewModel.selectedLoadTypes.insert(loadType) }
                                    else { viewModel.selectedLoadTypes.remove(loadType) }
                                }))
                            .toggleStyle(.checkbox)
                        }
                    }

                    FilterSection("版本", alignment: .leading) {
                        if viewModel.availableVersions.isEmpty {
                            Text("暂无").foregroundStyle(.secondary)
                        }
                        ForEach(viewModel.availableVersions, id: \.self) { version in
                            Toggle(version, isOn: Binding<Bool>(
                                get: { viewModel.selectedVersions.contains(version) },
                                set: { on in
                                    if on { viewModel.selectedVersions.insert(version) }
                                    else { viewModel.selectedVersions.remove(version) }
                                }))
                            .toggleStyle(.checkbox)
                        }
                    }
                }
                .padding(.trailing)
            }
            .lineLimit(1)
        }
        Divider()
    }
}
