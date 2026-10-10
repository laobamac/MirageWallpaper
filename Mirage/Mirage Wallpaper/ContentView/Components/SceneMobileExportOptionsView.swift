//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import ImageIO

struct SceneMobileExportOptionsView: View {
    private enum Quality: Equatable { case highQuality, balanced, preRendered }

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
    @State private var showsVideoAdvanced = false
    @ObservedObject private var mobileDevices = AppDelegate.shared.mobileDevicesViewModel
    @State private var fitPhone = true
    @State private var videoPreset = SceneMobileExportOptions.VideoPreset.fullHD
    @State private var videoFPS = 60
    @State private var videoAlignment = 0.5
    @State private var videoDuration: Int? = nil
    @State private var sourceResolution: MobileScreenResolution?
    @State private var sourceError: String?
    @State private var previewImage: NSImage?

    private var isPreRendered: Bool { selectedQuality == .preRendered }
    private var phoneResolution: MobileScreenResolution? {
        switch request.destination {
        case .device(let device):
            return mobileDevices.devices.first(where: { $0.id == device.id })?.screenResolution ?? device.screenResolution
        case .file:
            let connected = mobileDevices.devices.first { $0.isConnected && $0.screenResolution?.isValid == true }
            return connected?.screenResolution ?? mobileDevices.devices.first { $0.screenResolution?.isValid == true }?.screenResolution
        }
    }
    private var videoOptions: SceneMobileExportOptions.PreRendered {
        .init(fitPhone: fitPhone, screenResolution: phoneResolution, preset: videoPreset,
              fps: videoFPS, alignment: videoAlignment, duration: videoDuration)
    }
    private var canConfirm: Bool {
        guard hasSelectedQuality else { return false }
        guard isPreRendered else { return true }
        guard let sourceResolution else { return false }
        return (try? videoOptions.outputResolution(source: sourceResolution)) != nil
    }

    private var exportOptions: SceneMobileExportOptions {
        SceneMobileExportOptions(
            textureReduction: textureReduction,
            pixelArtOptimization: pixelArtOptimization,
            preRendered: isPreRendered ? videoOptions : nil
        )
    }

