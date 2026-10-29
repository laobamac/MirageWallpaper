//
//  EditorManager.swift
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Combine
import QuartzCore
import SwiftUI

@MainActor
final class EditorManager: ObservableObject {
    static let shared = EditorManager()
    enum State { case idle, login, checking, confirmation, downloading, preparing, ready, failed, notOwned }
    @Published private(set) var state: State = .idle
    @Published private(set) var installed = EditorInstallation.isInstalled
    @Published private var lastError: Error?
    var errorMessage: String? { lastError?.localizedDescription }
    @Published private(set) var currentFile = ""
    @Published private(set) var receivedBytes: Int64 = 0
    @Published private(set) var totalBytes: Int64 = 0
    @Published private(set) var bytesPerSecond: Double = 0
    @Published private(set) var etaSeconds: Double?
    @Published private(set) var requiredBytes: Int64 = 0
    @Published private(set) var installedBytes: Int64 = 0
    @Published private(set) var availableBytes: Int64 = 0
    @Published private(set) var usage = EditorDiskUsage()
    @Published private(set) var phase = ""
    @Published private(set) var cancelling = false
    private let steam = SteamServiceManager.shared
    private var subscriptions = Set<AnyCancellable>()
    private var task: Task<Void, Never>?
    private var worker: Task<Void, Error>?
    private var operation = UUID()
    private var planId: String?
    private var catalog: EditorCatalog?
    private var pendingOpen = false
    private var repairRequested = false
    private var account = ""
    private var downloadingRuntime = false
    private var originalDownloadBytes: Int64 = 0

    var busy: Bool { [.checking, .downloading, .preparing].contains(state) }
    var canCancel: Bool { state == .checking || state == .downloading || (state == .preparing && worker != nil) }
    var running: Bool { !NSRunningApplication.runningApplications(withBundleIdentifier: "cn.laobamac.Mirage.WEEditor.Editor").isEmpty }
    var canInstall: Bool { state == .confirmation && availableBytes >= requiredBytes }

    private init() {
        steam.$isLoggedIn.combineLatest(steam.$steamId).receive(on: RunLoop.main).sink { [weak self] loggedIn, id in
            Task { @MainActor in
                guard let self else { return }
                if self.state == .login && self.pendingOpen && loggedIn && !id.isEmpty { self.check() }
                else if !self.account.isEmpty && (!loggedIn || id != self.account) && self.busy {
                    self.cancel()
                }
            }
        }.store(in: &subscriptions)
    }

    func open() {
        if !busy && state != .confirmation {
            pendingOpen = true
            repairRequested = false
            begin()
        }
        EditorWindowController.shared.show()
    }

    func manage() {
        EditorWindowController.shared.show()
        refreshUsage()
        if !busy && state != .login && state != .confirmation { state = installed ? .ready : .idle }
    }

    func repair() {
        guard !busy else { return }
        guard !running else { fail(EditorServiceError(code: "EDITOR_BUSY")); return }
        pendingOpen = true
        repairRequested = true
        begin()
    }

    private func begin() {
        lastError = nil
        do { catalog = try EditorInstallation.catalog() }
        catch { fail(error); return }
        steam.startForEditor()
        if steam.isLoggedIn { check() }
        else { state = .login }
    }

    private func check() {
        guard !busy else { return }
        state = .checking
        phase = "正在验证购买资格…"
        let id = UUID(); operation = id
        task = Task {
            do {
                if catalog == nil { catalog = try EditorInstallation.catalog() }
                account = steam.steamId
                if installed && !repairRequested {
                    let result = try await steam.editorRequest("editorOwnership")
                    try Task.checkCancellation()
                    guard operation == id, result["steamId"] as? String == account else { return }
                    try await launch()
                    return
                }
                guard let catalog else { throw EditorServiceError(code: "EDITOR_SUPPORT_MISSING") }
                phase = "正在计算下载大小和所需空间…"
                let compatibility = try JSONSerialization.jsonObject(with: JSONEncoder().encode(catalog.compatibility))
                let planResult = try await steam.editorRequest("editorPlan", fields: ["editorCompatibility": compatibility])
                try Task.checkCancellation()
                guard operation == id, planResult["steamId"] as? String == account else { return }
                guard let plan = planResult["planId"] as? String,
                      let download = planResult["downloadBytes"] as? NSNumber,
                      let disk = planResult["installedBytes"] as? NSNumber else { throw EditorServiceError(code: "EDITOR_FILES") }
                try EditorInstallation.validateRoot()
                planId = plan
                originalDownloadBytes = download.int64Value
                totalBytes = originalDownloadBytes + (catalog.archive?.bytes ?? 0)
                installedBytes = disk.int64Value + catalog.installedBytes + 512 * 1024 * 1024
                let runtimeCacheBytes = catalog.archive.map { $0.bytes + catalog.installedBytes } ?? 0
                requiredBytes = installedBytes + disk.int64Value + runtimeCacheBytes + 128 * 1024 * 1024
                availableBytes = EditorInstallation.availableBytes()
                state = .confirmation
            } catch {
                if operation == id && !Task.isCancelled { fail(error) }
            }
        }
    }

