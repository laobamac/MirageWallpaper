//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct WallpaperPreview: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    let creatorStore: WorkshopCreatorStore
    let interactionStore: WorkshopInteractionStore
    let libraryStore: WorkshopLibraryStore
    let selectionCoordinator: WorkshopSelectionCoordinator
    let sessionStore: SteamSessionStore
    let subscriptionStore: SubscriptionStore
    let isActive: Bool
    
    @Environment(\.undoManager) var undoManager
    
    @State var isEditingId = ""
    @State var title = ""
    @State var newTag = ""
    @FocusState private var titleFieldFocused: Bool
    
    @State var hoveredTag: String?
    @State var isTagsHovered = false
    @State private var isConfirmingUnsubscribe = false

    // 目录大小异步计算并缓存，避免每次重绘在主线程遍历整个壁纸目录造成卡顿。
    @State private var sizeText: String = "…"

    init(contentViewModel viewModel: ContentViewModel,
         wallpaperViewModel: WallpaperViewModel,
         creatorStore: WorkshopCreatorStore,
         interactionStore: WorkshopInteractionStore,
         libraryStore: WorkshopLibraryStore,
         selectionCoordinator: WorkshopSelectionCoordinator,
         sessionStore: SteamSessionStore,
         subscriptionStore: SubscriptionStore,
         isActive: Bool = true) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
        self.creatorStore = creatorStore
        self.interactionStore = interactionStore
        self.libraryStore = libraryStore
        self.selectionCoordinator = selectionCoordinator
        self.sessionStore = sessionStore
        self.subscriptionStore = subscriptionStore
        self.isActive = isActive
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

    private func beginTitleEditing() {
        title = wallpaperViewModel.currentWallpaper.project.title
        isEditingId = "title"
        titleFieldFocused = true
    }

    private func saveTitle() {
        let value = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty,
              wallpaperViewModel.updateStoredMetadata(title: value) != nil else { return }
        isEditingId = ""
        titleFieldFocused = false
        viewModel.refresh()
    }

    private func saveTags(_ tags: [String]) {
        guard wallpaperViewModel.updateStoredMetadata(tags: tags) != nil else { return }
        viewModel.refresh()
    }
    
    var body: some View {
        VStack {
            ScrollView {
                LazyVStack(spacing: 16) {
                    HStack {
                        Image(systemName: "display")
                            .foregroundStyle(.secondary)
                        Text("正在为 \(wallpaperViewModel.selectedDisplayName) 设置")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        Spacer()
                    }
                    .padding(.horizontal)

                    VStack(spacing: 10) {
                        GifImage(contentsOf: wallpaperViewModel.currentWallpaper.project.preview.isEmpty
                            ? Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")!
                            : wallpaperViewModel.currentWallpaper.previewURL,
                            animates: isActive)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .background(Color(nsColor: NSColor.controlBackgroundColor))
                            .frame(width: 280, height: 280)
                            .clipShape(RoundedRectangle(cornerRadius: 16.0))
                            .border(Color.white, width: 4)
                        HStack {
                            if isEditingId == "title" {
                                TextField("壁纸名称", text: $title)
                                    .focused($titleFieldFocused)
                                    .onSubmit {
                                        saveTitle()
                                    }
                                    .onExitCommand {
                                        isEditingId = ""
                                        titleFieldFocused = false
                                    }
                            } else {
                                Text(wallpaperViewModel.currentWallpaper.project.title.isEmpty ? L("未命名") : wallpaperViewModel.currentWallpaper.project.title)
                                    .frame(minWidth: 50)
                                    .id("title")
                                    .lineLimit(1)
                                    .onTapGesture(count: 2) {
                                        beginTitleEditing()
                                    }
                                Button(action: beginTitleEditing) {
                                    Image(systemName: "square.and.pencil")
                                }
                                .buttonStyle(.plain)
                                .help("编辑壁纸名称")
                            }
                            
                        }
                    }
                    authorSection
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
                            toggleCurrentFavorite()
                        } label: {
                            if isChangingCurrentFavorite {
                                ProgressView()
                                    .controlSize(.small)
                                    .frame(width: 16, height: 16)
                            } else {
                                Image(systemName: isCurrentFavorite ? "heart.fill" : "heart")
                                    .foregroundStyle(isCurrentFavorite ? .red : .secondary)
                            }
                        }
                        .disabled(isChangingCurrentFavorite)
                        .help(L(isCurrentFavorite ? "取消收藏" : "加入收藏"))
                    }
                    HStack {
                        Text(wallpaperViewModel.currentWallpaper.isPreset
                            ? (wallpaperViewModel.currentWallpaper.presetStatusDescription.map { L("预设 · %@", $0) }
                                ?? L("预设 · %@", wallpaperViewModel.currentWallpaper.kind.displayName))
                            : wallpaperViewModel.currentWallpaper.kind.displayName)
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
                                    
                                    saveTags(tags.sorted())
                                }
                        }
                    }
                    workshopActions

                    sectionHeader("播放控制")
                    VStack(spacing: 16) {
                        HStack {
                            Label("音量", systemImage: "speaker.wave.3.fill")
                            Spacer()
                            MirageSlider(value: Binding(
                                get: { wallpaperViewModel.playVolume },
                                set: { wallpaperViewModel.playVolume = $0 }), in: 0...1)
                                .frame(width: 100)
                            Text(String(format: "%.0f", wallpaperViewModel.playVolume * 100) + "%")
                                .frame(width: 35)
                        }
                        if wallpaperViewModel.currentWallpaper.kind == .scene ||
                            wallpaperViewModel.currentWallpaper.kind == .video {
                            HStack {
                                Label("速度", systemImage: "gauge.with.dots.needle.67percent")
                                Spacer()
                                MirageSlider(value: Binding(
                                    get: { wallpaperViewModel.playRate },
                                    set: { wallpaperViewModel.playRate = $0 }), in: 0...2, step: 0.1)
                                    .frame(width: 100)
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
                            Label("停止此显示器", systemImage: "stop.fill")
                                .frame(maxWidth: .infinity)
                        }
                        Button {
                            wallpaperViewModel.stopAllWallpapers()
                        } label: {
                            Label("全部停止", systemImage: "stop.circle.fill")
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
        .onAppear {
            libraryStore.loadInstalledMetadata(
                for: wallpaperViewModel.currentWallpaper
            )
            recomputeSize(for: wallpaperViewModel.currentWallpaper)
        }
        .onChange(of: wallpaperViewModel.currentWallpaper.id) { _, _ in
            libraryStore.loadInstalledMetadata(
                for: wallpaperViewModel.currentWallpaper
            )
            recomputeSize(for: wallpaperViewModel.currentWallpaper)
        }
        .confirmationDialog(
            "取消订阅",
            isPresented: $isConfirmingUnsubscribe,
            presenting: libraryStore.installedWorkshopItem(
                for: wallpaperViewModel.currentWallpaper
            )
        ) { item in
            Button("取消订阅", role: .destructive) {
                subscriptionStore.unsubscribe(item)
            }
            Button("取消", role: .cancel) { }
        } message: { _ in
            Text("取消订阅后，Mirage 会停止下载并删除 Mirage 下载目录中的副本。Steam 内容目录中的文件不会被删除。")
        }
    }

    @ViewBuilder
    private var workshopActions: some View {
        if let item = libraryStore.installedWorkshopItem(
            for: wallpaperViewModel.currentWallpaper
        ) {
            let id = item.publishedFileId
            let subscriptionStatus = subscriptionStore.status(for: id)

            VStack(spacing: 3) {
                if sessionStore.setupState == .checking {
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text(sessionStore.checkingMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
                } else if sessionStore.setupState != .ready {
                    Button {
                        AppDelegate.shared.openSteamSetup()
                    } label: {
                        Label("登录 Steam", systemImage: "person.crop.circle.badge.exclamationmark")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                } else if subscriptionStatus.isChecking || subscriptionStatus.isChanging {
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text(LocalizedStringKey(subscriptionStatus.isChanging ? "正在同步订阅状态…" : "正在检查订阅状态…"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
                } else if subscriptionStatus.state == .unknown {
                    Button {
                        subscriptionStore.refreshStates(for: [item])
                    } label: {
                        Label("重新检查订阅状态", systemImage: "arrow.clockwise")
                            .frame(maxWidth: .infinity)
                    }
                } else if subscriptionStatus.state == .subscribed {
                    Button {
                        isConfirmingUnsubscribe = true
                    } label: {
                        Label("取消订阅", systemImage: "xmark.circle.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                } else {
                    Button {
                        subscriptionStore.subscribe(item)
                    } label: {
                        Label("订阅并下载", systemImage: "plus.circle.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                }

                HStack(spacing: 3) {
                    Button {
                        openWorkshopItem(item)
                    } label: {
                        Label("评论", systemImage: "text.bubble.fill")
                            .frame(maxWidth: .infinity)
                    }
                    Button {
                        copyWorkshopURL(item)
                    } label: {
                        Image(systemName: "doc.on.doc.fill")
                    }
                    .help(L("复制创意工坊链接"))
                    Button {
                        openWorkshopItemInSteam(item)
                    } label: {
                        Image(systemName: "exclamationmark.triangle.fill")
                    }
                    .help(L("在 Steam 中查看或举报"))
                }

                if let error = subscriptionStatus.actionError {
                    Text(error)
                        .font(.caption2)
                        .foregroundStyle(.red)
                        .multilineTextAlignment(.center)
                }
            }
        }
    }

    private func openWorkshopItem(_ item: WorkshopItem) {
        selectionCoordinator.showDetail(item)
        AppDelegate.shared.navigationModel.selection = .workshop
    }

    private func copyWorkshopURL(_ item: WorkshopItem) {
        let value = "https://steamcommunity.com/sharedfiles/filedetails/?id=\(item.publishedFileId)"
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(value, forType: .string)
    }

    private func openWorkshopItemInSteam(_ item: WorkshopItem) {
        guard let url = URL(string: "https://steamcommunity.com/sharedfiles/filedetails/?id=\(item.publishedFileId)") else { return }
        NSWorkspace.shared.open(url)
    }

    private var currentWorkshopID: String? {
        wallpaperViewModel.currentWallpaper.steamFavoriteWorkshopID()
    }

    private var isCurrentFavorite: Bool {
        if let currentWorkshopID {
            return interactionStore.isFavorite(currentWorkshopID)
        }
        return FavoritesManager.shared.isFavorite(wallpaperViewModel.currentWallpaper.id)
    }

    private var isChangingCurrentFavorite: Bool {
        currentWorkshopID.map {
            interactionStore.changingFavoriteIDs.contains($0)
        } == true
    }

    private func toggleCurrentFavorite() {
        if let currentWorkshopID {
            interactionStore.toggleFavorite(workshopID: currentWorkshopID)
        } else {
            FavoritesManager.shared.toggle(wallpaperViewModel.currentWallpaper.id)
            NotificationCenter.default.post(name: .favoritesChanged, object: nil)
        }
    }

    private var authorSection: some View {
        let wallpaper = wallpaperViewModel.currentWallpaper
        let creator = libraryStore.installedCreator(for: wallpaper)
        let name = libraryStore.installedAuthorName(for: wallpaper)
            ?? L("佚名作者")
        return Button {
            if let creator {
                creatorStore.open(creator)
            }
        } label: {
            HStack(spacing: 8) {
                AsyncImage(url: creator?.avatarURL) { phase in
                    if case .success(let image) = phase {
                        image.resizable().scaledToFill()
                    } else {
                        Image(systemName: "person.crop.circle.fill")
                            .resizable()
                            .foregroundStyle(.secondary)
                    }
                }
                .frame(width: 32, height: 32)
                .clipShape(Circle())
                Text(name)
                    .lineLimit(1)
                Spacer(minLength: 0)
                if creator != nil {
                    Image(systemName: "arrow.up.right")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .buttonStyle(.plain)
        .disabled(creator == nil)
    }
    
    func sectionHeader(_ title: LocalizedStringKey) -> some View {
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
                                    
                                    saveTags(tags)
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