    private var background: Color { colorScheme == .dark ? Color(white: 34 / 255) : Color(nsColor: .windowBackgroundColor) }
    private var primaryText: Color { Color(nsColor: .labelColor) }
    private let accent = Color(red: 0.24, green: 0.49, blue: 0.97)
    private var cardBackground: Color { colorScheme == .dark ? Color(white: 61 / 255) : Color(white: 0.95) }
    private var subtleBorder: Color { colorScheme == .dark ? Color(white: 72 / 255) : Color(nsColor: .separatorColor) }
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
        let desired: CGFloat = isPreRendered ? 680 : showsAdvanced ? 460 : (hasSelectedQuality ? 355 : 320)
        return min(desired, max(220, (NSScreen.main?.visibleFrame.height ?? 900) - 190))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(heading)
                Spacer(minLength: 0)
                if isPreRendered {
                    Button { showsVideoAdvanced.toggle() } label: {
                        Image(systemName: "slider.horizontal.3")
                            .font(.system(size: 16))
                            .frame(width: 28, height: 28)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(L("显示高级设置"))
                    .help(L("显示高级设置"))
                    .popover(isPresented: $showsVideoAdvanced, arrowEdge: .top) {
                        videoAdvancedSettings
                    }
                }
            }
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
                            qualityCard(title: "高性能", quality: .preRendered, preset: .init(), icon: .film)
                        }
                    }
                    .padding(.top, 3)
                    .frame(maxWidth: .infinity)

                    if isPreRendered {
                        videoSettings
                    } else if hasSelectedQuality {
                        advancedSettingsControl
                    }

                    if showsAdvanced && !isPreRendered {
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
                        .foregroundStyle(.white)
                        .opacity(canConfirm ? 1 : 0.55)
                }
                .disabled(!canConfirm)
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
        .task {
            let wallpaper = request.wallpaper
            let (result, image) = await Task.detached(priority: .userInitiated) {
                let result = Result { try SceneMobileMPKGExporter.sourceResolution(wallpaper) }
                let imageSource = CGImageSourceCreateWithURL(wallpaper.previewURL as CFURL, nil)
                let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                    kCGImageSourceThumbnailMaxPixelSize: 512, kCGImageSourceCreateThumbnailWithTransform: true,
                    kCGImageSourceShouldCacheImmediately: true]
                let image = imageSource.flatMap { CGImageSourceCreateThumbnailAtIndex($0, 0, options as CFDictionary) }
                return (result, image)
            }.value
            switch result {
            case .success(let resolution): sourceResolution = resolution
            case .failure(let error): sourceError = error.localizedDescription
            }
            previewImage = image.map { NSImage(cgImage: $0, size: .zero) }

        }
    }

    private var videoSettings: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text(L("预渲染场景壁纸可大幅提高性能，但时钟等动态元素或互动式触摸事件将无法正常工作。"))
                .font(.system(size: 15))
                .fixedSize(horizontal: false, vertical: true)
                .padding(.bottom, 2)

            VStack(alignment: .leading, spacing: 20) {
                videoSettingRow("视频裁剪") {
                    exportDropdown("视频裁剪", selection: $fitPhone, choices: [
                        (true, L("适应手机屏幕"), phoneResolution != nil),
                        (false, L("保持原始宽高比"), true)
                    ])
                    .help(phoneResolution.map { L("手机屏幕：%d × %d", $0.width, $0.height) }
                          ?? L("尚未获取手机屏幕分辨率，请重新连接手机，或选择保持原始宽高比。"))
                }
                if phoneResolution == nil {
                    videoSettingRow("") {
                        Text(L("尚未获取手机屏幕分辨率，请重新连接手机，或选择保持原始宽高比。"))
                            .font(.system(size: 12)).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                videoSettingRow("视频预设") {
                    exportDropdown("视频预设", selection: $videoPreset,
                        choices: SceneMobileExportOptions.VideoPreset.allCases.map { ($0, $0.title, true) })
                }
                videoSettingRow("帧率") {
                    exportDropdown("帧率", selection: $videoFPS,
                        choices: [24, 30, 60].map { ($0, String($0), true) })
                }
                if let source = sourceResolution, let output = try? videoOptions.outputResolution(source: source) {
                    videoSettingRow("") {
                        cropPreview(source: source, output: output)
                            .help(L("导出分辨率：%d × %d", output.width, output.height))
                    }
                    .padding(.bottom, 4)
                    if fitPhone {
                        videoSettingRow("对齐") {
                            MobileExportAlignmentSlider(value: $videoAlignment, accent: accent,
                                track: colorScheme == .dark ? cardBackground : Color(white: 0.82))
                        }
                    }
                }
            }
            .padding(.horizontal, 14)
            if let sourceError { Text(sourceError).foregroundStyle(.red).font(.system(size: 12)) }
        }
    }

    private var videoAdvancedSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(L("视频时长（秒）")).font(.system(size: 14, weight: .bold))
            exportDropdown("视频时长（秒）", selection: $videoDuration,
                choices: [(nil as Int?, L("自适应"), true)] +
                    [10, 15, 30, 60].map { (Optional($0), String($0), true) })
                .help(L("优先使用动画或视频周期；无法识别周期时使用 30 秒。"))
            if let resolution = phoneResolution {
                Text(L("手机屏幕：%d × %d", resolution.width, resolution.height))
            }
            if let source = sourceResolution, let output = try? videoOptions.outputResolution(source: source) {
                Text(L("导出分辨率：%d × %d", output.width, output.height))
            }
        }
        .font(.system(size: 12))
        .padding(16)
        .frame(width: 280)
        .background(background)
    }

    private func exportDropdown<Value: Hashable>(_ title: String, selection: Binding<Value>,
        choices: [(Value, String, Bool)]) -> some View {
        MobileExportDropdown(title: L(title), selection: selection, choices: choices,
                             background: title == "视频裁剪" && colorScheme == .dark ? background : cardBackground,
                             border: subtleBorder, accent: accent)
    }

    private func videoSettingRow<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        HStack(spacing: 8) {
            settingLabel(title)
            content().frame(maxWidth: .infinity)
        }
    }

    private func cropPreview(source: MobileScreenResolution, output: MobileScreenResolution) -> some View {
        let width: CGFloat = 176
        let height = min(148, width / source.aspectRatio)
        let viewWidth = height * source.aspectRatio
        let cropWidth = min(viewWidth, height * output.aspectRatio)
        let cropHeight = min(height, viewWidth / output.aspectRatio)
        let x = (viewWidth - cropWidth) * videoAlignment
        let y = (height - cropHeight) * (1 - videoAlignment)
        return ZStack(alignment: .topLeading) {
            if let previewImage {
                Image(nsImage: previewImage).resizable().scaledToFill()
                    .frame(width: viewWidth, height: height).clipped()
            } else { Rectangle().fill(cardBackground) }
            Path { path in
                path.addRect(CGRect(x: 0, y: 0, width: viewWidth, height: height))
                path.addRect(CGRect(x: x, y: y, width: cropWidth, height: cropHeight))
            }.fill(accent.opacity(0.38), style: FillStyle(eoFill: true))
        }
        .frame(width: viewWidth, height: height)
        .overlay(Rectangle().strokeBorder(accent, lineWidth: 1))
        .frame(maxWidth: .infinity)
        .accessibilityLabel(L("视频裁剪预览"))
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

    private func qualityCard(title: String, quality: Quality, preset: SceneMobileExportOptions, icon: MobileExportQualityIcon.Kind) -> some View {
        let selected = selectedQuality == quality
        let fill = selected ? accent : cardBackground
        let face = selected ? Color.white : primaryText
        return Button {
            textureReduction = preset.textureReduction
            pixelArtOptimization = preset.pixelArtOptimization
            selectedQuality = quality
            if quality == .preRendered { fitPhone = phoneResolution != nil }
            hasSelectedQuality = true
        } label: {
            VStack(spacing: 12) {
                Text(L(title))
                    .font(.system(size: 16))
                    .foregroundStyle(face)
                MobileExportQualityIcon(kind: icon, face: face, cutout: fill)
                    .frame(width: 52, height: 52)
                    .accessibilityHidden(true)
            }
            .frame(width: cardWidth, height: 114)
            .background(fill, in: RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(selected ? accent : subtleBorder))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(L(title))
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

private struct MobileExportDropdown<Value: Hashable>: View {
    let title: String
    @Binding var selection: Value
    let choices: [(Value, String, Bool)]
    let background: Color
    let border: Color
    let accent: Color
    @State private var isPresented = false

    private var selectedTitle: String { choices.first(where: { $0.0 == selection })?.1 ?? "" }

    var body: some View {
        GeometryReader { geometry in
            Button { isPresented.toggle() } label: {
                HStack(spacing: 10) {
                    Text(selectedTitle).lineLimit(1)
                    Spacer(minLength: 0)
                    Image(systemName: "triangle.fill")
                        .font(.system(size: 7))
                        .rotationEffect(.degrees(isPresented ? 180 : 0))
                }
                .font(.system(size: 15))
                .padding(.horizontal, 20)
                .frame(maxWidth: .infinity, minHeight: 26, maxHeight: 26)
                .background(background, in: RoundedRectangle(cornerRadius: 3))
                .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(border, lineWidth: 1))
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(title)
            .accessibilityValue(selectedTitle)
            .popover(isPresented: $isPresented, arrowEdge: .top) {
                VStack(spacing: 2) {
                    ForEach(choices.indices, id: \.self) { index in
                        let choice = choices[index]
                        Button {
                            selection = choice.0
                            isPresented = false
                        } label: {
                            HStack(spacing: 12) {
                                Text(choice.1)
                                Spacer(minLength: 8)
                                if selection == choice.0 {
                                    Image(systemName: "checkmark").foregroundStyle(accent)
                                }
                            }
                            .font(.system(size: 14))
                            .padding(.horizontal, 14)
                            .frame(maxWidth: .infinity, minHeight: 30, alignment: .leading)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .disabled(!choice.2)
                        .opacity(choice.2 ? 1 : 0.45)
                        .accessibilityAddTraits(selection == choice.0 ? .isSelected : [])
                    }
                }
                .padding(6)
                .frame(width: max(200, geometry.size.width))
                .background(background)
            }
        }
        .frame(height: 26)
    }
}

private struct MobileExportAlignmentSlider: View {
    @Binding var value: Double
    let accent: Color
    let track: Color

    var body: some View {
        GeometryReader { geometry in
            let travel = max(1, geometry.size.width - 8)
            ZStack(alignment: .leading) {
                Rectangle().fill(track).frame(height: 4)
                Rectangle().fill(accent)
                    .frame(width: 8, height: 24)
                    .offset(x: travel * value)
            }
            .frame(height: 26)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { event in
                value = min(1, max(0, (event.location.x - 4) / travel))
            })
        }
        .frame(height: 26)
        .focusable()
        .onKeyPress(.leftArrow) { value = max(0, value - 0.02); return .handled }
        .onKeyPress(.rightArrow) { value = min(1, value + 0.02); return .handled }
        .accessibilityElement()
        .accessibilityLabel(L("对齐"))
        .accessibilityValue("\(Int(value * 100))%")
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: value = min(1, value + 0.05)
            case .decrement: value = max(0, value - 0.05)
            @unknown default: break
            }
        }
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
