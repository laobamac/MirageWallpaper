//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Darwin

/// Process state is guarded by `processLock`; all published state is written on
/// the main queue. SteamCMD itself must run on background queues.
final class SteamCMDManager: ObservableObject, @unchecked Sendable {
    static let shared = SteamCMDManager()

    static let bootstrapURL = URL(string: "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz")!
    static let bootstrapDomain = "steamcdn-a.akamaihd.net"

    @Published private(set) var steamCMDPath: URL?
    @Published private(set) var isLoggedIn = false
    @Published private(set) var authenticationState: SteamServiceState = .unknown
    @Published private(set) var diagnosticEvents: [SteamDiagnosticEvent] = []

    private let fm = FileManager.default
    private let processLock = NSLock()
    private var downloadProcesses: [String: Process] = [:]
    private var cancelledDownloadIDs: Set<String> = []
    private var activeLoginProcess: Process?
    private var activeLoginMasterFD: Int32?
    private var activeLoginWaitingForGuard = false
    private var activeLoginCancelled = false
    private var installationInProgress = false
    private var installationCancelled = false
    private var activeInstallProcess: Process?
    private var bootstrapDownloadTask: URLSessionDownloadTask?
    private var hasRefreshedSession = false
    private var workshopSession: SteamCMDWorkshopSession?
    private var workshopSessionStarting = false

