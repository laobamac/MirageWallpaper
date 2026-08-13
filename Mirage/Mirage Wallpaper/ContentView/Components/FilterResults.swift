//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct FilterSection<Content>: View where Content: View {
    private let content: Content
    private let alignment: HorizontalAlignment
    private var spacing: CGFloat?
    private let titleKey: LocalizedStringKey
    
    @AppStorage private var isExpanded: Bool
    
    init(_ titleKey: LocalizedStringKey, id: String, alignment: HorizontalAlignment = .center, spacing: CGFloat? = nil, @ViewBuilder content: () -> Content) {
        self.content = content()
        self.alignment = alignment
        self.spacing = spacing
        self.titleKey = titleKey
        self._isExpanded = AppStorage(wrappedValue: true, "FilterSection.\(id).isExpanded")
    }
    
    var body: some View {
        VStack(alignment: self.alignment, spacing: self.spacing) {
            Button {
                withAnimation {
                    isExpanded.toggle()
                }
            } label: {
                HStack {
                    Image(systemName: "arrowtriangle.down.fill")
                        .font(.caption)
                        .rotationEffect(isExpanded ? .zero : .degrees(-90.0))
                        .animation(.spring(), value: isExpanded)
                    Text(self.titleKey)
                    Spacer()
                }
            }
            .buttonStyle(.plain)
            if isExpanded {
                content.padding(.leading, (self.alignment == .leading) ? 10 : 0)
            }
        }
    }
}

private struct ResolutionFilterGroup<Filter>: View where Filter: FilterResultsModel {
    private let titleKey: LocalizedStringKey
    private let options: [String]
    @Binding private var selection: Filter

    init(_ titleKey: LocalizedStringKey, selection: Binding<Filter>, options: [String]) {
        self.titleKey = titleKey
        self._selection = selection
        self.options = options
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(titleKey)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            ForEach(options.indices, id: \.self) { index in
                let option = Filter.option(at: index)
                Toggle(LocalizedStringKey(options[index]), isOn: Binding(
                    get: { selection.contains(option) },
                    set: { isSelected in
                        if isSelected {
                            selection.insert(option)
                        } else {
                            selection.remove(option)
                        }
                    }
                ))
                .toggleStyle(.checkbox)
            }
        }
    }
}

