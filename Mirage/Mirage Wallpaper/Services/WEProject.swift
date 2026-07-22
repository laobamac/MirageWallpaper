//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import ImageIO

// MARK: - 属性值

enum WEPropertyValue: Codable, Equatable, Hashable {
    case bool(Bool)
    case number(Double)
    case string(String)

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let b = try? container.decode(Bool.self) {
            self = .bool(b); return
        }
        if let d = try? container.decode(Double.self) {
            self = .number(d); return
        }
        if let s = try? container.decode(String.self) {
            self = .string(s); return
        }
        self = .string("")
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .bool(let b): try container.encode(b)
        case .number(let d):
            if d == d.rounded() && abs(d) < 1e15 {
                try container.encode(Int(d))
            } else {
                try container.encode(d)
            }
        case .string(let s): try container.encode(s)
        }
    }

    var boolValue: Bool {
        switch self {
        case .bool(let b): return b
        case .number(let d): return d != 0
        case .string(let s): return (s as NSString).boolValue
        }
    }

    var doubleValue: Double {
        switch self {
        case .bool(let b): return b ? 1 : 0
        case .number(let d): return d
        case .string(let s): return Double(s) ?? 0
        }
    }

    var stringValue: String {
        switch self {
        case .bool(let b): return b ? "true" : "false"
        case .number(let d):
            if d == d.rounded() && abs(d) < 1e15 { return String(Int(d)) }
            return String(d)
        case .string(let s): return s
        }
    }

    // Value suitable for JSONSerialization while preserving the manifest's
    // primitive type. In particular, WE combo values may be numbers or bools;
    // converting them to display strings breaks strict JavaScript comparisons.
    var jsonObjectValue: Any {
        switch self {
        case .bool(let b): return b
        case .number(let d): return d
        case .string(let s): return s
        }
    }
}

// MARK: - 属性选项

struct WEProjectPropertyOption: Codable, Equatable, Hashable {
    var label: String
    var value: WEPropertyValue
    var condition: String?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.label = (try? c.decode(String.self, forKey: .label)) ?? ""
        self.condition = try? c.decode(String.self, forKey: .condition)
        self.value = (try? c.decode(WEPropertyValue.self, forKey: .value)) ?? .string("")
    }

    init(label: String, value: WEPropertyValue, condition: String? = nil) {
        self.label = label
        self.value = value
        self.condition = condition
    }

    enum CodingKeys: String, CodingKey {
        case label, value, condition
    }
}

// MARK: - 单条属性

enum WEPropertyType: String {
    case bool
    case slider
    case color
    case combo
    case textinput
    case text
    case group
    case file
    case directory
    case scenetexture
    case usershortcut
    case unknown

    init(raw: String) {
        self = WEPropertyType(rawValue: raw.lowercased()) ?? .unknown
    }
}

struct WEProjectProperty: Codable, Equatable, Hashable {
    var condition: String?
    var index: Int?
    var options: [WEProjectPropertyOption]?
    var order: Int?
    var min: Double?
    var max: Double?
    var step: Double?
    var fraction: Bool?
    var mode: String?
    var isPresetOnly: Bool

    var text: String?
    var type: String
    var value: WEPropertyValue

    var propertyType: WEPropertyType { WEPropertyType(raw: type) }

    // Runtime data written by older Mirage versions stored every combo as a
    // string. Map such values back to the exact typed option from project.json.
    // Exact matches win so legitimate string-valued combos remain strings.
    func normalizedComboValue(_ candidate: WEPropertyValue) -> WEPropertyValue {
        guard propertyType == .combo, let options, !options.isEmpty else { return candidate }
        if options.contains(where: { $0.value == candidate }) { return candidate }
        return options.first(where: { $0.value.stringValue == candidate.stringValue })?.value ?? candidate
    }

    func displayText(fallbackKey key: String) -> String {
        if let t = text, !t.isEmpty {
            return WELocalization.resolve(t)
        }
        return WELocalization.resolve(key)
    }

