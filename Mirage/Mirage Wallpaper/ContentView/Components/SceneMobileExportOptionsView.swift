import SwiftUI

struct SceneMobileExportOptionsView: View {
    let request: SceneMobileExportRequest
    let confirm: (SceneMobileExportOptions) -> Void

    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var localization = MirageLocalization.shared
    @State private var options = SceneMobileExportOptions.highQuality
    @State private var hasSelectedQuality = false
    @State private var showsAdvanced = false

    private let background = Color(red: 0.133, green: 0.133, blue: 0.133)
    private let accent = Color(red: 0.24, green: 0.49, blue: 0.97)
    private var sheetWidth: CGFloat { min(900, (NSScreen.main?.visibleFrame.width ?? 980) - 80) }
    private var cardWidth: CGFloat { min(150, (sheetWidth - 180) / 3) }
    private var bodyHeight: CGFloat {
        let desired: CGFloat = showsAdvanced ? 460 : (hasSelectedQuality ? 355 : 320)
        return min(desired, max(220, (NSScreen.main?.visibleFrame.height ?? 900) - 190))
    }

    var body: some View {
        VStack(spacing: 0) {
            Text(heading)
                .font(.system(size: 20, weight: .regular))
                .frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.horizontal, 18)
                .padding(.vertical, 18)
            divider

            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    Text(L("Mirage 将优化壁纸，以便您的设备达到最佳性能。请选择最能代表您设备的性能等级。如果视觉质量或性能不符合您的喜好，您可以稍后更改此选择，然后将壁纸重新上传到设备。"))
                        .font(.system(size: 15))
                        .fixedSize(horizontal: false, vertical: true)

                    Text(informationNotice)
                        .font(.system(size: 15))
                        .tint(accent)
                        .foregroundStyle(Color(red: 0.19, green: 0.46, blue: 0.58))
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(16)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color(red: 0.84, green: 0.93, blue: 0.97))
                        .overlay(Rectangle().strokeBorder(Color(red: 0.66, green: 0.87, blue: 0.91)))

                    HStack(alignment: .top, spacing: 20) {
                        optionGroup(title: "动态") {
                            HStack(spacing: 5) {
                                qualityCard(title: "高质量", preset: .highQuality, icon: .high)
                                qualityCard(title: "均衡", preset: .balanced, icon: .balanced)
                            }
                        }
                        optionGroup(title: "预渲染") {
                            qualityCard(title: "高性能", preset: nil, icon: .film)
                                .help(L("预渲染视频导出暂不可用"))
                        }
                    }
                    .padding(.top, 3)
                    .frame(maxWidth: .infinity)

                    if hasSelectedQuality {
                        Toggle(L("显示高级设置"), isOn: $showsAdvanced)
                            .toggleStyle(MobileExportCheckboxStyle())
                            .frame(maxWidth: .infinity)
                    }