struct FilterResults: View {
    @ObservedObject var viewModel: FilterResultsViewModel
    
    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 30) {
                    Button {
                        viewModel.reset()
                    } label: {
                        Label("重置筛选", systemImage: "arrow.triangle.2.circlepath")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 5)
                    }
                    .buttonStyle(.borderedProminent)
                    ShowOnlyFilterSection(
                        id: "library.showOnly",
                        selection: viewModel.showOnly
                    ) { option, isOn in
                        if isOn {
                            viewModel.showOnly.insert(option)
                        } else {
                            viewModel.showOnly.remove(option)
                        }
                    }

                    VStack(spacing: 15) {
                        FilterSection("类型", id: "library.type", alignment: .leading) {
                            ForEach(Array(zip(FRType.allOptions.indices, FRType.allOptions)), id: \.0) { (i, option) in
                                Toggle(LocalizedStringKey(option), isOn: Binding<Bool>(get: {
                                    viewModel.type.contains(FRType(rawValue: 1 << i))
                                }, set: {
                                    if $0 {
                                        viewModel.type.insert(FRType(rawValue: 1 << i))
                                    } else {
                                        viewModel.type.remove(FRType(rawValue: 1 << i))
                                    }
                                    print(String(describing: viewModel.type))
                                }))
                            }
                        }
                        FilterSection("分级", id: "library.ageRating", alignment: .leading) {
                            ForEach(Array(zip(FRAgeRating.allOptions.indices, FRAgeRating.allOptions)), id: \.0) { (i, option) in
                                Toggle(LocalizedStringKey(option), isOn: Binding<Bool>(get: {
                                    viewModel.ageRating.contains(FRAgeRating(rawValue: 1 << i))
                                }, set: {
                                    if $0 {
                                        viewModel.ageRating.insert(FRAgeRating(rawValue: 1 << i))
                                    } else {
                                        viewModel.ageRating.remove(FRAgeRating(rawValue: 1 << i))
                                    }
                                    print(String(describing: viewModel.ageRating))
                                }))
                            }
                        }
                        FilterSection("分辨率", id: "library.resolution", alignment: .leading) {
                            VStack(alignment: .leading, spacing: 16) {
                                HStack {
                                    Button("全选") {
                                        viewModel.selectAllResolutions()
                                    }
                                    .disabled(viewModel.areAllResolutionsSelected)
                                    Button("清空") {
                                        viewModel.clearResolutions()
                                    }
                                    .disabled(viewModel.areAllResolutionsCleared)
                                }
                                .buttonStyle(.link)
                                ResolutionFilterGroup("其他", selection: $viewModel.miscResolution, options: FRMiscResolution.allOptions)
                                ResolutionFilterGroup("宽屏", selection: $viewModel.widescreenResolution, options: FRWidescreenResolution.allOptions)
                                ResolutionFilterGroup("超宽屏", selection: $viewModel.ultraWidescreenResolution, options: FRUltraWidescreenResolution.allOptions)
                                ResolutionFilterGroup("双显示器", selection: $viewModel.dualscreenResolution, options: FRDualscreenResolution.allOptions)
                                ResolutionFilterGroup("三显示器", selection: $viewModel.triplescreenResolution, options: FRTriplescreenResolution.allOptions)
                                ResolutionFilterGroup("纵向监视器/手机", selection: $viewModel.potraitscreenResolution, options: FRPortraitScreenResolution.allOptions)
                            }
                        }
                        FilterSection("来源", id: "library.source", alignment: .leading) {
                            Group {
                                ForEach(Array(zip(FRSource.allOptions.indices, FRSource.allOptions)), id: \.0) { (i, option) in
                                    // 仅工坊 / 我的壁纸 两项有意义
                                    if i == 1 || i == 2 {
                                        Toggle(LocalizedStringKey(option), isOn: Binding<Bool>(get: {
                                            viewModel.source.contains(FRSource(rawValue: 1 << i))
                                        }, set: {
                                            if $0 {
                                                viewModel.source.insert(FRSource(rawValue: 1 << i))
                                            } else {
                                                viewModel.source.remove(FRSource(rawValue: 1 << i))
                                            }
                                        }))
                                    }
                                }
                            }
                            .toggleStyle(.checkbox)
                        }
                        FilterSection("标签", id: "library.tags", alignment: .leading) {
                            HStack {
                                Button("全选")  {
                                    viewModel.tag = .all
                                }
                                Button("清空") {
                                    viewModel.tag = .none
                                }
                            }
                            .buttonStyle(.link)
                            Group {
                                ForEach(Array(zip(FRTag.allOptions.indices, FRTag.allOptions)), id: \.0) { (i, option) in
                                    Toggle(LocalizedStringKey(option), isOn: Binding<Bool>(get: {
                                        viewModel.tag.contains(FRTag(rawValue: 1 << i))
                                    }, set: {
                                        if $0 {
                                            viewModel.tag.insert(FRTag(rawValue: 1 << i))
                                        } else {
                                            viewModel.tag.remove(FRTag(rawValue: 1 << i))
                                        }
                                    }))
                                }
                            }
                            .toggleStyle(.checkbox)
                        }
                    }
                }
                .padding(.trailing)
            }
            .lineLimit(1)
        }
    }
}

struct ShowOnlyFilterSection: View {
    let id: String
    let selection: FRShowOnly
    let onChange: (FRShowOnly, Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(FRShowOnly.allOptions.indices, id: \.self) { index in
                let metadata = FRShowOnly.allOptions[index]
                Toggle(isOn: Binding(
                    get: { selection.contains(metadata.0) },
                    set: { onChange(metadata.0, $0) }
                )) {
                    Label {
                        Text(LocalizedStringKey(metadata.1))
                    } icon: {
                        Image(systemName: metadata.2)
                            .foregroundStyle(metadata.3)
                    }
                }
                .toggleStyle(.checkbox)
            }
        }
        .padding()
        .padding(.top)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay {
            ZStack {
                Rectangle()
                    .stroke(
                        Color(nsColor: NSColor.unemphasizedSelectedTextBackgroundColor),
                        lineWidth: 1
                    )
                    .padding(.top, 8)

                VStack {
                    HStack {
                        Text("仅显示：")
                            .background(Color(nsColor: NSColor.windowBackgroundColor))
                            .padding(.leading, 5)
                        Spacer()
                    }
                    Spacer()
                }
            }
            .allowsHitTesting(false)
        }
        .accessibilityIdentifier(id)
    }
}