    enum CodingKeys: String, CodingKey {
        case condition, index, options, order, min, max, step, fraction, mode, text, type, value
        case isPresetOnly = "_miragePresetOnly"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.condition = try? c.decode(String.self, forKey: .condition)
        self.index = try? c.decode(Int.self, forKey: .index)
        self.options = try? c.decode([WEProjectPropertyOption].self, forKey: .options)
        self.order = try? c.decode(Int.self, forKey: .order)
        self.min = try? c.decode(Double.self, forKey: .min)
        self.max = try? c.decode(Double.self, forKey: .max)
        self.step = try? c.decode(Double.self, forKey: .step)
        self.fraction = try? c.decode(Bool.self, forKey: .fraction)
        self.mode = try? c.decode(String.self, forKey: .mode)
        self.isPresetOnly = (try? c.decode(Bool.self, forKey: .isPresetOnly)) ?? false
        self.text = try? c.decode(String.self, forKey: .text)
        self.type = (try? c.decode(String.self, forKey: .type)) ?? "text"
        self.value = (try? c.decode(WEPropertyValue.self, forKey: .value)) ?? .string("")
    }

    init(type: String, value: WEPropertyValue, text: String? = nil, order: Int? = nil, isPresetOnly: Bool = false) {
        self.type = type
        self.value = value
        self.text = text
        self.order = order
        self.isPresetOnly = isPresetOnly
    }
}

// MARK: - Localization

// Resolves WE `ui_*` labels with the official table matching Mirage's selected
// language.  Wallpaper metadata itself is intentionally never translated.
enum WELocalization {
    private static var tables: [String: [String: String]] = [:]
    private static let lock = NSLock()

    private static var resourceName: String {
        switch MirageLocalization.shared.locale.identifier {
        case let id where id.hasPrefix("zh-Hant"): return "ui_zh-cht"
        case let id where id.hasPrefix("zh-Hans"): return "ui_zh-chs"
        default: return "ui_en-us"
        }
    }

    private static func table(named name: String) -> [String: String] {
        lock.lock()
        defer { lock.unlock() }
        if let cached = tables[name] { return cached }
        guard let url = Bundle.main.url(forResource: name, withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            tables[name] = [:]
            return [:]
        }
        var out: [String: String] = [:]
        out.reserveCapacity(obj.count)
        for (k, v) in obj {
            if let s = v as? String { out[k] = s }
        }
        tables[name] = out
        return out
    }

    static func resolve(_ raw: String) -> String {
        let key = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if key.isEmpty { return raw }
        guard key.hasPrefix("ui_") else { return raw }
        if let mapped = table(named: resourceName)[key] { return mapped }
        return humanize(key)
    }

    private static let prefixes: [String] = [
        "ui_editor_script_snippet_",
        "ui_editor_properties_",
        "ui_browse_properties_",
        "ui_editor_general_",
        "ui_editor_effect_",
        "ui_editor_preset_",
        "ui_editor_",
        "ui_browse_",
        "ui_",
    ]

    private static func humanize(_ key: String) -> String {
        var s = key
        for p in prefixes where s.hasPrefix(p) {
            s = String(s.dropFirst(p.count))
            break
        }
        s = s.replacingOccurrences(of: "_", with: " ").trimmingCharacters(in: .whitespaces)
        if s.isEmpty { return key }
        return s.split(separator: " ").map { w -> String in
            guard let f = w.first else { return String(w) }
            return f.uppercased() + w.dropFirst()
        }.joined(separator: " ")
    }
}

// MARK: - 属性容器
struct WEProjectProperties: Codable, Equatable, Hashable {
    var items: [String: WEProjectProperty]

    init(items: [String: WEProjectProperty] = [:]) {
        self.items = items
    }

    private struct DynamicKey: CodingKey {
        var stringValue: String
        var intValue: Int?
        init?(stringValue: String) { self.stringValue = stringValue }
        init?(intValue: Int) { self.intValue = intValue; self.stringValue = String(intValue) }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DynamicKey.self)
        var result: [String: WEProjectProperty] = [:]
        for key in c.allKeys {
            if let prop = try? c.decode(WEProjectProperty.self, forKey: key) {
                result[key.stringValue] = prop
            }
        }
        self.items = result
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: DynamicKey.self)
        for (key, prop) in items {
            if let dynamicKey = DynamicKey(stringValue: key) {
                try c.encode(prop, forKey: dynamicKey)
            }
        }
    }

    var sorted: [(key: String, property: WEProjectProperty)] {
        items.map { ($0.key, $0.value) }.sorted { a, b in
            let oa = a.property.order ?? a.property.index ?? Int.max
            let ob = b.property.order ?? b.property.index ?? Int.max
            if oa != ob { return oa < ob }
            return a.key < b.key
        }
    }
}

