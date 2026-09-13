//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import CryptoKit
import Foundation

enum MirageLockBridge {
    #if DEBUG
    static let groupID = "group.cn.laobamac.Mirage.Development"
    private static let notificationPrefix = "cn.laobamac.Mirage.Development.dynamicLockScreen"
    #else
    static let groupID = "group.cn.laobamac.Mirage"
    private static let notificationPrefix = "cn.laobamac.Mirage.dynamicLockScreen"
    #endif
    static let configurationName = "dynamic-lock-screen.json"
    static let probeNotification = notificationPrefix + ".probe"
    static let statusNotification = notificationPrefix + ".status"
    static let configurationNotification = notificationPrefix + ".configurationChanged"
    static let previewNotification = notificationPrefix + ".previewChanged"
    static let desktopFallbackNotification = notificationPrefix + ".desktopFallbackChanged"
    static let lockedNotification = notificationPrefix + ".locked"
    static let unlockedNotification = notificationPrefix + ".unlocked"
    static let wakeNotification = notificationPrefix + ".wake"
    static let sleepNotification = notificationPrefix + ".sleep"

    static func containerURL() throws -> URL {
        guard let url = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID) else {
            throw failure("Shared container is unavailable")
        }
        return url
    }

    static func failure(_ message: String) -> NSError {
        NSError(domain: "MirageWallpaperExtension", code: 10,
                userInfo: [NSLocalizedDescriptionKey: message])
    }

    static func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    static func normalizedPath(_ url: URL) -> String {
        url.standardizedFileURL.resolvingSymlinksInPath().path
    }

    static func fingerprint(at extensionURL: URL) throws -> String {
        let fm = FileManager.default
        var directory: ObjCBool = false
        guard fm.fileExists(atPath: extensionURL.appendingPathComponent("Contents/Resources/assets").path,
                            isDirectory: &directory), directory.boolValue else {
            throw failure("Wallpaper extension scene resources are missing")
        }
        let paths = [
            "Contents/Info.plist",
            "Contents/MacOS/MirageWallpaperExtension",
            "Contents/Frameworks/libMirageSceneSaver.dylib",
            "Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
        ]
        var hasher = SHA256()
        for path in paths {
            hasher.update(data: Data(path.utf8))
            hasher.update(data: try Data(contentsOf: extensionURL.appendingPathComponent(path), options: .mappedIfSafe))
        }
        let signature = extensionURL.appendingPathComponent("Contents/_CodeSignature/CodeResources")
        if fm.fileExists(atPath: signature.path) {
            hasher.update(data: try Data(contentsOf: signature, options: .mappedIfSafe))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func runtimeDirectory(in container: URL) -> URL {
        container.appendingPathComponent("DynamicLockScreen/Runtime", isDirectory: true)
    }

    static func readProbe(in container: URL) throws -> MirageLockProbe {
        try JSONDecoder().decode(MirageLockProbe.self, from: Data(contentsOf:
            runtimeDirectory(in: container).appendingPathComponent("probe.json")))
    }

    static func writeProbe(_ probe: MirageLockProbe, in container: URL) throws {
        let directory = runtimeDirectory(in: container)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try JSONEncoder().encode(probe).write(to: directory.appendingPathComponent("probe.json"), options: .atomic)
        let files = (try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        for file in files where file.lastPathComponent.hasPrefix("status-") {
            if let date = try? file.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate,
               date < Date().addingTimeInterval(-86_400) {
                try? FileManager.default.removeItem(at: file)
            }
        }
    }

    static func readReports(in container: URL) -> [MirageLockReport] {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: runtimeDirectory(in: container), includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.lastPathComponent.hasPrefix("status-") }.compactMap {
            guard let data = try? Data(contentsOf: $0) else { return nil }
            return try? JSONDecoder().decode(MirageLockReport.self, from: data)
        }
    }

    static func post(_ name: String) {
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
            CFNotificationName(name as CFString), nil, nil, true)
    }
}

struct MirageLockProbe: Codable, Equatable {
    let id: UUID
    let createdAt: Date
    let configurationDigest: String

    init(configurationData: Data) {
        id = UUID()
        createdAt = Date()
        configurationDigest = MirageLockBridge.digest(configurationData)
    }
}

struct MirageLockReport: Codable {
    let probeID: UUID
    let instanceID: UUID
    let processID: Int32
    let extensionPath: String
    let fingerprint: String
    let version: String
    let configurationDigest: String?
    let settingsReady: Bool
    let readyDisplayIDs: [UInt32]
    let error: String?

    func matches(_ probe: MirageLockProbe, extensionURL: URL, fingerprint expected: String) -> Bool {
        probeID == probe.id && fingerprint == expected
            && extensionPath == MirageLockBridge.normalizedPath(extensionURL)
            && configurationDigest == probe.configurationDigest
    }

    func write(in container: URL) throws {
        let url = MirageLockBridge.runtimeDirectory(in: container)
            .appendingPathComponent("status-\(instanceID.uuidString).json")
        try JSONEncoder().encode(self).write(to: url, options: .atomic)
        MirageLockBridge.post(MirageLockBridge.statusNotification)
    }
}
