//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AVFoundation

extension ContentViewModel {
    var areAllResolutionsSelected: Bool {
        widescreenResolution == .all &&
            ultraWidescreenResolution == .all &&
            dualscreenResolution == .all &&
            triplescreenResolution == .all &&
            potraitscreenResolution == .all &&
            miscResolution == .all
    }

    var areAllResolutionsCleared: Bool {
        widescreenResolution.isEmpty &&
            ultraWidescreenResolution.isEmpty &&
            dualscreenResolution.isEmpty &&
            triplescreenResolution.isEmpty &&
            potraitscreenResolution.isEmpty &&
            miscResolution.isEmpty
    }

    func selectAllResolutions() {
        widescreenResolution = .all
        ultraWidescreenResolution = .all
        dualscreenResolution = .all
        triplescreenResolution = .all
        potraitscreenResolution = .all
        miscResolution = .all
    }

    func clearResolutions() {
        widescreenResolution = .none
        ultraWidescreenResolution = .none
        dualscreenResolution = .none
        triplescreenResolution = .none
        potraitscreenResolution = .none
        miscResolution = .none
    }
}

typealias FilterResultsViewModel = ContentViewModel

protocol FilterResultsModel: OptionSet where Element == Self, RawValue == Int {
    static var allOptions: [String] { get }
    static func option(at index: Int) -> Self
}

extension FilterResultsModel {
    static func option(at index: Int) -> Self {
        Self(rawValue: 1 << index)
    }
}

struct FRShowOnly: OptionSet {
    let rawValue: Int
    
    static let allOptions = [
        (approved, "广受好评", "trophy.fill", Color.green),
        (myFavourites, "我的收藏", "heart.fill", Color.pink),
        (mobileCompatible, "移动端兼容", "iphone.gen3", Color.orange),
        (audioResponsive, "音频响应", "waveform.path.ecg", Color.blue),
        (customizable, "可自定义", "slider.horizontal.3", Color.accentColor)
    ]
    
    static let approved             = FRShowOnly(rawValue: 1 << 0)
    static let myFavourites         = FRShowOnly(rawValue: 1 << 1)
    static let audioResponsive      = FRShowOnly(rawValue: 1 << 2)
    static let customizable         = FRShowOnly(rawValue: 1 << 3)
    static let mobileCompatible     = FRShowOnly(rawValue: 1 << 4)
    
    static let all: FRShowOnly = [.approved, myFavourites, .audioResponsive, .customizable, .mobileCompatible]
    static let none: FRShowOnly = []

    static let approvedSteamTag = "Approved"
    static let mobileCompatibleSteamTag = "Mobile"
    static let audioResponsiveSteamTag = "Audio responsive"
    static let customizableSteamTag = "Customizable"

    var requiredSteamTags: [String] {
        var tags: [String] = []
        if contains(.approved) { tags.append(Self.approvedSteamTag) }
        if contains(.mobileCompatible) { tags.append(Self.mobileCompatibleSteamTag) }
        if contains(.audioResponsive) { tags.append(Self.audioResponsiveSteamTag) }
        if contains(.customizable) { tags.append(Self.customizableSteamTag) }
        return tags
    }

    static func migratedLegacyRawValue(_ rawValue: Int) -> FRShowOnly {
        if rawValue == 0b1_1111 { return .none }
        var migrated = FRShowOnly.none
        if rawValue & (1 << 0) != 0 { migrated.insert(.approved) }
        if rawValue & (1 << 1) != 0 { migrated.insert(.myFavourites) }
        if rawValue & (1 << 3) != 0 { migrated.insert(.audioResponsive) }
        if rawValue & (1 << 4) != 0 { migrated.insert(.customizable) }
        return migrated
    }

    func matches(workshopItem: WorkshopItem, favoriteIDs: Set<String>) -> Bool {
        if contains(.approved), !workshopItem.isApproved { return false }
        if contains(.myFavourites), !favoriteIDs.contains(workshopItem.publishedFileId) { return false }
        if contains(.mobileCompatible), !workshopItem.isMobileCompatible { return false }
        if contains(.audioResponsive), !workshopItem.isAudioResponsive { return false }
        if contains(.customizable), !workshopItem.isCustomizable { return false }
        return true
    }

