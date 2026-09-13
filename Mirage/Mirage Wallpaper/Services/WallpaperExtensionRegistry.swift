//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Darwin
import Foundation

struct WallpaperExtensionProcess {
    let processID: Int32
    let executablePath: String
    let startSeconds: UInt64
    let startMicroseconds: UInt64

    static func running(identifier: String) -> [Self] {
        let count = proc_listallpids(nil, 0)
        guard count > 0 else { return [] }
        var pids = [Int32](repeating: 0, count: Int(count) + 32)
        let found = pids.withUnsafeMutableBytes {
            proc_listallpids($0.baseAddress, Int32($0.count))
        }
        guard found > 0 else { return [] }
        return pids.prefix(min(Int(found), pids.count)).compactMap { pid in
            guard pid > 0, let info = processInfo(pid), let path = path(pid) else { return nil }
            let executable = URL(fileURLWithPath: path)
            guard executable.lastPathComponent == "MirageWallpaperExtension" else { return nil }
            let bundleURL = executable.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            guard bundleURL.pathExtension == "appex", Bundle(url: bundleURL)?.bundleIdentifier == identifier else { return nil }
            return Self(processID: pid, executablePath: path,
                        startSeconds: info.pbi_start_tvsec, startMicroseconds: info.pbi_start_tvusec)
        }
    }

    var isRunning: Bool {
        guard let info = Self.processInfo(processID), info.pbi_start_tvsec == startSeconds,
              info.pbi_start_tvusec == startMicroseconds else { return false }
        return Self.path(processID) == executablePath
    }

    @discardableResult
    func terminate(force: Bool = false) -> Bool {
        guard isRunning else { return false }
        return Darwin.kill(processID, force ? SIGKILL : SIGTERM) == 0
    }

    private static func processInfo(_ pid: Int32) -> proc_bsdinfo? {
        var info = proc_bsdinfo()
        let size = Int32(MemoryLayout<proc_bsdinfo>.size)
        guard proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, size) == size,
              info.pbi_uid == geteuid() else { return nil }
        return info
    }

    private static func path(_ pid: Int32) -> String? {
        var buffer = [CChar](repeating: 0, count: 4 * Int(MAXPATHLEN))
        guard proc_pidpath(pid, &buffer, UInt32(buffer.count)) > 0 else { return nil }
        return String(cString: buffer)
    }
}

struct WallpaperExtensionRecord: Equatable {
    let identifier: String
    let path: String
    let elected: Bool

    static func parse(_ output: String, identifier: String) -> [Self] {
        output.split(whereSeparator: \.isNewline).compactMap { line in
            let columns = line.split(separator: "\t", omittingEmptySubsequences: true)
            guard columns.count >= 4,
                  let path = columns.last?.trimmingCharacters(in: .whitespacesAndNewlines),
                  path.hasPrefix("/"), path.hasSuffix(".appex") else { return nil }
            let header = columns[0].trimmingCharacters(in: .whitespacesAndNewlines)
            let tokens = header.split(whereSeparator: \.isWhitespace)
            guard let bundle = tokens.last,
                  bundle.split(separator: "(", maxSplits: 1).first == Substring(identifier) else { return nil }
            return Self(identifier: identifier,
                        path: MirageLockBridge.normalizedPath(URL(fileURLWithPath: path)),
                        elected: tokens.count > 1 && tokens.first == "+")
        }
    }
}

enum WallpaperSettingsCache {
    enum Policy {
        case all, emptyDesktop
    }

    static func directory() throws -> URL {
        let length = confstr(_CS_DARWIN_USER_CACHE_DIR, nil, 0)
        guard length > 0 else { throw MirageLockBridge.failure("The user cache directory is unavailable") }
        var path = [CChar](repeating: 0, count: length)
        guard confstr(_CS_DARWIN_USER_CACHE_DIR, &path, length) == length else {
            throw MirageLockBridge.failure("The user cache directory could not be read")
        }
        return URL(fileURLWithPath: String(cString: path), isDirectory: true)
            .appendingPathComponent("com.apple.wallpaper.agent/com.apple.wallpaper.view-model-cache", isDirectory: true)
    }

    @discardableResult
    static func invalidate(identifier: String, policy: Policy, directory: URL? = nil,
                           isCurrent: () -> Bool) throws -> Bool {
        let directory = try directory ?? Self.directory()
        let desktop = directory.appendingPathComponent("extension-\(identifier)-desktop")
        let screenSaver = directory.appendingPathComponent("extension-\(identifier)-screenSaver")
        guard isCurrent() else { throw CancellationError() }
        if policy == .emptyDesktop {
            let data: Data
            do {
                data = try Data(contentsOf: desktop)
            } catch CocoaError.fileReadNoSuchFile {
                return false
            }
            guard let root = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
                  let viewModel = root["viewModel"] as? [String: Any],
                  let groups = viewModel["groups"] as? [Any], groups.isEmpty else { return false }
        }
        var removed = false
        for url in [desktop, screenSaver] {
            guard isCurrent() else { throw CancellationError() }
            do {
                try FileManager.default.removeItem(at: url)
                removed = true
                NSLog("[MirageLock] invalidated cached wallpaper settings: %@", url.lastPathComponent)
            } catch CocoaError.fileNoSuchFile {
                continue
            }
        }
        return removed
    }
}

final class WallpaperExtensionRequest: @unchecked Sendable {
    private let lock = NSLock()
    private var current: UUID?

    func begin() -> UUID {
        lock.lock()
        defer { lock.unlock() }
        let id = UUID()
        current = id
        return id
    }

    func cancel() {
        lock.lock()
        current = nil
        lock.unlock()
    }

    func isCurrent(_ id: UUID) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return current == id
    }
}

enum WallpaperServiceCoordinator {
    static let queue = DispatchQueue(label: "cn.laobamac.Mirage.wallpaper-services", qos: .utility)
    static let lock = NSRecursiveLock()
}
