//
//  Mirage Wallpaper
//
//  Scene-wallpaper quality selection before Android conversion.
//

import SwiftUI

struct SceneMobileExportOptionsView: View {
    private enum QualityOption: Equatable {
        case dynamicHighQuality
        case dynamicBalanced
        case prerenderedHighPerformance
    }

    let request: SceneMobileExportRequest
    let confirm: () -> Void

    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var localization = MirageLocalization.shared
    @State private var selectedQuality: QualityOption = .dynamicHighQuality

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 18) {
                Text(heading)
                    .font(.title3.weight(.semibold))

                Text(L("Mirage 会针对移动设备转换场景壁纸。请选择最适合设备的性能等级；以后可以重新转换并发送其他版本。"))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                notice(
                    icon: "iphone.radiowaves.left.and.right",
                    color: .blue,
                    text: informationNotice
                )

                notice(
                    icon: "exclamationmark.triangle.fill",
                    color: .yellow,
                    text: L("高分辨率场景对移动设备的性能要求较高。如果出现卡顿，可以在后续版本中改用均衡或预渲染模式。")
                )

                HStack(alignment: .top, spacing: 16) {
                    optionGroup(title: "动态") {
                        HStack(spacing: 10) {
                            qualityCard(
                                option: .dynamicHighQuality,
                                title: "高质量",
                                subtitle: "实时动态场景",
                                icon: "gauge.with.dots.needle.67percent",
                                enabled: true
                            )
                            qualityCard(
                                option: .dynamicBalanced,
                                title: "均衡",
                                subtitle: "即将推出",
                                icon: "dial.medium",
                                enabled: false
                            )
                        }
                    }

                    optionGroup(title: "预渲染") {
                        qualityCard(
                            option: .prerenderedHighPerformance,
                            title: "高性能",
                            subtitle: "即将推出",
                            icon: "film.stack",
                            enabled: false
                        )
                    }
                    .frame(width: 190)
                }
            }
            .padding(24)

            Divider()

            HStack {
                Text(L("动态 · 高质量"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button(L("取消")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(L("确认")) {
                    dismiss()
                    confirm()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(selectedQuality != .dynamicHighQuality)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 14)
        }
        .frame(width: 720)
        .background(Color(nsColor: .windowBackgroundColor))
        .environment(\.locale, localization.locale)
    }

    private var heading: String {
        switch request.destination {
        case .device:
            return L("将“%@”发送至 Android", request.wallpaper.project.title)
        case .file:
            return L("导出“%@”以在 Android 上使用", request.wallpaper.project.title)
        }
    }

    private var informationNotice: String {
        switch request.destination {
        case .device:
            return L("设备保持连接时，转换完成后会自动开始发送，无需先导出文件。")
        case .file:
            return L("转换完成后会将可供 Wallpaper Engine Android 使用的 .mpkg 文件保存到所选位置。")
        }
    }

    private func notice(icon: String, color: Color, text: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(color)
                .frame(width: 18)
            Text(text)
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(color.opacity(0.11), in: RoundedRectangle(cornerRadius: 8))
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(color.opacity(0.22))
        }
    }

    private func optionGroup<Content: View>(
        title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            Text(L(title))
                .font(.subheadline.weight(.medium))
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(Color.accentColor.opacity(0.55))
        }
    }

    private func qualityCard(
        option: QualityOption,
        title: String,
        subtitle: String,
        icon: String,
        enabled: Bool
    ) -> some View {
        let selected = selectedQuality == option
        return Button {
            selectedQuality = option
        } label: {
            VStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 29, weight: .medium))
                    .frame(height: 34)
                Text(L(title))
                    .font(.subheadline.weight(.medium))
                Text(L(subtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, minHeight: 104)
            .padding(.horizontal, 10)
            .background(
                selected ? Color.accentColor.opacity(0.18) : Color.primary.opacity(0.055),
                in: RoundedRectangle(cornerRadius: 7)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 7)
                    .strokeBorder(
                        selected ? Color.accentColor : Color.primary.opacity(0.1),
                        lineWidth: selected ? 1.5 : 1
                    )
            }
            .contentShape(RoundedRectangle(cornerRadius: 7))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.48)
        .accessibilityLabel(L(title))
        .accessibilityValue(L(subtitle))
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}
