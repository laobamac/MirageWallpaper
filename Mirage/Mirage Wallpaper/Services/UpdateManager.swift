//
//  UpdateManager.swift
//  Mirage Wallpaper
//

import Cocoa
import Sparkle

/// Owns Mirage's Sparkle updater and exposes the two user-facing update paths:
/// the regular channel and the opt-in beta channel.
@MainActor
final class UpdateManager: NSObject, SPUUpdaterDelegate {
    static let shared = UpdateManager()

    private lazy var updaterController = SPUStandardUpdaterController(
        startingUpdater: false,
        updaterDelegate: self,
        userDriverDelegate: nil
    )

    private var hasStarted = false

    private override init() {
        super.init()
    }

    func start() {
        guard !hasStarted else { return }
        hasStarted = true
        updaterController.startUpdater()
        applyAutomaticUpdatePreference()
    }

    @objc func checkForUpdates(_ sender: Any?) {
        updaterController.checkForUpdates(sender)
    }

    func applyAutomaticUpdatePreference() {
        let enabled = AppDelegate.shared.globalSettingsViewModel.settings.shouldAutomaticallyUpdate
        updaterController.updater.automaticallyChecksForUpdates = enabled
        updaterController.updater.automaticallyDownloadsUpdates = enabled
    }

    func allowedChannels(for updater: SPUUpdater) -> Set<String> {
        guard AppDelegate.shared.globalSettingsViewModel.settings.shouldReceivePrereleaseUpdates else {
            return []
        }
        return ["beta"]
    }
}