struct WEProjectGeneral: Codable, Equatable, Hashable {
    var properties: WEProjectProperties?
}

// MARK: - Workshop ID

enum WorkshopId: Codable, Equatable, Hashable, RawRepresentable {
    case int(Int)
    case string(String)

    var rawValue: String {
        switch self {
        case .int(let x): return String(x)
        case .string(let x): return x
        }
    }

    init?(rawValue: String) {
        guard rawValue.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
        self = .string(rawValue)
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let x = try? container.decode(Int.self) { self = .int(x); return }
        if let x = try? container.decode(String.self) { self = .string(x); return }
        throw DecodingError.typeMismatch(Self.self, DecodingError.Context(codingPath: decoder.codingPath, debugDescription: "Workshop ID 类型错误"))
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .int(let x): try container.encode(x)
        case .string(let x): try container.encode(x)
        }
    }
}

// MARK: - WEProject

struct WEProject: Codable, Equatable, Hashable {
    var approved: Bool?
    var author: String?
    var contentrating: String?
    var dependency: WorkshopId?
    var description: String?
    var file: String
    var general: WEProjectGeneral?
    var preset: [String: WEPropertyValue]?
    var preview: String
    var tags: [String]?
    var title: String
    var visibility: String?
    var workshopid: WorkshopId?
    var workshopurl: String?
    var type: String
    var version: Int?

    var normalizedType: WallpaperKind { WallpaperKind(rawType: type) }
    var isWorkshopPreset: Bool { dependency != nil && preset != nil }

    // WE project.json rarely has an author field; derive it from the title
    // (`[name]…`) or description (`作者：X` / `by X`). nil when nothing matches.
    var resolvedAuthor: String? {
        if let a = author?.trimmingCharacters(in: .whitespacesAndNewlines), !a.isEmpty {
            return a
        }
        if let fromTitle = Self.extractBracketAuthor(title) { return fromTitle }
        if let desc = description, let fromDesc = Self.extractLabeledAuthor(desc) {
            return fromDesc
        }
        return nil
    }

    private static func extractBracketAuthor(_ s: String) -> String? {
        let pattern = "^\\s*[\\[【]([^\\]】]{1,40})[\\]】]"
        guard let r = s.range(of: pattern, options: .regularExpression) else { return nil }
        let inside = s[r].dropFirst().dropLast().trimmingCharacters(in: .whitespaces)
        return inside.isEmpty ? nil : String(inside)
    }

    private static func extractLabeledAuthor(_ s: String) -> String? {
        let patterns = ["作者[:：]\\s*([^\\n\\r，,。;；]{1,40})",
                        "(?i)\\bby\\s+([^\\n\\r,;]{1,40})",
                        "(?i)author[:：]\\s*([^\\n\\r,;]{1,40})"]
        for p in patterns {
            guard let r = s.range(of: p, options: .regularExpression) else { continue }
            let match = String(s[r])
            if let sep = match.range(of: "[:：]|(?i)\\bby\\b", options: .regularExpression) {
                let name = match[sep.upperBound...].trimmingCharacters(in: .whitespaces)
                if !name.isEmpty { return name }
            }
        }
        return nil
    }

    static let invalid = Self(file: "",
                              preview: "",
                              title: "未知",
                              type: "video")

    init(approved: Bool? = nil, author: String? = nil, contentrating: String? = nil,
         dependency: WorkshopId? = nil, description: String? = nil,
         file: String, general: WEProjectGeneral? = nil, preset: [String: WEPropertyValue]? = nil, preview: String,
         tags: [String]? = nil, title: String, visibility: String? = nil,
         workshopid: WorkshopId? = nil, workshopurl: String? = nil, type: String, version: Int? = nil) {
        self.approved = approved
        self.author = author
        self.contentrating = contentrating
        self.dependency = dependency
        self.description = description
        self.file = file
        self.general = general
        self.preset = preset
        self.preview = preview
        self.tags = tags
        self.title = title
        self.visibility = visibility
        self.workshopid = workshopid
        self.workshopurl = workshopurl
        self.type = type
        self.version = version
    }

