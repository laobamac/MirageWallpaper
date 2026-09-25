//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Security
import Darwin

final class DirectWorkshopService: ObservableObject, @unchecked Sendable {
    static let shared = DirectWorkshopService()
    @Published private(set) var isEnabled = false {
        didSet { modeLock.withLock { modeEnabled = isEnabled } }
    }
    @Published private(set) var isActivated = false
    @Published private(set) var isChecking = false
    @Published private(set) var deviceCode = ""
    @Published private(set) var errorMessage: String?
    @Published private(set) var expiresAt: Date?
    var isReady: Bool { isEnabled && isActivated && isAvailable }
    var isAvailable: Bool { launchConfiguration != nil }
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.direct-workshop")
    private let enabledKey = "DirectWorkshopEnabled"
    private let keychainName: String
    private let defaults: UserDefaults
    private let resources: URL?
    private var activationCode = ""
    private var generation = UUID()
    private var jobs: [String: Job] = [:]
    private let modeLock = NSLock()
    private var modeEnabled = false
    var blocksSteamCommunity: Bool { modeLock.withLock { modeEnabled } }
    var requestsDirectMode: Bool { defaults.bool(forKey: enabledKey) && isAvailable }

    private final class Job: @unchecked Sendable {
        let process = Process()
        let pipe = Pipe()
        var buffer = Data()
        var terminal = false
        let event: ([String: Any]) -> Void
        let failure: (String) -> Void
        init(event: @escaping ([String: Any]) -> Void, failure: @escaping (String) -> Void) {
            self.event = event
            self.failure = failure
        }
    }

    private var launchConfiguration: (URL, URL, URL)? {
        guard let resources else { return nil }
        #if arch(arm64)
        let architecture = "arm64"
        #else
        let architecture = "x86_64"
        #endif
        let runtime = resources.appending(path: "SteamService/\(architecture)/runtime")
        let executable = runtime.appending(path: "dotnet")
        let assembly = resources.appending(path: "DirectWorkshop/MirageDirectWorkshop.dll")
        guard FileManager.default.isExecutableFile(atPath: executable.path),
              FileManager.default.fileExists(atPath: assembly.path) else { return nil }
        return (executable, assembly, runtime)
    }

