//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

enum MirageLockAnyValue: Codable {
    case string(String)
    case number(Double)
    case bool(Bool)
    case object([String: MirageLockAnyValue])
    case array([MirageLockAnyValue])
    case null

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null; return }
        if let value = try? container.decode(Bool.self) { self = .bool(value); return }
        if let value = try? container.decode(Double.self) { self = .number(value); return }
        if let value = try? container.decode(String.self) { self = .string(value); return }
        if let value = try? container.decode([String: MirageLockAnyValue].self) { self = .object(value); return }
        self = .array(try container.decode([MirageLockAnyValue].self))
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .bool(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .null: try container.encodeNil()
        }
    }

    var foundationValue: Any {
        switch self {
        case .string(let value): return value
        case .number(let value): return value
        case .bool(let value): return value
        case .object(let value): return value.mapValues { $0.foundationValue }
        case .array(let value): return value.map(\.foundationValue)
        case .null: return NSNull()
        }
    }
}

struct MirageLockDisplayConfiguration: Codable {
    let displayID: UInt32
    let wallpaperID: String
    let title: String
    let kind: String
    let renderDirectory: String
    let entryPath: String
    let previewPath: String?
    let desktopFallbackPath: String?
    let rawProperties: [String: MirageLockAnyValue]
    let fps: Int
    let fillMode: String
    var position: WallpaperPosition? = nil
    let loadFromMemory: Bool?
}

struct MirageLockConfiguration: Codable {
    let version: Int
    let enabled: Bool?
    let displays: [String: MirageLockDisplayConfiguration]
}


struct MirageLockRuntimeIdentity {
    static let current = Self()
    let path: String
    let fingerprint: String
    let version: String

    private init() {
        path = MirageLockBridge.normalizedPath(Bundle.main.bundleURL)
        fingerprint = (try? MirageLockBridge.fingerprint(at: Bundle.main.bundleURL)) ?? ""
        version = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"
    }
}

final class MirageLockConfigurationStore {
    static let shared = MirageLockConfigurationStore()
    private let lock = NSLock()
    private let containerURL: URL?
    private var stored: (configuration: MirageLockConfiguration, data: Data)?

    init(containerURL: URL? = nil) {
        self.containerURL = containerURL
    }

    func load() throws -> (configuration: MirageLockConfiguration, data: Data) {
        lock.lock()
        defer { lock.unlock() }
        let container = try containerURL ?? MirageLockBridge.containerURL()
        let data = try Data(contentsOf: container.appendingPathComponent(MirageLockBridge.configurationName))
        if let stored, stored.data == data { return stored }
        let configuration = try JSONDecoder().decode(MirageLockConfiguration.self, from: data)
        guard (1...2).contains(configuration.version), !configuration.displays.isEmpty,
              configuration.displays.values.allSatisfy({ $0.kind == "video" || $0.kind == "scene" }) else {
            throw MirageLockBridge.failure("Unsupported lock screen configuration")
        }
        let result = (configuration, data)
        stored = result
        return result
    }

    var lastKnownConfiguration: MirageLockConfiguration? {
        lock.lock()
        defer { lock.unlock() }
        return stored?.configuration
    }
}
