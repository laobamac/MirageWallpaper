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
    private struct Resolution: Hashable {
        let width: Int
        let height: Int
    }

    private static let resolutions: [Resolution] = [
        Resolution(width: 1280, height: 720),
        Resolution(width: 1920, height: 1080),
        Resolution(width: 2560, height: 1440),
        Resolution(width: 3840, height: 2160),
        Resolution(width: 720, height: 1280),
        Resolution(width: 1080, height: 1920),
        Resolution(width: 1440, height: 2560),
        Resolution(width: 2160, height: 3840)
    ]

    let wallpaper: WEWallpaper
    @ObservedObject private var service = WallpaperBakeService.shared
    @Environment(\.dismiss) private var dismiss
    @State private var settings = WallpaperBakeSettings()
    @State private var selectedResolution: Resolution? = Resolution(width: 1920, height: 1080)
    @State private var starting = false
    @State private var permission = CGPreflightScreenCaptureAccess()
    @State private var trusted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("烘焙为视频", systemImage: "flame.fill").font(.title2.bold())
            Text(wallpaper.project.title).font(.headline).lineLimit(2)
            Form {
                Picker("输出尺寸", selection: $selectedResolution) {
                    Text("自定义").tag(Resolution?.none)
                    ForEach(Self.resolutions, id: \.self) { resolution in
                        Text("\(resolution.width) × \(resolution.height)").tag(Optional(resolution))
                    }
                }
                .pickerStyle(.menu)
                .onChange(of: selectedResolution) { _, resolution in
                    if let resolution {
                        settings.width = resolution.width
                        settings.height = resolution.height
                    }
                }
                HStack {
                    TextField("宽度", value: Binding(
                        get: { settings.width },
                        set: { settings.width = $0; selectedResolution = nil }
                    ), format: .number.grouping(.never)).frame(width: 80)
                    Text("×")
                    TextField("高度", value: Binding(
                        get: { settings.height },
                        set: { settings.height = $0; selectedResolution = nil }
                    ), format: .number.grouping(.never)).frame(width: 80)
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
            .frame(height: wallpaper.kind == .scene ? 310 : 255)
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
                    selectedResolution = Self.resolutions.first {
                        $0.width == settings.width && $0.height == settings.height
                    }
                }
            }
        }
    }
}

struct WallpaperBakeTasksView: View {
    @ObservedObject private var service = WallpaperBakeService.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Label("烘焙任务", systemImage: "flame.fill")
                    .font(.title3.weight(.semibold))
                Spacer()
                Button { service.clearFinished() } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.borderless)
                .help("清除已结束任务")
                .disabled(!service.jobs.contains(where: \.finished))
            }
            .padding(.horizontal, 22)
            .padding(.vertical, 17)

            Divider()

            if service.jobs.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "film.stack")
                        .font(.system(size: 28, weight: .light))
                        .foregroundStyle(.tertiary)
                    Text("暂无烘焙任务")
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(service.jobs) { job in
                            taskRow(job)
                            if job.id != service.jobs.last?.id { Divider().padding(.leading, 90) }
                        }
                    }
                }
            }

            Divider()
            HStack {
                Spacer()
                Button("关闭") { dismiss() }.keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 22)
            .padding(.vertical, 13)
        }
        .frame(width: 620, height: 440)
    }

    private func taskRow(_ job: WallpaperBakeService.Job) -> some View {
        HStack(alignment: .top, spacing: 14) {
            WorkshopImage(wallpaper: job.wallpaper, contentMode: .fill,
                isAnimating: false, isLoadingEnabled: true, preloadsWhenInactive: false,
                showsLoadingIndicator: false)
                .frame(width: 54, height: 54)
                .clipped()
                .clipShape(RoundedRectangle(cornerRadius: 5))

            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Text(job.wallpaper.project.title)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    if job.state == "complete" || (!job.finished && job.progress > 0) {
                        Text("\(Int((job.progress * 100).rounded()))%")
                            .font(.subheadline.weight(.semibold))
                            .monospacedDigit()
                            .foregroundStyle(job.state == "complete" ? .green : .primary)
                            .frame(minWidth: 42, alignment: .trailing)
                    }
                }

                HStack(spacing: 6) {
                    Image(systemName: statusIcon(job.state))
                        .font(.caption)
                    Text(status(job.state))
                        .font(.caption)
                }
                .foregroundStyle(statusColor(job.state))

                GeometryReader { geometry in
                    ZStack(alignment: .leading) {
                        RoundedRectangle(cornerRadius: 2)
                            .fill(Color.primary.opacity(0.1))
                        RoundedRectangle(cornerRadius: 2)
                            .fill(statusColor(job.state))
                            .frame(width: geometry.size.width * min(1, max(0, job.progress)))
                    }
                }
                .frame(height: 4)

                if let error = job.error {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            HStack(spacing: 12) {
                if let output = job.output {
                    Button { AppDelegate.shared.wallpaperViewModel.requestApply(
                        WEWallpaper.load(from: output), to: AppDelegate.shared.wallpaperViewModel.selectedDisplayKey)
                    } label: { Image(systemName: "desktopcomputer") }
                    .help("设为壁纸")
                    Button { NSWorkspace.shared.activateFileViewerSelecting([output]) } label: {
                        Image(systemName: "folder")
                    }
                    .help("在访达中显示")
                }
                if let log = job.log {
                    Button { NSWorkspace.shared.open(log) } label: { Image(systemName: "doc.text") }
                        .help("查看日志")
                }
                if !job.finished {
                    Button { service.cancel(job.id) } label: { Image(systemName: "xmark") }
                        .help("取消烘焙")
                        .disabled(job.state == "cancelling")
                }
            }
            .font(.system(size: 13))
            .buttonStyle(.borderless)
            .foregroundStyle(.secondary)
            .frame(minWidth: 55, alignment: .trailing)
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 16)
    }

    private func statusIcon(_ state: String) -> String {
        switch state {
        case "complete": return "checkmark.circle.fill"
        case "failed", "error": return "exclamationmark.circle.fill"
        case "cancelled", "cancelling": return "xmark.circle"
        case "queued": return "clock"
        default: return "flame.fill"
        }
    }

    private func statusColor(_ state: String) -> Color {
        switch state {
        case "complete": return .green
        case "failed", "error": return .red
        case "cancelled", "cancelling": return .secondary
        default: return .orange
        }
    }

    private func status(_ state: String) -> String {
        switch state {
        case "queued": return L("等待烘焙")
        case "checking": return L("检查源文件")
        case "preparing": return L("准备烘焙")
        case "warming": return L("预热场景")
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
