//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import AVFoundation
import Foundation

enum WPImportError: LocalizedError, Identifiable {
    case permissionDenied
    case doesNotContainWallpaper
    case unsupportedType
    case copyFailed(String)
    case unknown

    var id: String { errorDescription ?? "unknown" }

    var errorDescription: String? {
        switch self {
        case .permissionDenied: return L("没有访问权限")
        case .doesNotContainWallpaper: return L("文件夹内没有壁纸")
        case .unsupportedType: return L("不支持的壁纸类型")
        case .copyFailed(let m): return L("复制失败：%@", m)
        case .unknown: return L("未知错误")
        }
    }

    var recoverySuggestion: String? {
        switch self {
        case .permissionDenied: return L("请在“系统设置 - 隐私与安全性”中授予访问权限后重试。")
        case .doesNotContainWallpaper: return L("所选文件夹需包含 project.json，请确认后重试。")
        case .unsupportedType: return L("Mirage 仅支持 场景 / 网页 / 视频 类壁纸。")
        case .copyFailed: return L("请检查磁盘空间与权限后重试。")
        case .unknown: return nil
        }
    }
}

enum WallpaperLibrarySourceRole: String, Hashable {
    case steam
    case customSteam
    case managedSteamCMD
    case legacySteamCMD
    case imported
}

struct WallpaperLibrarySource: Identifiable, Hashable {
    let role: WallpaperLibrarySourceRole
    let title: String
    let detail: String
    let url: URL
    let exists: Bool

    var id: String { role.rawValue + ":" + url.standardizedFileURL.path }
}

final class WallpaperLibrary {
    static let shared = WallpaperLibrary()

    private let fm = FileManager.default
    private struct CachedWallpaper {
        let signature: String
        let wallpaper: WEWallpaper
    }
    private let loadCacheLock = NSLock()
    private var loadCache: [String: CachedWallpaper] = [:]

    private let workshopKey = "CustomWorkshopDirectory"
    private let importedKey = "CustomImportedDirectory"

    private struct DirectoryMonitor {
        let source: DispatchSourceFileSystemObject
        let descriptor: Int32
    }

    private var directoryMonitors: [DirectoryMonitor] = []
    private var monitoringCallback: (() -> Void)?

    var defaultSteamWorkshopDirectory: URL {
        fm.homeDirectoryForCurrentUser
            .appending(path: "Library/Application Support/Steam/steamapps/workshop/content/431960")
    }

    var defaultImportedDirectory: URL {
        fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/Wallpapers")
    }

    var steamWorkshopDirectory: URL {
        if let path = UserDefaults.standard.string(forKey: workshopKey), !path.isEmpty {
            return URL(fileURLWithPath: path)
        }
        return defaultSteamWorkshopDirectory
    }

    var importedDirectory: URL {
        let base: URL
        if let path = UserDefaults.standard.string(forKey: importedKey), !path.isEmpty {
            base = URL(fileURLWithPath: path)
        } else {
            base = defaultImportedDirectory
        }
        if !fm.fileExists(atPath: base.path) {
            try? fm.createDirectory(at: base, withIntermediateDirectories: true)
        }
        return base
    }

    var isWorkshopDirectoryCustomized: Bool {
        !(UserDefaults.standard.string(forKey: workshopKey) ?? "").isEmpty
    }
    var isImportedDirectoryCustomized: Bool {
        !(UserDefaults.standard.string(forKey: importedKey) ?? "").isEmpty
    }

    func setWorkshopDirectory(_ url: URL?) {
        UserDefaults.standard.set(url?.path, forKey: workshopKey)
        restartMonitoringIfNeeded()
    }
    func setImportedDirectory(_ url: URL?) {
        UserDefaults.standard.set(url?.path, forKey: importedKey)
        restartMonitoringIfNeeded()
    }

