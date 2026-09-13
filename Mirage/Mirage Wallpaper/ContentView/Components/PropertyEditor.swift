//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import WebKit
import UniformTypeIdentifiers

// MARK: - Property panel

// Faithful Wallpaper Engine customization sidebar: every property type,
// condition-driven show/hide, official localization, and real HTML labels.
struct PropertyEditor: View {
    @Environment(WallpaperViewModel.self) var wallpaperViewModel
    let wallpaper: WEWallpaper
    var isActive = true

    @StateObject private var conditions = ConditionStore()

    private var allProperties: [String: WEProjectProperty] {
        wallpaper.project.general?.properties?.items ?? [:]
    }

    private var sortedProperties: [(key: String, property: WEProjectProperty)] {
        (wallpaper.project.general?.properties?.sorted ?? []).filter { !$0.property.isPresetOnly }
    }

    private var visibleProperties: [(key: String, property: WEProjectProperty)] {
        sortedProperties.filter { conditions.isVisible($0.property.condition) }
    }

    var body: some View {
        @Bindable var wallpaperViewModel = wallpaperViewModel
        Group {
            if sortedProperties.isEmpty {
                HStack {
                    Text("此壁纸没有可调节的属性。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            } else {
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(visibleProperties, id: \.key) { entry in
                        PropertyRow(wallpaper: wallpaper, key: entry.key,
                                    property: entry.property, conditions: conditions)
                            .environment(wallpaperViewModel)
                    }
                }
            }
        }
        .onAppear { refreshConditions() }
        .onChange(of: isActive ? wallpaperViewModel.runtime.propertyOverrides : [:]) { _, _ in refreshConditions() }
        .onChange(of: wallpaper.id) { _, _ in refreshConditions() }
        .onChange(of: allProperties) { _, _ in refreshConditions() }
        .onChange(of: isActive) { _, active in
            if active { refreshConditions() } else { conditions.cancel() }
        }
        .onDisappear { conditions.cancel() }
    }

    private func refreshConditions() {
        guard isActive else { return }
        conditions.update(identity: wallpaper.id, properties: allProperties,
                          overrides: wallpaperViewModel.runtime.propertyOverrides)
    }
}

final class ConditionStore: ObservableObject {
    private let evaluator = WEConditionEvaluator()
    @Published private(set) var verdicts: [String: Bool] = [:]
    private var identity: String?
    private var lastProperties: [String: WEProjectProperty]?
    private var lastOverrides: [String: WEPropertyValue]?

    func update(identity: String, properties: [String: WEProjectProperty],
                overrides: [String: WEPropertyValue]) {
        guard self.identity != identity || lastProperties != properties || lastOverrides != overrides else {
            return
        }
        if self.identity != identity {
            evaluator.cancel()
            verdicts = [:]
        }
        self.identity = identity
        lastProperties = properties
        lastOverrides = overrides
        var expressions = Set<String>()
        var values: [String: Any] = [:]
        for (key, property) in properties {
            for expression in [property.condition] + (property.options ?? []).map(\.condition) {
                if let expression, !expression.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    expressions.insert(expression)
                }
            }
            let raw = overrides[key] ?? property.value
            let value: Any
            switch raw {
            case .bool(let flag): value = flag
            case .number(let number): value = number
            case .string(let string):
                if property.propertyType == .bool { value = (string as NSString).boolValue }
                else if let integer = Int(string) { value = integer }
                else if let number = Double(string) { value = number }
                else if string == "true" { value = true }
                else if string == "false" { value = false }
                else { value = string }
            }
            values[key] = ["value": value]
        }
        evaluator.evaluate(identity: identity, conditions: expressions.sorted(), values: values) { [weak self] result in
            guard let self, self.identity == identity, self.verdicts != result else { return }
            self.verdicts = result
        }
    }

    func isVisible(_ condition: String?) -> Bool {
        guard let condition else { return true }
        return verdicts[condition] ?? true
    }

    func cancel() {
        evaluator.cancel()
        lastProperties = nil
        lastOverrides = nil
    }
}

// MARK: - Property row

struct PropertyRow: View {
    @Environment(WallpaperViewModel.self) var wallpaperViewModel
    let wallpaper: WEWallpaper
    let key: String
    let property: WEProjectProperty
    @ObservedObject var conditions: ConditionStore
    @State private var pickerError: String?

    private var currentValue: WEPropertyValue {
        wallpaperViewModel.runtime.propertyOverrides[key] ?? property.value
    }

    private var rawText: String { property.displayText(fallbackKey: key) }

