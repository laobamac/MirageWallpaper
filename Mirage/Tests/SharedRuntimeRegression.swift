import AppKit
import Foundation

private let copyrightNotice = "Copyright © 2026 王孝慈. All rights reserved."

@main
struct SharedRuntimeRegression {
    static func main() throws {
        let fm = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        let identifier = "cn.laobamac.Mirage"
        func fixture(_ name: String, id: String = "cn.laobamac.Mirage") throws -> URL {
            let app = root.appendingPathComponent(name, isDirectory: true)
            let contents = app.appendingPathComponent("Contents", isDirectory: true)
            for path in ["Frameworks", "Resources/assets", "Resources/Renderers/vulkan/icd.d"] {
                try fm.createDirectory(at: contents.appendingPathComponent(path), withIntermediateDirectories: true)
            }
            let info: [String: Any] = ["CFBundleIdentifier": id, "CFBundlePackageType": "APPL", "CFBundleVersion": "1", "MirageSceneRuntimeABI": 1]
            try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0).write(to: contents.appendingPathComponent("Info.plist"))
            for path in ["Frameworks/libMirageSceneSaver.dylib", "Resources/Renderers/vulkan/icd.d/MoltenVK_icd.json"] {
                try Data("fixture".utf8).write(to: contents.appendingPathComponent(path))
            }
            return app
        }
        let app = try fixture("Location with spaces/Mirage.app")
        let other = try fixture("Other.app", id: "example.unrelated")
        let nested = app.appendingPathComponent("Contents/Resources/Screen Savers/Mirage.saver")
        try fm.createDirectory(at: nested.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        let info: [String: Any] = ["CFBundleIdentifier": "cn.laobamac.Mirage.ScreenSaver", "CFBundlePackageType": "BNDL", "MirageHostBundleIdentifier": identifier]
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0).write(to: nested.appendingPathComponent("Contents/Info.plist"))
        let standalone = root.appendingPathComponent("Library/Screen Savers/Mirage.saver")
        try fm.createDirectory(at: standalone.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.copyItem(at: nested, to: standalone)
        let nestedBundle = Bundle(url: nested)!
        let installedBundle = Bundle(url: standalone)!
        let saved = root.appendingPathComponent("host.json")
        func locate(_ component: Bundle, path: String? = nil, lookup: (String) -> URL? = { _ in nil }) -> MirageHostApplication? {
            MirageHostApplication.locate(configuredPath: path, component: component, savedURL: saved, applicationLookup: lookup)
        }
        let nestedHost = locate(nestedBundle)
        precondition(nestedHost?.contentsURL.standardizedFileURL.path == app.appendingPathComponent("Contents").standardizedFileURL.path)
        precondition(locate(installedBundle) == nil)
        precondition(locate(installedBundle, path: other.path) == nil)
        precondition(locate(installedBundle, path: app.path) != nil)
        precondition(locate(installedBundle, path: "/nonexistent/Mirage.app", lookup: { $0 == identifier ? app : nil }) != nil)
        try JSONSerialization.data(withJSONObject: ["path": app.path, "bundleIdentifier": identifier]).write(to: saved)
        precondition(locate(installedBundle) != nil)
        try fm.removeItem(at: app.appendingPathComponent("Contents/Frameworks/libMirageSceneSaver.dylib"))
        precondition(locate(installedBundle) == nil)
        print("PASS: nested and standalone saver lookup, moved app, spaces, wrong identity, missing shared runtime")
    }
}