    enum CodingKeys: String, CodingKey {
        case approved, author, contentrating, dependency, description, file, general, preset
        case preview, tags, title, visibility, workshopid, workshopurl, type, version
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        approved = try? c.decode(Bool.self, forKey: .approved)
        author = try? c.decode(String.self, forKey: .author)
        contentrating = try? c.decode(String.self, forKey: .contentrating)
        dependency = try? c.decode(WorkshopId.self, forKey: .dependency)
        description = try? c.decode(String.self, forKey: .description)
        file = (try? c.decode(String.self, forKey: .file)) ?? ""
        general = try? c.decode(WEProjectGeneral.self, forKey: .general)
        preset = try? c.decode([String: WEPropertyValue].self, forKey: .preset)
        preview = (try? c.decode(String.self, forKey: .preview)) ?? ""
        tags = try? c.decode([String].self, forKey: .tags)
        title = (try? c.decode(String.self, forKey: .title)) ?? "未命名"
        visibility = try? c.decode(String.self, forKey: .visibility)
        workshopid = try? c.decode(WorkshopId.self, forKey: .workshopid)
        workshopurl = try? c.decode(String.self, forKey: .workshopurl)
        type = (try? c.decode(String.self, forKey: .type)) ?? ""
        version = try? c.decode(Int.self, forKey: .version)
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encodeIfPresent(approved, forKey: .approved)
        try c.encodeIfPresent(author, forKey: .author)
        try c.encodeIfPresent(contentrating, forKey: .contentrating)
        try c.encodeIfPresent(dependency, forKey: .dependency)
        try c.encodeIfPresent(description, forKey: .description)
        if !file.isEmpty { try c.encode(file, forKey: .file) }
        try c.encodeIfPresent(general, forKey: .general)
        try c.encodeIfPresent(preset, forKey: .preset)
        if !preview.isEmpty { try c.encode(preview, forKey: .preview) }
        try c.encodeIfPresent(tags, forKey: .tags)
        try c.encode(title, forKey: .title)
        try c.encodeIfPresent(visibility, forKey: .visibility)
        try c.encodeIfPresent(workshopid, forKey: .workshopid)
        try c.encodeIfPresent(workshopurl, forKey: .workshopurl)
        if !type.isEmpty { try c.encode(type, forKey: .type) }
        try c.encodeIfPresent(version, forKey: .version)
    }

    func applyingPreset(_ presetProject: WEProject) -> WEProject {
        var result = self
        var properties = result.general?.properties?.items ?? [:]
        for (key, value) in presetProject.preset ?? [:] {
            if var property = properties[key] {
                property.value = value
                properties[key] = property
            } else {
                properties[key] = WEProjectProperty(
                    type: Self.inferredPropertyType(for: value),
                    value: value,
                    isPresetOnly: true
                )
            }
        }
        result.general = WEProjectGeneral(properties: WEProjectProperties(items: properties))
        result.approved = presetProject.approved ?? result.approved
        result.author = presetProject.author ?? result.author
        result.contentrating = presetProject.contentrating ?? result.contentrating
        result.dependency = presetProject.dependency
        result.description = presetProject.description ?? result.description
        result.preset = presetProject.preset
        if !presetProject.preview.isEmpty { result.preview = presetProject.preview }
        result.tags = presetProject.tags ?? result.tags
        if !presetProject.title.isEmpty { result.title = presetProject.title }
        result.visibility = presetProject.visibility ?? result.visibility
        result.workshopid = presetProject.workshopid ?? result.workshopid
        result.workshopurl = presetProject.workshopurl ?? result.workshopurl
        return result
    }

    private static func inferredPropertyType(for value: WEPropertyValue) -> String {
        switch value {
        case .bool: return "bool"
        case .number: return "slider"
        case .string: return "textinput"
        }
    }
}

enum WallpaperKind: String {
    case scene, web, video, unsupported

    init(rawType: String) {
        switch rawType.lowercased() {
        case "scene": self = .scene
        case "web": self = .web
        case "video": self = .video
        default: self = .unsupported
        }
    }

    var displayName: String {
        switch self {
        case .scene: return L("场景")
        case .web: return L("网页")
        case .video: return L("视频")
        case .unsupported: return L("不支持")
        }
    }
}

// MARK: - WEWallpaper

struct WEWallpaper: Codable, RawRepresentable, Identifiable, Equatable, Hashable {

