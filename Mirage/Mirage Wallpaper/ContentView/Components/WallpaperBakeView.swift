//
//  WallpaperBakeView.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import SwiftUI

struct WallpaperBakeBadge: View {
    var body: some View {
        Label("已烘焙", systemImage: "flame.fill")
            .font(.caption2.bold())
            .foregroundStyle(.white)
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .background(.orange.gradient, in: RoundedRectangle(cornerRadius: 5))
            .accessibilityLabel(Text("已烘焙的视频壁纸"))
    }
}

struct WallpaperBakeView: View {
    let wallpaper: WEWallpaper
    @ObservedObject private var service = WallpaperBakeService.shared
    @Environment(\.dismiss) private var dismiss
    @State private var settings = WallpaperBakeSettings()
    @State private var starting = false
    @State private var permission = CGPreflightScreenCaptureAccess()
    @State private var trusted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("烘焙为视频", systemImage: "flame.fill").font(.title2.bold())
            Text(wallpaper.project.title).font(.headline).lineLimit(2)
            Form {
                HStack {
                    Text("输出尺寸")
                    TextField("宽度", value: $settings.width, format: .number.grouping(.never)).frame(width: 80)
                    Text("×")
                    TextField("高度", value: $settings.height, format: .number.grouping(.never)).frame(width: 80)
                    Text("像素").foregroundStyle(.secondary)
                }
                Picker("帧率", selection: $settings.fps) {
                    ForEach([24, 30, 60], id: \.self) { Text("\($0) FPS").tag($0) }
                }
                Stepper(value: $settings.duration, in: 1...600) {
                    HStack { Text("烘焙时长（秒）"); TextField("时长", value: $settings.duration, format: .number).frame(width: 80) }
                }
                if wallpaper.kind == .scene {
                    Stepper(value: $settings.warmup, in: 0...10) { Text(L("预热：%d 秒", settings.warmup)) }
                }
                if wallpaper.kind != .web {
                    Toggle("包含壁纸自身音频", isOn: $settings.audio)
                }
                if wallpaper.kind != .video { Toggle("高画质（文件更大）", isOn: $settings.highQuality) }
            }
            .formStyle(.grouped)
            .frame(height: wallpaper.kind == .scene ? 270 : 215)
            Text("烘焙会固定当前属性，生成新的 SDR 视频壁纸。鼠标交互、时钟和实时媒体信息不会继续更新；首尾不保证无缝。")
                .font(.callout).foregroundStyle(.secondary)
            if wallpaper.kind == .web {
                Text("网页将以原速实时录制，输出静音。录制期间会显示独立的桌面层窗口。")
                    .font(.callout).foregroundStyle(.secondary)
                if !WallpaperViewModel.isWallpaperTrusted(wallpaper) {
                    Toggle("我信任此网页壁纸并允许运行其脚本", isOn: $trusted)
                }
                if !permission {
                    Button("允许屏幕录制") {
                        permission = CGRequestScreenCaptureAccess()
                        if !permission, let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
                            NSWorkspace.shared.open(url)
                        }
                    }
                    Text("授权后可能需要重新启动 Mirage。").font(.caption).foregroundStyle(.secondary)
                }
            }
            Text(L("预计视频大小：%@", ByteCountFormatter.string(fromByteCount: settings.estimatedBytes, countStyle: .file)))
                .font(.caption).foregroundStyle(.secondary)
            if !settings.isValid {
                Text("尺寸需为 128–4096 的偶数，时长需为 1–600 秒。").foregroundStyle(.red).font(.caption)
            }
            HStack {
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("开始烘焙") {
                    starting = true
                    if wallpaper.kind == .web && trusted { WallpaperViewModel.trustForSession(wallpaper) }
                    Task { await service.enqueue(wallpaper, settings: settings); starting = false }
                }
                .buttonStyle(.borderedProminent)
                .disabled(starting || !settings.isValid || (wallpaper.kind == .web &&
                    (!permission || (!trusted && !WallpaperViewModel.isWallpaperTrusted(wallpaper)))))
            }
        }
        .padding(24)
        .frame(width: 540)
        .onAppear {
            if let display = AppDelegate.shared.wallpaperViewModel.selectedDisplay {
                let width = CGDisplayPixelsWide(display.displayID), height = CGDisplayPixelsHigh(display.displayID)
                if width > 0 && height > 0 {
                    let scale = min(1, 4096.0 / Double(max(width, height)))
                    settings.width = max(128, Int(Double(width) * scale) / 2 * 2)
                    settings.height = max(128, Int(Double(height) * scale) / 2 * 2)
                }
            }
        }
    }
}

struct WallpaperBakeTasksView: View {
    @ObservedObject private var service = WallpaperBakeService.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Label("烘焙任务", systemImage: "flame.fill").font(.title2.bold())
                Spacer()
                Button("清除已结束任务") { service.clearFinished() }
            }
            if service.jobs.isEmpty { Text("暂无烘焙任务").foregroundStyle(.secondary) }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 16) {
                    ForEach(service.jobs) { job in
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text(job.wallpaper.project.title).font(.headline).lineLimit(1)
                                Spacer()
                                Text(status(job.state)).foregroundStyle(job.state == "failed" ? .red : .secondary)
                                if job.state == "complete" || (!job.finished && job.progress > 0) {
                                    Text("\(Int((job.progress * 100).rounded()))%")
                                        .monospacedDigit().foregroundStyle(.secondary)
                                }
                            }
                            if job.state == "complete" {
                                ProgressView(value: 1)
                            } else if !job.finished {
                                if job.state == "queued" || job.state == "preparing" {
                                    ProgressView()
                                } else {
                                    ProgressView(value: job.progress)
                                }
                            }
                            if let error = job.error { Text(error).font(.callout).foregroundStyle(.red).textSelection(.enabled) }
                            HStack {
                                if !job.finished { Button("取消烘焙") { service.cancel(job.id) }.disabled(job.state == "cancelling") }
                                if let output = job.output {
                                    Button("在访达中显示") { NSWorkspace.shared.activateFileViewerSelecting([output]) }
                                    Button("设为壁纸") { AppDelegate.shared.wallpaperViewModel.requestApply(WEWallpaper.load(from: output), to: AppDelegate.shared.wallpaperViewModel.selectedDisplayKey) }
                                }
                                if let log = job.log { Button("查看日志") { NSWorkspace.shared.open(log) } }
                            }
                            Divider()
                        }
                    }
                }
            }
            HStack { Spacer(); Button("关闭") { dismiss() }.keyboardShortcut(.cancelAction) }
        }
        .padding(24).frame(width: 600, height: 450)
    }

    private func status(_ state: String) -> String {
        switch state {
        case "queued": return L("等待烘焙")
        case "preparing": return L("准备烘焙")
        case "progress": return L("正在烘焙")
        case "verifying": return L("验证烘焙结果")
        case "complete": return L("烘焙完成")
        case "cancelled": return L("烘焙已取消")
        case "cancelling": return L("正在取消烘焙")
        case "failed", "error": return L("烘焙失败")
        default: return L("正在烘焙")
        }
    }
}
