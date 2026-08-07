//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers

// Right-hand detail / apply panel for the 小组件 tab.
struct WidgetPreview: View {
    @ObservedObject var viewModel: RmskinViewModel

    var body: some View {
        VStack(spacing: 0) {
            if let theme = viewModel.selectedTheme {
                detail(theme)
            } else {
                Spacer()
                Text("选择一个小组件主题")
                    .foregroundStyle(.secondary)
                Spacer()
            }
            Divider()
            importBar
        }
        .frame(maxHeight: .infinity)
    }

    @ViewBuilder private func detail(_ theme: RmskinTheme) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color(nsColor: .windowBackgroundColor))
                    if let url = theme.previewURL, let image = NSImage(contentsOf: url) {
                        Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                    } else {
                        Image(systemName: "square.grid.2x2")
                            .font(.system(size: 48)).foregroundStyle(.secondary)
                    }
                }
                .frame(height: 160)

                Text(theme.name).font(.title2).bold()
                infoRow("作者", theme.author)
                infoRow("加载类型", theme.loadType)
                infoRow("版本", theme.version)
                if let r = theme.minimumRainmeter { infoRow("最低 Rainmeter", r) }
                if let w = theme.minimumWindows { infoRow("最低 Windows", w) }

                HStack(spacing: 8) {
                    if viewModel.isApplied(theme) {
                        Button {
                            viewModel.stop(theme)
                        } label: {
                            Label("停止", systemImage: "stop.fill").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                        .tint(.red)
                    }
                    Button {
                        viewModel.apply(theme)
                    } label: {
                        Label(viewModel.isApplied(theme) ? "已应用" : "应用",
                              systemImage: viewModel.isApplied(theme) ? "checkmark" : "play.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(viewModel.isApplied(theme))
                }
                .padding(.top, 4)
            }
            .padding()
        }
    }

    @ViewBuilder private func infoRow(_ label: String, _ value: String) -> some View {
        if !value.isEmpty {
            HStack(alignment: .top) {
                Text(label).foregroundStyle(.secondary).frame(width: 96, alignment: .leading)
                Text(value).textSelection(.enabled)
                Spacer()
            }
            .font(.callout)
        }
    }

    private var importBar: some View {
        Button {
            importPanel()
        } label: {
            Label("导入小组件", systemImage: "square.and.arrow.down")
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
        }
        .buttonStyle(.bordered)
        .padding(8)
    }

    private func importPanel() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        if let rmskinType = UTType(filenameExtension: "rmskin") {
            panel.allowedContentTypes = [rmskinType, .zip, .folder]
        }
        if panel.runModal() == .OK {
            viewModel.importThemes(urls: panel.urls)
        }
    }
}