    var librarySources: [WallpaperLibrarySource] {
        var result: [WallpaperLibrarySource] = []
        var paths = Set<String>()

        func append(_ role: WallpaperLibrarySourceRole, _ title: String, _ detail: String, _ url: URL, always: Bool = false) {
            let normalized = url.standardizedFileURL
            let exists = fm.fileExists(atPath: normalized.path)
            guard always || exists else { return }
            guard paths.insert(normalized.path).inserted else { return }
            result.append(WallpaperLibrarySource(role: role, title: title, detail: detail, url: normalized, exists: exists))
        }

        if isWorkshopDirectoryCustomized {
            append(.customSteam, L("自定义创意工坊目录"), L("用户选择的 Wallpaper Engine 内容目录"), steamWorkshopDirectory, always: true)
            append(.steam, L("Steam 创意工坊目录"), L("系统 Steam 的默认内容目录"), defaultSteamWorkshopDirectory)
        } else {
            append(.steam, L("Steam 创意工坊目录"), L("系统 Steam 的默认内容目录"), defaultSteamWorkshopDirectory, always: true)
        }

        append(.managedSteamCMD, L("Mirage 下载目录"), L("SteamCMD 当前下载和更新壁纸的位置"), SteamCMDManager.shared.isolatedSteamCMDContentDirectory, always: true)
        append(.legacySteamCMD, L("Mirage 旧版下载目录"), L("仅用于兼容旧版本中已经下载的壁纸"), SteamCMDManager.shared.steamCMDContentDirectory)
        append(.imported, L("导入壁纸目录"), L("手动导入或由视频创建的本地壁纸"), importedDirectory, always: true)
        return result
    }

    private var sourceDirectories: [URL] {
        librarySources.filter(\.exists).map(\.url)
    }

    func allWallpaperURLs() -> [URL] {
        var result: [URL] = []
        for dir in sourceDirectories {
            guard let contents = try? fm.contentsOfDirectory(
                at: dir, includingPropertiesForKeys: [.isDirectoryKey],
                options: [.skipsHiddenFiles]) else { continue }
            for url in contents {
                let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
                guard isDir else { continue }
                if fm.fileExists(atPath: url.appending(path: "project.json").path) {
                    result.append(url)
                }
            }
        }
        return result
    }

    func workshopItemDirectory(for workshopId: String) -> URL? {
        workshopItemDirectories(for: workshopId).first
    }

    func workshopItemDirectories(for workshopId: String) -> [URL] {
        sourceDirectories
            .map { $0.appending(path: workshopId) }
            .filter { fm.fileExists(atPath: $0.appending(path: "project.json").path) }
    }

    /// Every installed workshop item keyed by its directory name (the workshop
    /// id). First occurrence wins when the same id exists in multiple sources.
    /// Used to build the installed-state snapshot off the main thread so card
    /// views never touch the filesystem while rendering.
    func allWorkshopIDDirectories() -> [(id: String, url: URL)] {
        var result: [(id: String, url: URL)] = []
        var seen = Set<String>()
        for url in allWallpaperURLs() {
            let id = url.lastPathComponent
            guard seen.insert(id).inserted else { continue }
            result.append((id, url))
        }
        return result
    }

    func loadAll() -> [WEWallpaper] {
        let urls = allWallpaperURLs()
        loadCacheLock.lock()
        let previous = loadCache
        loadCacheLock.unlock()

        var next: [String: CachedWallpaper] = [:]
        next.reserveCapacity(urls.count)
        let wallpapers = urls.map { url -> WEWallpaper in
            let path = url.standardizedFileURL.path
            let projectURL = url.appending(path: "project.json")
            let projectValues = try? projectURL.resourceValues(
                forKeys: [.contentModificationDateKey, .fileSizeKey])
            let directoryValues = try? url.resourceValues(forKeys: [.contentModificationDateKey])
            let signature = "\(projectValues?.contentModificationDate?.timeIntervalSince1970 ?? 0)#\(projectValues?.fileSize ?? 0)#\(directoryValues?.contentModificationDate?.timeIntervalSince1970 ?? 0)"
            if let cached = previous[path], cached.signature == signature {
                next[path] = cached
                return cached.wallpaper
            }
            let wallpaper = WEWallpaper.load(from: url)
            next[path] = CachedWallpaper(signature: signature, wallpaper: wallpaper)
            return wallpaper
        }

        loadCacheLock.lock()
        loadCache = next
        loadCacheLock.unlock()
        return wallpapers
    }

    // MARK: - 导入