    func install() {
        guard state == .confirmation, let planId, let catalog else { return }
        guard !running else { fail(EditorServiceError(code: "EDITOR_BUSY")); return }
        availableBytes = EditorInstallation.availableBytes()
        guard availableBytes >= requiredBytes else { fail(EditorServiceError(code: "EDITOR_SPACE")); return }
        state = .downloading
        downloadingRuntime = false
        phase = "正在校验可继续下载的文件…"
        lastError = nil; currentFile = ""; receivedBytes = 0; bytesPerSecond = 0; etaSeconds = nil
        let id = UUID(); operation = id
        steam.downloadEditor(planId: planId, output: EditorInstallation.downloads) { [weak self] event in
            Task { @MainActor in
                guard let self, self.operation == id else { return }
                switch event["state"] as? String {
                case "downloading":
                    self.phase = "正在下载 Wallpaper Engine…"
                    self.receivedBytes = (event["receivedBytes"] as? NSNumber)?.int64Value ?? 0
                    self.originalDownloadBytes = (event["totalBytes"] as? NSNumber)?.int64Value ?? self.originalDownloadBytes
                    self.totalBytes = self.originalDownloadBytes + (catalog.archive?.bytes ?? 0)
                    self.bytesPerSecond = (event["bytesPerSecond"] as? NSNumber)?.doubleValue ?? 0
                    self.etaSeconds = (event["etaSeconds"] as? NSNumber)?.doubleValue
                    self.currentFile = event["currentFile"] as? String ?? ""
                case "validating": self.phase = "正在校验下载文件…"
                case "completed":
                    if self.cancelling { self.finishCancellation() }
                    else { self.prepareRuntime(catalog: catalog, operation: id) }
                case "cancelled": self.finishCancellation()
                case "failed":
                    self.cancelling = false
                    self.fail(EditorServiceError(code: event["errorCode"] as? String ?? "DOWNLOAD_FAILED", detail: event["message"] as? String))
                default: break
                }
            }
        }
    }

    private func prepareRuntime(catalog: EditorCatalog, operation id: UUID) {
        guard catalog.archive != nil else { prepare(catalog: catalog, operation: id); return }
        downloadingRuntime = true
        phase = "正在准备编辑器运行组件…"
        currentFile = ""
        bytesPerSecond = 0; etaSeconds = nil
        task = Task {
            do {
                try await EditorRuntimeDownload.prepare(catalog: catalog) { [weak self] progress in
                    Task { @MainActor in
                        guard let self, self.operation == id, self.downloadingRuntime, !self.cancelling else { return }
                        self.receivedBytes = self.originalDownloadBytes + progress.received
                        self.totalBytes = self.originalDownloadBytes + progress.total
                        self.bytesPerSecond = progress.bytesPerSecond
                        self.etaSeconds = progress.bytesPerSecond > 0 ? Double(progress.total - progress.received) / progress.bytesPerSecond : nil
                        switch progress.phase {
                        case .downloading: self.phase = "正在下载编辑器运行组件…"
                        case .verifying: self.phase = "正在校验编辑器运行组件…"
                        case .unpacking: self.phase = "正在解压编辑器运行组件…"
                        }
                    }
                }
                try Task.checkCancellation()
                guard operation == id else { return }
                downloadingRuntime = false
                prepare(catalog: catalog, operation: id)
            } catch {
                downloadingRuntime = false
                if cancelling || Task.isCancelled { finishCancellation() }
                else if operation == id { fail(error) }
            }
        }
    }

    private func prepare(catalog: EditorCatalog, operation id: UUID) {
        state = .preparing
        phase = "正在准备编辑器运行环境…"
        worker = Task.detached(priority: .userInitiated) { try EditorInstallation.assemble(catalog: catalog) }
        task = Task {
            do {
                try await worker?.value
                guard operation == id else { return }
                installed = EditorInstallation.isInstalled
                if cancelling { finishCancellation(); return }
                let result = try await steam.editorRequest("editorOwnership")
                guard result["steamId"] as? String == account else { throw EditorServiceError(code: "EDITOR_ACCOUNT_CHANGED") }
                try await launch()
                refreshUsage()
            } catch {
                if cancelling { finishCancellation() }
                else if operation == id { fail(error) }
            }
            worker = nil
        }
    }