    func matches(
        wallpaper: WEWallpaper,
        localFavoriteIDs: Set<String>,
        workshopFavoriteIDs: Set<String>,
        importedDirectoryPrefix: String
    ) -> Bool {
        if contains(.approved), wallpaper.project.approved != true { return false }
        if contains(.myFavourites) {
            if wallpaper.wallpaperDirectory.path.hasPrefix(importedDirectoryPrefix) {
                if !localFavoriteIDs.contains(wallpaper.id) { return false }
            } else if let workshopID = wallpaper.verifiedWorkshopID() {
                if !workshopFavoriteIDs.contains(workshopID) { return false }
            } else if !localFavoriteIDs.contains(wallpaper.id) {
                return false
            }
        }
        if contains(.mobileCompatible) {
            let matches = (wallpaper.project.tags ?? []).contains {
                $0.localizedCaseInsensitiveContains(Self.mobileCompatibleSteamTag)
            }
            if !matches { return false }
        }
        if contains(.audioResponsive) {
            let matches = (wallpaper.project.tags ?? []).contains {
                $0.caseInsensitiveCompare(Self.audioResponsiveSteamTag) == .orderedSame
            }
            if !matches { return false }
        }
        if contains(.customizable) {
            let properties = wallpaper.project.general?.properties?.items.values ?? Dictionary<String, WEProjectProperty>().values
            let editable = properties.contains { property in
                guard !property.isPresetOnly else { return false }
                switch property.propertyType {
                case .text, .group, .unknown:
                    return false
                default:
                    return true
                }
            }
            if !editable { return false }
        }
        return true
    }
}

struct FRType: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "场景",
        "视频",
        "网页",
        "应用程序",
        "预设"
    ]
    static let scene            = FRType(rawValue: 1 << 0)
    static let video            = FRType(rawValue: 1 << 1)
    static let web              = FRType(rawValue: 1 << 2)
    static let application      = FRType(rawValue: 1 << 3)
    static let preset           = FRType(rawValue: 1 << 4)
    
    static let legacyAll        = FRType(rawValue: 0b1111)
    static let all: FRType      = [.scene, .video, .web, .application, .preset]
    static let none: FRType     = []
}

struct FRAgeRating: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "所有人",
        "轻度裸露",
        "成人"
    ]
    
    static let everyone             = FRAgeRating(rawValue: 1 << 0)
    static let partialNudity        = FRAgeRating(rawValue: 1 << 1)
    static let mature               = FRAgeRating(rawValue: 1 << 2)
    
    static let all: FRAgeRating     = [.everyone, .partialNudity, .mature]
    static let none: FRAgeRating    = []
}

struct FRWidescreenResolution: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "标清",
        "1280 x 720",
        "1366 x 768",
        "1920 x 1080 - 全高清",
        "2560 x 1440",
        "3840 x 2160 - 4K",
        "7680 x 4320 - 8K"
    ]
    
    static let standardDefinition   = Self.init(rawValue: 1 << 0)
    static let resolution1280x720   = Self.init(rawValue: 1 << 1)
    static let resolution1366x768   = Self.init(rawValue: 1 << 2)
    static let resolution1920x1080  = Self.init(rawValue: 1 << 3)
    static let resolution2560x1440  = Self.init(rawValue: 1 << 4)
    static let resolution3840x2160  = Self.init(rawValue: 1 << 5)
    static let resolution7680x4320  = Self.init(rawValue: 1 << 6)

    static let legacyAll: Self      = Self(rawValue: 31)
    static let interimAll: Self     = Self(rawValue: 63)
    static let all: Self            = [.standardDefinition, resolution1280x720, resolution1366x768, resolution1920x1080, .resolution2560x1440, .resolution3840x2160, .resolution7680x4320]
    static let none: Self           = []
}

struct FRUltraWidescreenResolution: FilterResultsModel {
    let rawValue: Int
    
    
    static let allOptions: [String] = [
        "超宽（标准）",
        "2560 x 1080",
        "3440 x 1440"
    ]
    
    static let ultrawideStandard    = FRUltraWidescreenResolution(rawValue: 1 << 0)
    static let resolution2560x1080  = FRUltraWidescreenResolution(rawValue: 1 << 1)
    static let resolution3440x1440  = FRUltraWidescreenResolution(rawValue: 1 << 2)
    
    static let all: FRUltraWidescreenResolution = [.ultrawideStandard, resolution2560x1080, .resolution3440x1440]
    static let none: FRUltraWidescreenResolution = []
}

struct FRDualscreenResolution: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions: [String] = [
        "双显示器（标准）",
        "3840 x 1080",
        "5120 x 1440",
        "7680 x 2160"
    ]
    
    static let dualStandard         = Self.init(rawValue: 1 << 0)
    static let resolution3840x1080  = Self.init(rawValue: 1 << 1)
    static let resolution5120x1440  = Self.init(rawValue: 1 << 2)
    static let resolution7680x2160  = Self.init(rawValue: 1 << 3)
    
    static let all: Self = [.dualStandard, .resolution3840x1080, .resolution5120x1440, .resolution7680x2160]
    static let none: Self = []
}

struct FRTriplescreenResolution: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions: [String] = [
            "三显示器（标准）",
            "4096 x 768",
            "5760 x 1080",
            "7680 x 1440",
            "11520 x 2160"
        ]
    
    static let tripleStandard        = FRTriplescreenResolution(rawValue: 1 << 0)
    static let resolution4096x768    = FRTriplescreenResolution(rawValue: 1 << 1)
    static let resolution5760x1080   = FRTriplescreenResolution(rawValue: 1 << 2)
    static let resolution7680x1440   = FRTriplescreenResolution(rawValue: 1 << 3)
    static let resolution11520x2160  = FRTriplescreenResolution(rawValue: 1 << 4)
    
    static let all: FRTriplescreenResolution = [.tripleStandard, resolution4096x768, resolution5760x1080, resolution7680x1440, resolution11520x2160]
    static let none: FRTriplescreenResolution = []
}