    @ViewBuilder
    private func labelView(lineLimit: Int? = nil, expand: Bool = true) -> some View {
        if WEHTML.isRich(rawText) {
            RichHTMLText(html: rawText)
                .frame(maxWidth: expand ? .infinity : nil, alignment: .leading)
        } else {
            Text(WEHTML.plain(rawText))
                .lineLimit(lineLimit)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: expand ? .infinity : nil, alignment: .leading)
        }
    }

    var body: some View {
        @Bindable var wallpaperViewModel = wallpaperViewModel
        Group {
            switch property.propertyType {
        case .bool:
            Toggle(isOn: Binding(
                get: { currentValue.boolValue },
                set: { wallpaperViewModel.setProperty(key: key, value: .bool($0)) })) {
                labelView()
            }

        case .slider:
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    labelView(lineLimit: 1, expand: false)
                    Spacer()
                    Text(sliderValueText)
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                MirageSlider(
                    value: Binding(
                        get: { currentValue.doubleValue },
                        set: { newVal in
                            let v = (property.fraction == true) ? newVal : newVal.rounded()
                            wallpaperViewModel.setProperty(key: key, value: .number(v))
                        }),
                    in: sliderRange)
            }

        case .color:
            ColorPicker(selection: Binding(
                get: { Self.parseColor(currentValue.stringValue) },
                set: { wallpaperViewModel.setProperty(key: key, value: .string(Self.encodeColor($0))) }),
                supportsOpacity: false) {
                labelView(lineLimit: 2)
            }

        case .combo:
            HStack(alignment: .firstTextBaseline) {
                labelView(lineLimit: 2, expand: false)
                Spacer()
                Picker("", selection: Binding(
                    get: { property.normalizedComboValue(currentValue) },
                    set: { wallpaperViewModel.setProperty(key: key, value: $0) })) {
                    ForEach(visibleOptions, id: \.value) { opt in
                        Text(WELocalization.resolve(opt.label)).tag(opt.value)
                    }
                }
                .labelsHidden()
                .frame(maxWidth: 170)
            }

        case .textinput:
            VStack(alignment: .leading, spacing: 4) {
                labelView(lineLimit: 2)
                TextField("", text: Binding(
                    get: { currentValue.stringValue },
                    set: { wallpaperViewModel.setProperty(key: key, value: .string($0)) }))
                    .textFieldStyle(.roundedBorder)
            }

        case .text:
            labelView()

        case .group:
            VStack(alignment: .leading, spacing: 4) {
                if WEHTML.isRich(rawText) {
                    RichHTMLText(html: rawText)
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    Text(WEHTML.plain(rawText))
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.primary)
                        .multilineTextAlignment(.leading)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Divider().overlay(Color.accentColor.opacity(0.5))
            }
            .padding(.top, 10)

        case .file, .scenetexture:
            filePickerRow(kind: .file)

        case .directory:
            filePickerRow(kind: .directory)

        case .usershortcut:
            HStack(alignment: .firstTextBaseline) {
                labelView(lineLimit: 2, expand: false)
                Spacer()
                TextField("快捷方式", text: Binding(
                    get: { currentValue.stringValue },
                    set: { wallpaperViewModel.setProperty(key: key, value: .string($0)) }))
                    .textFieldStyle(.roundedBorder)
                    .frame(maxWidth: 170)
                Button {
                    pickUserShortcut()
                } label: {
                    Image(systemName: "folder")
                }
                .buttonStyle(.borderless)
                .help(L("选择快捷方式"))
            }

        case .unknown:
            EmptyView()
            }
        }
        .alert(L("无法使用所选文件"), isPresented: Binding(
            get: { pickerError != nil },
            set: { if !$0 { pickerError = nil } }
        )) {
            Button(L("好"), role: .cancel) { pickerError = nil }
        } message: {
            Text(pickerError ?? "")
        }
    }

    // Options whose own condition passes (WE allows per-option conditions).
    private var visibleOptions: [WEProjectPropertyOption] {
        (property.options ?? []).filter { conditions.isVisible($0.condition) }
    }

    // MARK: File / directory / texture pickers

    private enum PickKind { case file, directory }

    @ViewBuilder
    private func filePickerRow(kind: PickKind) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            labelView(lineLimit: 2)
            HStack {
                Text(displayPath.isEmpty ? L("未选择") : displayPath)
                    .font(.caption)
                    .foregroundStyle(displayPath.isEmpty ? .secondary : .primary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer()
                if !displayPath.isEmpty {
                    Button {
                        wallpaperViewModel.setProperty(key: key, value: .string(""))
                    } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)
                }
                Button("选择…") { pick(kind: kind) }
            }
        }
    }

    private var displayPath: String {
        let p = currentValue.stringValue
        guard !p.isEmpty else { return "" }
        return (p as NSString).lastPathComponent
    }

    private func pick(kind: PickKind) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = kind == .file
        panel.canChooseDirectories = kind == .directory
        panel.allowsMultipleSelection = false
        if kind == .file, property.propertyType != .file {
            panel.allowedContentTypes = [.image] // scenetexture: images only
        }
        if panel.runModal() == .OK, let url = panel.url {
            if property.propertyType == .scenetexture {
                do {
                    let cached = try UserTextureCache.shared.importImage(at: url)
                    wallpaperViewModel.setProperty(key: key, value: .string(cached.path))
                } catch {
                    pickerError = error.localizedDescription
                }
            } else {
                wallpaperViewModel.setProperty(key: key, value: .string(url.path))
            }
        }
    }

    private func pickUserShortcut() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.treatsFilePackagesAsDirectories = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            wallpaperViewModel.setProperty(
                key: key,
                value: .string(url.standardizedFileURL.path))
        }
    }

    private var sliderRange: ClosedRange<Double> {
        let lo = property.min ?? 0
        let hi = property.max ?? 100
        return lo < hi ? lo...hi : lo...(lo + 1)
    }

    private var sliderValueText: String {
        let v = currentValue.doubleValue
        return property.fraction == true ? String(format: "%.2f", v) : String(Int(v.rounded()))
    }

    static func parseColor(_ s: String) -> Color {
        let comps = s.split(separator: " ").compactMap { Double($0) }
        guard comps.count >= 3 else { return .white }
        return Color(.sRGB, red: comps[0], green: comps[1], blue: comps[2], opacity: 1)
    }

    static func encodeColor(_ color: Color) -> String {
        let ns = NSColor(color).usingColorSpace(.sRGB) ?? .white
        return String(format: "%.5f %.5f %.5f", ns.redComponent, ns.greenComponent, ns.blueComponent)
    }
}