    @discardableResult
    func importWallpaperFolder(at url: URL) throws -> URL {
        guard (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true else {
            throw WPImportError.doesNotContainWallpaper
        }
        guard fm.fileExists(atPath: url.appending(path: "project.json").path) else {
            throw WPImportError.doesNotContainWallpaper
        }
        let dest = uniqueDestination(for: url.lastPathComponent)
        do {
            try fm.copyItem(at: url, to: dest)
        } catch {
            throw WPImportError.copyFailed(error.localizedDescription)
        }
        return dest
    }

    @discardableResult
    func importVideoFile(at url: URL) throws -> URL {
        let ext = url.pathExtension.lowercased()
        guard ["mp4", "mov", "m4v"].contains(ext) else {
            throw WPImportError.unsupportedType
        }
        let baseName = url.deletingPathExtension().lastPathComponent
        let dest = uniqueDestination(for: baseName)
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            let fileName = url.lastPathComponent
            try fm.copyItem(at: url, to: dest.appending(path: fileName))

            let previewName = "preview.jpg"
            if let jpeg = try? Self.generateThumbnail(for: url) {
                try? jpeg.write(to: dest.appending(path: previewName), options: .atomic)
            }

            let project = WEProject(file: fileName,
                                    preview: previewName,
                                    title: baseName,
                                    type: "video")
            let data = try JSONEncoder().encode(project)
            try data.write(to: dest.appending(path: "project.json"), options: .atomic)
        } catch let e as WPImportError {
            throw e
        } catch {
            throw WPImportError.copyFailed(error.localizedDescription)
        }
        return dest
    }

    @discardableResult
    func importAny(at url: URL) throws -> URL {
        let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
        if isDir {
            return try importWallpaperFolder(at: url)
        } else {
            return try importVideoFile(at: url)
        }
    }

    private func uniqueDestination(for name: String) -> URL {
        var dest = importedDirectory.appending(path: name)
        var counter = 1
        while fm.fileExists(atPath: dest.path) {
            dest = importedDirectory.appending(path: "\(name)_\(counter)")
            counter += 1
        }
        return dest
    }

    static func generateThumbnail(for videoURL: URL) throws -> Data? {
        let asset = AVURLAsset(url: videoURL)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        let time = CMTimeMake(value: 1, timescale: 1)
        let cgImage = try generator.copyCGImage(at: time, actualTime: nil)
        let rep = NSBitmapImageRep(cgImage: cgImage)
        return rep.representation(using: .jpeg, properties: [.compressionFactor: 0.85])
    }

    func trash(_ wallpaper: WEWallpaper) throws {
        try fm.trashItem(at: wallpaper.wallpaperDirectory, resultingItemURL: nil)
    }

    func delete(_ wallpaper: WEWallpaper) throws {
        try fm.removeItem(at: wallpaper.wallpaperDirectory)
    }

    func isImported(_ wallpaper: WEWallpaper) -> Bool {
        wallpaper.wallpaperDirectory.path.hasPrefix(importedDirectory.path)
    }

    // MARK: - Directory Monitoring

    func startMonitoringWorkshopDirectory(onChange: @escaping () -> Void) {
        cancelDirectoryMonitors()
        monitoringCallback = onChange

        for directory in sourceDirectories {
            let descriptor = open(directory.path, O_EVTONLY)
            guard descriptor >= 0 else { continue }
            let source = DispatchSource.makeFileSystemObjectSource(
                fileDescriptor: descriptor,
                eventMask: [.write, .delete, .rename],
                queue: .main
            )
            source.setEventHandler {
                onChange()
            }
            source.setCancelHandler { close(descriptor) }
            directoryMonitors.append(DirectoryMonitor(source: source, descriptor: descriptor))
            source.resume()
        }
    }

    func stopMonitoringWorkshopDirectory() {
        monitoringCallback = nil
        cancelDirectoryMonitors()
    }

    private func cancelDirectoryMonitors() {
        directoryMonitors.forEach { $0.source.cancel() }
        directoryMonitors.removeAll()
    }

    private func restartMonitoringIfNeeded() {
        guard let callback = monitoringCallback else { return }
        startMonitoringWorkshopDirectory(onChange: callback)
    }
}
