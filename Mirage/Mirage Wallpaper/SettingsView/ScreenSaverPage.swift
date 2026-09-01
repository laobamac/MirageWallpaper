import SwiftUI

struct ScreenSaverPage: SettingsPage {
    private struct Status {
        let isInstalled: Bool
        let configuredTitle: String

        init(manager: ScreenSaverManager) {
            isInstalled = manager.isInstalled
            configuredTitle = manager.configuredWallpaperTitle() ?? "尚未选择"
        }
    }

    @ObservedObject var viewModel: GlobalSettingsViewModel
    @ObservedObject private var wallpaperViewModel: WallpaperViewModel
    @ObservedObject private var dynamicLockScreenManager = DynamicLockScreenManager.shared
    @ObservedObject private var screenSaverDynamicLockScreenManager = ScreenSaverDynamicLockScreenManager.shared
    @State private var status: Status
    @State private var message = ""
    @State private var showingError = false
    @State private var showingFullDiskAccessPrompt = false
    @State private var isWorking = false

    init(globalSettings viewModel: GlobalSettingsViewModel) {
        self.viewModel = viewModel
        wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        _status = State(initialValue: Status(manager: .shared))
    }

    private var manager: ScreenSaverManager { .shared }
    private var wallpaper: WEWallpaper { wallpaperViewModel.currentWallpaper }
    private func refreshStatus() {
        status = Status(manager: manager)
    }

