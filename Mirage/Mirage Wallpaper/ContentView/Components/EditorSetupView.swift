//
//  EditorSetupView.swift
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AppKit

struct EditorSetupView: View {
    @Environment(\.accessibilityReduceMotion) private var reducesMotion
    @ObservedObject private var editor = EditorManager.shared
    @ObservedObject private var steam = SteamServiceManager.shared
    @ObservedObject private var localization = MirageLocalization.shared
    @StateObject private var login = SteamSetupViewModel()
    @State private var confirmUninstall = false
    @State private var confirmClean = false

    private func bytes(_ value: Int64) -> String { ByteCountFormatter.string(fromByteCount: value, countStyle: .file) }
    private var transitionAnimation: Animation? { reducesMotion ? nil : .easeInOut(duration: 0.18) }

    private var panelHeight: CGFloat {
        switch editor.state {
        case .login:
            if case .waitingForQR = login.loginState { return 530 }
            return 410
        case .confirmation: return 365
        case .downloading: return 360
        case .checking, .preparing: return 235
        case .failed, .notOwned: return editor.installed || editor.usage.reclaimable > 0 ? 380 : 270
        case .ready, .idle: return 330
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(systemName: "pencil.and.outline")
                    .font(.system(size: 23, weight: .medium))
                    .foregroundStyle(Color.accentColor)
                    .frame(width: 44, height: 44)
                    .background(Color.accentColor.opacity(0.1), in: RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 3) {
                    Text("Mirage Editor").font(.headline)
                    Text(editor.state == .login ? L("登录 Steam 以使用编辑器") : L("壁纸编辑器"))
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if !steam.accountName.isEmpty && editor.state != .login {
                    VStack(alignment: .trailing, spacing: 3) {
                        Label(steam.accountName, systemImage: "person.crop.circle").font(.caption).foregroundStyle(.secondary)
                        Button("切换账号") { editor.switchAccount() }.buttonStyle(.link).font(.caption).disabled(editor.busy)
                    }
                }
            }
            .padding(.horizontal, 20).padding(.top, 10).padding(.bottom, 16)
            Divider()
            VStack(alignment: .leading, spacing: 14) {
                if editor.state == .login {
                    Text("登录拥有 Wallpaper Engine 的 Steam 账号，以下载并打开编辑器。")
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    SteamLoginStep(viewModel: login, editorLogin: true)
                    Text("登录由本机直连 Steam，登录凭据保存在 macOS 钥匙串中。")
                        .font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                } else {
                    content.id(editor.state).transition(.opacity)
                }
            }
            .padding(20)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .animation(transitionAnimation, value: editor.state)
            if !editor.busy && editor.state != .confirmation && editor.state != .login &&
                (editor.installed || editor.usage.reclaimable > 0 || editor.state == .idle || editor.state == .ready) {
                management.padding(.horizontal, 20).padding(.bottom, 16)
            }
            Divider()
            HStack {
                if editor.busy && editor.canCancel {
                    Button(editor.cancelling ? L("正在取消…") : L("取消")) { editor.cancel() }.disabled(editor.cancelling)
                } else if editor.state == .login {
                    Button("取消登录") { login.cancelPendingWork(); editor.cancel() }
                }
                Spacer()
                Button(editor.state == .confirmation ? L("取消") : L("关闭")) {
                    if editor.state == .login { login.cancelPendingWork(); editor.cancel() }
                    if editor.state == .confirmation { editor.cancel() }
                    EditorWindowController.shared.close()
                }.keyboardShortcut(.cancelAction)
                if editor.state == .confirmation {
                    Button("下载并安装") { editor.install() }.buttonStyle(.borderedProminent).disabled(!editor.canInstall)
                } else if editor.state == .failed {
                    Button("重试") { editor.open() }.buttonStyle(.borderedProminent)
                } else if editor.state == .ready || editor.state == .idle {
                    Button(editor.installed ? L("打开编辑器") : L("安装编辑器")) { editor.open() }.buttonStyle(.borderedProminent)
                }
            }
            .padding(.horizontal, 20).padding(.vertical, 12).background(.bar)
        }
        .font(.callout)
        .frame(width: 520, height: panelHeight)
        .background(Color(nsColor: .windowBackgroundColor))
        .environment(\.locale, localization.locale)
        .onAppear { editor.refreshUsage(); EditorWindowController.shared.resize(height: panelHeight) }
        .onChange(of: panelHeight) { _, height in EditorWindowController.shared.resize(height: height) }
        .confirmationDialog("卸载编辑器？", isPresented: $confirmUninstall) {
            Button("卸载编辑器", role: .destructive) { editor.clean(uninstall: true) }
        } message: {
            Text(L("将移除编辑器程序、运行环境和下载缓存。工程会保留。预计释放 %@。", bytes(editor.usage.uninstallable)))
        }
        .confirmationDialog("清理编辑器缓存？", isPresented: $confirmClean) {
            Button("清理缓存") { editor.clean(uninstall: false) }
        } message: {
            Text(L("清理下载残留、旧运行环境、界面缓存和日志，保留当前编辑器及工程。预计释放 %@。", bytes(editor.usage.reclaimable)))
        }
    }