    var wallpaperDirectory: URL
    var renderDirectory: URL
    var assetOverlayDirectories: [URL]
    var project: WEProject
    var presetDependency: WorkshopId?
    var presetStatus: WEPresetStatus

    var id: String { wallpaperDirectory.path(percentEncoded: false) }

    var entryURL: URL { renderDirectory.appending(path: project.file) }
    var resolvedEntryURL: URL {
        if kind == .scene {
            let package = renderDirectory.appending(path: "scene.pkg")
            if FileManager.default.fileExists(atPath: package.path) { return package }
        }
        return entryURL
    }
    var previewURL: URL { wallpaperDirectory.appending(path: project.preview) }
    var kind: WallpaperKind { project.normalizedType }
    var isPreset: Bool { presetDependency != nil }
    var needsPresetDependency: Bool {
        isPreset && (presetStatus == .missingDependency || presetStatus == .invalidDependency)
    }
    var presetStatusDescription: String? {
        switch presetStatus {
        case .notPreset, .resolved: return nil
        case .missingDependency: return L("缺少基础壁纸")
        case .invalidDependency: return L("基础壁纸无效")
        case .circularDependency: return L("预设循环依赖")
        }
    }
    var isValid: Bool {
        project != .invalid && !project.file.isEmpty && (!isPreset || presetStatus == .resolved)
    }

    var rawValue: String {
        guard let data = try? JSONEncoder().encode(self),
              let str = String(data: data, encoding: .utf8) else { return "" }
        return str
    }

    var wallpaperSize: Int {
        let path = wallpaperDirectory.path(percentEncoded: false)
        Self.sizeCacheLock.lock()
        if let cached = Self.sizeCache[path] {
            Self.sizeCacheLock.unlock()
            return cached
        }
        Self.sizeCacheLock.unlock()
        let size = (try? wallpaperDirectory.directoryTotalAllocatedSize(includingSubfolders: true)) ?? 0
        Self.sizeCacheLock.lock()
        Self.sizeCache[path] = size
        Self.sizeCacheLock.unlock()
        return size
    }

    init(using project: WEProject, where url: URL, renderDirectory: URL? = nil,
         assetOverlayDirectories: [URL] = [], presetDependency: WorkshopId? = nil,
         presetStatus: WEPresetStatus = .notPreset) {
        self.wallpaperDirectory = url
        self.renderDirectory = renderDirectory ?? url
        self.assetOverlayDirectories = assetOverlayDirectories
        self.project = project
        self.presetDependency = presetDependency
        self.presetStatus = presetStatus
    }

    init?(rawValue: String) {
        guard let data = rawValue.data(using: .utf8),
              let wallpaper = try? JSONDecoder().decode(WEWallpaper.self, from: data) else {
            return nil
        }
        self = wallpaper
    }

    enum CodingKeys: CodingKey {
        case wallpaperDirectory, renderDirectory, assetOverlayDirectories, project
        case presetDependency, presetStatus
    }

    nonisolated(unsafe) static var sizeCache: [String: Int] = [:]
    static let sizeCacheLock = NSLock()

    static func invalidateSizeCache() {
        sizeCacheLock.lock()
        sizeCache.removeAll()
        sizeCacheLock.unlock()
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.wallpaperDirectory = try c.decode(URL.self, forKey: .wallpaperDirectory)
        self.renderDirectory = (try? c.decode(URL.self, forKey: .renderDirectory)) ?? wallpaperDirectory
        self.assetOverlayDirectories = (try? c.decode([URL].self, forKey: .assetOverlayDirectories)) ?? []
        self.project = try c.decode(WEProject.self, forKey: .project)
        self.presetDependency = try? c.decode(WorkshopId.self, forKey: .presetDependency)
        self.presetStatus = (try? c.decode(WEPresetStatus.self, forKey: .presetStatus)) ?? .notPreset
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(wallpaperDirectory, forKey: .wallpaperDirectory)
        try c.encode(renderDirectory, forKey: .renderDirectory)
        try c.encode(assetOverlayDirectories, forKey: .assetOverlayDirectories)
        try c.encode(project, forKey: .project)
        try c.encodeIfPresent(presetDependency, forKey: .presetDependency)
        try c.encode(presetStatus, forKey: .presetStatus)
    }

    static func == (lhs: WEWallpaper, rhs: WEWallpaper) -> Bool {
        lhs.wallpaperDirectory == rhs.wallpaperDirectory && lhs.project == rhs.project
    }