struct FRPortraitScreenResolution: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "纵向（标准）",
        "720 x 1280",
        "1080 x 1920",
        "1440 x 2560",
        "2160 x 3840"
    ]
    
    static let portraitStandard     = Self.init(rawValue: 1 << 0)
    static let resolution720x1280   = Self.init(rawValue: 1 << 1)
    static let resolution1080x1920  = Self.init(rawValue: 1 << 2)
    static let resolution1440x2560  = Self.init(rawValue: 1 << 3)
    static let resolution2160x3840  = Self.init(rawValue: 1 << 4)
    
    static let all: Self            = [.portraitStandard, .resolution720x1280, .resolution1080x1920, .resolution1440x2560, .resolution2160x3840]
    static let none: Self           = []
}

struct FRMiscResolution: FilterResultsModel {
    let rawValue: Int

    static let allOptions = [
        "其他分辨率",
        "动态分辨率"
    ]

    static let otherResolution     = Self.init(rawValue: 1 << 0)
    static let dynamicResolution   = Self.init(rawValue: 1 << 1)

    static let all: Self           = [.otherResolution, .dynamicResolution]
    static let none: Self          = []
}

enum FRResolutionFilter {
    private enum Kind: Equatable {
        case widescreen(FRWidescreenResolution)
        case ultraWidescreen(FRUltraWidescreenResolution)
        case dualscreen(FRDualscreenResolution)
        case triplescreen(FRTriplescreenResolution)
        case portrait(FRPortraitScreenResolution)
        case misc(FRMiscResolution)
    }

    private struct Dimensions {
        let width: Int
        let height: Int
    }

    private struct FileSignature: Equatable {
        let path: String
        let exists: Bool
        let isDirectory: Bool
        let size: Int64
        let modificationTime: TimeInterval
    }

    private struct CachedMeasurement {
        let signature: [FileSignature]
        let kinds: [Kind]
    }

    private static let cacheLock = NSLock()
    private static var measuredKinds: [String: CachedMeasurement] = [:]

    static func selectedSteamTags(
        widescreen: FRWidescreenResolution,
        ultraWidescreen: FRUltraWidescreenResolution,
        dualscreen: FRDualscreenResolution,
        triplescreen: FRTriplescreenResolution,
        portrait: FRPortraitScreenResolution,
        misc: FRMiscResolution
    ) -> [String]? {
        if allResolutionsSelected(
            widescreen: widescreen,
            ultraWidescreen: ultraWidescreen,
            dualscreen: dualscreen,
            triplescreen: triplescreen,
            portrait: portrait,
            misc: misc
        ) {
            return nil
        }
        let groups: [(Int, [String])] = [
            (widescreen.rawValue, [
                "Standard Definition", "1280 x 720", "1366 x 768", "1920 x 1080",
                "2560 x 1440", "3840 x 2160", "7680 x 4320"
            ]),
            (ultraWidescreen.rawValue, [
                "Ultrawide Standard Definition", "Ultrawide 2560 x 1080", "Ultrawide 3440 x 1440"
            ]),
            (dualscreen.rawValue, [
                "Dual Standard Definition", "Dual 3840 x 1080", "Dual 5120 x 1440", "Dual 7680 x 2160"
            ]),
            (triplescreen.rawValue, [
                "Triple Standard Definition", "Triple 4096 x 768", "Triple 5760 x 1080",
                "Triple 7680 x 1440", "Triple 11520 x 2160"
            ]),
            (portrait.rawValue, [
                "Portrait Standard Definition", "Portrait 720 x 1280", "Portrait 1080 x 1920",
                "Portrait 1440 x 2560", "Portrait 2160 x 3840"
            ]),
            (misc.rawValue, ["Other resolution", "Dynamic resolution"])
        ]
        return groups.flatMap { rawValue, tags in
            tags.indices.compactMap { rawValue & (1 << $0) != 0 ? tags[$0] : nil }
        }
    }

