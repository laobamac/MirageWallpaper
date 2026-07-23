//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct PlaylistSettingsSheet: View {
    @ObservedObject var manager: PlaylistManager
    let screen: Int
    @Binding var isPresented: Bool

    @State private var settings = PlaylistSettings.default()
    @State private var didLoad = false
    @State private var showDayOfWeekWarning = false

    private static let weekdaySymbols: [String] = [
        L("星期日"), L("星期一"), L("星期二"), L("星期三"),
        L("星期四"), L("星期五"), L("星期六")
    ]

    private var itemCount: Int { manager.current(on: screen).items.count }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(L("播放列表设置"))
                    .font(.title2.bold())
                Spacer()
                Button(L("重置")) {
                    settings = .default()
                }
                .buttonStyle(.bordered)
            }
            .padding(20)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    orderSection
                    timingSection
                    transitionSection
                    optionsSection
                }
                .padding(20)
            }
            .frame(height: 420)

            Divider()

            HStack {
                Spacer()
                Button(L("取消")) { isPresented = false }
                Button(action: commit) {
                    Text(L("好")).frame(width: 60)
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
            }
            .padding(20)
        }
        .frame(width: 520)
        .onAppear {
            guard !didLoad else { return }
            didLoad = true
            settings = manager.current(on: screen).settings
        }
    }

    // MARK: Sections

    private var orderSection: some View {
        section(L("播放顺序")) {
            Picker("", selection: $settings.order) {
                ForEach(PlaylistOrder.allCases) { Text($0.displayName).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
        }
    }

    private var timingSection: some View {
        section(L("更换壁纸")) {
            VStack(alignment: .leading, spacing: 14) {
                Picker("", selection: $settings.timing) {
                    ForEach(PlaylistTiming.allCases) { Text($0.displayName).tag($0) }
                }
                .labelsHidden()
                .frame(maxWidth: 220)

                switch settings.timing {
                case .timer:
                    timerControls
                case .daytime:
                    daytimeControls
                case .dayOfWeek:
                    dayOfWeekControls
                case .logon:
                    Text(L("仅在 Mirage 启动时切换到列表中的壁纸。"))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                case .never:
                    Text(L("壁纸不会自动更换，仅手动点选切换。"))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private var timerControls: some View {
        HStack(spacing: 16) {
            HStack(spacing: 6) {
                Stepper(value: $settings.timerHours, in: 0...24) {
                    Text("\(settings.timerHours)")
                        .frame(minWidth: 24, alignment: .trailing)
                        .monospacedDigit()
                }
                Text(L("小时"))
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 6) {
                Stepper(value: $settings.timerMinutes, in: 0...59) {
                    Text("\(settings.timerMinutes)")
                        .frame(minWidth: 24, alignment: .trailing)
                        .monospacedDigit()
                }
                Text(L("分钟"))
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var daytimeControls: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(L("选择每天切换壁纸的时刻"))
                .font(.footnote)
                .foregroundStyle(.secondary)
            let columns = Array(repeating: GridItem(.fixed(44), spacing: 6), count: 8)
            LazyVGrid(columns: columns, spacing: 6) {
                ForEach(0..<24, id: \.self) { hour in
                    let active = settings.daytimeAnchors.contains(hour)
                    Button {
                        toggleAnchor(hour)
                    } label: {
                        Text(String(format: "%02d", hour))
                            .font(.caption.monospacedDigit())
                            .frame(width: 40, height: 26)
                            .background(
                                RoundedRectangle(cornerRadius: 6)
                                    .fill(active ? Color.accentColor : Color.secondary.opacity(0.15))
                            )
                            .foregroundStyle(active ? Color.white : Color.primary)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private var dayOfWeekControls: some View {
        VStack(alignment: .leading, spacing: 8) {
            if itemCount > 7 {
                HStack(spacing: 6) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                    Text(L("星期播放列表不支持超过 7 张壁纸。"))
                        .font(.footnote)
                    Button(L("立即移除多余的壁纸")) {
                        trimToSeven()
                    }
                    .buttonStyle(.link)
                }
            }
            Text(L("列表中的前 7 张壁纸将分别对应星期日至星期六。"))
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    private var transitionSection: some View {
        section(L("显示壁纸过渡")) {
            VStack(alignment: .leading, spacing: 14) {
                Picker("", selection: $settings.transition) {
                    ForEach(PlaylistTransitionKind.allCases) { Text($0.displayName).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()

                if settings.transition != .disabled {
                    HStack {
                        Text(L("过渡时间"))
                            .foregroundStyle(.secondary)
                        MirageSlider(value: $settings.transitionSeconds, in: 0.2...5.0, step: 0.1)
                            .frame(width: 160)
                        Text(String(format: "%.1fs", settings.transitionSeconds))
                            .frame(width: 40)
                            .monospacedDigit()
                    }
                }
            }
        }
    }

    private var optionsSection: some View {
        section(L("选项")) {
            VStack(alignment: .leading, spacing: 10) {
                Toggle(L("总是从第一张壁纸开始"), isOn: $settings.alwaysBeginFirst)
                Toggle(L("第一张壁纸仅在启动时播放"), isOn: $settings.introOnStartup)
                Toggle(L("在视频结束时更换壁纸"), isOn: $settings.videoSequence)
                Toggle(L("允许壁纸在暂停时更换"), isOn: $settings.updateOnPause)
            }
            .toggleStyle(.checkbox)
        }
    }

    // MARK: Helpers

    private func section<Content: View>(_ title: String,
                                        @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 3) {
                Text(title).font(.headline)
                VStack { Divider().overlay(Color.accentColor) }
            }
            content()
        }
    }

    private func toggleAnchor(_ hour: Int) {
        if let idx = settings.daytimeAnchors.firstIndex(of: hour) {
            settings.daytimeAnchors.remove(at: idx)
        } else {
            settings.daytimeAnchors.append(hour)
            settings.daytimeAnchors.sort()
        }
    }

    private func trimToSeven() {
        manager.trimItems(to: 7, on: screen)
    }

    private func commit() {
        manager.updateSettings(on: screen) { $0 = settings }
        isPresented = false
    }
}
