//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Combine

class SteamSetupViewModel: ObservableObject {
    @Published var currentStep: Int = 0
    @Published var steamCMDPath: URL?
    @Published var steamCMDInstallState: SteamCMDInstallState = .detecting
    @Published var loginState: SteamLoginState = .idle

    @Published var username: String = ""
    @Published var password: String = ""
    @Published var guardCode: String = ""
    @Published var errorMessage: String?

    @Published var loginLog: [String] = []
    @Published private(set) var guardWaitElapsed: Int = 0
    @Published private(set) var reusableSessionUsername: String?

    private var guardWaitStartedAt: Date?
    private var guardWaitTimer: Timer?
    private var lastGuardHeartbeat = 0
    private var sessionCancellable: AnyCancellable?

    let totalSteps = 4

    var canProceed: Bool {
        switch currentStep {
        case 0:
            return true
        case 1:
            switch steamCMDInstallState {
            case .found, .installed: return true
            default: return false
            }
        case 2:
            return loginState == .success
        case 3:
            return true
        default:
            return false
        }
    }

    init() {
        let manager = SteamCMDManager.shared
        username = manager.savedUsername
        reusableSessionUsername = manager.isLoggedIn && !manager.savedUsername.isEmpty
            ? manager.savedUsername
            : nil
        sessionCancellable = manager.$isLoggedIn
            .receive(on: RunLoop.main)
            .sink { [weak self] isLoggedIn in
                guard let self else { return }
                let savedUsername = SteamCMDManager.shared.savedUsername
                self.reusableSessionUsername = isLoggedIn && !savedUsername.isEmpty ? savedUsername : nil
            }
    }

    func useSavedSession() {
        guard let reusableSessionUsername, SteamCMDManager.shared.isLoggedIn else {
            errorMessage = "保存的 Steam 会话已失效，请使用密码重新登录"
            return
        }
        username = reusableSessionUsername
        password = ""
        guardCode = ""
        errorMessage = nil
        loginLog = ["[Mirage] 已使用验证有效的本机 SteamCMD 会话"]
        loginState = .success
    }

    func detectSteamCMD() {
        steamCMDInstallState = .detecting
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let found = SteamCMDManager.shared.detectSteamCMD()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                switch found {
                case .found(let path):
                    self?.steamCMDPath = path
                    self?.steamCMDInstallState = .found(path.path)
                case .rosettaRequired:
                    self?.steamCMDInstallState = .rosettaRequired
                case .notFound:
                    self?.steamCMDInstallState = .notFound
                }
            }
        }
    }

    func installSteamCMD() {
        SteamCMDManager.shared.installSteamCMD { [weak self] state in
            self?.steamCMDInstallState = state
            if case .installed(let path) = state {
                self?.steamCMDPath = URL(fileURLWithPath: path)
            }
        }
    }

    func cancelSteamCMDInstallation() {
        SteamCMDManager.shared.cancelSteamCMDInstallation()
    }

    func login() {
        guard !username.isEmpty, !password.isEmpty else {
            errorMessage = "请输入用户名和密码"
            return
        }
        stopGuardWaitUpdates()
        errorMessage = nil
        loginLog.removeAll()
        SteamCMDManager.shared.login(
            username: username,
            password: password,
            onLog: { [weak self] line in
                self?.loginLog.append(line)
            }
        ) { [weak self] state in
            guard let self else { return }
            if self.loginState == .success, state != .success { return }
            self.loginState = state
            if case .waitingForGuard(.mobileConfirm) = state {
                self.startGuardWaitUpdates()
            } else if state != .loggingIn {
                self.stopGuardWaitUpdates()
            }
            if case .failed(let msg) = state {
                self.errorMessage = msg
            }
            if case .success = state {
                self.password = ""
                self.guardCode = ""
            }
            if case .waitingForGuard = state {
                self.errorMessage = nil
            }
        }
    }

    func submitGuardCode() {
        let code = guardCode.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !code.isEmpty else {
            errorMessage = "请输入验证码"
            return
        }
        guard SteamCMDManager.shared.submitGuardCode(code) else {
            errorMessage = "Steam Guard 会话已结束，请重新登录"
            loginState = .failed("Steam Guard 会话已结束")
            return
        }
        guardCode = ""
        errorMessage = nil
        loginState = .loggingIn
    }

    func cancelLogin() {
        SteamCMDManager.shared.cancelLogin()
        stopGuardWaitUpdates()
        password = ""
        guardCode = ""
        loginState = .idle
        errorMessage = nil
    }

    func cancelPendingWork() {
        SteamCMDManager.shared.cancelLogin()
        SteamCMDManager.shared.cancelSteamCMDInstallation()
        stopGuardWaitUpdates()
        password = ""
        guardCode = ""
    }

    func reset() {
        cancelPendingWork()
        currentStep = 0
        steamCMDPath = SteamCMDManager.shared.steamCMDPath
        steamCMDInstallState = .detecting
        loginState = .idle
        errorMessage = nil
        loginLog.removeAll()
        username = SteamCMDManager.shared.savedUsername
    }

    func completeSetup() {
        SteamCMDManager.shared.savedUsername = username
    }

    func nextStep() {
        guard currentStep < totalSteps - 1 else { return }
        currentStep += 1
    }

    func previousStep() {
        guard currentStep > 0 else { return }
        if currentStep == 2 { cancelLogin() }
        if currentStep == 1 { cancelSteamCMDInstallation() }
        currentStep -= 1
    }

    private func startGuardWaitUpdates() {
        guard guardWaitTimer == nil else { return }
        guardWaitStartedAt = Date()
        guardWaitElapsed = 0
        lastGuardHeartbeat = 0
        let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
            guard let self, let startedAt = self.guardWaitStartedAt else { return }
            let elapsed = max(0, Int(Date().timeIntervalSince(startedAt)))
            self.guardWaitElapsed = elapsed
            let heartbeat = elapsed / 5 * 5
            if heartbeat >= 5, heartbeat != self.lastGuardHeartbeat {
                self.lastGuardHeartbeat = heartbeat
                self.loginLog.append("[Mirage] Steam 手机确认仍在等待（\(heartbeat) 秒）")
            }
        }
        guardWaitTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func stopGuardWaitUpdates() {
        guardWaitTimer?.invalidate()
        guardWaitTimer = nil
        guardWaitStartedAt = nil
        guardWaitElapsed = 0
        lastGuardHeartbeat = 0
    }

    deinit {
        guardWaitTimer?.invalidate()
    }
}
