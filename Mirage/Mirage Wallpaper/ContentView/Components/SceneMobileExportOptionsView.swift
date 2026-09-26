import SwiftUI

struct SceneMobileExportOptionsView: View {
    private enum Quality: Equatable { case highQuality, balanced }

    let request: SceneMobileExportRequest
    let confirm: (SceneMobileExportOptions) -> Void

    @Environment(\.dismiss) private var dismiss
    @Environment(\.colorScheme) private var colorScheme
    @ObservedObject private var localization = MirageLocalization.shared
    @State private var textureReduction = SceneMobileExportOptions.TextureReduction.half
    @State private var pixelArtOptimization = false
    @State private var selectedQuality: Quality?
    @State private var hasSelectedQuality = false
    @State private var showsAdvanced = false
    @State private var showsTextureChoices = false

    private var exportOptions: SceneMobileExportOptions {
        SceneMobileExportOptions(
            textureReduction: textureReduction,
            pixelArtOptimization: pixelArtOptimization
        )
    }

    private var background: Color { Color(nsColor: .windowBackgroundColor) }
    private var primaryText: Color { Color(nsColor: .labelColor) }
    private let accent = Color(red: 0.24, green: 0.49, blue: 0.97)
    private var cardBackground: Color { Color(nsColor: .controlBackgroundColor) }
    private var subtleBorder: Color { Color(nsColor: .separatorColor) }
    private var noticeBackground: Color {
        colorScheme == .dark
            ? Color(red: 0.14, green: 0.22, blue: 0.26)
            : Color(red: 0.84, green: 0.93, blue: 0.97)
    }
    private var noticeText: Color {
        colorScheme == .dark
            ? Color(red: 0.72, green: 0.86, blue: 0.91)
            : Color(red: 0.19, green: 0.46, blue: 0.58)
    }
    private var sheetWidth: CGFloat { min(900, (NSScreen.main?.visibleFrame.width ?? 980) - 80) }
    private var cardWidth: CGFloat { min(150, (sheetWidth - 180) / 3) }
    private var textureMenuWidth: CGFloat { max(200, sheetWidth - 36 - 28 - 210 - 8) }
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
                        .foregroundStyle(noticeText)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(16)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(noticeBackground)
                        .overlay(Rectangle().strokeBorder(noticeText.opacity(0.28)))

                    HStack(alignment: .top, spacing: 20) {
                        optionGroup(title: "动态") {
                            HStack(spacing: 5) {
                                qualityCard(title: "高质量", quality: .highQuality, preset: .highQuality, icon: .high)
                                qualityCard(title: "均衡", quality: .balanced, preset: .balanced, icon: .balanced)
                            }
                        }
                        optionGroup(title: "预渲染") {
                            qualityCard(title: "高性能", quality: nil, preset: nil, icon: .film)
                                .help(L("预渲染视频导出暂不可用"))
                        }
                    }
                    .padding(.top, 3)
                    .frame(maxWidth: .infinity)

                    if hasSelectedQuality {
                        advancedSettingsControl
                    }