    private let steamCMDDir: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/steamcmd")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: dir.path)
        return dir
    }()

    private let usernameKey = "SteamCMDUsername"
    private let pathKey = "SteamCMDPath"
    private var installationMarker: URL { steamCMDDir.appending(path: ".mirage-ready") }

    var savedUsername: String {
        get { UserDefaults.standard.string(forKey: usernameKey) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: usernameKey) }
    }

    var steamCMDContentDirectory: URL {
        steamCMDDir.appending(path: "steamapps/workshop/content/431960")
    }

    private var steamCMDHomeDirectory: URL {
        let directory = steamCMDDir.appending(path: "home")
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        try? fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
        return directory
    }

    private var isolatedSteamDataDirectory: URL {
        steamCMDHomeDirectory.appending(path: "Library/Application Support/Steam")
    }

    var isolatedSteamCMDContentDirectory: URL {
        isolatedSteamDataDirectory.appending(path: "steamapps/workshop/content/431960")
    }

    var steamCMDContentDirectories: [URL] {
        [
            steamCMDContentDirectory,
            isolatedSteamCMDContentDirectory
        ]
    }

    init() {
        if let path = UserDefaults.standard.string(forKey: pathKey), !path.isEmpty {
            let url = preferredLauncher(for: URL(fileURLWithPath: path))
            if isReadyLauncher(url) {
                steamCMDPath = url
            }
        }
        if !savedUsername.isEmpty,
           fm.fileExists(atPath: isolatedSteamDataDirectory.appending(path: "config/config.vdf").path) {
            isLoggedIn = true
            authenticationState = .available("已保存会话，下载时自动验证")
            hasRefreshedSession = true
            DispatchQueue.global(qos: .utility).async { [weak self] in
                self?.refreshSessionIfNeeded()
            }
        }
    }

    // MARK: - Redacted support log

    func diagnosticReport() -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let lines = diagnosticEvents.map {
            "[\(formatter.string(from: $0.timestamp))] [\($0.category.rawValue)] [\($0.domain)] \($0.message)"
        }
        return ([
            "Mirage Steam 创意工坊支持报告（已脱敏）",
            "生成时间：\(formatter.string(from: Date()))",
            "系统：\(ProcessInfo.processInfo.operatingSystemVersionString)",
            "SteamCMD：\(steamCMDPath?.path ?? "未安装")",
            ""
        ] + lines).joined(separator: "\n")
    }

    private func record(_ category: SteamDiagnosticCategory, domain: String, _ message: String, secrets: [String] = []) {
        let event = SteamDiagnosticEvent(
            timestamp: Date(),
            category: category,
            domain: domain,
            message: redact(message, secrets: secrets)
        )
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.diagnosticEvents.append(event)
            if self.diagnosticEvents.count > 500 {
                self.diagnosticEvents.removeFirst(self.diagnosticEvents.count - 500)
            }
        }
    }

    private func redact(_ text: String, secrets: [String] = []) -> String {
        var result = text
        for secret in secrets where !secret.isEmpty {
            result = result.replacingOccurrences(of: secret, with: "[已隐藏]")
        }
        let patterns = [
            "(?i)(key|api[_-]?key|token|access[_-]?token|refresh[_-]?token|password)\\s*[=:]\\s*[^\\s&]+",
            "(?i)([?&](?:key|token|access_token|password)=)[^&\\s]+"
        ]
        for pattern in patterns {
            guard let regex = try? NSRegularExpression(pattern: pattern) else { continue }
            let range = NSRange(result.startIndex..., in: result)
            result = regex.stringByReplacingMatches(in: result, range: range, withTemplate: "$1[已隐藏]")
        }
        return result
    }

    // MARK: - Detect

    func detectSteamCMD() -> URL? {
        if let steamCMDPath, isReadyLauncher(steamCMDPath) {
            return steamCMDPath
        }

        let managedLauncher = steamCMDDir.appending(path: "steamcmd.sh")
        if isUsableLauncher(managedLauncher), !fm.fileExists(atPath: installationMarker.path) {
            record(.steamCMDInstall, domain: "SteamCMD", "正在验证旧版 SteamCMD 安装状态")
            let health = runWithPTY(executable: managedLauncher, arguments: ["+quit"], onLine: { line in
                self.record(.steamCMDInstall, domain: "SteamCMD", line)
            }, timeout: 300)
            if health.status == 0, !health.timedOut,
               (try? Data("ready".utf8).write(to: installationMarker, options: .atomic)) != nil {
                return saveSteamCMDPath(managedLauncher)
            }
        }

        let candidates = [
            managedLauncher.path,
            "/opt/homebrew/bin/steamcmd",
            "/usr/local/bin/steamcmd",
            NSHomeDirectory() + "/steamcmd/steamcmd.sh",
        ]

        for path in candidates {
            let url = preferredLauncher(for: URL(fileURLWithPath: path))
            if isReadyLauncher(url) {
                return saveSteamCMDPath(url)
            }
        }

        if let whichResult = try? runShellSync("/usr/bin/which", arguments: ["steamcmd"]),
           whichResult.status == 0 {
            let path = whichResult.output.trimmingCharacters(in: .whitespacesAndNewlines)
            let url = preferredLauncher(for: URL(fileURLWithPath: path))
            if isReadyLauncher(url) {
                return saveSteamCMDPath(url)
            }
        }

        return nil
    }

    private func saveSteamCMDPath(_ url: URL) -> URL {
        if Thread.isMainThread {
            steamCMDPath = url
        } else {
            DispatchQueue.main.sync { [weak self] in self?.steamCMDPath = url }
        }
        UserDefaults.standard.set(url.path, forKey: pathKey)
        return url
    }

    private func preferredLauncher(for url: URL) -> URL {
        guard url.lastPathComponent == "steamcmd" else { return url }
        let script = url.deletingLastPathComponent().appending(path: "steamcmd.sh")
        return fm.isReadableFile(atPath: script.path) ? script : url
    }

    private func isUsableLauncher(_ url: URL) -> Bool {
        url.pathExtension == "sh" ? fm.isReadableFile(atPath: url.path) : fm.isExecutableFile(atPath: url.path)
    }

    private func isReadyLauncher(_ url: URL) -> Bool {
        guard isUsableLauncher(url) else { return false }
        let managedPath = url.standardizedFileURL.path.hasPrefix(steamCMDDir.standardizedFileURL.path + "/")
        return !managedPath || fm.fileExists(atPath: installationMarker.path)
    }

    // MARK: - Install

    func installSteamCMD(onProgress: @escaping (SteamCMDInstallState) -> Void) {
        processLock.lock()
        let canStart = !installationInProgress && activeLoginProcess == nil && downloadProcesses.isEmpty &&
            workshopSession?.process.isRunning != true && !workshopSessionStarting
        if canStart {
            installationInProgress = true
            installationCancelled = false
            try? fm.removeItem(at: installationMarker)
        }
        processLock.unlock()

        guard canStart else {
            onProgress(.failed("SteamCMD 正在安装，请等待当前任务结束"))
            return
        }

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            defer {
                self.processLock.lock()
                self.installationInProgress = false
                self.installationCancelled = false
                self.activeInstallProcess = nil
                self.processLock.unlock()
            }

            self.record(.steamCMDInstall, domain: Self.bootstrapDomain, "开始下载 SteamCMD 官方 bootstrap")
            DispatchQueue.main.async { onProgress(.downloading(0)) }

            do {
                try self.throwIfInstallationCancelled()
                try self.ensureSufficientDiskSpace(minimumBytes: 150 * 1024 * 1024)
                let archive = try self.downloadBootstrap { progress in
                    DispatchQueue.main.async { onProgress(.downloading(progress)) }
                }
                defer { try? self.fm.removeItem(at: archive) }
                try self.throwIfInstallationCancelled()

                DispatchQueue.main.async { onProgress(.extracting) }
                try self.validateArchive(at: archive)
                try self.extractArchive(at: archive)
                try self.throwIfInstallationCancelled()

                let execPath = self.steamCMDDir.appending(path: "steamcmd.sh")
                _ = try? self.runShellSync("/bin/chmod", arguments: ["+x", execPath.path])
                guard self.isUsableLauncher(execPath) else {
                    throw SteamCMDError.installFailed("解压完成后未找到可执行的 steamcmd.sh")
                }

                DispatchQueue.main.async { onProgress(.initializing) }
                let health = self.runWithPTY(executable: execPath, arguments: ["+quit"], onLine: { line in
                    self.record(.steamCMDInstall, domain: "SteamCMD", line)
                }, timeout: 300, onProcessStart: { process in
                    self.processLock.lock()
                    self.activeInstallProcess = process
                    let cancelled = self.installationCancelled
                    self.processLock.unlock()
                    if cancelled { self.terminate(process) }
                })
                try self.throwIfInstallationCancelled()
                guard health.status == 0, !health.timedOut else {
                    throw SteamCMDError.installFailed(
                        health.timedOut ? "SteamCMD 首次初始化超时，请检查网络后重试" : "SteamCMD 首次初始化失败 (exit \(health.status))"
                    )
                }

                try Data("ready".utf8).write(to: self.installationMarker, options: .atomic)
                _ = self.saveSteamCMDPath(execPath)
                self.record(.steamCMDInstall, domain: Self.bootstrapDomain, "SteamCMD 安装并完成首次初始化")
                DispatchQueue.main.async { onProgress(.installed(execPath.path)) }
            } catch is CancellationError {
                self.record(.steamCMDInstall, domain: Self.bootstrapDomain, "SteamCMD 安装已取消")
                DispatchQueue.main.async { onProgress(.failed("SteamCMD 安装已取消")) }
            } catch {
                self.record(.steamCMDInstall, domain: Self.bootstrapDomain, "安装失败：\(error.localizedDescription)")
                DispatchQueue.main.async { onProgress(.failed(error.localizedDescription)) }
            }
        }
    }

    func cancelSteamCMDInstallation() {
        processLock.lock()
        let task = bootstrapDownloadTask
        let process = activeInstallProcess
        if installationInProgress { installationCancelled = true }
        processLock.unlock()
        task?.cancel()
        terminate(process)
        if task != nil || process != nil { record(.steamCMDInstall, domain: Self.bootstrapDomain, "用户取消 SteamCMD 安装") }
    }

    private func throwIfInstallationCancelled() throws {
        processLock.lock(); let cancelled = installationCancelled; processLock.unlock()
        if cancelled { throw CancellationError() }
    }

    private func ensureSufficientDiskSpace(minimumBytes: Int64) throws {
        let values = try steamCMDDir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        if let available = values.volumeAvailableCapacityForImportantUsage, available < minimumBytes {
            throw SteamCMDError.installFailed("可用磁盘空间不足，请至少预留 150 MB 后重试")
        }
    }

    private func downloadBootstrap(onProgress: @escaping (Double) -> Void) throws -> URL {
        let archive = steamCMDDir.appending(path: "steamcmd_osx.tar.gz")
        if fm.fileExists(atPath: archive.path) { try fm.removeItem(at: archive) }
        let delegate = SteamCMDDownloadDelegate(destination: archive, onProgress: onProgress)
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 180
        let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
        let task = session.downloadTask(with: Self.bootstrapURL)
        processLock.lock(); bootstrapDownloadTask = task; processLock.unlock()
        task.resume()
        delegate.semaphore.wait()
        session.finishTasksAndInvalidate()
        processLock.lock(); bootstrapDownloadTask = nil; processLock.unlock()

        if let error = delegate.error {
            processLock.lock(); let cancelled = installationCancelled; processLock.unlock()
            if cancelled { throw CancellationError() }
            throw SteamCMDError.downloadFailed(error.localizedDescription)
        }
        guard let response = delegate.response as? HTTPURLResponse,
              (200...299).contains(response.statusCode),
              let tempURL = delegate.downloadedURL else {
            throw SteamCMDError.downloadFailed("服务器未返回有效的 SteamCMD 安装包")
        }
        let size = (try? tempURL.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard size > 0 else { throw SteamCMDError.downloadFailed("SteamCMD 安装包为空") }
        self.record(.steamCMDInstall, domain: Self.bootstrapDomain, "下载完成（\(size) bytes）")
        return archive
    }

    private func validateArchive(at archive: URL) throws {
        let listing = try runShellSync("/usr/bin/tar", arguments: ["-tzf", archive.path])
        guard listing.status == 0 else {
            throw SteamCMDError.installFailed("SteamCMD 安装包无法校验：\(listing.output.prefix(200))")
        }
        let paths = listing.output.split(separator: "\n").map(String.init)
        guard paths.contains(where: { $0 == "steamcmd" || $0.hasSuffix("/steamcmd") }),
              paths.contains(where: { $0 == "steamcmd.sh" || $0.hasSuffix("/steamcmd.sh") }),
              !paths.contains(where: { $0.hasPrefix("/") || $0.split(separator: "/").contains("..") }) else {
            throw SteamCMDError.installFailed("SteamCMD 安装包内容异常")
        }
    }

    private func extractArchive(at archive: URL) throws {
        let result = try runShellSync("/usr/bin/tar", arguments: ["-xzf", archive.path, "-C", steamCMDDir.path])
        guard result.status == 0 else {
            throw SteamCMDError.installFailed("解压 SteamCMD 失败：\(result.output.prefix(200))")
        }
    }

    // MARK: - Session

    func refreshSessionIfNeeded() {
        processLock.lock()
        let shouldRefresh = steamCMDPath != nil && !savedUsername.isEmpty &&
            activeLoginProcess == nil && !installationInProgress && downloadProcesses.isEmpty &&
            workshopSession == nil && !workshopSessionStarting
        if shouldRefresh { hasRefreshedSession = true }
        processLock.unlock()
        guard shouldRefresh else { return }
        setAuthenticationState(.checking)
        startWorkshopSession(task: nil)
    }

    /// SteamCMD receives the password through the PTY, never through process arguments.
    /// The same PTY stays open for Steam Guard, so entering a code cannot race a second login process.
    func login(username: String, password: String,
               onLog: @escaping (String) -> Void,
               onResult: @escaping (SteamLoginState) -> Void) {
        guard steamCMDPath != nil else {
            onResult(.failed("SteamCMD 未安装"))
            return
        }
        guard beginLoginSession() else {
            onResult(.failed("SteamCMD 正在执行其他任务，请先等待或取消当前任务"))
            return
        }

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            self.setAuthenticationState(.checking)
            DispatchQueue.main.async { onResult(.loggingIn) }
            self.record(.authentication, domain: "Steam 登录服务", "开始 SteamCMD 登录（密码不会写入命令行或日志）")

            var masterFD: Int32 = 0
            var slaveFD: Int32 = 0
            guard openpty(&masterFD, &slaveFD, nil, nil, nil) == 0 else {
                _ = self.finishLoginSession(process: nil)
                self.setAuthenticationState(.unavailable("无法创建安全登录终端"))
                DispatchQueue.main.async { onResult(.failed("无法创建伪终端")) }
                return
            }
            self.disableTerminalEcho(slaveFD: slaveFD)

            guard let cmdPath = self.steamCMDPath else {
                close(masterFD); close(slaveFD)
                _ = self.finishLoginSession(process: nil)
                DispatchQueue.main.async { onResult(.failed("SteamCMD 未安装")) }
                return
            }

            let process = Process()
            let command = self.command(for: cmdPath, arguments: ["+login", username, "+quit"])
            process.executableURL = command.executable
            process.arguments = command.arguments
            process.currentDirectoryURL = self.steamCMDDir
            self.configureSteamCMDProcess(process)
            process.standardInput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
            process.standardOutput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
            process.standardError = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
            let masterHandle = FileHandle(fileDescriptor: masterFD, closeOnDealloc: false)

            do {
                try process.run()
                close(slaveFD)
                self.attachLoginSession(process: process, masterFD: masterFD)

                var output = ""
                var passwordSent = false
                var promptBuffer = ""
                var successPublished = false
                let timedOut = LockedFlag(initialValue: false)
                let timeoutWorkItem = DispatchWorkItem {
                    if process.isRunning {
                        timedOut.value = true
                        self.terminate(process)
                    }
                }
                let publishSuccess = {
                    guard !successPublished else { return }
                    successPublished = true
                    timeoutWorkItem.cancel()
                    DispatchQueue.main.async {
                        if !passwordSent {
                            self.record(.authentication, domain: "Steam 登录服务", "已复用该账号的有效 SteamCMD 会话")
                        }
                        self.isLoggedIn = true
                        self.savedUsername = username
                        self.authenticationState = .available("已登录 \(username)")
                        onResult(.success)
                    }
                }
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 300, execute: timeoutWorkItem)
                self.readPTY(masterHandle: masterHandle, onChunk: { chunk in
                    output += chunk
                    promptBuffer = String((promptBuffer + chunk).suffix(1024))
                    if !passwordSent && self.isPasswordPrompt(promptBuffer) {
                        passwordSent = self.writeToPTY(password + "\n", masterFD: masterFD)
                        promptBuffer = ""
                        if !passwordSent { process.terminate() }
                    }
                    if let type = self.guardType(for: promptBuffer), self.markWaitingForGuard(process: process) {
                        promptBuffer = ""
                        DispatchQueue.main.async { onResult(.waitingForGuard(type)) }
                    }
                    if self.isLoginSuccessful(output) {
                        publishSuccess()
                    }
                }, onLine: { line in
                    if !passwordSent && self.isPasswordPrompt(line) {
                        passwordSent = self.writeToPTY(password + "\n", masterFD: masterFD)
                        if !passwordSent { process.terminate() }
                    }
                    let safeLine = self.redact(line, secrets: [username, password])
                    self.record(.authentication, domain: "Steam 登录服务", safeLine)
                    DispatchQueue.main.async { onLog(safeLine) }
                    if let type = self.guardType(for: line), self.markWaitingForGuard(process: process) {
                        DispatchQueue.main.async { onResult(.waitingForGuard(type)) }
                    }
                })
                process.waitUntilExit()
                timeoutWorkItem.cancel()
                close(masterFD)

                let wasCancelled = self.finishLoginSession(process: process)
                let success = successPublished || (process.terminationStatus == 0 && self.isLoginSuccessful(output))
                if success {
                    publishSuccess()
                } else {
                    DispatchQueue.main.async {
                        if wasCancelled {
                            self.isLoggedIn = false
                            self.authenticationState = .needsAction("登录已取消")
                            onResult(.failed("登录已取消"))
                        } else if timedOut.value {
                            self.isLoggedIn = false
                            self.authenticationState = .needsAction("登录超时")
                            onResult(.failed("Steam 登录长时间无响应，请检查网络后重试"))
                        } else if self.containsGuardRequest(output) {
                            self.isLoggedIn = false
                            self.authenticationState = .needsAction("Steam Guard 验证未完成")
                            onResult(.failed("Steam Guard 验证未完成或已超时，请重试登录"))
                        } else {
                            self.isLoggedIn = false
                            self.authenticationState = .needsAction("登录失败，需要重新验证")
                            onResult(.failed(self.loginFailureMessage(output, status: process.terminationStatus)))
                        }
                    }
                }
            } catch {
                close(masterFD)
                _ = Darwin.close(slaveFD)
                _ = self.finishLoginSession(process: process)
                self.setAuthenticationState(.unavailable("SteamCMD 登录运行失败"))
                self.record(.authentication, domain: "Steam 登录服务", "登录运行失败：\(error.localizedDescription)")
                DispatchQueue.main.async { onResult(.failed(error.localizedDescription)) }
            }
        }
    }

    func submitGuardCode(_ code: String) -> Bool {
        guard !code.isEmpty else { return false }
        processLock.lock()
        let process = activeLoginProcess
        let masterFD = activeLoginMasterFD
        let canSubmit = activeLoginWaitingForGuard && process?.isRunning == true && masterFD != nil
        if canSubmit { activeLoginWaitingForGuard = false }
        processLock.unlock()
        guard canSubmit, let masterFD else { return false }

        let sent = writeToPTY(code + "\n", masterFD: masterFD)
        record(.authentication, domain: "Steam 登录服务", sent ? "已安全提交 Steam Guard 验证码" : "提交 Steam Guard 验证码失败")
        return sent
    }

    func cancelLogin() {
        processLock.lock()
        let process = activeLoginProcess
        if process != nil { activeLoginCancelled = true }
        processLock.unlock()
        terminate(process)
    }

    func logout(completion: @escaping (Result<Void, Error>) -> Void) {
        processLock.lock()
        let busy = installationInProgress || !downloadProcesses.isEmpty
        let session = workshopSession
        if !busy { workshopSession = nil }
        processLock.unlock()
        guard !busy else {
            completion(.failure(SteamCMDError.operationFailed("请先等待安装或下载结束后再退出登录")))
            return
        }
        cancelLogin()
        terminate(session?.process)
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            self.clearLocalSessionArtifacts()
            DispatchQueue.main.async {
                self.savedUsername = ""
                self.isLoggedIn = false
                self.authenticationState = .needsAction("未登录")
                self.record(.authentication, domain: "Steam 登录服务", "已清除 Mirage 专用 SteamCMD 会话")
                completion(.success(()))
            }
        }
    }

    private func clearLocalSessionArtifacts() {
        let sessionDirectories = [
            steamCMDDir.appending(path: "config"),
            steamCMDDir.appending(path: "steam/config"),
            steamCMDDir.appending(path: "steam/userdata"),
            isolatedSteamDataDirectory.appending(path: "config"),
            isolatedSteamDataDirectory.appending(path: "userdata")
        ]
        for directory in sessionDirectories {
            try? fm.removeItem(at: directory)
        }
        for directory in [steamCMDDir, steamCMDDir.appending(path: "steam"), isolatedSteamDataDirectory] {
            let contents = (try? fm.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil, options: .skipsHiddenFiles)) ?? []
            for item in contents where item.lastPathComponent.hasPrefix("ssfn") {
                try? fm.removeItem(at: item)
            }
        }
    }

    // MARK: - Workshop downloads

    func downloadItem(workshopId: String, expectedFileSize: Int64 = 0, onProgress: @escaping (DownloadState) -> Void) {
        guard steamCMDPath != nil else {
            onProgress(.failed("SteamCMD 未安装"))
            return
        }
        guard isLoggedIn, !savedUsername.isEmpty else {
            onProgress(.failed("Steam 会话未验证，请重新登录后再下载"))
            return
        }

        guard reserveDownloadSlot(workshopId: workshopId) else {
            onProgress(.failed("SteamCMD 正在执行其他任务，请等待当前任务完成"))
            return
        }
        let task = SteamCMDWorkshopTask(
            workshopId: workshopId,
            expectedFileSize: expectedFileSize,
            onProgress: onProgress
        )
        DispatchQueue.main.async { onProgress(.starting) }
        record(.workshopDownload, domain: "Steam 内容 CDN", "开始下载创意工坊项目 \(workshopId)")

        do {
            try ensureDownloadDiskSpace(expectedFileSize: expectedFileSize)
        } catch {
            _ = finishDownloadProcess(workshopId: workshopId)
            DispatchQueue.main.async { onProgress(.failed(error.localizedDescription)) }
            return
        }

        processLock.lock()
        if let session = workshopSession, session.process.isRunning, session.attach(task) {
            downloadProcesses[workshopId] = session.process
            let ready = session.isReady
            processLock.unlock()
            if ready { startWorkshopDownload(task, in: session) }
            return
        }
        processLock.unlock()
        startWorkshopSession(task: task)
    }

    private func startWorkshopSession(task: SteamCMDWorkshopTask?) {
        processLock.lock()
        if let session = workshopSession, session.process.isRunning {
            let attached = task.map { session.attach($0) } ?? true
            if let task, attached { downloadProcesses[task.workshopId] = session.process }
            let ready = session.isReady
            processLock.unlock()
            if let task, attached, ready { startWorkshopDownload(task, in: session) }
            return
        }
        if workshopSessionStarting {
            processLock.unlock()
            if let task {
                DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.1) { [weak self] in
                    self?.startWorkshopSession(task: task)
                }
            }
            return
        }
        workshopSessionStarting = true
        processLock.unlock()

        guard let cmdPath = steamCMDPath else {
            failWorkshopSessionStart(task: task, message: "SteamCMD 未安装")
            return
        }

        var masterFD: Int32 = 0
        var slaveFD: Int32 = 0
        guard openpty(&masterFD, &slaveFD, nil, nil, nil) == 0 else {
            failWorkshopSessionStart(task: task, message: "无法创建伪终端")
            return
        }

        let process = Process()
        let command = command(for: cmdPath, arguments: ["+login", savedUsername])
        let inputPipe = Pipe()
        process.executableURL = command.executable
        process.arguments = command.arguments
        process.currentDirectoryURL = steamCMDDir
        configureSteamCMDProcess(process)
        process.standardInput = inputPipe
        process.standardOutput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        process.standardError = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)

        do {
            try process.run()
            close(slaveFD)
        } catch {
            close(masterFD)
            close(slaveFD)
            failWorkshopSessionStart(task: task, message: error.localizedDescription)
            return
        }

        let session = SteamCMDWorkshopSession(
            process: process,
            input: inputPipe.fileHandleForWriting,
            masterFD: masterFD,
            task: task
        )
        processLock.lock()
        workshopSession = session
        workshopSessionStarting = false
        if let task { downloadProcesses[task.workshopId] = process }
        processLock.unlock()

        DispatchQueue.global(qos: .userInitiated).async { [weak self, weak session] in
            guard let self, let session else { return }
            let handle = FileHandle(fileDescriptor: masterFD, closeOnDealloc: false)
            self.readPTY(masterHandle: handle, onChunk: { _ in }, onLine: { line in
                self.handleWorkshopSessionLine(line, session: session)
            })
            process.waitUntilExit()
            close(masterFD)
            self.finishWorkshopSession(session)
        }
    }

    private func failWorkshopSessionStart(task: SteamCMDWorkshopTask?, message: String) {
        processLock.lock(); workshopSessionStarting = false; processLock.unlock()
        guard let task else {
            invalidateAuthentication(message)
            return
        }
        _ = finishDownloadProcess(workshopId: task.workshopId)
        DispatchQueue.main.async { task.onProgress(.failed(message)) }
    }

    private func handleWorkshopSessionLine(_ line: String, session: SteamCMDWorkshopSession) {
        record(.workshopDownload, domain: "Steam 内容 CDN", line, secrets: [savedUsername])
        let lower = line.lowercased()

        if lower.contains("cached credentials not found") || lower.contains("login failure") {
            let message = "Steam 缓存会话不可用，请重新登录"
            if let task = session.currentTask() {
                task.markFailure()
                failWorkshopDownload(task, in: session, message: message)
            } else {
                terminate(session.process)
            }
            invalidateAuthentication(message)
            return
        }

        if lower.contains("waiting for user info"), lower.contains("ok") {
            let task = session.markReady()
            DispatchQueue.main.async {
                self.isLoggedIn = true
                self.authenticationState = .available("会话有效")
            }
            if let task {
                startWorkshopDownload(task, in: session)
            }
            return
        }

        guard let task = session.currentTask() else { return }
        task.append(line)
        if lower.contains("downloading item"), lower.contains(task.workshopId), !task.downloadStarted.value {
            task.baselineBytes.value = task.receivedBytes.value
            task.downloadStarted.value = true
        }
        if lower.contains("success. downloaded item"), lower.contains(task.workshopId) {
            DispatchQueue.main.async { task.onProgress(.downloading(percent: 1)) }
            DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 0.8) { [weak self, weak session, weak task] in
                guard let self, let session, let task else { return }
                self.completeWorkshopDownload(task, in: session)
            }
            return
        }
        if isFatalDownloadLine(lower) || lower.contains("cached credentials not found") {
            task.markFailure()
            failWorkshopDownload(task, in: session, message: "Steam 会话不可用，请重新登录")
            invalidateAuthentication("Steam 会话不可用，请重新登录")
        }
    }

    private func startWorkshopDownload(_ task: SteamCMDWorkshopTask, in session: SteamCMDWorkshopSession) {
        guard task.markCommandSent() else { return }
        DispatchQueue.main.async { task.onProgress(.downloading(percent: nil)) }
        monitorReceivedBytes(
            parentPID: session.process.processIdentifier,
            active: task.polling,
            receivedBytes: task.receivedBytes,
            downloadStarted: task.downloadStarted
        )
        monitorWorkshopProgress(task)
        do {
            try session.input.write(contentsOf: Data("workshop_download_item 431960 \(task.workshopId)\n".utf8))
        } catch {
            failWorkshopDownload(task, in: session, message: error.localizedDescription)
        }
    }

    private func monitorWorkshopProgress(_ task: SteamCMDWorkshopTask) {
        DispatchQueue.global(qos: .utility).async {
            var lastReported = 0.0
            while task.polling.value {
                Thread.sleep(forTimeInterval: 0.1)
                let total = task.receivedBytes.value
                let baseline = task.baselineBytes.value
                guard task.downloadStarted.value, total >= baseline, task.expectedFileSize > 0 else { continue }
                let progress = min(Double(total - baseline) / Double(task.expectedFileSize), 0.98)
                if progress >= lastReported + 0.002 {
                    lastReported = progress
                    DispatchQueue.main.async { task.onProgress(.downloading(percent: progress)) }
                }
            }
        }
    }

    private func completeWorkshopDownload(_ task: SteamCMDWorkshopTask, in session: SteamCMDWorkshopSession) {
        guard let result = task.finish() else { return }
        DispatchQueue.main.async { task.onProgress(.validating) }
        DispatchQueue.global(qos: .userInitiated).async {
            let wasCancelled = self.finishDownloadProcess(workshopId: task.workshopId)
            let validation = wasCancelled ? (nil, nil) : self.validateDownloadedItem(workshopId: task.workshopId)
            session.clearTask(task)
            if wasCancelled {
                DispatchQueue.main.async { task.onProgress(.failed("下载已取消")) }
            } else if result.receivedFailure {
                let message = self.downloadFailureMessage(result.output, status: 1, hasProject: validation.0 != nil)
                DispatchQueue.main.async { task.onProgress(.failed(message)) }
            } else if let directory = validation.0 {
                self.record(.workshopDownload, domain: "Steam 内容 CDN", "项目 \(task.workshopId) 已完成并通过本地文件校验：\(directory.path)")
                DispatchQueue.main.async { task.onProgress(.completed) }
            } else {
                DispatchQueue.main.async { task.onProgress(.failed(validation.1 ?? "下载文件校验失败")) }
            }
        }
    }

    private func failWorkshopDownload(_ task: SteamCMDWorkshopTask, in session: SteamCMDWorkshopSession, message: String) {
        guard task.finish() != nil else { return }
        session.clearTask(task)
        _ = finishDownloadProcess(workshopId: task.workshopId)
        DispatchQueue.main.async { task.onProgress(.failed(message)) }
        terminate(session.process)
    }

    private func finishWorkshopSession(_ session: SteamCMDWorkshopSession) {
        if let task = session.currentTask() {
            let cancelled = isDownloadCancelled(workshopId: task.workshopId)
            failWorkshopDownload(task, in: session, message: cancelled ? "下载已取消" : "SteamCMD 会话已结束")
        }
        processLock.lock()
        if workshopSession === session { workshopSession = nil }
        processLock.unlock()
    }

    func cancelDownload(workshopId: String) {
        processLock.lock()
        let process = downloadProcesses[workshopId]
        if process != nil { cancelledDownloadIDs.insert(workshopId) }
        processLock.unlock()
        terminate(process)
        record(.workshopDownload, domain: "Steam 内容 CDN", "用户取消下载项目 \(workshopId)")
    }

    func downloadedItemDirectory(workshopId: String) -> URL? {
        workshopItemDirectories(workshopId: workshopId).first {
            fm.fileExists(atPath: $0.appending(path: "project.json").path)
        }
    }

    // MARK: - PTY helpers

    private func runWithPTY(
        executable: URL? = nil,
        arguments: [String],
        onLine: @escaping (String) -> Void,
        timeout: TimeInterval? = nil,
        onProcessStart: ((Process) -> Void)? = nil
    ) -> SteamCMDRunResult {
        guard let cmdPath = executable ?? steamCMDPath else {
            return SteamCMDRunResult(status: -1, output: "", timedOut: false)
        }

        var masterFD: Int32 = 0
        var slaveFD: Int32 = 0
        guard openpty(&masterFD, &slaveFD, nil, nil, nil) == 0 else {
            return SteamCMDRunResult(status: -2, output: "", timedOut: false)
        }

        let process = Process()
        let command = command(for: cmdPath, arguments: arguments)
        process.executableURL = command.executable
        process.arguments = command.arguments
        process.currentDirectoryURL = steamCMDDir
        configureSteamCMDProcess(process)
        process.standardInput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        process.standardOutput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        process.standardError = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        let masterHandle = FileHandle(fileDescriptor: masterFD, closeOnDealloc: false)
        let timeoutLock = NSLock()
        var timedOut = false

        do {
            try process.run()
            onProcessStart?(process)
            close(slaveFD)
            let timeoutWorkItem = DispatchWorkItem {
                if process.isRunning {
                    timeoutLock.lock(); timedOut = true; timeoutLock.unlock()
                    self.terminate(process)
                }
            }
            if let timeout {
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + timeout, execute: timeoutWorkItem)
            }

            var output = ""
            readPTY(masterHandle: masterHandle, onChunk: { output += $0 }, onLine: onLine)
            process.waitUntilExit()
            timeoutWorkItem.cancel()
            timeoutLock.lock(); let didTimeout = timedOut; timeoutLock.unlock()
            close(masterFD)
            return SteamCMDRunResult(status: process.terminationStatus, output: output, timedOut: didTimeout)
        } catch {
            close(masterFD)
            _ = Darwin.close(slaveFD)
            return SteamCMDRunResult(status: -3, output: error.localizedDescription, timedOut: false)
        }
    }

    private func readPTY(masterHandle: FileHandle, onChunk: (String) -> Void, onLine: (String) -> Void) {
        var pendingLine = ""
        while true {
            let data = masterHandle.availableData
            if data.isEmpty { break }
            guard let chunk = String(data: data, encoding: .utf8) else { continue }
            onChunk(chunk)
            let segments = (pendingLine + chunk).components(separatedBy: .newlines)
            pendingLine = segments.last ?? ""
            for raw in segments.dropLast() {
                let line = raw.replacingOccurrences(of: "\r", with: "").trimmingCharacters(in: .whitespaces)
                if !line.isEmpty { onLine(line) }
            }
        }
        let line = pendingLine.replacingOccurrences(of: "\r", with: "").trimmingCharacters(in: .whitespaces)
        if !line.isEmpty { onLine(line) }
    }

    private func disableTerminalEcho(slaveFD: Int32) {
        var attributes = termios()
        guard tcgetattr(slaveFD, &attributes) == 0 else { return }
        attributes.c_lflag &= ~tcflag_t(ECHO)
        _ = tcsetattr(slaveFD, TCSANOW, &attributes)
    }

    private func writeToPTY(_ text: String, masterFD: Int32) -> Bool {
        guard let data = text.data(using: .utf8) else { return false }
        return data.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return false }
            var written = 0
            while written < bytes.count {
                let result = Darwin.write(masterFD, base.advanced(by: written), bytes.count - written)
                if result > 0 {
                    written += result
                } else if errno != EINTR {
                    return false
                }
            }
            return true
        }
    }

    private func command(for launcher: URL, arguments: [String]) -> (executable: URL, arguments: [String]) {
        if launcher.pathExtension == "sh" {
            return (URL(fileURLWithPath: "/bin/bash"), [launcher.path] + arguments)
        }
        return (launcher, arguments)
    }

    private func configureSteamCMDProcess(_ process: Process) {
        var environment = ProcessInfo.processInfo.environment
        environment["HOME"] = steamCMDHomeDirectory.path
        process.environment = environment
    }

    private func terminate(_ process: Process?) {
        guard let process, process.isRunning else { return }
        let pid = process.processIdentifier
        process.terminate()
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 5) {
            if process.isRunning { _ = Darwin.kill(pid, SIGKILL) }
        }
    }

    // MARK: - Process state

    private func beginLoginSession() -> Bool {
        processLock.lock()
        defer { processLock.unlock() }
        guard activeLoginProcess == nil, !installationInProgress, downloadProcesses.isEmpty,
              workshopSession?.process.isRunning != true, !workshopSessionStarting else { return false }
        activeLoginWaitingForGuard = false
        activeLoginCancelled = false
        activeLoginProcess = Process()
        return true
    }

    private func attachLoginSession(process: Process, masterFD: Int32) {
        processLock.lock()
        let wasCancelled = activeLoginCancelled
        activeLoginProcess = process
        activeLoginMasterFD = masterFD
        processLock.unlock()
        if wasCancelled { terminate(process) }
    }

    private func finishLoginSession(process: Process?) -> Bool {
        processLock.lock()
        let wasCancelled = activeLoginCancelled
        if process == nil || activeLoginProcess === process || activeLoginProcess?.isRunning != true {
            activeLoginProcess = nil
            activeLoginMasterFD = nil
            activeLoginWaitingForGuard = false
            activeLoginCancelled = false
        }
        processLock.unlock()
        return wasCancelled
    }

    private func markWaitingForGuard(process: Process) -> Bool {
        processLock.lock()
        defer { processLock.unlock() }
        guard activeLoginProcess === process, !activeLoginWaitingForGuard else { return false }
        activeLoginWaitingForGuard = true
        return true
    }

    private func reserveDownloadSlot(workshopId: String) -> Bool {
        processLock.lock()
        defer { processLock.unlock() }
        guard downloadProcesses.isEmpty, !installationInProgress, activeLoginProcess == nil else { return false }
        downloadProcesses[workshopId] = Process()
        cancelledDownloadIDs.remove(workshopId)
        return true
    }

    private func attachDownloadProcess(_ process: Process, workshopId: String) {
        processLock.lock(); downloadProcesses[workshopId] = process; processLock.unlock()
    }

    private func isDownloadCancelled(workshopId: String) -> Bool {
        processLock.lock(); defer { processLock.unlock() }
        return cancelledDownloadIDs.contains(workshopId)
    }

    @discardableResult
    private func finishDownloadProcess(workshopId: String) -> Bool {
        processLock.lock()
        let cancelled = cancelledDownloadIDs.remove(workshopId) != nil
        downloadProcesses.removeValue(forKey: workshopId)
        processLock.unlock()
        return cancelled
    }

    private func setAuthenticationState(_ state: SteamServiceState) {
        DispatchQueue.main.async { [weak self] in self?.authenticationState = state }
    }

    private func invalidateAuthentication(_ reason: String) {
        DispatchQueue.main.async { [weak self] in
            self?.isLoggedIn = false
            self?.authenticationState = .needsAction(reason)
        }
    }

    private func ensureDownloadDiskSpace(expectedFileSize: Int64) throws {
        guard expectedFileSize > 0 else { return }
        let values = try steamCMDDir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        let reserve = max(256 * 1024 * 1024, expectedFileSize / 2)
        let required = expectedFileSize + reserve
        if let available = values.volumeAvailableCapacityForImportantUsage, available < required {
            let formatted = ByteCountFormatter.string(fromByteCount: required, countStyle: .file)
            throw SteamCMDError.operationFailed("磁盘空间不足，建议至少保留 \(formatted) 可用空间")
        }
    }

    private func workshopItemDirectories(workshopId: String) -> [URL] {
        let roots = steamCMDContentDirectories + [
            WallpaperLibrary.shared.steamWorkshopDirectory,
            WallpaperLibrary.shared.defaultSteamWorkshopDirectory
        ]
        var paths = Set<String>()
        return roots.compactMap { root in
            let directory = root.appending(path: workshopId).standardizedFileURL
            return paths.insert(directory.path).inserted ? directory : nil
        }
    }

    private func monitorReceivedBytes(
        parentPID: pid_t,
        active: LockedFlag,
        receivedBytes: LockedUInt64,
        downloadStarted: LockedFlag
    ) {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self,
                  let steamCMDPID = self.waitForSteamCMDChild(of: parentPID, active: active) else { return }
            while active.value {
                if let total = self.receivedNetworkBytes(pid: steamCMDPID) {
                    receivedBytes.value = max(receivedBytes.value, total)
                }
                Thread.sleep(forTimeInterval: downloadStarted.value ? 0.1 : 0.5)
            }
        }
    }

    private func receivedNetworkBytes(pid: pid_t) -> UInt64? {
        var masterFD: Int32 = 0
        var slaveFD: Int32 = 0
        guard openpty(&masterFD, &slaveFD, nil, nil, nil) == 0 else { return nil }

        let monitor = Process()
        monitor.executableURL = URL(fileURLWithPath: "/usr/bin/nettop")
        monitor.arguments = ["-P", "-L", "0", "-x", "-J", "bytes_in", "-p", String(pid)]
        monitor.standardInput = FileHandle.nullDevice
        monitor.standardOutput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        monitor.standardError = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)

        do {
            try monitor.run()
            close(slaveFD)
        } catch {
            close(slaveFD)
            close(masterFD)
            return nil
        }

        var output = ""
        var totalBytes: UInt64?
        let deadline = Date().addingTimeInterval(0.4)
        while Date() < deadline, totalBytes == nil {
            var descriptor = pollfd(fd: masterFD, events: Int16(POLLIN), revents: 0)
            guard Darwin.poll(&descriptor, 1, 50) > 0 else { continue }
            var buffer = [UInt8](repeating: 0, count: 4096)
            let count = Darwin.read(masterFD, &buffer, buffer.count)
            guard count > 0 else { break }
            output += String(decoding: buffer.prefix(Int(count)), as: UTF8.self)
            totalBytes = output.split(whereSeparator: { $0.isNewline }).compactMap { line in
                let columns = line.split(separator: ",", omittingEmptySubsequences: false)
                guard columns.count > 1 else { return nil }
                return UInt64(columns[1].trimmingCharacters(in: .whitespacesAndNewlines))
            }.max()
        }
        if monitor.isRunning {
            monitor.terminate()
            monitor.waitUntilExit()
        }
        close(masterFD)
        return totalBytes
    }

    private func waitForSteamCMDChild(of parentPID: pid_t, active: LockedFlag) -> pid_t? {
        let deadline = Date().addingTimeInterval(15)
        while active.value, Date() < deadline {
            if let pid = descendantPIDs(of: parentPID).first(where: { processName(pid: $0) == "steamcmd" }) {
                return pid
            }
            Thread.sleep(forTimeInterval: 0.05)
        }
        return nil
    }

    private func descendantPIDs(of parentPID: pid_t) -> [pid_t] {
        var result: [pid_t] = []
        var pending = [parentPID]
        var visited = Set<pid_t>()
        while let parent = pending.popLast(), visited.insert(parent).inserted {
            var children = [pid_t](repeating: 0, count: 64)
            let byteCount = children.withUnsafeMutableBytes { buffer in
                proc_listchildpids(parent, buffer.baseAddress, Int32(buffer.count))
            }
            guard byteCount > 0 else { continue }
            let count = min(Int(byteCount) / MemoryLayout<pid_t>.stride, children.count)
            let found = children.prefix(count).filter { $0 > 0 }
            result.append(contentsOf: found)
            pending.append(contentsOf: found)
        }
        return result
    }

    private func processName(pid: pid_t) -> String {
        var buffer = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        let length = buffer.withUnsafeMutableBytes { bytes in
            proc_name(pid, bytes.baseAddress, UInt32(bytes.count))
        }
        guard length > 0 else { return "" }
        return String(cString: buffer)
    }

    private func validateDownloadedItem(workshopId: String) -> (directory: URL?, error: String?) {
        var firstError: String?
        for directory in workshopItemDirectories(workshopId: workshopId) {
            guard fm.fileExists(atPath: directory.appending(path: "project.json").path) else { continue }
            if let error = validateDownloadedItem(at: directory) {
                if firstError == nil { firstError = error }
            } else {
                return (directory, nil)
            }
        }
        return (nil, firstError ?? "下载未完成：未找到 project.json")
    }

    private func validateDownloadedItem(at directory: URL) -> String? {
        let projectURL = directory.appending(path: "project.json")
        guard fm.fileExists(atPath: projectURL.path) else {
            return L("下载未完成：未找到 project.json")
        }
        let wallpaper = WEWallpaper.load(from: directory)
        if wallpaper.isPreset {
            switch wallpaper.presetStatus {
            case .resolved:
                guard fm.fileExists(atPath: wallpaper.resolvedEntryURL.path) else {
                    return L("下载内容不完整：基础壁纸缺少主文件")
                }
                return nil
            case .missingDependency, .invalidDependency:
                return nil
            case .circularDependency:
                return L("预设包含循环依赖")
            case .notPreset:
                break
            }
        }
        guard wallpaper.isValid, wallpaper.kind != .unsupported else {
            return L("下载内容无效或壁纸类型不受支持")
        }
        guard fm.fileExists(atPath: wallpaper.resolvedEntryURL.path) else {
            return L("下载内容不完整：缺少壁纸主文件")
        }
        return nil
    }

    private func isFatalDownloadLine(_ lower: String) -> Bool {
        lower.contains("error! download item") ||
            lower.contains("access denied") ||
            lower.contains("no subscription") ||
            lower.contains("login failure") ||
            lower.contains("invalid password")
    }

    private func isAuthenticationFailure(_ output: String) -> Bool {
        let lower = output.lowercased()
        return lower.contains("not logged") ||
            lower.contains("login failure") ||
            lower.contains("invalid password") ||
            lower.contains("account logon denied") ||
            lower.contains("steam guard")
    }

    // MARK: - Output interpretation

    private func isLoginSuccessful(_ output: String) -> Bool {
        let lower = output.replacingOccurrences(of: "\r", with: "\n").lowercased()
        let lines = lower.components(separatedBy: .newlines)
        let lineSucceeded: (String) -> Bool = { marker in
            lines.contains { line in
                line.contains(marker) && line.range(of: "\\bok\\b", options: .regularExpression) != nil
            }
        }
        let reachedUserInfo = lineSucceeded("waiting for user info")
        let hasSuccess = lower.contains("logged in ok") ||
            lower.contains("login successful") ||
            (lower.contains("logging in user") && reachedUserInfo)
        let hasFailure = lower.contains("login failure") || lower.contains("invalid password") ||
            lower.contains("account logon denied") || lower.contains("access denied")
        return hasSuccess && !hasFailure
    }

    private func isPasswordPrompt(_ output: String) -> Bool {
        let lower = output.lowercased()
        guard !lower.contains("invalid password") else { return false }
        return lower.contains("password:") || lower.contains("enter your password")
    }

    private func containsGuardRequest(_ output: String) -> Bool {
        guardType(for: output) != nil
    }

    private func guardType(for output: String) -> SteamGuardType? {
        let lower = output.lowercased()
        if lower.contains("please confirm the login in the steam mobile app") || lower.contains("confirm the login in the steam mobile") {
            return .mobileConfirm
        }
        guard lower.contains("steam guard") || lower.contains("two-factor") || lower.contains("two factor") ||
                (lower.contains("code") && (lower.contains("email") || lower.contains("mobile"))) else {
            return nil
        }
        return (lower.contains("email") || lower.contains("mail")) ? .email : .mobile
    }

    private func loginFailureMessage(_ output: String, status: Int32) -> String {
        let lower = output.lowercased()
        if lower.contains("invalid password") || lower.contains("login failure") {
            return L("登录失败，请检查用户名和密码")
        }
        if lower.contains("access denied") || lower.contains("no subscription") {
            return L("该 Steam 账号没有 Wallpaper Engine 的可用所有权")
        }
        if lower.contains("network") || lower.contains("connection") || lower.contains("timeout") || lower.contains("no connection") {
            return L("无法连接 Steam 登录服务，请检查网络后重试")
        }
        return L("Steam 登录失败 (exit %@)", String(status))
    }

    private func downloadFailureMessage(_ output: String, status: Int32, hasProject: Bool) -> String {
        let lower = output.lowercased()
        if lower.contains("no subscription") || lower.contains("access denied") {
            return L("下载被 Steam 拒绝：请确认该账号拥有 Wallpaper Engine 并有权访问此项目")
        }
        if isAuthenticationFailure(output) {
            return L("Steam 会话已失效，请重新登录后再试")
        }
        if lower.contains("network") || lower.contains("connection") || lower.contains("timeout") || lower.contains("no connection") {
            return L("下载网络连接失败，请检查网络后重试")
        }
        if !hasProject {
            return L("下载未完成：未找到有效的 project.json (exit %@)", String(status))
        }
        return L("SteamCMD 未确认下载成功 (exit %@)", String(status))
    }

    // MARK: - Shell helper

    private func runShellSync(_ executable: String, arguments: [String]) throws -> ShellResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        process.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return ShellResult(status: process.terminationStatus, output: String(data: data, encoding: .utf8) ?? "")
    }
}