    static func matches(
        wallpaper: WEWallpaper,
        widescreen: FRWidescreenResolution,
        ultraWidescreen: FRUltraWidescreenResolution,
        dualscreen: FRDualscreenResolution,
        triplescreen: FRTriplescreenResolution,
        portrait: FRPortraitScreenResolution,
        misc: FRMiscResolution
    ) -> Bool {
        if allResolutionsSelected(
            widescreen: widescreen,
            ultraWidescreen: ultraWidescreen,
            dualscreen: dualscreen,
            triplescreen: triplescreen,
            portrait: portrait,
            misc: misc
        ) {
            return true
        }
        let kinds: [Kind]
        switch wallpaper.kind {
        case .video, .scene, .web:
            kinds = measuredKinds(for: wallpaper)
        default:
            kinds = wallpaper.project.tags?.compactMap { kind(forTag: normalized($0)) } ?? []
        }
        if kinds.isEmpty { return misc.contains(.otherResolution) }
        return kinds.contains {
            isSelected(
                $0,
                widescreen: widescreen,
                ultraWidescreen: ultraWidescreen,
                dualscreen: dualscreen,
                portrait: portrait,
                triplescreen: triplescreen,
                misc: misc
            )
        }
    }

    static func matches(
        tags: [String],
        widescreen: FRWidescreenResolution,
        ultraWidescreen: FRUltraWidescreenResolution,
        dualscreen: FRDualscreenResolution,
        triplescreen: FRTriplescreenResolution,
        portrait: FRPortraitScreenResolution,
        misc: FRMiscResolution
    ) -> Bool {
        if allResolutionsSelected(
            widescreen: widescreen,
            ultraWidescreen: ultraWidescreen,
            dualscreen: dualscreen,
            triplescreen: triplescreen,
            portrait: portrait,
            misc: misc
        ) {
            return true
        }
        let kinds = tags.compactMap { kind(forTag: normalized($0)) }
        if kinds.isEmpty { return misc.contains(.otherResolution) }
        return kinds.contains {
            isSelected(
                $0,
                widescreen: widescreen,
                ultraWidescreen: ultraWidescreen,
                dualscreen: dualscreen,
                portrait: portrait,
                triplescreen: triplescreen,
                misc: misc
            )
        }
    }

    private static func measuredKinds(for wallpaper: WEWallpaper) -> [Kind] {
        let key = wallpaper.wallpaperDirectory.path(percentEncoded: false)
        let signature = sourceSignature(for: wallpaper)
        cacheLock.lock()
        if let cached = measuredKinds[key], cached.signature == signature {
            cacheLock.unlock()
            return cached.kinds
        }
        cacheLock.unlock()

        var result: [Kind] = []
        switch wallpaper.kind {
        case .video:
            if let dimensions = videoDimensions(at: wallpaper.resolvedEntryURL),
               let kind = kind(for: dimensions) {
                result = [kind]
            }
            if result.isEmpty { result = [.misc(.otherResolution)] }
        case .scene:
            if sceneIsDynamic(at: wallpaper.resolvedEntryURL) {
                result.append(.misc(.dynamicResolution))
            }
            if let dimensions = sceneDimensions(at: wallpaper.resolvedEntryURL),
               let kind = kind(for: dimensions) {
                result.append(kind)
            }
            if result.isEmpty { result = [.misc(.otherResolution)] }
        case .web:
            result = webKinds(for: wallpaper)
        default:
            break
        }

        cacheLock.lock()
        measuredKinds[key] = CachedMeasurement(signature: signature, kinds: result)
        cacheLock.unlock()
        return result
    }

    private static func sourceSignature(for wallpaper: WEWallpaper) -> [FileSignature] {
        var urls = [
            wallpaper.wallpaperDirectory,
            wallpaper.wallpaperDirectory.appending(path: "project.json"),
            wallpaper.renderDirectory.appending(path: "project.json"),
            wallpaper.entryURL,
            wallpaper.resolvedEntryURL
        ]
        var seen = Set<String>()
        urls = urls.filter { seen.insert($0.standardizedFileURL.path).inserted }
        return urls.map(fileSignature(for:))
    }