                    if showsAdvanced {
                        VStack(alignment: .leading, spacing: 20) {
                            pixelOptimizationControl
                            HStack(spacing: 0) {
                                settingLabel("纹理分辨率降低")
                                Button {
                                    showsTextureChoices.toggle()
                                } label: {
                                    HStack {
                                        Text(textureReduction.title)
                                        Spacer(minLength: 8)
                                        Image(systemName: "chevron.down")
                                            .font(.system(size: 10, weight: .bold))
                                    }
                                    .font(.system(size: 14))
                                    .frame(width: textureMenuWidth - 28, height: 30, alignment: .leading)
                                    .padding(.horizontal, 14)
                                    .background(cardBackground, in: RoundedRectangle(cornerRadius: 3))
                                    .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(subtleBorder))
                                }
                                .buttonStyle(.plain)
                                .popover(isPresented: $showsTextureChoices, arrowEdge: .top) {
                                    VStack(spacing: 2) {
                                        ForEach(SceneMobileExportOptions.TextureReduction.allCases) { reduction in
                                            Button {
                                                textureReduction = reduction
                                                switch reduction {
                                                case .original: selectedQuality = nil
                                                case .half: selectedQuality = .highQuality
                                                case .quarter: selectedQuality = .balanced
                                                }
                                                showsTextureChoices = false
                                            } label: {
                                                HStack(spacing: 10) {
                                                    Text(reduction.title)
                                                    Spacer(minLength: 8)
                                                    if textureReduction == reduction {
                                                        Image(systemName: "checkmark")
                                                    }
                                                }
                                                .font(.system(size: 14))
                                                .frame(maxWidth: .infinity, alignment: .leading)
                                                .padding(.horizontal, 12)
                                                .frame(height: 30)
                                                .contentShape(Rectangle())
                                            }
                                            .buttonStyle(.plain)
                                            .accessibilityAddTraits(textureReduction == reduction ? .isSelected : [])
                                        }
                                    }
                                    .padding(6)
                                    .frame(width: textureMenuWidth)
                                    .background(cardBackground, in: RoundedRectangle(cornerRadius: 4))
                                    .foregroundStyle(primaryText)
                                }
                                .accessibilityLabel(L("纹理分辨率降低"))
                                .accessibilityValue(textureReduction.title)
                                .help(L("蒙版和较小纹理保留原尺寸。最高质量会保留原始纹理分辨率。"))
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
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
                    confirm(exportOptions)
                    dismiss()
                } label: {
                    footerLabel("确认", fill: accent)
                        .opacity(hasSelectedQuality ? 1 : 0.55)
                }
                .disabled(!hasSelectedQuality)
                .keyboardShortcut(.defaultAction)
                Button { dismiss() } label: {
                    footerLabel("取消", fill: cardBackground)
                }
                .keyboardShortcut(.cancelAction)
            }
            .buttonStyle(.plain)
            .padding(16)
        }
        .frame(width: sheetWidth)
        .background(background)
        .foregroundStyle(primaryText)
        .environment(\.locale, localization.locale)
    }

    private var divider: some View { Rectangle().fill(subtleBorder).frame(height: 1) }

    private var advancedSettingsControl: some View {
        Button {
            showsAdvanced.toggle()
        } label: {
            HStack(spacing: 6) {
                Spacer(minLength: 0)
                MobileCheckboxMark(isOn: showsAdvanced)
                Text(L("显示高级设置")).font(.system(size: 15))
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, minHeight: 32)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(L("显示高级设置"))
        .accessibilityValue(showsAdvanced ? L("已开启") : L("已关闭"))
        .accessibilityAddTraits(.isToggle)
    }

    private var pixelOptimizationControl: some View {
        HStack(spacing: 0) {
            settingLabel("像素画优化")
            Button {
                withAnimation(.easeInOut(duration: 0.14)) {
                    pixelArtOptimization.toggle()
                }
            } label: {
                MobileCheckboxMark(isOn: pixelArtOptimization)
                    .frame(width: 24, height: 30, alignment: .leading)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L("像素画优化"))
            .accessibilityValue(pixelArtOptimization ? L("已开启") : L("已关闭"))
            .accessibilityAddTraits(.isToggle)
            .help(L("保留 RGBA 纹理，减少像素画的压缩损失。文件可能增大。"))
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

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
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(subtleBorder))
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

    private func qualityCard(title: String, quality: Quality?, preset: SceneMobileExportOptions?, icon: MobileExportQualityIcon.Kind) -> some View {
        let selected = quality != nil && selectedQuality == quality
        let fill = selected ? accent : cardBackground
        return Button {
            guard let quality, let preset else { return }
            textureReduction = preset.textureReduction
            pixelArtOptimization = preset.pixelArtOptimization
            selectedQuality = quality
            hasSelectedQuality = true
        } label: {
            VStack(spacing: 12) {
                Text(L(title))
                    .font(.system(size: 16))
                    .foregroundStyle(primaryText)
                MobileExportQualityIcon(kind: icon, face: primaryText, cutout: fill)
                    .frame(width: 52, height: 52)
                    .accessibilityHidden(true)
            }
            .frame(width: cardWidth, height: 114)
            .background(fill, in: RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(subtleBorder))
        }
        .buttonStyle(.plain)
        .disabled(preset == nil)
        .opacity(preset == nil ? 0.65 : 1)
        .accessibilityLabel(L(title))
        .accessibilityHint(preset == nil ? L("预渲染视频导出暂不可用") : "")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

private struct MobileCheckboxMark: View {
    let isOn: Bool
    private let accent = Color(red: 0.24, green: 0.49, blue: 0.97)

    var body: some View {
        RoundedRectangle(cornerRadius: 3)
            .fill(isOn ? accent : .clear)
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(accent, lineWidth: 2))
            .overlay {
                if isOn {
                    Image(systemName: "checkmark")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(.white)
                        .transition(.scale.combined(with: .opacity))
                }
            }
            .frame(width: 20, height: 20)
            .animation(.easeInOut(duration: 0.14), value: isOn)
    }
}

private struct MobileExportQualityIcon: View {
    enum Kind { case high, balanced, film }
    let kind: Kind
    let face: Color
    let cutout: Color

    var body: some View {
        Canvas { context, size in
            let side = min(size.width, size.height)
            func rect(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> CGRect {
                CGRect(x: x * side, y: y * side, width: w * side, height: h * side)
            }
            if kind == .film {
                context.fill(Path(roundedRect: rect(0.05, 0.08, 0.9, 0.84), cornerRadius: side * 0.12),
                             with: .color(face))
                for y in [0.2, 0.55] {
                    context.fill(Path(roundedRect: rect(0.33, y, 0.34, 0.24), cornerRadius: side * 0.035), with: .color(cutout))
                }
                for x in [0.14, 0.75] {
                    for y in [0.2, 0.43, 0.66] {
                        context.fill(Path(roundedRect: rect(x, y, 0.11, 0.12), cornerRadius: side * 0.02), with: .color(cutout))
                    }
                }
            } else {
                context.fill(Path(ellipseIn: rect(0.04, 0.04, 0.92, 0.92)),
                             with: .color(face))
                let angles: [Double] = kind == .high
                    ? [180, 225, 270, 315, 360]
                    : [180, 225, 315, 360]
                for angle in angles {
                    let radians = angle * Double.pi / 180
                    let x = 0.5 + cos(radians) * 0.29
                    let y = 0.5 + sin(radians) * 0.29
                    context.fill(Path(ellipseIn: rect(x - 0.055, y - 0.055, 0.11, 0.11)), with: .color(cutout))
                }
                let center = CGPoint(x: side * 0.5, y: side * 0.67)
                var needle = Path()
                needle.move(to: center)
                needle.addLine(to: CGPoint(x: side * (kind == .high ? 0.8 : 0.5), y: side * (kind == .high ? 0.48 : 0.21)))
                context.stroke(needle, with: .color(cutout), style: StrokeStyle(lineWidth: side * 0.08, lineCap: .round))
                context.fill(Path(ellipseIn: rect(0.39, 0.56, 0.22, 0.22)), with: .color(cutout))
            }
        }
    }
}
