//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WallpaperPreview: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    
    @Environment(\.undoManager) var undoManager
    
    @State var isEditingId = ""
    @State var title = ""
    @State var newTag = ""
    
    @State var hoveredTag: String?
    @State var isTagsHovered = false

    // 目录大小异步计算并缓存，避免每次重绘在主线程遍历整个壁纸目录造成卡顿。
    @State private var sizeText: String = "…"

    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
    }

    private func recomputeSize(for wallpaper: WEWallpaper) {
        sizeText = "…"
        let dir = wallpaper.wallpaperDirectory
        Task.detached(priority: .utility) {
            let bytes = (try? dir.directoryTotalAllocatedSize(includingSubfolders: true)) ?? 0
            let text = ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
            await MainActor.run { self.sizeText = text }
        }
    }
    
    var body: some View {
        VStack {
            ScrollView {
                VStack(spacing: 16) {
                    VStack(spacing: 10) {
                        GifImage(contentsOf: wallpaperViewModel.currentWallpaper.project.preview.isEmpty
                            ? Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")!
                            : wallpaperViewModel.currentWallpaper.previewURL)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .background(Color(nsColor: NSColor.controlBackgroundColor))
                            .frame(width: 280, height: 280)
                            .clipShape(RoundedRectangle(cornerRadius: 16.0))
                            .border(Color.white, width: 4)
                        HStack {
                            if isEditingId == "title" {
                                TextField("壁纸名称", text: $title)
                                    .onSubmit {
                                        guard let wallpaper = wallpaperViewModel.currentWallpaper
                                            .updateStoredMetadata(title: title) else { return }
                                        wallpaperViewModel.currentWallpaper = wallpaper
                                        
                                        isEditingId = ""
                                    }
                            } else {
                                Text(wallpaperViewModel.currentWallpaper.project.title.isEmpty ? "未命名" : wallpaperViewModel.currentWallpaper.project.title)
                                    .frame(minWidth: 50)
                                    .id("title")
                                    .lineLimit(1)
                                    .onTapGesture(count: 2) {
                                        title = wallpaperViewModel.currentWallpaper.project.title
                                        isEditingId = "title"
                                    }
                                Image(systemName: "square.and.pencil")
                            }
                            
                        }
                    }
                    HStack {
                        Image(systemName: "person.crop.circle.fill")
                            .resizable()
                            .frame(width: 32, height: 32)
                            .foregroundStyle(.secondary)
                        Text(wallpaperViewModel.currentWallpaper.project.resolvedAuthor ?? "佚名作者")
                            .lineLimit(1)
                    }
                    HStack {
                        HStack(spacing: 5) {
                            Image(systemName: "star")
                            Image(systemName: "star")
                            Image(systemName: "star")
                            Image(systemName: "star")
                            Image(systemName: "star")
                        }
                        .font(.caption)
                        Button {
                            FavoritesManager.shared.toggle(wallpaperViewModel.currentWallpaper.id)
                            NotificationCenter.default.post(name: .favoritesChanged, object: nil)
                        } label: {
                            Image(systemName: FavoritesManager.shared.isFavorite(wallpaperViewModel.currentWallpaper.id) ? "heart.fill" : "heart")
                                .foregroundStyle(FavoritesManager.shared.isFavorite(wallpaperViewModel.currentWallpaper.id) ? .red : .secondary)
                        }
                    }
                    HStack {
                        Text(wallpaperViewModel.currentWallpaper.isPreset
                            ? (wallpaperViewModel.currentWallpaper.presetStatusDescription.map { "预设 · \($0)" }
                                ?? "预设 · \(wallpaperViewModel.currentWallpaper.kind.displayName)")
                            : wallpaperViewModel.currentWallpaper.project.type)
                        Text(sizeText)
                    }
                    .font(.footnote)

                    if wallpaperViewModel.currentWallpaper.isPreset,
                       let dependency = wallpaperViewModel.currentWallpaper.presetDependency {
                        Label("基础壁纸：\(dependency.rawValue)", systemImage: "square.stack.3d.up.fill")
                            .font(.caption)
                            .foregroundStyle(wallpaperViewModel.currentWallpaper.needsPresetDependency ? .orange : .secondary)
                    }
                    
                    ViewThatFits(in: .horizontal) {
                        tags.animation(.spring(), value: isTagsHovered)
                        ScrollView(.horizontal, showsIndicators: false) {
                            tags.animation(.spring(), value: isTagsHovered)
                        }
                    }
                    
                    .onHover { isTagsHovered = $0 }
                    
                    if isEditingId == "tags" {
                        HStack {
                            Button {
                                newTag = ""
                                isEditingId = ""
                            } label: {
                                Image(systemName: "arrow.uturn.backward")
                            }
                            TextField("新标签", text: $newTag)
                                .onSubmit {
                                    defer {
                                        newTag = ""
                                        isEditingId = ""
                                    }
                                    
                                    guard !newTag.isEmpty else { return }
                                    
                                    let current = wallpaperViewModel.currentWallpaper
                                    var tags = current.project.tags ?? []
                                    
                                    tags = Array(Set(tags))
                                    
                                    tags.append(newTag)
                                    
                                    tags = Array(Set(tags))
                                    
                                    guard let wallpaper = current.updateStoredMetadata(tags: tags.sorted()) else { return }
                                    wallpaperViewModel.currentWallpaper = wallpaper
                                }
                        }
                    }
                    VStack(spacing: 3) {
                        Button { } label: {
                            Label("Unsubscribe", systemImage: "xmark")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.red)
                        HStack(spacing: 3) {
                            Button { } label: {
                                Label("Comment", systemImage: "text.badge.star")
                                    .frame(maxWidth: .infinity)
                            }
                            Button { } label: {
                                Image(systemName: "doc.on.doc.fill")
                            }
                            Button { } label: {
                                Image(systemName: "exclamationmark.triangle.fill")
                            }
                        }
                    }
                    .disabled(true)

                    sectionHeader("播放控制")
                    VStack(spacing: 16) {
                        HStack {
                            Label("音量", systemImage: "speaker.wave.3.fill")
                            Spacer()
                            MirageSlider(value: $wallpaperViewModel.playVolume, in: 0...1).frame(width: 100)
                            Text(String(format: "%.0f", wallpaperViewModel.playVolume * 100) + "%")
                                .frame(width: 35)
                        }
                        if wallpaperViewModel.currentWallpaper.kind == .scene {
                            HStack {
                                Label("速度", systemImage: "gauge.with.dots.needle.67percent")
                                Spacer()
                                MirageSlider(value: $wallpaperViewModel.playRate, in: 0...2, step: 0.1).frame(width: 100)
                                Text(String(format: "%.01fx", wallpaperViewModel.playRate))
                                .frame(width: 35)
                            }
                        }
                        if wallpaperViewModel.currentWallpaper.kind == .video {
                            HStack {
                                Label("填充模式", systemImage: "aspectratio.fill")
                                Spacer()
                                Picker("", selection: Binding(
                                    get: { wallpaperViewModel.runtime.fillMode },
                                    set: { wallpaperViewModel.setFillMode($0) })) {
                                    ForEach(FillMode.allCases) { Text($0.displayName).tag($0) }
                                }
                                .labelsHidden().frame(width: 120)
                            }
                        }
                    }

                    sectionHeader("壁纸属性")
                    PropertyEditor(wallpaper: wallpaperViewModel.currentWallpaper)
                        .environmentObject(wallpaperViewModel)

                    sectionHeader("壁纸")
                    VStack(spacing: 3) {
                        Button {
                            wallpaperViewModel.applyToAllScreens()
                        } label: {
                            Label("覆盖到所有显示器", systemImage: "rectangle.on.rectangle")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        Button {
                            wallpaperViewModel.stopWallpaper()
                        } label: {
                            Label("停止壁纸", systemImage: "stop.fill")
                                .frame(maxWidth: .infinity)
                        }
                    }

                    sectionHeader("预设")
                    VStack(spacing: 3) {
                        HStack(spacing: 3) {
                            Button {
                                if let r = PresetManager.shared.importPreset() {
                                    wallpaperViewModel.runtime = r
                                    wallpaperViewModel.saveRuntime()
                                    wallpaperViewModel.reapplyCurrent()
                                }
                            } label: {
                                Label("导入", systemImage: "folder.fill").frame(maxWidth: .infinity)
                            }
                            Button {
                                PresetManager.shared.exportPreset(for: wallpaperViewModel.currentWallpaper,
                                                                  runtime: wallpaperViewModel.runtime)
                            } label: {
                                Label("导出", systemImage: "square.and.arrow.down.fill").frame(maxWidth: .infinity)
                            }
                        }
                        Button(role: .destructive) {
                            wallpaperViewModel.resetProperties()
                        } label: {
                            Label("重置为默认", systemImage: "arrow.triangle.2.circlepath")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.red)
                    }
                }
                .blur(radius: wallpaperViewModel.currentWallpaper.project == .invalid ? 16.0 : 0)
                .overlay {
                    if wallpaperViewModel.currentWallpaper.project == .invalid {
                        Text("请选择一个有效的壁纸")
                    }
                }
                .disabled(wallpaperViewModel.currentWallpaper.project == .invalid ? true : false)
                .animation(.default, value: wallpaperViewModel.currentWallpaper.project)
                .padding([.horizontal, .top])
            }
            .task(id: wallpaperViewModel.currentWallpaper.id) {
                recomputeSize(for: wallpaperViewModel.currentWallpaper)
            }

            HStack {
                Spacer()
                Button {
                    AppDelegate.shared.mainWindowController.close()
                } label: {
                    Text("确定").frame(width: 50)
                }
                .buttonStyle(.borderedProminent)
                Button { 
                    AppDelegate.shared.mainWindowController.close()
                } label: {
                    Text("取消").frame(width: 50)
                }
            }
            .padding()
        }
    }
    
    func sectionHeader(_ title: String) -> some View {
        HStack(spacing: 3) {
            Text(title)
            VStack {
                Divider().frame(height: 1).overlay(Color.accentColor)
            }
        }
    }

    var tags: some View {
        HStack {
            if let tags = wallpaperViewModel.currentWallpaper.project.tags {
                ForEach(tags, id: \.self) { tag in
                    Text(tag)
                        .padding(5)
                        .background {
                            RoundedRectangle(cornerRadius: 25.0)
                                .colorInvert()
                                .foregroundStyle(Color.primary)
                            RoundedRectangle(cornerRadius: 25.0)
                                .stroke(Color.secondary, lineWidth: 1.6)
                        }
                        .overlay(alignment: .topTrailing) {
                            if hoveredTag == tag {
                                Button {
                                    let current = wallpaperViewModel.currentWallpaper
                                    guard var tags = current.project.tags else { return }
                                    
                                    tags = Array(Set(tags))
                                    
                                    guard let index = tags.firstIndex(where: { $0 == tag }) else { return }
                                    
                                    tags.remove(at: index)
                                    
                                    guard let wallpaper = current.updateStoredMetadata(tags: tags) else { return }
                                    wallpaperViewModel.currentWallpaper = wallpaper
                                } label: {
                                    Image(systemName: "xmark.circle.fill")
                                }
                                .buttonStyle(.plain)
                                .foregroundStyle(.white, .red)
                                .symbolRenderingMode(.palette)
                                .offset(x: 5, y: -2.5)
                            }
                        }
                        .onHover { hovered in
                            if hovered {
                                hoveredTag = tag
                            } else {
                                hoveredTag = nil
                            }
                        }
                }
            } else {
                Text("暂无标签")
                    .foregroundStyle(Color.secondary)
            }
            
            if isTagsHovered {
                Button {
                    isEditingId = "tags"
                } label: {
                    Image(systemName: "plus")
                        .font(.body)
                }
                .buttonStyle(.plain)
            }
        }
        .font(.footnote)
        .lineLimit(1)
    }
}
