//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

// A Rainmeter (.rmskin) theme installed under the Mirage Rmskins directory.
// Parsed from the theme's RMSKIN.ini [rmskin] section.
struct RmskinTheme: Identifiable, Hashable {
    var themeDirectory: URL          // ~/…/Mirage/Rmskins/<Name>
    var name: String                 // [rmskin] Name
    var author: String               // Author
    var version: String              // Version (filter facet)
    var loadType: String             // LoadType — Layout / Skin (filter facet)
    var load: String                 // Load — layout name or skin path
    var minimumRainmeter: String?
    var minimumWindows: String?
    var previewURL: URL?             // representative screenshot, if any

    var id: String { themeDirectory.path(percentEncoded: false) }

    var skinsDirectory: URL { themeDirectory.appending(path: "Skins") }

    // Parse a theme directory that contains RMSKIN.ini. Returns nil if absent.
    static func load(from directory: URL) -> RmskinTheme? {
        let iniURL = directory.appending(path: "RMSKIN.ini")
        guard let values = parseRmskinIni(at: iniURL) else { return nil }
        let name = values["name"] ?? directory.lastPathComponent
        return RmskinTheme(
            themeDirectory: directory,
            name: name,
            author: values["author"] ?? "",
            version: values["version"] ?? "",
            loadType: values["loadtype"] ?? "",
            load: values["load"] ?? "",
            minimumRainmeter: values["minimumrainmeter"],
            minimumWindows: values["minimumwindows"],
            previewURL: findPreview(in: directory)
        )
    }

    // Minimal case-insensitive parse of the [rmskin] section (keys lowercased).
    private static func parseRmskinIni(at url: URL) -> [String: String]? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        let text = decodeIni(data)
        var values: [String: String] = [:]
        var inSection = false
        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix(";") { continue }
            if line.hasPrefix("[") {
                inSection = line.lowercased().hasPrefix("[rmskin]")
                continue
            }
            guard inSection, let eq = line.firstIndex(of: "=") else { continue }
            let key = line[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            if !key.isEmpty { values[key] = value }
        }
        return values.isEmpty ? nil : values
    }

    private static func decodeIni(_ data: Data) -> String {
        // RMSKIN.ini is normally UTF-8/ANSI; tolerate UTF-16 BOM just in case.
        if data.count >= 2, data[0] == 0xFF, data[1] == 0xFE {
            return String(data: data, encoding: .utf16LittleEndian) ?? ""
        }
        if let s = String(data: data, encoding: .utf8) { return s }
        return String(data: data, encoding: .isoLatin1) ?? ""
    }

    private static func findPreview(in directory: URL) -> URL? {
        let fm = FileManager.default
        // 1) A screenshot at the theme root.
        if let items = try? fm.contentsOfDirectory(at: directory,
                                                   includingPropertiesForKeys: nil) {
            for url in items where ["png", "jpg", "jpeg"].contains(url.pathExtension.lowercased()) {
                return url
            }
        }
        // 2) First image found under Skins/<root>/@Resources.
        let skins = directory.appending(path: "Skins")
        if let enumerator = fm.enumerator(at: skins, includingPropertiesForKeys: nil) {
            for case let url as URL in enumerator
            where ["png", "jpg", "jpeg"].contains(url.pathExtension.lowercased()) {
                return url
            }
        }
        return nil
    }
}
