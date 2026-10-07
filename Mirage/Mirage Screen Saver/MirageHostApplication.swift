import AppKit
import Foundation

private let runtimeCopyright = "Copyright © 2026 王孝慈. All rights reserved."

struct MirageHostApplication {
    let contentsURL: URL

    var sceneLibraryURL: URL { contentsURL.appendingPathComponent("Frameworks/libMirageSceneSaver.dylib") }
    var assetsURL: URL { contentsURL.appendingPathComponent("Resources/assets", isDirectory: true) }
    var vulkanICDURL: URL { contentsURL.appendingPathComponent("Resources/Renderers/vulkan/icd.d/MoltenVK_icd.json") }

    static var savedHostURL: URL {
        let home = getpwuid(getuid()).flatMap { record in
            String(validatingUTF8: record.pointee.pw_dir)
        }.map { URL(fileURLWithPath: $0, isDirectory: true) } ?? FileManager.default.homeDirectoryForCurrentUser
        return home.appendingPathComponent("Library/Application Support/Mirage/screen-saver-host.json")
    }

    static func locate(configuredPath: String?, component: Bundle,
                       savedURL: URL = savedHostURL,
                       applicationLookup: (String) -> URL? = { NSWorkspace.shared.urlForApplication(withBundleIdentifier: $0) }) -> MirageHostApplication? {
        let identifier = component.object(forInfoDictionaryKey: "MirageHostBundleIdentifier") as? String ?? "cn.laobamac.Mirage"
        var candidates: [URL] = []
        var ancestor = component.bundleURL.deletingLastPathComponent()
        while ancestor.path != "/" {
            if ancestor.pathExtension == "app" { candidates.append(ancestor); break }
            ancestor.deleteLastPathComponent()
        }
        if let data = try? Data(contentsOf: savedURL),
           let record = try? JSONSerialization.jsonObject(with: data) as? [String: String],
           record["bundleIdentifier"] == identifier, let path = record["path"] {
            candidates.append(URL(fileURLWithPath: path, isDirectory: true))
        }
        if let configuredPath, !configuredPath.isEmpty {
            candidates.append(URL(fileURLWithPath: configuredPath, isDirectory: true))
        }
        if let url = applicationLookup(identifier) { candidates.append(url) }
        for candidate in candidates {
            guard candidate.pathExtension == "app", let bundle = Bundle(url: candidate),
                  bundle.bundleIdentifier == identifier,
                  bundle.object(forInfoDictionaryKey: "MirageSceneRuntimeABI") as? Int == 1 else { continue }
            let host = MirageHostApplication(contentsURL: candidate.appendingPathComponent("Contents", isDirectory: true))
            if FileManager.default.isReadableFile(atPath: host.sceneLibraryURL.path),
               FileManager.default.isReadableFile(atPath: host.assetsURL.path),
               FileManager.default.isReadableFile(atPath: host.vulkanICDURL.path) { return host }
        }
        return nil
    }
}
