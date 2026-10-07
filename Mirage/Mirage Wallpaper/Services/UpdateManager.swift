//
//  UpdateManager.swift
//  Mirage Wallpaper
//

import Cocoa
import Combine
import Sparkle

/// Owns Mirage's Sparkle updater and exposes the two user-facing update paths:
/// the regular channel and the opt-in beta channel.
@MainActor
final class UpdateManager: NSObject, ObservableObject, SPUUpdaterDelegate, @preconcurrency SPUStandardUserDriverDelegate {
    static let shared = UpdateManager()

    private enum Status: Equatable {
        case idle(Date?)
        case disabled
        case checking
        case available(String)
        case downloading(String)
        case readyToInstall(String)
        case current(Date)
        case noCompatibleUpdate(Date)
        case failed(String)
    }

    @Published private var status: Status = .idle(nil)

    private lazy var updaterController = SPUStandardUpdaterController(
        startingUpdater: false,
        updaterDelegate: self,
        userDriverDelegate: self
    )

    private var hasStarted = false

    private override init() {
        super.init()
    }

    func start() {
        guard !hasStarted else { return }
        let enabled = AppDelegate.shared.globalSettingsViewModel.settings.shouldAutomaticallyUpdate
        updaterController.updater.automaticallyChecksForUpdates = enabled
        updaterController.updater.automaticallyDownloadsUpdates = enabled
        status = enabled ? .idle(updaterController.updater.lastUpdateCheckDate) : .disabled
        do {
            try updaterController.updater.start()
        } catch {
            status = .failed(error.localizedDescription)
            return
        }
        hasStarted = true
        if enabled {
            status = .checking
            updaterController.updater.checkForUpdatesInBackground()
        }
    }

    @objc func checkForUpdates(_ sender: Any?) {
        status = .checking
        updaterController.checkForUpdates(sender)
    }

    func applyAutomaticUpdatePreference() {
        let enabled = AppDelegate.shared.globalSettingsViewModel.settings.shouldAutomaticallyUpdate
        updaterController.updater.automaticallyChecksForUpdates = enabled
        updaterController.updater.automaticallyDownloadsUpdates = enabled
        guard hasStarted else { return }
        if enabled {
            guard updaterController.updater.canCheckForUpdates else { return }
            status = .checking
            updaterController.updater.checkForUpdatesInBackground()
        } else {
            status = .disabled
        }
    }

    func applyUpdateChannelPreference() {
        guard hasStarted else { return }
        updaterController.updater.resetUpdateCycle()
    }

    var isChecking: Bool {
        status == .checking
    }

    var statusSymbolName: String {
        switch status {
        case .idle:
            return "clock"
        case .disabled:
            return "pause.circle"
        case .checking, .downloading:
            return "arrow.triangle.2.circlepath"
        case .available:
            return "arrow.down.circle"
        case .readyToInstall:
            return "checkmark.circle.fill"
        case .current, .noCompatibleUpdate:
            return "checkmark.circle"
        case .failed:
            return "exclamationmark.triangle"
        }
    }

    var statusText: String {
        switch status {
        case .idle(let date):
            guard let date else { return L("尚未检查更新") }
            return L("上次检查：%@", formatted(date))
        case .disabled:
            return L("自动更新已关闭")
        case .checking:
            return L("正在检查更新…")
        case .available(let version):
            return L("发现新版本 %@", version)
        case .downloading(let version):
            return L("正在下载版本 %@…", version)
        case .readyToInstall(let version):
            return L("版本 %@ 已就绪，将在退出 Mirage 后安装", version)
        case .current(let date):
            return L("已是最新版本 · %@", formatted(date))
        case .noCompatibleUpdate(let date):
            return L("未找到适用于此 Mac 的更新 · %@", formatted(date))
        case .failed(let message):
            return L("检查更新失败：%@", message)
        }
    }

    private func formatted(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = MirageLocalization.shared.locale
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    func allowedChannels(for updater: SPUUpdater) -> Set<String> {
        guard AppDelegate.shared.globalSettingsViewModel.settings.shouldReceivePrereleaseUpdates else {
            return []
        }
        return ["beta"]
    }

    func updater(_ updater: SPUUpdater, didFindValidUpdate item: SUAppcastItem) {
        status = .available(item.displayVersionString)
    }

    func updaterDidNotFindUpdate(_ updater: SPUUpdater, error: Error) {
        let reasonValue = (error as NSError).userInfo[SPUNoUpdateFoundReasonKey] as? NSNumber
        let reason = reasonValue.flatMap { SPUNoUpdateFoundReason(rawValue: $0.int32Value) }
        if reason == .onLatestVersion || reason == .onNewerThanLatestVersion {
            status = .current(Date())
        } else {
            status = .noCompatibleUpdate(Date())
        }
    }

    func updater(_ updater: SPUUpdater, willDownloadUpdate item: SUAppcastItem, with request: NSMutableURLRequest) {
        status = .downloading(item.displayVersionString)
    }

    func updater(_ updater: SPUUpdater, didDownloadUpdate item: SUAppcastItem) {
        status = .readyToInstall(item.displayVersionString)
    }

    func updater(_ updater: SPUUpdater, failedToDownloadUpdate item: SUAppcastItem, error: Error) {
        status = .failed(error.localizedDescription)
    }

    func updater(
        _ updater: SPUUpdater,
        willInstallUpdateOnQuit item: SUAppcastItem,
        immediateInstallationBlock immediateInstallHandler: @escaping () -> Void
    ) -> Bool {
        status = .readyToInstall(item.displayVersionString)
        return false
    }

    func updater(_ updater: SPUUpdater, didFinishUpdateCycleFor updateCheck: SPUUpdateCheck, error: Error?) {
        if let error {
            let nsError = error as NSError
            if nsError.domain == SUSparkleErrorDomain && nsError.code == SUError.noUpdateError.rawValue {
                if case .checking = status {
                    status = .noCompatibleUpdate(Date())
                }
            } else if nsError.domain == SUSparkleErrorDomain &&
                        (nsError.code == SUError.installationCanceledError.rawValue ||
                         nsError.code == SUError.installationAuthorizeLaterError.rawValue) {
                if case .checking = status {
                    status = .idle(updater.lastUpdateCheckDate)
                }
            } else {
                status = .failed(error.localizedDescription)
            }
        } else if case .checking = status {
            status = .current(Date())
        }
    }

    var supportsGentleScheduledUpdateReminders: Bool {
        true
    }

    func standardUserDriverShouldHandleShowingScheduledUpdate(
        _ update: SUAppcastItem,
        andInImmediateFocus immediateFocus: Bool
    ) -> Bool {
        true
    }

    func standardUserDriverWillHandleShowingUpdate(
        _ handleShowingUpdate: Bool,
        forUpdate update: SUAppcastItem,
        state: SPUUserUpdateState
    ) {
        switch state.stage {
        case .downloaded, .installing:
            status = .readyToInstall(update.displayVersionString)
        case .notDownloaded:
            status = .available(update.displayVersionString)
        @unknown default:
            status = .available(update.displayVersionString)
        }
        guard handleShowingUpdate else { return }
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func standardUserDriverWillFinishUpdateSession() {
        DispatchQueue.main.async {
            AppDelegate.shared.hideDockIconIfNoWindowsAreVisible()
        }
    }
}
