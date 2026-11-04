//
//  DesktopWallpaperStore.swift
//  Mirage Wallpaper
//
//  Created by 王孝慈 on 2026/11/5.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit

struct DesktopWallpaperOptions: Codable {
    var scaling: Int?
    var allowClipping: Bool?
    var fillColor: Data?

    init(_ options: [NSWorkspace.DesktopImageOptionKey: Any]) {
        scaling = (options[.imageScaling] as? NSNumber)?.intValue
        allowClipping = (options[.allowClipping] as? NSNumber)?.boolValue
        if let color = options[.fillColor] as? NSColor {
            fillColor = try? NSKeyedArchiver.archivedData(
                withRootObject: color, requiringSecureCoding: true)
        }
    }

    var workspaceOptions: [NSWorkspace.DesktopImageOptionKey: Any] {
        var result: [NSWorkspace.DesktopImageOptionKey: Any] = [:]
        if let scaling { result[.imageScaling] = scaling }
        if let allowClipping { result[.allowClipping] = allowClipping }
        if let fillColor,
           let color = try? NSKeyedUnarchiver.unarchivedObject(ofClass: NSColor.self, from: fillColor) {
            result[.fillColor] = color
        }
        return result
    }
}

struct DesktopWallpaperBackup: Codable {
    let url: URL
    let options: DesktopWallpaperOptions
    let encodedOptions: Data?
}

struct DesktopWallpaperStore {
    struct Picture: Equatable {
        let url: URL
        let encodedOptions: Data?

        static func == (lhs: Self, rhs: Self) -> Bool {
            lhs.url == rhs.url && DesktopWallpaperStore.equalOptions(lhs.encodedOptions, rhs.encodedOptions)
        }

        var options: DesktopWallpaperOptions? {
            guard let encodedOptions,
                  let object = try? PropertyListSerialization.propertyList(
                    from: encodedOptions, format: nil) as? [String: Any],
                  let values = object["values"] as? [String: Any],
                  let placement = values["placement"] as? [String: Any],
                  let picker = placement["picker"] as? [String: Any],
                  let selected = picker["_0"] as? [String: Any],
                  selected["id"] as? String == "Crop",
                  let colorValue = values["color"] as? [String: Any],
                  let colorCase = colorValue["color"] as? [String: Any],
                  let colorPayload = colorCase["_0"] as? [String: Any],
                  let color = colorPayload["color"] as? [String: Any],
                  let components = color["components"] as? [CGFloat],
                  let spaceData = color["colorSpace"] as? Data,
                  let spaceName = try? PropertyListSerialization.propertyList(
                    from: spaceData, format: nil) as? String,
                  let space = CGColorSpace(name: spaceName as CFString),
                  components.count == space.numberOfComponents + 1,
                  let cgColor = CGColor(colorSpace: space, components: components),
                  let nsColor = NSColor(cgColor: cgColor) else { return nil }
            return DesktopWallpaperOptions([
                .imageScaling: NSImageScaling.scaleProportionallyUpOrDown.rawValue,
                .allowClipping: true,
                .fillColor: nsColor
            ])
        }
    }