    private static func fileSignature(for url: URL) -> FileSignature {
        let path = url.standardizedFileURL.path(percentEncoded: false)
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: path) else {
            return FileSignature(path: path, exists: false, isDirectory: false, size: -1, modificationTime: -1)
        }
        let type = (attributes[.type] as? FileAttributeType) ?? .typeUnknown
        let size = (attributes[.size] as? NSNumber)?.int64Value ?? -1
        let modificationTime = (attributes[.modificationDate] as? Date)?.timeIntervalSinceReferenceDate ?? -1
        return FileSignature(
            path: path,
            exists: true,
            isDirectory: type == .typeDirectory,
            size: size,
            modificationTime: modificationTime
        )
    }

    private static func kind(forTag tag: String) -> Kind? {
        switch tag {
        case "standarddefinition": return .widescreen(.standardDefinition)
        case "1280x720": return .widescreen(.resolution1280x720)
        case "1366x768": return .widescreen(.resolution1366x768)
        case let value where value.hasPrefix("1920x1080"): return .widescreen(.resolution1920x1080)
        case "2560x1440": return .widescreen(.resolution2560x1440)
        case let value where value.hasPrefix("3840x2160"): return .widescreen(.resolution3840x2160)
        case let value where value.hasPrefix("7680x4320"): return .widescreen(.resolution7680x4320)
        case "ultrawidestandard", "ultrawidestandarddefinition": return .ultraWidescreen(.ultrawideStandard)
        case "ultrawide2560x1080", "2560x1080": return .ultraWidescreen(.resolution2560x1080)
        case "ultrawide3440x1440", "3440x1440": return .ultraWidescreen(.resolution3440x1440)
        case "dualstandard", "dualstandarddefinition": return .dualscreen(.dualStandard)
        case "dual3840x1080", "3840x1080": return .dualscreen(.resolution3840x1080)
        case "dual5120x1440", "5120x1440": return .dualscreen(.resolution5120x1440)
        case "dual7680x2160", "7680x2160": return .dualscreen(.resolution7680x2160)
        case "triplestandard", "triplestandarddefinition": return .triplescreen(.tripleStandard)
        case "triple4096x768", "4096x768": return .triplescreen(.resolution4096x768)
        case "triple5760x1080", "5760x1080": return .triplescreen(.resolution5760x1080)
        case "triple7680x1440", "7680x1440": return .triplescreen(.resolution7680x1440)
        case "triple11520x2160", "11520x2160": return .triplescreen(.resolution11520x2160)
        case "portraitstandard", "portraitstandarddefinition", "potraitstandard": return .portrait(.portraitStandard)
        case "portrait720x1280", "720x1280": return .portrait(.resolution720x1280)
        case "portrait1080x1920", "1080x1920": return .portrait(.resolution1080x1920)
        case "portrait1440x2560", "1440x2560": return .portrait(.resolution1440x2560)
        case "portrait2160x3840", "2160x3840": return .portrait(.resolution2160x3840)
        case "otherresolution": return .misc(.otherResolution)
        case "dynamicresolution": return .misc(.dynamicResolution)
        default: return nil
        }
    }

    private static func kind(for dimensions: Dimensions) -> Kind? {
        switch (dimensions.width, dimensions.height) {
        case (1280, 720): return .widescreen(.resolution1280x720)
        case (1366, 768): return .widescreen(.resolution1366x768)
        case (1920, 1080): return .widescreen(.resolution1920x1080)
        case (2560, 1440): return .widescreen(.resolution2560x1440)
        case (3840, 2160): return .widescreen(.resolution3840x2160)
        case (7680, 4320): return .widescreen(.resolution7680x4320)
        case (2560, 1080): return .ultraWidescreen(.resolution2560x1080)
        case (3440, 1440): return .ultraWidescreen(.resolution3440x1440)
        case (3840, 1080): return .dualscreen(.resolution3840x1080)
        case (5120, 1440): return .dualscreen(.resolution5120x1440)
        case (7680, 2160): return .dualscreen(.resolution7680x2160)
        case (4096, 768): return .triplescreen(.resolution4096x768)
        case (5760, 1080): return .triplescreen(.resolution5760x1080)
        case (7680, 1440): return .triplescreen(.resolution7680x1440)
        case (11520, 2160): return .triplescreen(.resolution11520x2160)
        case (720, 1280): return .portrait(.resolution720x1280)
        case (1080, 1920): return .portrait(.resolution1080x1920)
        case (1440, 2560): return .portrait(.resolution1440x2560)
        case (2160, 3840): return .portrait(.resolution2160x3840)
        default:
            let ratio = Double(dimensions.width) / Double(dimensions.height)
            if aspectMatches(ratio, 16.0 / 9.0) {
                return .widescreen(.standardDefinition)
            }
            if aspectMatches(ratio, 21.0 / 9.0) || aspectMatches(ratio, 43.0 / 18.0) {
                return .ultraWidescreen(.ultrawideStandard)
            }
            if aspectMatches(ratio, 32.0 / 9.0) {
                return .dualscreen(.dualStandard)
            }
            if aspectMatches(ratio, 16.0 / 3.0) {
                return .triplescreen(.tripleStandard)
            }
            if aspectMatches(ratio, 9.0 / 16.0) {
                return .portrait(.portraitStandard)
            }
            return .misc(.otherResolution)
        }
    }

    private static func aspectMatches(_ ratio: Double, _ target: Double) -> Bool {
        abs(ratio - target) <= target * 0.04
    }

    private static func videoDimensions(at url: URL) -> Dimensions? {
        guard url.isFileURL else { return nil }
        let asset = AVURLAsset(url: url)
        guard let track = asset.tracks(withMediaType: .video).first else { return nil }
        let rect = CGRect(origin: .zero, size: track.naturalSize).applying(track.preferredTransform)
        let width = Int(abs(rect.width).rounded())
        let height = Int(abs(rect.height).rounded())
        guard width > 0, height > 0 else { return nil }
        return Dimensions(width: width, height: height)
    }

    private static func sceneDimensions(at url: URL) -> Dimensions? {
        guard let data = sceneJSONData(at: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if let general = root["general"] as? [String: Any],
           let projection = general["orthogonalprojection"] as? [String: Any] {
            let isAuto = boolValue(projection["auto"])
            if !isAuto, let dimensions = dimensions(width: projection["width"], height: projection["height"]) {
                return dimensions
            }
            if isAuto, let objects = root["objects"] as? [[String: Any]] {
                var largest: Dimensions?
                for object in objects {
                    guard object["image"] != nil,
                          let dimensions = dimensions(from: object["size"]) else { continue }
                    if largest == nil || dimensions.width * dimensions.height > largest!.width * largest!.height {
                        largest = dimensions
                    }
                }
                if let largest { return largest }
            }
        }
        return explicitDimensions(in: root)
    }

    private static func webKinds(for wallpaper: WEWallpaper) -> [Kind] {
        let projectURLs = [
            wallpaper.wallpaperDirectory.appending(path: "project.json"),
            wallpaper.renderDirectory.appending(path: "project.json")
        ]
        var seen = Set<String>()
        for projectURL in projectURLs where seen.insert(projectURL.standardizedFileURL.path).inserted {
            if let projectData = try? Data(contentsOf: projectURL),
               let project = try? JSONSerialization.jsonObject(with: projectData) as? [String: Any],
               let dimensions = explicitDimensions(in: project),
               let kind = kind(for: dimensions) {
                return [kind]
            }
        }
        if let html = try? String(contentsOf: wallpaper.entryURL, encoding: .utf8) {
            if let dimensions = htmlDimensions(html), let kind = kind(for: dimensions) {
                return [kind]
            }
        }
        return [.misc(.dynamicResolution)]
    }

    private static func sceneIsDynamic(at url: URL) -> Bool {
        guard let data = sceneJSONData(at: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let general = root["general"] as? [String: Any],
              let projection = general["orthogonalprojection"] as? [String: Any] else {
            return false
        }
        return boolValue(projection["auto"])
    }

    private static func sceneJSONData(at url: URL) -> Data? {
        guard url.isFileURL else { return nil }
        guard url.pathExtension.lowercased() == "pkg" else {
            return try? Data(contentsOf: url)
        }
        guard let data = try? Data(contentsOf: url), data.count >= 12 else { return nil }
        var reader = BinaryReader(data: data)
        guard let stampLength = reader.readInt32(), stampLength > 0, stampLength <= 64,
              reader.readData(count: Int(stampLength)) != nil,
              let entryCount = reader.readInt32(), entryCount >= 0,
              Int64(entryCount) <= Int64(data.count / 13) else { return nil }
        var entries: [(path: String, offset: Int, length: Int)] = []
        entries.reserveCapacity(min(Int(entryCount), 1024))
        for _ in 0..<entryCount {
            guard let pathLength = reader.readInt32(), pathLength > 0, pathLength <= 4096,
                  let pathData = reader.readData(count: Int(pathLength)),
                  let offset = reader.readInt32(), offset >= 0,
                  let length = reader.readInt32(), length >= 0 else { return nil }
            let path = String(data: pathData, encoding: .utf8) ?? ""
            entries.append((path, Int(offset), Int(length)))
        }
        let headerEnd = reader.offset
        guard let entry = entries.first(where: { normalizedPackagePath($0.path) == "scene.json" }),
              entry.length <= 64 * 1024 * 1024,
              entry.offset <= data.count - headerEnd,
              entry.length <= data.count - headerEnd - entry.offset else { return nil }
        return data.subdata(in: (headerEnd + entry.offset)..<(headerEnd + entry.offset + entry.length))
    }

    private static func explicitDimensions(in object: [String: Any]) -> Dimensions? {
        let containers: [[String: Any]] = [
            object,
            object["general"] as? [String: Any] ?? [:],
            object["resolution"] as? [String: Any] ?? [:]
        ]
        for container in containers {
            if let dimensions = dimensions(width: container["width"], height: container["height"]) {
                return dimensions
            }
            if let dimensions = dimensions(from: container["resolution"]) {
                return dimensions
            }
        }
        return nil
    }

    private static func dimensions(from value: Any?) -> Dimensions? {
        if let object = value as? [String: Any] {
            return dimensions(width: object["width"], height: object["height"])
        }
        if let array = value as? [Any] { return dimensions(size: array) }
        if let string = value as? String {
            let patterns = [
                "([0-9]{2,6})\\s*[x×]\\s*([0-9]{2,6})",
                "^\\s*([0-9]{2,6}(?:\\.[0-9]+)?)\\s*[,; ]\\s*([0-9]{2,6}(?:\\.[0-9]+)?)\\s*$",
                "(?i)\\bwidth\\s*[=:]\\s*([0-9]{2,6}(?:\\.[0-9]+)?)[^0-9]+\\bheight\\s*[=:]\\s*([0-9]{2,6}(?:\\.[0-9]+)?)"
            ]
            for pattern in patterns {
                guard let regex = try? NSRegularExpression(pattern: pattern),
                      let match = regex.firstMatch(in: string, range: NSRange(string.startIndex..., in: string)),
                      let wRange = Range(match.range(at: 1), in: string),
                      let hRange = Range(match.range(at: 2), in: string),
                      let width = Double(string[wRange]), let height = Double(string[hRange]),
                      width > 0, height > 0 else { continue }
                return Dimensions(width: Int(width.rounded()), height: Int(height.rounded()))
            }
        }
        return nil
    }

    private static func dimensions(width: Any?, height: Any?) -> Dimensions? {
        let w = numericDimension(width)
        let h = numericDimension(height)
        guard let w, let h, w > 0, h > 0 else { return nil }
        return Dimensions(width: w, height: h)
    }

    private static func numericDimension(_ value: Any?) -> Int? {
        if let number = value as? NSNumber { return Int(number.doubleValue.rounded()) }
        if let integer = value as? Int { return integer }
        if let string = value as? String, let number = Double(string.trimmingCharacters(in: .whitespacesAndNewlines)) {
            return Int(number.rounded())
        }
        return nil
    }

    private static func dimensions(size: [Any]) -> Dimensions? {
        guard size.count >= 2 else { return nil }
        return dimensions(width: size[0], height: size[1])
    }

    private static func htmlDimensions(_ html: String) -> Dimensions? {
        let tagPattern = "(?is)<meta\\b[^>]*>"
        guard let tagRegex = try? NSRegularExpression(pattern: tagPattern) else { return nil }
        let range = NSRange(html.startIndex..., in: html)
        for match in tagRegex.matches(in: html, range: range) {
            guard let tagRange = Range(match.range, in: html) else { continue }
            let tag = String(html[tagRange])
            let lower = tag.lowercased()
            if lower.contains("resolution") {
                if let dimensions = dimensions(from: attributeValue("content", in: tag)) { return dimensions }
            }
            if lower.contains("viewport"),
               let content = attributeValue("content", in: tag) {
                if let dimensions = dimensions(from: content) { return dimensions }
            }
        }
        let canvasPattern = "(?is)<canvas\\b[^>]*\\bwidth\\s*=\\s*['\"]([0-9]{2,6})['\"][^>]*\\bheight\\s*=\\s*['\"]([0-9]{2,6})['\"][^>]*>"
        guard let canvasRegex = try? NSRegularExpression(pattern: canvasPattern),
              let match = canvasRegex.firstMatch(in: html, range: range),
              let wRange = Range(match.range(at: 1), in: html),
              let hRange = Range(match.range(at: 2), in: html),
              let width = Int(html[wRange]), let height = Int(html[hRange]) else { return nil }
        return Dimensions(width: width, height: height)
    }

    private static func attributeValue(_ name: String, in tag: String) -> String? {
        let pattern = "(?i)\\b" + NSRegularExpression.escapedPattern(for: name) + "\\s*=\\s*['\"]([^'\"]*)['\"]"
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: tag, range: NSRange(tag.startIndex..., in: tag)),
              let valueRange = Range(match.range(at: 1), in: tag) else { return nil }
        return String(tag[valueRange])
    }

    private static func boolValue(_ value: Any?) -> Bool {
        if let bool = value as? Bool { return bool }
        if let number = value as? NSNumber { return number.boolValue }
        if let string = value as? String { return (string as NSString).boolValue }
        return false
    }

    private static func normalizedPackagePath(_ path: String) -> String {
        path.split(separator: "/").filter { $0 != "." }.reduce(into: "") { result, component in
            if component == ".." {
                if let slash = result.lastIndex(of: "/") { result.removeSubrange(slash...) }
            } else {
                result += result.isEmpty ? "" : "/"
                result += component.lowercased()
            }
        }
    }

    private static func normalized(_ tag: String) -> String {
        tag.lowercased().filter { $0.isLetter || $0.isNumber }
    }

    private static func isSelected(
        _ kind: Kind,
        widescreen: FRWidescreenResolution,
        ultraWidescreen: FRUltraWidescreenResolution,
        dualscreen: FRDualscreenResolution,
        portrait: FRPortraitScreenResolution,
        triplescreen: FRTriplescreenResolution,
        misc: FRMiscResolution
    ) -> Bool {
        switch kind {
        case .widescreen(let value): return widescreen.contains(value)
        case .ultraWidescreen(let value): return ultraWidescreen.contains(value)
        case .dualscreen(let value): return dualscreen.contains(value)
        case .triplescreen(let value): return triplescreen.contains(value)
        case .portrait(let value): return portrait.contains(value)
        case .misc(let value): return misc.contains(value)
        }
    }

    private static func allResolutionsSelected(
        widescreen: FRWidescreenResolution,
        ultraWidescreen: FRUltraWidescreenResolution,
        dualscreen: FRDualscreenResolution,
        triplescreen: FRTriplescreenResolution,
        portrait: FRPortraitScreenResolution,
        misc: FRMiscResolution
    ) -> Bool {
        widescreen == .all &&
            ultraWidescreen == .all &&
            dualscreen == .all &&
            triplescreen == .all &&
            portrait == .all &&
            misc == .all
    }

    private struct BinaryReader {
        let bytes: [UInt8]
        var offset = 0

        init(data: Data) { bytes = Array(data) }

        mutating func readInt32() -> Int32? {
            guard offset <= bytes.count - 4 else { return nil }
            let value = UInt32(bytes[offset])
                | (UInt32(bytes[offset + 1]) << 8)
                | (UInt32(bytes[offset + 2]) << 16)
                | (UInt32(bytes[offset + 3]) << 24)
            offset += 4
            return Int32(bitPattern: value)
        }

        mutating func readData(count: Int) -> Data? {
            guard count >= 0, count <= bytes.count - offset else { return nil }
            defer { offset += count }
            return Data(bytes[offset..<(offset + count)])
        }
    }
}