private struct SteamCMDRunResult {
    let status: Int32
    let output: String
    let timedOut: Bool
}

private struct ShellResult {
    let status: Int32
    let output: String
}

private final class SteamCMDWorkshopTask {
    let workshopId: String
    let expectedFileSize: Int64
    let onProgress: (DownloadState) -> Void
    let polling = LockedFlag(initialValue: true)
    let downloadStarted = LockedFlag(initialValue: false)
    let receivedBytes = LockedUInt64(0)
    let baselineBytes = LockedUInt64(0)
    private let lock = NSLock()
    private var commandSent = false
    private var finished = false
    private var output = ""
    private var receivedFailure = false

    init(workshopId: String, expectedFileSize: Int64, onProgress: @escaping (DownloadState) -> Void) {
        self.workshopId = workshopId
        self.expectedFileSize = expectedFileSize
        self.onProgress = onProgress
    }

    func markCommandSent() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !commandSent, !finished else { return false }
        commandSent = true
        return true
    }

    func append(_ line: String) {
        lock.lock(); output += line + "\n"; lock.unlock()
    }

    func markFailure() {
        lock.lock(); receivedFailure = true; lock.unlock()
    }

    func finish() -> (output: String, receivedFailure: Bool)? {
        lock.lock(); defer { lock.unlock() }
        guard !finished else { return nil }
        finished = true
        polling.value = false
        return (output, receivedFailure)
    }
}