    var body: some View {
        Form {
            Section {
                HStack {
                    Label(LocalizedStringKey(status.isInstalled ? "Mirage 动态屏保已安装" : "Mirage 动态屏保尚未安装"),
                          systemImage: status.isInstalled ? "checkmark.seal.fill" : "rectangle.dashed")
                        .foregroundStyle(status.isInstalled ? .green : .secondary)
                    Spacer()
                    Button(LocalizedStringKey(status.isInstalled ? "重新安装" : "安装")) {
                        performAsync { try manager.install() }
                    }
                    if status.isInstalled {
                        Button("卸载", role: .destructive) { performAsync { try manager.uninstall() } }
                    }
                }
                .disabled(isWorking)
                if isWorking {
                    HStack {
                        ProgressView().controlSize(.small)
                        Text("正在更新屏保组件…").foregroundStyle(.secondary)
                    }
                }
                Button("打开系统屏保设置") { manager.openSystemSettings() }
                    .disabled(!status.isInstalled)
            } header: {
                Label("屏保组件", systemImage: "sparkles.tv")
            } footer: {
                Text("Mirage 会把独立的屏保组件安装到当前用户的“Library/Screen Savers”目录。安装后仍需在系统设置中选择 Mirage。")
                    .font(.caption)
            }

            Section {
                LabeledContent("当前屏保壁纸", value: status.configuredTitle)
                LabeledContent("正在播放", value: wallpaper.isValid ? wallpaper.project.title : "无")
                Button("将正在播放的壁纸设为屏保") {
                    perform { try configureCurrentWallpaper() }
                }
                .disabled(!wallpaper.isValid || (wallpaper.kind != .video && wallpaper.kind != .scene))
            } header: {
                Label("屏保壁纸", systemImage: "photo.on.rectangle.angled")
            } footer: {
                Text("屏保始终静音，并保存当前预设、自定义属性、填充方式和最高 60 FPS 的帧率设置。之后对同一壁纸的自定义修改会自动同步。")
                    .font(.caption)
            }

            Section {
                Text("视频和场景壁纸由 Mirage 自己的屏保宿主加载，不要求 Mirage 主程序保持运行。屏保始终保持静音。")
                    .foregroundStyle(.secondary)
            } header: {
                Label("运行方式", systemImage: "info.circle")
            }

            Section {
                Toggle("启用动态锁屏方案 A", isOn: Binding(
                    get: { dynamicLockScreenManager.isEnabled },
                    set: { dynamicLockScreenManager.setEnabled($0) }
                ))
                .disabled(!dynamicLockScreenManager.isAvailable)
                if let registrationErrorMessage = dynamicLockScreenManager.registrationErrorMessage {
                    Text(registrationErrorMessage)
                        .foregroundStyle(.red)
                }
                if !dynamicLockScreenManager.isAvailable {
                    Text(LocalizedStringKey("动态锁屏需要 macOS 26 或更高版本。"))
                        .foregroundStyle(.secondary)
                } else {
                    LabeledContent("方案 A 锁屏壁纸", value: dynamicLockScreenManager.configuredWallpaperTitle ?? L("尚未设置"))
                    Button("将正在播放的壁纸设为动态锁屏") {
                        perform { try configureCurrentDynamicLockScreen() }
                    }
                    .disabled(!dynamicLockScreenManager.canUse || !wallpaper.isValid || (wallpaper.kind != .video && wallpaper.kind != .scene))
                    Button("打开系统墙纸设置") {
                        dynamicLockScreenManager.openSystemSettings()
                    }
                    Text(LocalizedStringKey("动态锁屏使用逆向得到的系统 API，可能随 macOS 更新失效。动态锁屏支持视频和场景壁纸。启用后请前往墙纸内切换为Mirage动态锁屏。"))
                        .foregroundStyle(.secondary)
                }
                Toggle("启用动态锁屏方案 B", isOn: Binding(
                    get: { screenSaverDynamicLockScreenManager.isEnabled },
                    set: { screenSaverDynamicLockScreenManager.setEnabled($0) }
                ))
                .disabled(!screenSaverDynamicLockScreenManager.isAvailable)
                if !screenSaverDynamicLockScreenManager.isAvailable {
                    Text(LocalizedStringKey("动态锁屏方案 B 需要 macOS 14.2 或更高版本。"))
                        .foregroundStyle(.secondary)
                } else {
                    LabeledContent("方案 B 锁屏壁纸", value: screenSaverDynamicLockScreenManager.configuredWallpaperTitle ?? L("尚未设置"))
                    Button("将正在播放的壁纸设为方案 B 锁屏") {
                        perform { try configureCurrentScreenSaverDynamicLockScreen() }
                    }
                    .disabled(!screenSaverDynamicLockScreenManager.isEnabled || !wallpaper.isValid || (wallpaper.kind != .video && wallpaper.kind != .scene))
                    Text(LocalizedStringKey("方案 B 使用 Mirage 屏保组件，仅在锁屏时临时接管系统墙纸槽位，解锁后恢复原桌面配置。"))
                        .foregroundStyle(.secondary)
                }
            } header: {
                Label("动态锁屏", systemImage: "lock.rectangle")
            }
        }
        .formStyle(.grouped)
        .alert("屏保操作失败", isPresented: $showingError) {
            Button("好", role: .cancel) {}
        } message: {
            Text(message)
        }
        .alert("动态锁屏需要完全磁盘访问权限", isPresented: $showingFullDiskAccessPrompt) {
            Button("打开完全磁盘访问权限设置") {
                dynamicLockScreenManager.openFullDiskAccessSettings()
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("由于当前 Mirage 版本未使用开发者证书签名，macOS 不允许 Mirage 与动态锁屏扩展共享部署文件。请在“隐私与安全性 > 完全磁盘访问权限”中添加并启用 Mirage，然后重新打开 Mirage 并再次设置动态锁屏。")
        }
        .sheet(isPresented: $dynamicLockScreenManager.isConfirmationPresented) {
            DynamicLockScreenConfirmationSheet(manager: dynamicLockScreenManager)
        }
        .sheet(isPresented: $screenSaverDynamicLockScreenManager.isConfirmationPresented) {
            ScreenSaverDynamicLockScreenConfirmationSheet(manager: screenSaverDynamicLockScreenManager)
        }
    }

    private func configureCurrentWallpaper() throws {
        try manager.configure(
            with: wallpaperViewModel.currentWallpaper,
            runtime: wallpaperViewModel.runtime,
            properties: wallpaperViewModel.effectiveProperties(for: wallpaperViewModel.currentWallpaper),
            fps: Int(viewModel.settings.fps)
        )
    }

    private func configureCurrentDynamicLockScreen() throws {
        guard wallpaper.kind == .video || wallpaper.kind == .scene else {
            throw DynamicLockScreenError.unsupportedWallpaper
        }
        let displayIDs = wallpaperViewModel.connectedDisplays.compactMap { DisplayRegistry.shared.displayID(for: $0.key) }
        try dynamicLockScreenManager.configureCurrentWallpaper(
            wallpaperViewModel.currentWallpaper,
            runtime: wallpaperViewModel.runtime,
            properties: wallpaperViewModel.effectiveProperties(for: wallpaperViewModel.currentWallpaper),
            fps: Int(viewModel.settings.fps),
            displayIDs: displayIDs
        )
    }

    private func configureCurrentScreenSaverDynamicLockScreen() throws {
        try screenSaverDynamicLockScreenManager.configureCurrentWallpaper(
            wallpaperViewModel.currentWallpaper,
            runtime: wallpaperViewModel.runtime,
            properties: wallpaperViewModel.effectiveProperties(for: wallpaperViewModel.currentWallpaper),
            fps: Int(viewModel.settings.fps)
        )
    }

    private func perform(_ operation: () throws -> Void) {
        do {
            try operation()
            refreshStatus()
        } catch {
            show(error)
        }
    }

    private func performAsync(_ operation: @escaping () throws -> Void) {
        isWorking = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try operation() }
            DispatchQueue.main.async {
                isWorking = false
                switch result {
                case .success: refreshStatus()
                case .failure(let error): show(error)
                }
            }
        }
    }

    private func show(_ error: Error) {
        if case DynamicLockScreenError.fullDiskAccessRequired = error {
            showingFullDiskAccessPrompt = true
            return
        }
        message = error.localizedDescription
        showingError = true
    }
}