struct FRSource: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "官方",
        "创意工坊",
        "我的壁纸"
    ]
    
    static let official        = Self.init(rawValue: 1 << 0)
    static let workshop        = Self.init(rawValue: 1 << 1)
    static let myWallpapers    = Self.init(rawValue: 1 << 2)
    
    static let all: Self       = [.official, .workshop, .myWallpapers]
    static let none: Self      = []
}

struct FRTag: FilterResultsModel {
    let rawValue: Int
    
    static let allOptions = [
        "抽象",
        "动物",
        "动漫",
        "卡通",
        "CGI",
        "赛博朋克",
        "奇幻",
        "游戏",
        "女孩",
        "男孩",
        "风景",
        "中世纪",
        "表情包",
        "MMD",
        "音乐",
        "自然",
        "像素艺术",
        "治愈",
        "复古",
        "科幻",
        "运动",
        "科技",
        "影视",
        "载具",
        "未分类"
    ]
    
    static let abstract             = FRTag(rawValue: 1 << 0)
    static let animal               = FRTag(rawValue: 1 << 1)
    static let anime                = FRTag(rawValue: 1 << 2)
    static let cartoon              = FRTag(rawValue: 1 << 3)
    static let cgi                  = FRTag(rawValue: 1 << 4)
    static let cyberpunk            = FRTag(rawValue: 1 << 5)
    static let fantasy              = FRTag(rawValue: 1 << 6)
    static let game                 = FRTag(rawValue: 1 << 7)
    static let girls                = FRTag(rawValue: 1 << 8)
    static let guys                 = FRTag(rawValue: 1 << 9)
    static let landscape            = FRTag(rawValue: 1 << 10)
    static let medieval             = FRTag(rawValue: 1 << 11)
    static let memes                = FRTag(rawValue: 1 << 12)
    static let mmd                  = FRTag(rawValue: 1 << 13)
    static let music                = FRTag(rawValue: 1 << 14)
    static let nature               = FRTag(rawValue: 1 << 15)
    static let pixelArt             = FRTag(rawValue: 1 << 16)
    static let relaxing             = FRTag(rawValue: 1 << 17)
    static let retro                = FRTag(rawValue: 1 << 18)
    static let sciFi                = FRTag(rawValue: 1 << 19)
    static let sports               = FRTag(rawValue: 1 << 20)
    static let technology           = FRTag(rawValue: 1 << 21)
    static let television           = FRTag(rawValue: 1 << 22)
    static let vehicle              = FRTag(rawValue: 1 << 23)
    static let unspecifiedGenre     = FRTag(rawValue: 1 << 24)
    