private final class SteamCMDWorkshopSession {
    let process: Process
    let input: FileHandle
    let masterFD: Int32
    private let lock = NSLock()
    private var task: SteamCMDWorkshopTask?
    private var ready = false

    init(process: Process, input: FileHandle, masterFD: Int32, task: SteamCMDWorkshopTask?) {
        self.process = process
        self.input = input
        self.masterFD = masterFD
        self.task = task
    }

    func attach(_ task: SteamCMDWorkshopTask) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard self.task == nil, process.isRunning else { return false }
        self.task = task
        return true
    }

    func currentTask() -> SteamCMDWorkshopTask? {
        lock.lock(); defer { lock.unlock() }
        return task
    }

    func clearTask(_ task: SteamCMDWorkshopTask) {
        lock.lock()
        if self.task === task { self.task = nil }
        lock.unlock()
    }

    func markReady() -> SteamCMDWorkshopTask? {
        lock.lock(); ready = true; let current = task; lock.unlock()
        return current
    }

    var isReady: Bool {
        lock.lock(); defer { lock.unlock() }
        return ready
    }

}

private final class LockedFlag {
    private let lock = NSLock()
    private var storage: Bool

    init(initialValue: Bool) { storage = initialValue }

    var value: Bool {
        get { lock.lock(); defer { lock.unlock() }; return storage }
        set { lock.lock(); storage = newValue; lock.unlock() }
    }
}