    func hash(into hasher: inout Hasher) { hasher.combine(wallpaperDirectory) }

    static func load(from url: URL) -> WEWallpaper {
        load(from: url, visited: [])
    }

    private static func load(from url: URL, visited: Set<String>) -> WEWallpaper {
        if let data = try? Data(contentsOf: url.appending(path: "project.json")),
           let project = try? JSONDecoder().decode(WEProject.self, from: data) {
            guard project.isWorkshopPreset, let dependency = project.dependency else {
                return WEWallpaper(using: project, where: url)
            }

            let dependencyID = dependency.rawValue
            guard dependencyID != url.lastPathComponent, !visited.contains(dependencyID) else {
                return WEWallpaper(using: project, where: url, presetDependency: dependency, presetStatus: .circularDependency)
            }
            let dependencyDirectories = WallpaperLibrary.shared.workshopItemDirectories(for: dependencyID)
            guard !dependencyDirectories.isEmpty else {
                return WEWallpaper(using: project, where: url, presetDependency: dependency, presetStatus: .missingDependency)
            }
            var foundCircularDependency = false
            for dependencyDirectory in dependencyDirectories {
                let base = load(from: dependencyDirectory, visited: visited.union([url.lastPathComponent]))
                guard base.isValid,
                      base.kind != .unsupported,
                      FileManager.default.fileExists(atPath: base.resolvedEntryURL.path) else {
                    foundCircularDependency = foundCircularDependency || base.presetStatus == .circularDependency
                    continue
                }
                var effectiveProject = base.project.applyingPreset(project)
                effectiveProject.workshopid = project.workshopid ?? WorkshopId(rawValue: url.lastPathComponent)
                return WEWallpaper(
                    using: effectiveProject,
                    where: url,
                    renderDirectory: base.renderDirectory,
                    assetOverlayDirectories: [url] + base.assetOverlayDirectories,
                    presetDependency: dependency,
                    presetStatus: .resolved
                )
            }
            let status: WEPresetStatus = foundCircularDependency ? .circularDependency : .invalidDependency
            return WEWallpaper(using: project, where: url, presetDependency: dependency, presetStatus: status)
        }
        return WEWallpaper(using: .invalid, where: url)
    }

    @discardableResult
    func updateStoredMetadata(title: String? = nil, tags: [String]? = nil) -> WEWallpaper? {
        let url = wallpaperDirectory.appending(path: "project.json")
        guard let data = try? Data(contentsOf: url),
              var object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if let title { object["title"] = title }
        if let tags { object["tags"] = tags }
        guard let updated = try? JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]) else {
            return nil
        }
        do {
            try updated.write(to: url, options: .atomic)
            return Self.load(from: wallpaperDirectory)
        } catch {
            return nil
        }
    }
}

enum WEPresetStatus: String, Codable {
    case notPreset
    case resolved
    case missingDependency
    case invalidDependency
    case circularDependency
}

// MARK: - 排序

enum WEWallpaperSortingMethod: String, CaseIterable, Identifiable {
    var id: Self { self }
    case name = "名称"
    case rating = "评分"
    case fileSize = "文件大小"
}

enum WEWallpaperSortingSequence: Int, Codable {
    case decrease = 0, increase = 1
}

// MARK: - URL 目录大小工具

extension URL {
    func isDirectoryAndReachable() throws -> Bool {
        guard try resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true else { return false }
        return try checkResourceIsReachable()
    }

    func directoryTotalAllocatedSize(includingSubfolders: Bool = false) throws -> Int? {
        guard try isDirectoryAndReachable() else { return nil }
        if includingSubfolders {
            guard let urls = FileManager.default.enumerator(at: self, includingPropertiesForKeys: nil)?.allObjects as? [URL] else { return nil }
            return try urls.lazy.reduce(0) {
                (try $1.resourceValues(forKeys: [.totalFileAllocatedSizeKey]).totalFileAllocatedSize ?? 0) + $0
            }
        }
        return try FileManager.default.contentsOfDirectory(at: self, includingPropertiesForKeys: nil).lazy.reduce(0) {
            (try $1.resourceValues(forKeys: [.totalFileAllocatedSizeKey]).totalFileAllocatedSize ?? 0) + $0
        }
    }
}
