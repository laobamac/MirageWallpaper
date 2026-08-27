//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Combine
import Foundation
import Observation

@MainActor
@Observable
final class SteamSessionStore {
    private(set) var setupState: SteamSetupState = .checking
    private(set) var serviceStatus = SteamServiceStatus()
    var logoutResultMessage: String?
    private(set) var isLoggingOut = false

    @ObservationIgnored private var cancellables = Set<AnyCancellable>()
    @ObservationIgnored private var onLoggedIn: () -> Void = {}
    @ObservationIgnored private var onLoggedOut: () -> Void = {}

    init() {
        let manager = SteamServiceManager.shared
        manager.$isLoggedIn
            .receive(on: RunLoop.main)
            .sink { [weak self] isLoggedIn in
                guard let self else { return }
                self.refreshSetupState()
                if isLoggedIn {
                    self.onLoggedIn()
                }
            }
            .store(in: &cancellables)

        manager.$authenticationState
            .receive(on: RunLoop.main)
            .sink { [weak self] state in
                guard let self else { return }
                self.serviceStatus.authentication = state
                self.refreshSetupState()
            }
            .store(in: &cancellables)

        manager.$isAvailable
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.refreshSetupState()
            }
            .store(in: &cancellables)
    }

    func configure(onLoggedIn: @escaping () -> Void, onLoggedOut: @escaping () -> Void) {
        self.onLoggedIn = onLoggedIn
        self.onLoggedOut = onLoggedOut
        refreshSetupState()
        if SteamServiceManager.shared.isLoggedIn {
            onLoggedIn()
        }
    }

    var checkingMessage: String {
        SteamServiceManager.shared.savedUsername.isEmpty
            ? L("正在连接 Steam…")
            : L("正在恢复 Steam 会话…")
    }

    var isReady: Bool {
        setupState == .ready
    }

    func checkSetup() {
        let manager = SteamServiceManager.shared
        serviceStatus.client = manager.isAvailable
            ? .available(L("内置 Steam 服务可用"))
            : .checking
        serviceStatus.authentication = manager.authenticationState
        manager.start()
        refreshSetupState()
    }

    func openSetupIfActionable() {
        if setupState == .needsLogin || setupState == .serviceUnavailable {
            AppDelegate.shared.openSteamSetup()
        }
    }

    func updateBrowsingAPIState(_ state: SteamServiceState) {
        serviceStatus.browsingAPI = state
    }

    func handleDownloadState(_ state: DownloadState) {
        switch state {
        case .completed:
            serviceStatus.workshopDownload = .available(L("最近一次下载已验证"))
        case .failed:
            serviceStatus.workshopDownload = .unavailable(L("最近一次下载失败"))
        case .resolving:
            serviceStatus.workshopDownload = .checking
        case .queued, .downloading, .validating:
            break
        }
    }

    func logout() {
        guard !isLoggingOut else { return }
        isLoggingOut = true
        serviceStatus.authentication = .checking
        SteamServiceManager.shared.logout { [weak self] result in
            guard let self else { return }
            self.isLoggingOut = false
            switch result {
            case .success:
                self.serviceStatus.authentication = .needsAction(L("已退出登录"))
                self.logoutResultMessage = L("已退出 Mirage 的 Steam 会话。")
                self.onLoggedOut()
            case .failure(let error):
                self.serviceStatus.authentication = .needsAction(error.localizedDescription)
                self.logoutResultMessage = error.localizedDescription
            }
            self.refreshSetupState()
        }
    }

    static func resolveSetupState(
        isAvailable: Bool,
        isLoggedIn: Bool,
        authenticationState: SteamServiceState
    ) -> SteamSetupState {
        if isAvailable && isLoggedIn {
            return .ready
        }
        switch authenticationState {
        case .unknown, .checking:
            return .checking
        case .available, .needsAction, .unavailable:
            return isAvailable ? .needsLogin : .serviceUnavailable
        }
    }

    private func refreshSetupState() {
        let manager = SteamServiceManager.shared
        setupState = Self.resolveSetupState(
            isAvailable: manager.isAvailable,
            isLoggedIn: manager.isLoggedIn,
            authenticationState: manager.authenticationState
        )
        serviceStatus.authentication = manager.authenticationState
        switch setupState {
        case .checking:
            serviceStatus.client = manager.isAvailable
                ? .available(L("内置 Steam 服务可用"))
                : .checking
            serviceStatus.workshopDownload = .checking
        case .serviceUnavailable:
            serviceStatus.client = .unavailable(L("Steam 服务组件不可用"))
            serviceStatus.workshopDownload = .needsAction(L("Steam 服务尚未就绪"))
        case .needsLogin:
            serviceStatus.client = .available(L("内置 Steam 服务可用"))
            if manager.savedUsername.isEmpty {
                serviceStatus.authentication = .needsAction(L("需要登录 Steam"))
            }
            serviceStatus.workshopDownload = .needsAction(L("需要有效的 Steam 会话"))
        case .ready:
            serviceStatus.client = .available(L("内置 Steam 服务可用"))
            if serviceStatus.workshopDownload == .unknown ||
                serviceStatus.workshopDownload == .checking {
                serviceStatus.workshopDownload = .needsAction(L("尚未开始下载"))
            }
        }
    }
}