private final class LockedDate {
    private let lock = NSLock()
    private var storage: Date

    init(_ value: Date) { storage = value }

    var value: Date {
        get { lock.lock(); defer { lock.unlock() }; return storage }
        set { lock.lock(); storage = newValue; lock.unlock() }
    }
}

private final class LockedUInt64 {
    private let lock = NSLock()
    private var storage: UInt64

    init(_ value: UInt64) { storage = value }

    var value: UInt64 {
        get { lock.lock(); defer { lock.unlock() }; return storage }
        set { lock.lock(); storage = newValue; lock.unlock() }
    }
}

private final class SteamCMDDownloadDelegate: NSObject, URLSessionDownloadDelegate {
    let semaphore = DispatchSemaphore(value: 0)
    let onProgress: (Double) -> Void
    let destination: URL
    var downloadedURL: URL?
    var response: URLResponse?
    var error: Error?

    init(destination: URL, onProgress: @escaping (Double) -> Void) {
        self.destination = destination
        self.onProgress = onProgress
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        onProgress(min(max(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite), 0), 1))
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        response = downloadTask.response
        do {
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try FileManager.default.moveItem(at: location, to: destination)
            downloadedURL = destination
        } catch {
            self.error = error
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { self.error = error }
        if response == nil { response = task.response }
        semaphore.signal()
    }
}

enum SteamCMDError: LocalizedError {
    case downloadFailed(String)
    case installFailed(String)
    case operationFailed(String)

    var errorDescription: String? {
        switch self {
        case .downloadFailed(let message): return L("SteamCMD 下载失败：%@", message)
        case .installFailed(let message): return L("SteamCMD 安装失败：%@", message)
        case .operationFailed(let message): return message
        }
    }
}
