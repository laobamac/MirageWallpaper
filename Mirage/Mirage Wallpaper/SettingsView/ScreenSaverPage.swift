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
    @State private var status: Status
    @State private var message = ""
    @State private var showingError = false
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
                    Label(status.isInstalled ? "Mirage 动态屏保已安装" : "Mirage 动态屏保尚未安装",
                          systemImage: status.isInstalled ? "checkmark.seal.fill" : "rectangle.dashed")
                        .foregroundStyle(status.isInstalled ? .green : .secondary)
                    Spacer()
                    Button(status.isInstalled ? "重新安装" : "安装") {
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
                .disabled(!wallpaper.isValid || wallpaper.kind == .unsupported)
            } header: {
                Label("屏保壁纸", systemImage: "photo.on.rectangle.angled")
            } footer: {
                Text("屏保始终静音，并保存当前预设、自定义属性、填充方式和最高 60 FPS 的帧率设置。之后对同一壁纸的自定义修改会自动同步。")
                    .font(.caption)
            }

            Section {
                Text("视频、网页和场景壁纸由 Mirage 自己的屏保宿主加载，不要求 Mirage 主程序保持运行。网页屏保不会获得网络导航权限，音频响应在屏保环境中保持静音。")
                    .foregroundStyle(.secondary)
            } header: {
                Label("运行方式", systemImage: "info.circle")
            }
        }
        .formStyle(.grouped)
        .alert("屏保操作失败", isPresented: $showingError) {
            Button("好", role: .cancel) {}
        } message: {
            Text(message)
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
        message = error.localizedDescription
        showingError = true
    }
}
