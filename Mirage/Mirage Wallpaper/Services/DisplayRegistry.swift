//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CoreGraphics
import Foundation

struct DisplayKey: Codable, Hashable, Identifiable, RawRepresentable {
    let rawValue: String

    init(rawValue: String) {
        self.rawValue = rawValue
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        self.rawValue = try container.decode(String.self)
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }

    var id: String { rawValue }
}

struct DisplayInfo: Identifiable, Equatable {
    let key: DisplayKey
    let displayID: CGDirectDisplayID
    let index: Int
    let name: String
    let size: CGSize
    let isMain: Bool

    var id: DisplayKey { key }
}

final class DisplayRegistry {
    static let shared = DisplayRegistry()

    static let didChangeNotification = Notification.Name("MirageDisplayRegistryDidChange")

    private let lock = NSLock()
    private var cachedInfos: [DisplayInfo] = []
    private var cacheValid = false

    private init() {
        NotificationCenter.default.addObserver(
            self, selector: #selector(screenParametersChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func screenParametersChanged() {
        invalidate()
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }

    func invalidate() {
        lock.lock()
        cacheValid = false
        cachedInfos = []
        lock.unlock()
    }

    var connected: [DisplayInfo] {
        lock.lock()
        if cacheValid {
            let result = cachedInfos
            lock.unlock()
            return result
        }
        lock.unlock()

        let rebuilt = Self.buildInfos()

        lock.lock()
        cachedInfos = rebuilt
        cacheValid = true
        lock.unlock()
        return rebuilt
    }

    var connectedKeys: Set<DisplayKey> {
        Set(connected.map(\.key))
    }

    func info(for key: DisplayKey) -> DisplayInfo? {
        connected.first { $0.key == key }
    }

    func info(forDisplay displayID: CGDirectDisplayID) -> DisplayInfo? {
        connected.first { $0.displayID == displayID }
    }

    func key(forDisplay displayID: CGDirectDisplayID) -> DisplayKey? {
        if let existing = info(forDisplay: displayID)?.key { return existing }
        return Self.derivedKey(for: displayID)
    }

    func key(forScreenIndex index: Int) -> DisplayKey? {
        connected.first { $0.index == index }?.key
    }

    func displayID(for key: DisplayKey) -> CGDirectDisplayID? {
        info(for: key)?.displayID
    }

    func screenIndex(for key: DisplayKey) -> Int? {
        info(for: key)?.index
    }

    var mainKey: DisplayKey? {
        let main = CGMainDisplayID()
        if let matched = info(forDisplay: main)?.key { return matched }
        return connected.first?.key
    }

    func displayName(for key: DisplayKey) -> String {
        info(for: key)?.name ?? L("未连接的显示器")
    }

    private static func buildInfos() -> [DisplayInfo] {
        let main = CGMainDisplayID()
        var seen = Set<DisplayKey>()
        var result: [DisplayInfo] = []
        for (index, screen) in NSScreen.screens.enumerated() {
            guard let number = screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { continue }
            let displayID = number.uint32Value
            var key = derivedKey(for: displayID) ?? DisplayKey(rawValue: "idx:\(index)")
            if seen.contains(key) {
                key = DisplayKey(rawValue: "\(key.rawValue)#\(index)")
            }
            seen.insert(key)
            result.append(DisplayInfo(
                key: key,
                displayID: displayID,
                index: index,
                name: screen.localizedName,
                size: screen.frame.size,
                isMain: displayID == main))
        }
        return result
    }

    private static func derivedKey(for displayID: CGDirectDisplayID) -> DisplayKey? {
        guard displayID != 0 else { return nil }
        if let cfUUID = CGDisplayCreateUUIDFromDisplayID(displayID) {
            let uuid = cfUUID.takeRetainedValue()
            if let string = CFUUIDCreateString(nil, uuid) as String?, !string.isEmpty {
                return DisplayKey(rawValue: "uuid:\(string)")
            }
        }
        let vendor = CGDisplayVendorNumber(displayID)
        let model = CGDisplayModelNumber(displayID)
        let serial = CGDisplaySerialNumber(displayID)
        let unit = CGDisplayUnitNumber(displayID)
        guard vendor != 0 || model != 0 || serial != 0 else { return nil }
        return DisplayKey(rawValue: "vms:\(vendor):\(model):\(serial):\(unit)")
    }
}