    private func launch() async throws {
        guard let catalog else { throw EditorServiceError(code: "EDITOR_SUPPORT_MISSING") }
        phase = "正在启动编辑器…"
        try await Task.detached(priority: .userInitiated) { try EditorInstallation.verifyInstalled(catalog: catalog) }.value
        try Task.checkCancellation()
        guard steam.isLoggedIn, steam.steamId == account else { throw EditorServiceError(code: "EDITOR_ACCOUNT_CHANGED") }
        let receipt = try JSONSerialization.jsonObject(with: Data(contentsOf: EditorInstallation.active.appendingPathComponent("receipt.json"))) as? [String: Any]
        guard receipt?["identifier"] as? String == catalog.identifier else { throw EditorServiceError(code: "EDITOR_FILES") }
        if !running {
            let ticket = UUID().uuidString
            let url = EditorInstallation.runtime.appendingPathComponent("launch-" + ticket + ".json")
            let data = try JSONSerialization.data(withJSONObject: ["expires": Date().timeIntervalSince1970 + 30,
                                                                  "runtime": EditorInstallation.runtime.standardizedFileURL.path])
            try data.write(to: url, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.arguments = ["--launch-ticket", ticket]
            configuration.activates = false
            do { _ = try await NSWorkspace.shared.openApplication(at: EditorInstallation.application, configuration: configuration) }
            catch { try? FileManager.default.removeItem(at: url); throw error }
        }
        let editor = try await waitForEditor()
        pendingOpen = false
        let id = operation
        EditorWindowController.shared.dismiss(to: editor) { [weak self] in
            guard let self, self.operation == id else { return }
            self.state = .ready
        }
    }

    private func waitForEditor() async throws -> NSRunningApplication {
        let deadline = ContinuousClock.now.advanced(by: .seconds(30))
        repeat {
            try Task.checkCancellation()
            if cancelling { throw CancellationError() }
            guard steam.isLoggedIn, steam.steamId == account else { throw EditorServiceError(code: "EDITOR_ACCOUNT_CHANGED") }
            if let editor = NSRunningApplication.runningApplications(withBundleIdentifier: "cn.laobamac.Mirage.WEEditor.Editor")
                .first(where: { $0.isFinishedLaunching && !$0.isTerminated }) { return editor }
            try await Task.sleep(for: .milliseconds(100))
        } while ContinuousClock.now < deadline
        throw EditorServiceError(code: "EDITOR_LAUNCH_FAILED", detail: L("编辑器未能启动，请重试或修复安装。"))
    }

    func cancel() {
        pendingOpen = false
        if state == .downloading {
            cancelling = true
            if downloadingRuntime { task?.cancel() }
            else { steam.cancelEditorDownload() }
        } else if state == .preparing {
            cancelling = true
            worker?.cancel()
        } else {
            steam.cancelEditorRequest()
            task?.cancel(); operation = UUID()
            finishCancellation()
        }
    }

    private func finishCancellation() {
        cancelling = false
        downloadingRuntime = false
        state = EditorInstallation.isInstalled ? .ready : .idle
        installed = EditorInstallation.isInstalled
        refreshUsage()
    }

    func switchAccount() {
        guard !busy else { return }
        steam.logout { [weak self] result in
            Task { @MainActor in
                guard let self else { return }
                switch result {
                case .success: self.account = ""; self.pendingOpen = true; self.state = .login
                case .failure(let error): self.fail(error)
                }
            }
        }
    }

    func clean(uninstall: Bool) {
        guard !busy else { return }
        guard !running else { fail(EditorServiceError(code: "EDITOR_BUSY")); return }
        state = .preparing
        phase = uninstall ? "正在卸载编辑器…" : "正在清理编辑器缓存…"
        task = Task {
            do {
                try await Task.detached(priority: .utility) { try EditorInstallation.clean(uninstall: uninstall) }.value
                installed = EditorInstallation.isInstalled
                state = installed ? .ready : .idle
                lastError = nil
                refreshUsage()
            } catch { fail(error) }
        }
    }

    func refreshUsage() {
        installed = EditorInstallation.isInstalled
        Task {
            let value = await Task.detached(priority: .utility) { EditorInstallation.usage() }.value
            usage = value
        }
    }

    private func fail(_ error: Error) {
        lastError = error
        state = (error as? EditorServiceError)?.code == "WE_NOT_OWNED" ? .notOwned : .failed
        pendingOpen = false
        refreshUsage()
    }
}

@MainActor
final class EditorWindowController: NSWindowController {
    static let shared = EditorWindowController()
    private var dismissing = false
    private var transition = UUID()
    private var dismissalCompletions: [() -> Void] = []
    private var fadingWindows: [NSWindow] = []
    private var handoffApplication: NSRunningApplication?
    private var reducesMotion: Bool { NSWorkspace.shared.accessibilityDisplayShouldReduceMotion }

