//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import Observation
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

    var body: some View {
        let model = wallpaperViewModel.propertyModel
        let rows = model.rows.filter { conditions.isVisible($0.property.condition) }
        LazyVStack(alignment: .leading, spacing: 12) {
            if model.rows.isEmpty {
                PropertyEmptyState()
            } else {
                ForEach(rows) { entry in
                    PropertyRow(wallpaper: wallpaper, key: entry.id,
                                property: entry.property, valueState: entry.state,
                                displayKey: wallpaperViewModel.selectedDisplayKey, conditions: conditions)
                }
            }
        }
        .background {
            PropertyConditionObserver(model: model, identity: wallpaper.id,
                                      conditions: conditions, isActive: isActive)
        }
        .environment(\.mirageContentActive, isActive)
    }
}

struct PropertyEmptyState: View {
    var body: some View {
        HStack {
            Text("此壁纸没有可调节的属性。")
                .font(.footnote)
                .foregroundStyle(.secondary)
            Spacer()
        }
    }
}

struct PropertyConditionObserver: View {
    let model: WallpaperPropertyModel
    let identity: String
    let conditions: ConditionStore
    let isActive: Bool

    var body: some View {
        Color.clear
        .onAppear { refreshConditions() }
        .onChange(of: isActive ? model.overrides : [:]) { _, _ in refreshConditions() }
        .onChange(of: identity) { _, _ in refreshConditions() }
        .onChange(of: model.properties) { _, _ in refreshConditions() }
        .onChange(of: isActive) { _, active in
            if active { refreshConditions() } else { conditions.cancel() }
        }
        .onDisappear { conditions.cancel() }
    }

    private func refreshConditions() {
        guard isActive else { return }
        conditions.update(identity: identity, properties: model.properties, overrides: model.overrides)
    }
}

final class ConditionStore: ObservableObject {
    @Observable
    fileprivate final class Visibility {
        var value: Bool
        init(_ value: Bool) { self.value = value }
    }

    private let evaluator = WEConditionEvaluator()
    private(set) var verdicts: [String: Bool] = [:]
    private var visibility: [String: Visibility] = [:]
    private var identity: String?
    private var lastProperties: [String: WEProjectProperty]?
    private var lastOverrides: [String: WEPropertyValue]?
    private var expressions: [String] = []

    func update(identity: String, properties: [String: WEProjectProperty],
                overrides: [String: WEPropertyValue]) {
        guard self.identity != identity || lastProperties != properties || lastOverrides != overrides else {
            return
        }
        if self.identity != identity {
            evaluator.cancel()
            apply([:])
        }
        self.identity = identity
        if lastProperties != properties {
            var unique = Set<String>()
            for property in properties.values {
                for expression in [property.condition] + (property.options ?? []).map(\.condition) {
                    if let expression, !expression.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                        unique.insert(expression)
                    }
                }
            }
            expressions = unique.sorted()
            visibility = visibility.filter { unique.contains($0.key) }
        }
        lastProperties = properties
        lastOverrides = overrides
        guard !expressions.isEmpty else {
            evaluator.cancel()
            if !verdicts.isEmpty { apply([:]) }
            return
        }
        evaluator.evaluate(identity: identity, conditions: expressions,
                           properties: properties, overrides: overrides) { [weak self] result in
            guard let self, self.identity == identity, self.verdicts != result else { return }
            self.apply(result)
        }
    }

    func isVisible(_ condition: String?) -> Bool {
        guard let condition else { return true }
        if let state = visibility[condition] { return state.value }
        let state = Visibility(verdicts[condition] ?? true)
        visibility[condition] = state
        return state.value
    }

    private func apply(_ verdicts: [String: Bool]) {
        self.verdicts = verdicts
        for (expression, state) in visibility {
            let value = verdicts[expression] ?? true
            if state.value != value { state.value = value }
        }
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
    let valueState: WallpaperPropertyValue
    let displayKey: DisplayKey
    let conditions: ConditionStore
    @State private var pickerError: String?

    private var currentValue: WEPropertyValue {
        valueState.value
    }

    private func setValue(_ value: WEPropertyValue) {
        guard wallpaperViewModel.state(for: displayKey)?.wallpaper.id == wallpaper.id else { return }
        wallpaperViewModel.setProperty(key: key, value: value, for: displayKey)
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
                set: { setValue(.bool($0)) })) {
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
                            setValue(.number(v))
                        }),
                    in: sliderRange,
                    onEditingChanged: { editing in
                        if !editing { wallpaperViewModel.flushInteractiveChanges(for: displayKey) }
                    })
            }

        case .color:
            ColorPicker(selection: Binding(
                get: { Self.parseColor(currentValue.stringValue) },
                set: { setValue(.string(Self.encodeColor($0))) }),
                supportsOpacity: false) {
                labelView(lineLimit: 2)
            }

        case .combo:
            HStack(alignment: .firstTextBaseline) {
                labelView(lineLimit: 2, expand: false)
                Spacer()
                Picker("", selection: Binding(
                    get: { property.normalizedComboValue(currentValue) },
                    set: { setValue($0) })) {
                    ForEach(visibleOptions, id: \.value) { opt in
                        Text(WEHTML.plain(WELocalization.resolve(opt.label))).tag(opt.value)
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
                    set: { setValue(.string($0)) }))
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
                    set: { setValue(.string($0)) }))
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
                        setValue(.string(""))
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
                    setValue(.string(cached.path))
                } catch {
                    pickerError = error.localizedDescription
                }
            } else {
                setValue(.string(url.path))
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
            setValue(.string(url.standardizedFileURL.path))
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
