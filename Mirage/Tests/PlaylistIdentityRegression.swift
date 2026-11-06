import AppKit
import Foundation
@testable import Mirage_Wallpaper

@main
@MainActor
struct PlaylistIdentityRegression {
    struct File: Codable { var currents: [String: Playlist]; var saved: [Playlist] }
    static func require(_ value: @autoclosure () throws -> Bool, _ message: String) rethrows { let passed = try value(); precondition(passed, message) }
    static func main() async throws {
        let root = FileManager.default.temporaryDirectory.appending(path: UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appending(path: "playlists.json")
        let a = DisplayKey(rawValue: "uuid:test-a"), b = DisplayKey(rawValue: "uuid:test-b")
        var displays = [0: a, 1: b]
        let oldA = Playlist(name: "old-a", items: [.init(wallpaperID: "first", addedAt: Date())])
        let oldB = Playlist(name: "old-b", items: [.init(wallpaperID: "second", addedAt: Date())])
        let original = try JSONEncoder().encode(File(currents: ["0": oldA, "1": oldB], saved: [oldB]))
        try original.write(to: url)
        let manager = PlaylistManager(storageURL: url, displayKeys: { displays })
        require(manager.current(on: 0).items.isEmpty, "guessed old display ownership")
        require(manager.legacyCurrents.count == 2, "lost legacy lists")
        try require(try Data(contentsOf: url.appendingPathExtension("legacy-backup")) == original, "backup differs")
        manager.bindLegacy("0", to: 1)
        require(manager.current(on: 1).id == oldA.id && manager.legacyCurrents["1"]?.id == oldB.id, "explicit migration failed")
        manager.load(saved: oldB, into: 0)
        displays = [0: b, 1: a]
        manager.synchronizeDisplays()
        require(manager.current(on: 0).id == oldA.id && manager.current(on: 1).id == oldB.id, "order change swapped lists")
        displays = [0: a]
        manager.synchronizeDisplays()
        require(manager.current(on: 0).id == oldB.id, "unplug moved an absent list to main")
        manager.updateSettings(on: 0) { $0.timerMinutes = 2 }
        displays = [0: b, 1: a]
        manager.synchronizeDisplays()
        require(manager.current(on: 0).id == oldA.id && manager.current(on: 1).settings.timerMinutes == 2, "reconnect lost identity or edits")
        try await Task.sleep(for: .milliseconds(700))
        let restarted = PlaylistManager(storageURL: url, displayKeys: { displays })
        require(restarted.current(on: 0).id == oldA.id && restarted.current(on: 1).id == oldB.id, "restart lost identity")
        require(restarted.legacyCurrents.count == 1 && restarted.saved.count == 1, "save lost unassigned or saved lists")
        let file = try JSONDecoder().decode(File.self, from: Data(contentsOf: url))
        require(Set(file.currents.keys) == Set([a.rawValue, b.rawValue]), "persisted positional keys")
        try require(try Data(contentsOf: url.appendingPathExtension("legacy-backup")) == original, "migration modified backup")
        displays = [0: DisplayKey(rawValue: "idx:0")]
        restarted.synchronizeDisplays()
        restarted.load(saved: oldA, into: 0)
        require(restarted.unidentifiedScreens.contains(0) && restarted.current(on: 0).items.isEmpty,
                "treated a fallback screen index as stable identity")
        let badURL = root.appending(path: "broken.json")
        let bad = Data("broken".utf8)
        try bad.write(to: badURL)
        let broken = PlaylistManager(storageURL: badURL, displayKeys: { displays })
        broken.load(saved: oldA, into: 0)
        require(broken.storageError != nil, "corrupt file failure hidden")
        try await Task.sleep(for: .milliseconds(500))
        try require(try Data(contentsOf: badURL) == bad, "overwrote unreadable configuration")
        print("PASS: explicit legacy migration and backup, display reorder, unplug/reconnect, restart and unreadable-file preservation")
    }
}