    private init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 320),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Mirage Editor"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.contentViewController = NSHostingController(rootView: EditorSetupView())
        window.center()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    func resize(height: CGFloat) {
        guard let window, !dismissing else { return }
        let size = NSSize(width: 520, height: height)
        guard abs(window.contentLayoutRect.height - height) > 1 else { return }
        let top = window.frame.maxY
        let frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        let target = NSRect(x: window.frame.minX, y: top - frame.height, width: frame.width, height: frame.height)
        if window.isVisible && !reducesMotion {
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.22
                context.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                window.animator().setFrame(target, display: true)
            }
        } else { window.setFrame(target, display: true) }
    }

    override func close() { dismiss() }

    func dismiss(to editor: NSRunningApplication? = nil, completion: (() -> Void)? = nil) {
        guard let window, window.isVisible else {
            if let editor { activateEditor(editor) }
            completion?()
            return
        }
        if let completion { dismissalCompletions.append(completion) }
        if let editor { handoffApplication = editor }
        guard !dismissing else { return }
        dismissing = true
        window.ignoresMouseEvents = true
        var windows = [window]
        if editor != nil {
            var parent = window.sheetParent
            while let ancestor = parent {
                windows.append(ancestor)
                parent = ancestor.sheetParent
            }
        }
        fadingWindows = windows
        let id = UUID(); transition = id
        NSAnimationContext.runAnimationGroup { context in
            context.duration = reducesMotion ? 0 : 0.18
            context.timingFunction = CAMediaTimingFunction(name: .easeIn)
            for fadingWindow in windows { fadingWindow.animator().alphaValue = 0 }
        } completionHandler: { [weak self] in
            Task { @MainActor in
                guard let self, self.transition == id else { return }
                if let parent = window.sheetParent { parent.endSheet(window) }
                window.orderOut(nil)
                // Ending a sheet can reactivate its parent. Hand over focus afterwards.
                if let editor = self.handoffApplication { self.activateEditor(editor) }
                self.handoffApplication = nil
                for fadingWindow in self.fadingWindows { fadingWindow.alphaValue = 1 }
                self.fadingWindows.removeAll()
                window.ignoresMouseEvents = false
                self.dismissing = false
                self.completeDismissal()
            }
        }
    }

    func show() {
        guard let window else { return }
        let wasDismissing = dismissing
        let resumingWindows = fadingWindows
        if wasDismissing {
            // A quick reopen cancels the old fade-out instead of closing the new presentation.
            transition = UUID()
            dismissing = false
            window.ignoresMouseEvents = false
            fadingWindows.removeAll()
            handoffApplication = nil
            completeDismissal()
        }
        AppDelegate.shared.openMainWindow()
        if window.sheetParent == nil {
            window.contentViewController?.view.layoutSubtreeIfNeeded()
            window.alphaValue = reducesMotion ? 1 : 0
            let main = AppDelegate.shared.mainWindowController.window!
            let parent = main.attachedSheet ?? main
            parent.beginSheet(window)
        } else if !wasDismissing { return }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = reducesMotion ? 0 : 0.22
            context.timingFunction = CAMediaTimingFunction(name: .easeOut)
            for resumingWindow in resumingWindows { resumingWindow.animator().alphaValue = 1 }
            window.animator().alphaValue = 1
        }
    }

    private func activateEditor(_ editor: NSRunningApplication) {
        guard !editor.isTerminated else { return }
        NSApp.yieldActivation(to: editor)
        NSApp.hide(nil)
        AppDelegate.shared.mainWindowController.window?.orderOut(nil)
        AppDelegate.shared.enterMenuBarMode()
        AppDelegate.shared.contentViewModel.isWindowVisible = false
        AppDelegate.shared.contentViewModel.isStaging = false
        editor.activate(from: .current, options: [.activateAllWindows])
    }

    private func completeDismissal() {
        let completions = dismissalCompletions
        dismissalCompletions.removeAll()
        completions.forEach { $0() }
    }
}