    @ViewBuilder private var content: some View {
        switch editor.state {
        case .checking, .preparing:
            HStack(spacing: 12) {
                ProgressView().controlSize(.small)
                Text(L(editor.phase)).contentTransition(.opacity)
            }
            .animation(transitionAnimation, value: editor.phase)
            if editor.state == .preparing { Text("首次准备可能需要几分钟。关闭此窗口后任务会继续。").font(.caption).foregroundStyle(.secondary) }
        case .downloading:
            Text(L(editor.phase)).font(.headline)
            ProgressView(value: Double(editor.receivedBytes), total: Double(max(1, editor.totalBytes)))
            HStack {
                Text(bytes(editor.receivedBytes) + " / " + bytes(editor.totalBytes))
                Spacer()
                Text(bytes(Int64(editor.bytesPerSecond)) + "/s")
            }.font(.caption.monospacedDigit())
            if editor.totalFiles > 0 {
                Text(L("文件进度：%d / %d", editor.completedFiles, editor.totalFiles))
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            if editor.activeChunks > 0 {
                Text(L("正在并行下载 %d 个文件（%d 个分块）", editor.activeFiles, editor.activeChunks))
                    .font(.caption).foregroundStyle(.secondary)
            }
            if !editor.currentFile.isEmpty {
                Text(editor.currentFile).font(.caption).foregroundStyle(.secondary).lineLimit(2).truncationMode(.middle).textSelection(.enabled)
            }
            if let eta = editor.etaSeconds, eta.isFinite {
                Text(L("预计剩余 %d 分钟", max(1, Int(eta / 60)))).font(.caption).foregroundStyle(.secondary)
            }
            Text("关闭此窗口后下载会继续；取消后可继续下载。").font(.caption).foregroundStyle(.secondary)
        case .confirmation:
            Text("首次使用需要下载编辑器运行组件和 Wallpaper Engine。").fixedSize(horizontal: false, vertical: true)
            Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 10) {
                GridRow { Text("下载大小"); Text(bytes(editor.totalBytes)) }
                GridRow { Text("预计安装占用"); Text(bytes(editor.installedBytes)) }
                GridRow { Text("安装期间所需可用空间"); Text(bytes(editor.requiredBytes)).bold() }
                GridRow { Text("当前可用空间"); Text(bytes(editor.availableBytes)) }
            }.font(.callout).padding(14).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 8))
            Text("安装期间需要额外空间保存下载文件。安装后可在此清理缓存或卸载编辑器。").font(.caption).foregroundStyle(.secondary)
            if !editor.canInstall { Text("可用磁盘空间不足，请释放空间后重试。").foregroundStyle(.red) }
        case .notOwned:
            Text(editor.errorMessage ?? "").foregroundStyle(.secondary)
            Button("购买 Wallpaper Engine") { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/app/431960/")!) }
        case .failed:
            Text(editor.errorMessage ?? "").foregroundStyle(.red).textSelection(.enabled)
        case .ready, .idle:
            Text(editor.installed ? L("编辑器已安装。") : L("登录拥有 Wallpaper Engine 的 Steam 账号后，即可下载并使用编辑器。"))
        case .login: EmptyView()
        }
    }

    private var management: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(L("已安装：%@ · 可清理：%@", bytes(editor.usage.installed), bytes(editor.usage.reclaimable)))
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("修复安装") { editor.repair() }.disabled(!editor.installed)
                Button("清理缓存") { confirmClean = true }.disabled(editor.usage.reclaimable == 0)
                Button("卸载编辑器", role: .destructive) { confirmUninstall = true }.disabled(!editor.installed && editor.usage.reclaimable == 0)
                Spacer()
                Button { NSWorkspace.shared.open(EditorInstallation.root) } label: { Image(systemName: "folder") }
                    .help(L("在访达中显示"))
            }
        }
    }
}
