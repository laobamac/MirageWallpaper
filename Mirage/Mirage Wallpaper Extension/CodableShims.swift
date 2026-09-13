//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

struct MirageSettingsViewModels: Codable {
    let desktop: MirageSettingsViewModel?
    let screenSaver: MirageSettingsViewModel?
}

struct MirageSettingsViewModel: Codable {
    let groups: [MirageSettingsGroup]
    let refreshPolicy: MirageRefreshPolicy
    let isModificationDisabled: Bool
}

struct MirageSettingsGroup: Codable {
    let id: MirageGroupID
    let items: [MirageSettingsItem]
    let localizedName: String
    let disposability: MirageDisposability
    let sortOrder: Int
    let sortID: MirageGroupSortID?
    let allChoiceID: MirageChoiceID?
    let shouldHideItemLabels: Bool?
    let contextMenu: MirageContextMenu?
    let thumbnail: Data?
}

struct MirageSettingsItem: Codable {
    let id: MirageChoiceID
    let localizedName: String
    let thumbnail: MirageThumbnail
    let choice: MirageChoiceDescriptor
    let contentBadge: MirageContentBadge
    let showInTopLevel: Bool
    let sortOrder: Int
    let disposability: MirageDisposability
}

struct MirageChoiceID: Codable {
    let id: String
    let descriptor: MirageChoiceIDDescriptor
}

struct MirageChoiceIDDescriptor: Codable {
    let provider: MirageChoiceProviderID
    let identifier: String
    let files: [URL]
    let configuration: Data
}

struct MirageChoiceDescriptor: Codable {
    let id: MirageChoiceID
    let provider: MirageChoiceProviderID
    let identifier: String
    let name: String?
    let localizedDescription: String
    let thumbnail: MirageThumbnail
    let isDownloaded: Bool
    let options: [MirageWallpaperOption]
}

struct MirageChoiceProviderID: Codable {
    let rawValue: String

    init(_ rawValue: String) { self.rawValue = rawValue }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        rawValue = try container.decode(String.self)
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }
}

struct MirageWallpaperOption: Codable {}
struct MirageGroupID: Codable { let id: String }
struct MirageGroupSortID: Codable { let id: String }

enum MirageDisposability: Codable {
    case none
    case removable
    case purgeable

    private enum Keys: String, CodingKey { case none, removable, purgeable }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: Keys.self)
        switch self {
        case .none: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .none)
        case .removable: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .removable)
        case .purgeable: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .purgeable)
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: Keys.self)
        if container.contains(.removable) { self = .removable }
        else if container.contains(.purgeable) { self = .purgeable }
        else { self = .none }
    }
}

enum MirageContentBadge: Codable {
    case none
    case video
    case dynamic

    private enum Keys: String, CodingKey { case none, video, dynamic }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: Keys.self)
        switch self {
        case .none: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .none)
        case .video: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .video)
        case .dynamic: _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .dynamic)
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: Keys.self)
        if container.contains(.video) { self = .video }
        else if container.contains(.dynamic) { self = .dynamic }
        else { self = .none }
    }
}

enum MirageRefreshPolicy: Codable {
    case `default`

    private enum Keys: String, CodingKey { case `default` }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: Keys.self)
        _ = container.nestedContainer(keyedBy: EmptyKeys.self, forKey: .default)
    }

    init(from decoder: Decoder) throws { self = .default }
}

enum MirageThumbnail: Codable {
    case image(URL)
    case customButton(MirageCustomButton)

    private enum Keys: String, CodingKey { case image, customButton }
    private enum ImageKeys: String, CodingKey { case url }
    private enum ButtonKeys: String, CodingKey { case value }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: Keys.self)
        switch self {
        case .image(let url):
            var nested = container.nestedContainer(keyedBy: ImageKeys.self, forKey: .image)
            try nested.encode(url, forKey: .url)
        case .customButton(let button):
            var nested = container.nestedContainer(keyedBy: ButtonKeys.self, forKey: .customButton)
            try nested.encode(button, forKey: .value)
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: Keys.self)
        if container.contains(.image) {
            let nested = try container.nestedContainer(keyedBy: ImageKeys.self, forKey: .image)
            self = .image(try nested.decode(URL.self, forKey: .url))
        } else {
            self = .image(URL(fileURLWithPath: "/"))
        }
    }
}

enum MirageCustomButton: Codable { case addPhotoButton }
struct MirageContextMenu: Codable { let items: [MirageContextMenuItem] }
struct MirageContextMenuItem: Codable { let identifier: String; let name: String }
enum EmptyKeys: String, CodingKey { case value }

@objc(MirageShimViewModelsXPC)
final class MirageShimViewModelsXPC: NSObject, NSSecureCoding {
    static let supportsSecureCoding = true
    let value: MirageSettingsViewModels

    init(value: MirageSettingsViewModels) {
        self.value = value
        super.init()
    }

    required init?(coder: NSCoder) { return nil }

    func encode(with coder: NSCoder) {
        guard let archiver = coder as? NSKeyedArchiver else { return }
        try? archiver.encodeEncodable(value, forKey: "WallpaperSettingsViewModels")
    }
}

func mirageSettingsViewModelsXPC(_ value: MirageSettingsViewModels) -> AnyObject? {
    guard let data = try? NSKeyedArchiver.archivedData(
        withRootObject: MirageShimViewModelsXPC(value: value), requiringSecureCoding: false),
          let unarchiver = try? NSKeyedUnarchiver(forReadingFrom: data),
          let realClass = objc_getClass("WallpaperSettingsViewModelsXPC") as? AnyClass else { return nil }
    unarchiver.requiresSecureCoding = false
    unarchiver.setClass(realClass, forClassName: "MirageShimViewModelsXPC")
    let result = unarchiver.decodeObject(forKey: NSKeyedArchiveRootObjectKey)
    unarchiver.finishDecoding()
    return result as AnyObject?
}