    init(defaults: UserDefaults = .standard,
         keychainService: String = "cn.laobamac.Mirage.DirectWorkshop",
         resources: URL? = Bundle.main.resourceURL) {
        self.defaults = defaults
        self.keychainName = keychainService
        self.resources = resources
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: nil) { [weak self] _ in
            self?.shutdown()
        }
        DispatchQueue.main.async { [weak self] in
            guard let self, self.defaults.bool(forKey: self.enabledKey) else { return }
            self.refresh(enableAfterValidation: true)
        }
    }

    func refresh(enableAfterValidation: Bool = false) {
        guard !isChecking else { return }
        guard isAvailable else {
            errorMessage = L("此版本未包含免登录下载组件")
            isEnabled = false
            defaults.set(false, forKey: enabledKey)
            return
        }
        let token = UUID()
        generation = token
        isChecking = true
        errorMessage = nil
        let saved = readCode()
        let command: [String: Any] = saved.isEmpty ? ["command": "device"] : ["command": "verify", "activationCode": saved]
        run(id: token.uuidString, command: command, control: true) { [weak self] event in
            guard let self, self.generation == token else { return }
            self.isChecking = false
            self.deviceCode = event["deviceCode"] as? String ?? self.deviceCode
            self.isActivated = !saved.isEmpty
            if saved.isEmpty { self.setEnabled(false) }
            self.activationCode = saved
            self.setExpiry(event)
            if enableAfterValidation && self.isActivated { self.setEnabled(true) }
        } failure: { [weak self] code in
            guard let self, self.generation == token else { return }
            self.isChecking = false
            self.isActivated = false
            self.isEnabled = false
            self.defaults.set(false, forKey: self.enabledKey)
            self.errorMessage = Self.message(code)
            if self.deviceCode.isEmpty {
                self.run(id: UUID().uuidString, command: ["command": "device"], control: true) { [weak self] event in
                    self?.deviceCode = event["deviceCode"] as? String ?? ""
                } failure: { _ in }
            }
        }
    }

    func activate(_ code: String) {
        guard !isChecking else { return }
        let normalized = code.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalized.isEmpty, normalized.utf8.count <= 2048 else { errorMessage = Self.message("ACTIVATION_INVALID"); return }
        let token = UUID()
        generation = token
        isChecking = true
        errorMessage = nil
        run(id: token.uuidString, command: ["command": "verify", "activationCode": normalized], control: true) { [weak self] event in
            guard let self, self.generation == token else { return }
            self.isChecking = false
            guard self.saveCode(normalized) else { self.errorMessage = L("无法将激活信息保存到钥匙串"); return }
            self.activationCode = normalized
            self.deviceCode = event["deviceCode"] as? String ?? self.deviceCode
            self.isActivated = true
            self.setExpiry(event)
            self.setEnabled(true)
        } failure: { [weak self] code in
            guard let self, self.generation == token else { return }
            self.isChecking = false
            self.errorMessage = Self.message(code)
        }
    }

    func setEnabled(_ enabled: Bool) {
        if enabled && !isActivated { refresh(enableAfterValidation: true); return }
        errorMessage = nil
        isEnabled = enabled && isAvailable && isActivated
        defaults.set(isEnabled, forKey: enabledKey)
        if !isEnabled { stopAll() }
    }

    func removeActivation() {
        generation = UUID()
        setEnabled(false)
        isChecking = false
        let status = SecItemDelete(keychainQuery as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            errorMessage = L("无法清除钥匙串中的激活信息")
            return
        }
        activationCode = ""
        isActivated = false
        expiresAt = nil
    }

    func download(workshopId: String, taskId: String, outputRoot: URL, progress: @escaping (DownloadState) -> Void) {
        guard isReady else { progress(.failed(L("请先激活并开启免登录下载"))); return }
        if let expiresAt, expiresAt <= Date() {
            isActivated = false
            setEnabled(false)
            progress(.failed(Self.message("ACTIVATION_EXPIRED")))
            return
        }
        run(id: taskId, command: ["command": "download", "activationCode": activationCode,
                                 "workshopId": workshopId, "outputRoot": outputRoot.path], control: false) { event in
            switch event["state"] as? String {
            case "resolving": progress(.resolving)
            case "validating": progress(.validating)
            case "completed": progress(.completed)
            case "downloading":
                progress(.downloading(DownloadProgress(
                    receivedBytes: (event["receivedBytes"] as? NSNumber)?.int64Value ?? 0,
                    totalBytes: (event["totalBytes"] as? NSNumber)?.int64Value ?? 0,
                    bytesPerSecond: (event["bytesPerSecond"] as? NSNumber)?.doubleValue ?? 0,
                    etaSeconds: (event["etaSeconds"] as? NSNumber)?.doubleValue
                )))
            default: break
            }
        } failure: { code in progress(.failed(Self.message(code))) }
    }

    func cancel(taskId: String) { queue.async { [weak self] in self?.cancelOnQueue(taskId) } }
    func shutdown() { queue.sync { for id in Array(jobs.keys) { cancelOnQueue(id) } } }
    private func stopAll() { queue.async { [weak self] in guard let self else { return }; for id in Array(self.jobs.keys) { self.cancelOnQueue(id) } } }

    private func cancelOnQueue(_ id: String) {
        guard let job = jobs.removeValue(forKey: id) else { return }
        job.pipe.fileHandleForReading.readabilityHandler = nil
        if job.process.isRunning { job.process.terminate() }
        queue.asyncAfter(deadline: .now() + 2) { if job.process.isRunning { kill(job.process.processIdentifier, SIGKILL) } }
        if !job.terminal { DispatchQueue.main.async { job.failure("DOWNLOAD_CANCELLED") } }
    }

    private func run(id: String, command: [String: Any], control: Bool,
                     event: @escaping ([String: Any]) -> Void, failure: @escaping (String) -> Void) {
        guard let (executable, assembly, runtime) = launchConfiguration,
              let data = try? JSONSerialization.data(withJSONObject: command) else { failure("COMPONENT_UNAVAILABLE"); return }
        queue.async { [weak self] in
            guard let self else { return }
            let job = Job(event: event, failure: failure)
            let input = Pipe()
            job.process.executableURL = executable
            job.process.arguments = [assembly.path]
            var environment = ProcessInfo.processInfo.environment
            environment["DOTNET_ROOT"] = runtime.path
            environment["DOTNET_MULTILEVEL_LOOKUP"] = "0"
            environment["DOTNET_EnableDiagnostics"] = "0"
            job.process.environment = environment
            job.process.standardInput = input
            job.process.standardOutput = job.pipe
            job.process.standardError = FileHandle.nullDevice
            self.jobs[id] = job
            do {
                try job.process.run()
                try input.fileHandleForWriting.write(contentsOf: data + Data([10]))
                try input.fileHandleForWriting.close()
                DispatchQueue.global(qos: .utility).async {
                    var buffer = [UInt8](repeating: 0, count: 65536)
                    while true {
                        let count = Darwin.read(job.pipe.fileHandleForReading.fileDescriptor, &buffer, buffer.count)
                        if count < 0 && errno == EINTR { continue }
                        if count <= 0 { break }
                        let chunk = Data(buffer.prefix(count))
                        self.queue.sync { self.consume(chunk, id: id) }
                    }
                    job.process.waitUntilExit()
                    self.queue.async {
                        guard self.jobs[id] === job else { return }
                        self.jobs.removeValue(forKey: id)
                        if !job.terminal { DispatchQueue.main.async { failure("DIRECT_DOWNLOAD_FAILED") } }
                    }
                }
            } catch {
                self.jobs.removeValue(forKey: id)
                job.pipe.fileHandleForReading.readabilityHandler = nil
                if job.process.isRunning { job.process.terminate() }
                DispatchQueue.main.async { failure("COMPONENT_UNAVAILABLE") }
            }
            if control {
                self.queue.asyncAfter(deadline: .now() + 15) {
                    guard self.jobs[id] === job else { return }
                    self.cancelOnQueue(id)
                }
            }
        }
    }

    private func consume(_ data: Data, id: String) {
        guard let job = jobs[id] else { return }
        job.buffer.append(data)
        if job.buffer.count > 1048576 { cancelOnQueue(id); return }
        while let range = job.buffer.range(of: Data([10])) {
            let line = job.buffer.subdata(in: 0..<range.lowerBound)
            job.buffer.removeSubrange(0..<range.upperBound)
            guard !job.terminal, let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
            if object["type"] as? String == "error" {
                job.terminal = true
                let code = object["errorCode"] as? String ?? "DIRECT_DOWNLOAD_FAILED"
                DispatchQueue.main.async { job.failure(code) }
            } else {
                if object["type"] as? String == "response" || object["state"] as? String == "completed" { job.terminal = true }
                DispatchQueue.main.async { job.event(object) }
            }
        }
    }

    private func setExpiry(_ event: [String: Any]) {
        let seconds = (event["expiresAt"] as? NSNumber)?.doubleValue ?? 0
        expiresAt = seconds > 0 ? Date(timeIntervalSince1970: seconds) : nil
    }

    private var keychainQuery: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: keychainName, kSecAttrAccount as String: "activation"]
    }

    private func readCode() -> String {
        var query = keychainQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return "" }
        return String(data: data, encoding: .utf8) ?? ""
    }

    private func saveCode(_ code: String) -> Bool {
        let attributes: [String: Any] = [kSecValueData as String: Data(code.utf8)]
        let result = SecItemUpdate(keychainQuery as CFDictionary, attributes as CFDictionary)
        if result == errSecSuccess { return true }
        guard result == errSecItemNotFound else { return false }
        var query = keychainQuery.merging(attributes) { _, new in new }
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(query as CFDictionary, nil) == errSecSuccess
    }

    private static func message(_ code: String) -> String {
        switch code {
        case "ACTIVATION_INVALID": return L("激活码无效，请检查后重试")
        case "ACTIVATION_DEVICE_MISMATCH": return L("此激活码不适用于本机，请使用本机设备码领取")
        case "ACTIVATION_EXPIRED": return L("激活码已过期，请领取新的激活码")
        case "DEVICE_UNAVAILABLE": return L("无法读取本机设备标识")
        case "COMPONENT_UNAVAILABLE": return L("此版本未包含免登录下载组件")
        case "DIRECT_UPSTREAM_UNAVAILABLE", "MANIFEST_DOWNLOAD_FAILED": return L("免登录下载服务暂不可用，请稍后重试")
        case "WORKSHOP_DETAILS_UNAVAILABLE", "CONTENT_MANIFEST_MISSING", "WRONG_APP", "EMPTY_MANIFEST": return L("无法获取此创意工坊作品的下载内容")
        case "UNSAFE_PATH", "VALIDATION_FAILED", "PROJECT_JSON_MISSING", "INVALID_CONTENT_SIZE": return L("下载内容未通过安全或完整性校验")
        case "DOWNLOAD_CANCELLED": return L("下载已取消")
        default: return L("免登录下载失败，请检查网络后重试")
        }
    }
}
