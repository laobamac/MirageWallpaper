//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct GeneralPage: SettingsPage {
    @ObservedObject var viewModel: GlobalSettingsViewModel

    @State private var librarySources: [WallpaperLibrarySource]
    @State private var showMirrorWarning = false

    private var apiKeyIsEmpty: Bool {
        viewModel.settings.normalizedSteamAPIKey.isEmpty
    }

    init(globalSettings viewModel: GlobalSettingsViewModel) {
        self.viewModel = viewModel
        _librarySources = State(initialValue: WallpaperLibrary.shared.librarySources)
    }

    private func applyEndpointChange() {
        AppDelegate.shared.workshopViewModel.items = []
        AppDelegate.shared.workshopViewModel.currentPage = 1
        AppDelegate.shared.workshopViewModel.search()
    }

    private func refreshLibrarySources() {
        librarySources = WallpaperLibrary.shared.librarySources
    }

    private func chooseDirectory(message: String, completion: @escaping (URL) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.prompt = L("选择")
        panel.message = L(message)
        panel.begin { resp in
            if resp == .OK, let url = panel.url { completion(url) }
        }
    }

    @ViewBuilder
    private func librarySourceRow(_ source: WallpaperLibrarySource) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Text(source.title).font(.callout)
                if source.role == .managedSteamCMD {
                    Text("当前下载位置")
                        .font(.caption2)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Color.accentColor.opacity(0.15))
                        .clipShape(Capsule())
                } else if !source.exists {
                    Text("尚未创建")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            Text(source.detail)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(source.url.path(percentEncoded: false))
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .truncationMode(.middle)
                .textSelection(.enabled)
            HStack {
                if source.role == .steam || source.role == .customSteam {
                    Button("选择目录…") {
                        chooseDirectory(message: "选择 Wallpaper Engine 创意工坊壁纸所在目录（431960）") { url in
                            WallpaperLibrary.shared.setWorkshopDirectory(url)
                            refreshLibrarySources()
                            AppDelegate.shared.contentViewModel.refresh()
                        }
                    }
                } else if source.role == .imported {
                    Button("选择目录…") {
                        chooseDirectory(message: "选择用于存放导入壁纸的目录") { url in
                            WallpaperLibrary.shared.setImportedDirectory(url)
                            refreshLibrarySources()
                            AppDelegate.shared.contentViewModel.refresh()
                        }
                    }
                }
                Button("在访达中显示") {
                    if !source.exists {
                        try? FileManager.default.createDirectory(at: source.url, withIntermediateDirectories: true)
                    }
                    NSWorkspace.shared.activateFileViewerSelecting([source.url])
                    refreshLibrarySources()
                }
                if source.role == .customSteam && WallpaperLibrary.shared.isWorkshopDirectoryCustomized {
                    Button("恢复默认") {
                        WallpaperLibrary.shared.setWorkshopDirectory(nil)
                        refreshLibrarySources()
                        AppDelegate.shared.contentViewModel.refresh()
                    }
                } else if source.role == .imported && WallpaperLibrary.shared.isImportedDirectoryCustomized {
                    Button("恢复默认") {
                        WallpaperLibrary.shared.setImportedDirectory(nil)
                        refreshLibrarySources()
                        AppDelegate.shared.contentViewModel.refresh()
                    }
                }
            }
        }
    }

    var body: some View {
        Form {
            Section {
                Toggle("开机时自动启动 Mirage", isOn: $viewModel.settings.autoStart)
            } header: {
                Label("启动", systemImage: "star.fill")
            }

            Section {
                Toggle("自动检查并下载更新", isOn: Binding(
                    get: { viewModel.settings.shouldAutomaticallyUpdate },
                    set: { viewModel.settings.automaticUpdatesEnabled = $0 }
                ))
                    .onChange(of: viewModel.settings.shouldAutomaticallyUpdate) { _, _ in
                        UpdateManager.shared.applyAutomaticUpdatePreference()
                    }
                Toggle("接收测试版更新", isOn: Binding(
                    get: { viewModel.settings.shouldReceivePrereleaseUpdates },
                    set: { viewModel.settings.receivePrereleaseUpdates = $0 }
                ))
                .onChange(of: viewModel.settings.shouldReceivePrereleaseUpdates) { _, _ in
                    if viewModel.settings.shouldAutomaticallyUpdate {
                        UpdateManager.shared.checkForUpdates(nil)
                    }
                }
                Text("关闭自动更新后，Mirage 不会在后台检查或下载；仍可通过菜单中的“检查更新…”手动检查。开启测试版后，Mirage 会在正式更新之外检查最新的测试版。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Label("软件更新", systemImage: "arrow.triangle.2.circlepath")
            }

            Section {
                Picker("语言", selection: $viewModel.settings.language) {
                    Text("跟随系统").tag(GSLocalization.followSystem)
                    Text("English").tag(GSLocalization.en_US)
                    Text("简体中文").tag(GSLocalization.zh_CN)
                    Text("繁體中文").tag(GSLocalization.zh_TW)
                }
            } header: {
                Label("语言", systemImage: "character.bubble")
            }

            Section {
                Picker("外观", selection: $viewModel.settings.appearance) {
                    Text("浅色").tag(GSAppearance.light)
                    Text("深色").tag(GSAppearance.dark)
                    Text("跟随系统").tag(GSAppearance.followSystem)
                }
            } header: {
                Label("外观", systemImage: "paintpalette.fill")
            }

            Section {
                HStack {
                    Text("全局音量")
                    MirageSlider(value: $viewModel.settings.masterVolume, in: 0...1)
                        .onChange(of: viewModel.settings.masterVolume) { _, _ in
                            AppDelegate.shared.wallpaperViewModel.reapplyVolume()
                        }
                    Text("\(Int(viewModel.settings.masterVolume * 100))%")
                        .monospacedDigit().frame(width: 40)
                }
                Toggle("全局静音", isOn: $viewModel.settings.globalMuted)
                    .onChange(of: viewModel.settings.globalMuted) { _, _ in
                        AppDelegate.shared.wallpaperViewModel.reapplyVolume()
                    }
            } header: {
                Label("音频", systemImage: "speaker.wave.3.fill")
            }

            Section {
                ForEach(librarySources) { source in
                    librarySourceRow(source)
                }

                Toggle("自动刷新壁纸库", isOn: $viewModel.settings.autoRefresh)
            } header: {
                Label("壁纸库", systemImage: "folder.fill")
            }

            if MirageRegion.isMainlandChina {
                Section {
                    Picker("Steam API 线路", selection: $viewModel.settings.steamAPIEndpoint) {
                        Text("Steam 官方 Web API").tag(GSSteamAPIEndpoint.official)
                        Text("SteamCF 镜像").tag(GSSteamAPIEndpoint.mirror)
                    }
                    .onChange(of: viewModel.settings.steamAPIEndpoint) { _, newValue in
                        if newValue == .mirror {
                            showMirrorWarning = true
                        } else {
                            applyEndpointChange()
                        }
                    }
                } header: {
                    Label("创意工坊", systemImage: "network")
                }
                .alert("SteamCF 镜像警告", isPresented: $showMirrorWarning) {
                    Button("仍要使用") {
                        applyEndpointChange()
                    }
                    Button("取消", role: .cancel) {
                        viewModel.settings.steamAPIEndpoint = .official
                    }
                } message: {
                    Text("该镜像仅允许中国大陆用户访问，且并非 Steam 官方服务，不保证安全性和可用性。它只代理浏览 API，不能加速 SteamCMD 登录或壁纸下载。")
                }
            }

            Section {
                VStack(alignment: .leading, spacing: 4) {
                    if !viewModel.settings.hasValidCustomSteamAPIKey {
                        HStack(alignment: .top, spacing: 10) {
                            Image(systemName: "exclamationmark.key.fill")
                                .font(.title2)
                                .foregroundStyle(.orange)
                            VStack(alignment: .leading, spacing: 4) {
                                Text(apiKeyIsEmpty ? "请设置您自己的 Steam Web API Key" : "Steam Web API Key 格式无效")
                                    .font(.callout)
                                    .bold()
                                Text(apiKeyIsEmpty
                                     ? "内置 Key 由所有用户共享，可能因请求过多而暂时不可用。设置专属 Key 可显著提高创意工坊浏览稳定性。它只用于浏览，不影响 SteamCMD 登录和壁纸下载。"
                                     : "Steam Web API Key 应为 32 位十六进制字符。当前将继续使用内置 Key。")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                        .padding(10)
                        .background(Color.orange.opacity(0.1))
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    }

                    TextField("Steam Web API Key", text: $viewModel.settings.steamAPIKey)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                    if viewModel.settings.hasValidCustomSteamAPIKey {
                        Label("已设置专属 API Key", systemImage: "checkmark.seal.fill")
                            .font(.caption)
                            .foregroundStyle(.green)
                    }
                    Text("可前往 [Steam API Key 申请页面](https://steamcommunity.com/dev/apikey) 申请，并按页面要求填写域名。Key 仅用于 Mirage 在本机请求创意工坊浏览数据，请勿分享。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } header: {
                Label("Steam API Key", systemImage: "key.fill")
            }

            Section {
                Toggle("详细日志（供调试）", isOn: $viewModel.settings.verboseLog)
                HStack {
                    Text("重置所有设置")
                    Spacer()
                    Button {
                        viewModel.settings = GlobalSettings()
                    } label: {
                        Text("重置").frame(width: 100)
                    }
                    .tint(.red)
                    .buttonStyle(.borderedProminent)
                }
            } header: {
                Label("高级", systemImage: "wrench.and.screwdriver.fill")
            }
        }
        .formStyle(.grouped)
        .onChange(of: viewModel.settings) { _, _ in viewModel.save() }
    }
}