                    if showsAdvanced {
                        VStack(spacing: 20) {
                            HStack {
                                settingLabel("像素画优化")
                                Toggle("", isOn: $options.pixelArtOptimization)
                                    .toggleStyle(MobileExportCheckboxStyle())
                                    .accessibilityLabel(L("像素画优化"))
                                    .help(L("保留 RGBA 纹理，减少像素画的压缩损失。文件可能增大。"))
                                Spacer()
                            }
                            HStack {
                                settingLabel("纹理分辨率降低")
                                Menu {
                                    ForEach(SceneMobileExportOptions.TextureReduction.allCases) { reduction in
                                        Button {
                                            options.textureReduction = reduction
                                        } label: {
                                            if options.textureReduction == reduction {
                                                Label(reduction.title, systemImage: "checkmark")
                                            } else {
                                                Text(reduction.title)
                                            }
                                        }
                                    }
                                } label: {
                                    HStack {
                                        Text(options.textureReduction.title)
                                            .frame(maxWidth: .infinity, alignment: .leading)
                                        Image(systemName: "chevron.down")
                                            .font(.system(size: 10, weight: .bold))
                                    }
                                    .font(.system(size: 14))
                                    .padding(.horizontal, 14)
                                    .frame(height: 30)
                                    .background(Color.white.opacity(0.02), in: RoundedRectangle(cornerRadius: 3))
                                    .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Color.white.opacity(0.07)))
                                }
                                .menuStyle(.borderlessButton)
                                .menuIndicator(.hidden)
                                .accessibilityLabel(L("纹理分辨率降低"))
                                .accessibilityValue(options.textureReduction.title)
                                .help(L("蒙版和较小纹理保留原尺寸；超大纹理仍受 4096 像素兼容上限限制。"))
                            }
                        }
                        .padding(.horizontal, 14)
                        .padding(.vertical, 4)
                    }
                }
                .padding(.horizontal, 18)
                .padding(.top, 18)
                .padding(.bottom, 22)
            }
            .frame(height: bodyHeight)

            divider
            HStack(spacing: 10) {
                Spacer()
                Button {
                    confirm(options)
                    dismiss()
                } label: {
                    footerLabel("确认", fill: accent)
                        .opacity(hasSelectedQuality ? 1 : 0.55)
                }
                .disabled(!hasSelectedQuality)
                .keyboardShortcut(.defaultAction)
                Button { dismiss() } label: {
                    footerLabel("取消", fill: Color(white: 0.24))
                }
                .keyboardShortcut(.cancelAction)
            }
            .buttonStyle(.plain)
            .padding(16)
        }
        .frame(width: sheetWidth)
        .background(background)
        .foregroundStyle(Color(white: 0.93))
        .preferredColorScheme(.dark)
        .environment(\.locale, localization.locale)
    }

    private var divider: some View { Rectangle().fill(Color(white: 0.24)).frame(height: 1) }

    private var heading: String {
        switch request.destination {
        case .device: return L("将“%@”发送至 Android", request.wallpaper.project.title)
        case .file: return L("导出“%@”以在 Android 上使用", request.wallpaper.project.title)
        }
    }

    private var informationNotice: AttributedString {
        var prefix = AttributedString(L("提示："))
        prefix.font = .system(size: 15, weight: .bold)
        switch request.destination {
        case .device:
            prefix.append(AttributedString(L("设备保持连接时，转换完成后会自动开始发送，无需先导出文件。")))
        case .file:
            prefix.append(AttributedString(L("将您的 Android 设备连接到 Mirage，即可无线传输壁纸，无需先导出。")))
            var link = AttributedString(L("点击此处了解更多信息。"))
            link.link = URL(string: "https://help.wallpaperengine.io/mobile/pairing.html")
            prefix.append(link)
        }
        return prefix
    }

    private func settingLabel(_ title: String) -> some View {
        Text(L(title)).font(.system(size: 14, weight: .bold))
            .frame(width: 210, alignment: .leading)
    }

    private func footerLabel(_ title: String, fill: Color) -> some View {
        Text(L(title)).font(.system(size: 15))
            .frame(width: 100, height: 28)
            .background(fill, in: RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Color.white.opacity(0.06)))
    }

    private func optionGroup<Content: View>(title: String, @ViewBuilder content: () -> Content) -> some View {
        content()
            .padding(16)
            .overlay(Rectangle().strokeBorder(accent, lineWidth: 1))
            .overlay(alignment: .topLeading) {
                Text(L(title)).font(.system(size: 15))
                    .padding(.horizontal, 5)
                    .background(background)
                    .offset(x: 10, y: -10)
            }
    }

    private func qualityCard(title: String, preset: SceneMobileExportOptions?, icon: MobileExportQualityIcon.Kind) -> some View {
        let selected = hasSelectedQuality && preset == options
        let fill = selected ? accent : Color(white: 0.23)
        return Button {
            guard let preset else { return }
            options = preset
            hasSelectedQuality = true
        } label: {
            VStack(spacing: 12) {
                Text(L(title)).font(.system(size: 16))
                MobileExportQualityIcon(kind: icon, cutout: fill)
                    .frame(width: 52, height: 52)
                    .accessibilityHidden(true)
            }
            .frame(width: cardWidth, height: 114)
            .background(fill, in: RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Color.white.opacity(0.06)))
        }
        .buttonStyle(.plain)
        .disabled(preset == nil)
        .accessibilityLabel(L(title))
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

private struct MobileExportCheckboxStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        Button { configuration.isOn.toggle() } label: {
            HStack(spacing: 6) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(configuration.isOn ? Color(red: 0.24, green: 0.49, blue: 0.97) : .clear)
                    .overlay(RoundedRectangle(cornerRadius: 2).strokeBorder(Color(red: 0.24, green: 0.49, blue: 0.97), lineWidth: 2))
                    .overlay {
                        if configuration.isOn {
                            Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(Color(white: 0.13))
                        }
                    }
                    .frame(width: 19, height: 19)
                configuration.label.font(.system(size: 15))
            }
        }
        .buttonStyle(.plain)
        .accessibilityValue(configuration.isOn ? L("已开启") : L("已关闭"))
    }
}

private struct MobileExportQualityIcon: View {
    enum Kind { case high, balanced, film }
    let kind: Kind
    let cutout: Color

    var body: some View {
        Canvas { context, size in
            let side = min(size.width, size.height)
            func rect(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> CGRect {
                CGRect(x: x * side, y: y * side, width: w * side, height: h * side)
            }
            if kind == .film {
                context.fill(Path(roundedRect: rect(0.05, 0.08, 0.9, 0.84), cornerRadius: side * 0.12), with: .color(Color(white: 0.93)))
                for y in [0.2, 0.55] {
                    context.fill(Path(roundedRect: rect(0.33, y, 0.34, 0.24), cornerRadius: side * 0.035), with: .color(cutout))
                }
                for x in [0.14, 0.75] {
                    for y in [0.2, 0.43, 0.66] {
                        context.fill(Path(roundedRect: rect(x, y, 0.11, 0.12), cornerRadius: side * 0.02), with: .color(cutout))
                    }
                }
            } else {
                context.fill(Path(ellipseIn: rect(0.04, 0.04, 0.92, 0.92)), with: .color(Color(white: 0.97)))
                for angle in [180.0, 225, 270, 315, 360] {
                    let radians = angle * .pi / 180
                    let x = 0.5 + cos(radians) * 0.29
                    let y = 0.5 + sin(radians) * 0.29
                    context.fill(Path(ellipseIn: rect(x - 0.055, y - 0.055, 0.11, 0.11)), with: .color(cutout))
                }
                let hub = CGPoint(x: side * 0.5, y: side * 0.67)
                var needle = Path()
                needle.move(to: hub)
                needle.addLine(to: CGPoint(x: side * (kind == .high ? 0.8 : 0.5), y: side * (kind == .high ? 0.48 : 0.21)))
                context.stroke(needle, with: .color(cutout), style: StrokeStyle(lineWidth: side * 0.08, lineCap: .round))
                context.fill(Path(ellipseIn: rect(0.39, 0.56, 0.22, 0.22)), with: .color(cutout))
            }
        }
    }
}