    static let all: FRTag = [
        .abstract, .animal, .anime, .cartoon, .cgi, .cyberpunk, .fantasy, .game, .girls,
        .guys, .landscape, .medieval, .memes, .mmd, .music, .nature, .pixelArt, .relaxing,
        .retro, .sciFi, .sports, .technology, .television, .vehicle, .unspecifiedGenre
    ]
    static let none: FRTag = []

    static func bit(for tag: String) -> FRTag {
        switch tag.lowercased().replacingOccurrences(of: " ", with: "") {
        case "abstract": return .abstract
        case "animal": return .animal
        case "anime": return .anime
        case "cartoon": return .cartoon
        case "cgi": return .cgi
        case "cyberpunk": return .cyberpunk
        case "fantasy": return .fantasy
        case "game": return .game
        case "girls": return .girls
        case "guys": return .guys
        case "landscape": return .landscape
        case "medieval": return .medieval
        case "memes": return .memes
        case "mmd": return .mmd
        case "music": return .music
        case "nature": return .nature
        case "pixelart": return .pixelArt
        case "relaxing": return .relaxing
        case "retro": return .retro
        case "sci-fi", "scifi": return .sciFi
        case "sports": return .sports
        case "technology": return .technology
        case "television": return .television
        case "vehicle": return .vehicle
        default: return .unspecifiedGenre
        }
    }

    static func bits(from tags: [String]) -> FRTag {
        var result = FRTag.none
        for t in tags { result.insert(bit(for: t)) }
        return result
    }
}