    static var location: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(
            "Library/Application Support/com.apple.wallpaper/Store/Index.plist")
    }

    private let root: [String: Any]
    let referencedURLs: Set<URL>?

    init?(data: Data) {
        guard let root = try? PropertyListSerialization.propertyList(
            from: data, format: nil) as? [String: Any],
              ["Displays", "Spaces", "SystemDefault", "AllSpacesAndDisplays"].contains(where: {
                  root[$0] is [String: Any]
              }), ["Displays", "Spaces", "SystemDefault", "AllSpacesAndDisplays"].allSatisfy({
                  root[$0] == nil || root[$0] is [String: Any] || root[$0] as? String == "$null"
              }) else { return nil }
        self.root = root
        var references: Set<URL> = []
        var complete = true
        Self.collectReferences(root, into: &references, complete: &complete)
        referencedURLs = complete ? references : nil
    }

    static func read() -> Self? {
        guard let data = try? Data(contentsOf: location) else { return nil }
        return Self(data: data)
    }

    func picture(for displayUUID: String) -> Picture? {
        guard let desktops = desktops(for: displayUUID), !desktops.isEmpty else { return nil }
        let pictures = desktops.compactMap(Self.picture)
        guard pictures.count == desktops.count, let first = pictures.first,
              pictures.allSatisfy({ $0 == first }) else { return nil }
        return first
    }

    func matches(_ url: URL, for displayUUID: String) -> Bool {
        guard let desktops = desktops(for: displayUUID), !desktops.isEmpty else { return false }
        return desktops.allSatisfy {
            Self.picture($0)?.url == url.resolvingSymlinksInPath()
        }
    }

    func matches(_ backup: DesktopWallpaperBackup, for displayUUID: String) -> Bool {
        guard matches(backup.url, for: displayUUID),
              let desktops = desktops(for: displayUUID) else { return false }
        return desktops.allSatisfy {
            Self.equalOptions(Self.picture($0)?.encodedOptions, backup.encodedOptions)
        }
    }

    private static func equalOptions(_ lhs: Data?, _ rhs: Data?) -> Bool {
        if lhs == rhs { return true }
        guard let lhs, let rhs,
              let left = try? PropertyListSerialization.propertyList(from: lhs, format: nil) as? NSDictionary,
              let right = try? PropertyListSerialization.propertyList(from: rhs, format: nil) as? NSDictionary
        else { return false }
        return left.isEqual(right)
    }

    func hasSameDesktop(as other: Self, for displayUUID: String) -> Bool {
        guard let lhs = desktops(for: displayUUID),
              let rhs = other.desktops(for: displayUUID) else { return false }
        return NSArray(array: lhs).isEqual(to: rhs)
    }

    private func desktops(for displayUUID: String) -> [[String: Any]]? {
        var slots: [[String: Any]] = []
        if let shared = root["AllSpacesAndDisplays"] as? [String: Any] {
            slots.append(shared)
        } else if let shared = root["AllSpacesAndDisplays"] as? String, shared != "$null" {
            return nil
        }
        if let displays = root["Displays"] as? [String: Any],
           let display = displays[displayUUID] as? [String: Any] {
            slots.append(display)
        }
        if let spaces = root["Spaces"] as? [String: Any] {
            for key in spaces.keys.sorted() {
                guard let space = spaces[key] as? [String: Any] else { return nil }
                if let displays = space["Displays"] as? [String: Any],
                   let display = displays[displayUUID] as? [String: Any] {
                    slots.append(display)
                } else if let fallback = space["Default"] as? [String: Any] {
                    slots.append(fallback)
                }
            }
        }
        if slots.isEmpty, let fallback = root["SystemDefault"] as? [String: Any] {
            slots.append(fallback)
        }
        let desktops = slots.compactMap { $0["Desktop"] as? [String: Any] }
        guard desktops.count == slots.count, !desktops.isEmpty else { return nil }
        return desktops
    }

    private static func picture(_ desktop: [String: Any]) -> Picture? {
        guard let content = desktop["Content"] as? [String: Any],
              content["Shuffle"] == nil || content["Shuffle"] as? String == "$null",
              let choices = content["Choices"] as? [[String: Any]], choices.count == 1,
              let choice = choices.first,
              choice["Provider"] as? String == "com.apple.wallpaper.choice.image",
              let data = choice["Configuration"] as? Data,
              let configuration = try? PropertyListSerialization.propertyList(
                from: data, format: nil) as? [String: Any],
              configuration["type"] as? String == "imageFile",
              let location = configuration["url"] as? [String: Any],
              let relative = location["relative"] as? String,
              let url = fileURL(relative) else { return nil }
        return Picture(url: url, encodedOptions: content["EncodedOptionValues"] as? Data)
    }

    private static func fileURL(_ value: String) -> URL? {
        if value.hasPrefix("/") { return URL(fileURLWithPath: value).resolvingSymlinksInPath() }
        guard let url = URL(string: value), url.isFileURL, url.path.hasPrefix("/") else { return nil }
        return url.resolvingSymlinksInPath()
    }

    private static func collectReferences(_ value: Any, into urls: inout Set<URL>,
                                          complete: inout Bool) {
        if let dictionary = value as? [String: Any] {
            if dictionary["Provider"] as? String == "com.apple.wallpaper.choice.image" {
                guard let data = dictionary["Configuration"] as? Data,
                      let configuration = try? PropertyListSerialization.propertyList(
                        from: data, format: nil) as? [String: Any],
                      configuration["type"] as? String == "imageFile",
                      let location = configuration["url"] as? [String: Any],
                      let relative = location["relative"] as? String,
                      fileURL(relative) != nil
                else { complete = false; return }
            }
            for (key, nested) in dictionary where key != "EncodedOptionValues" {
                collectReferences(nested, into: &urls, complete: &complete)
            }
        } else if let array = value as? [Any] {
            for nested in array { collectReferences(nested, into: &urls, complete: &complete) }
        } else if let data = value as? Data {
            if data.isEmpty { return }
            if let nested = try? PropertyListSerialization.propertyList(from: data, format: nil) {
                collectReferences(nested, into: &urls, complete: &complete)
            } else if let nested = try? JSONSerialization.jsonObject(with: data) {
                collectReferences(nested, into: &urls, complete: &complete)
            } else if let string = String(data: data, encoding: .utf8),
                      let url = fileURL(string) {
                urls.insert(url)
            } else if let string = String(data: data, encoding: .utf8),
                      string.hasPrefix("display-"),
                      UInt32(string.dropFirst("display-".count)) != nil {
                return
            } else {
                complete = false
            }
        } else if let string = value as? String, let url = fileURL(string) {
            urls.insert(url)
        }
    }
}
