//
//  EditorInstallation.swift
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import CryptoKit

struct EditorServiceError: LocalizedError {
    let code: String
    var detail: String? = nil
    var errorDescription: String? {
        switch code {
        case "WE_NOT_OWNED": return L("当前 Steam 账号未拥有 Wallpaper Engine 的永久许可证。")
        case "NOT_AUTHENTICATED", "EDITOR_ACCOUNT_CHANGED": return L("Steam 会话已变化，请重新验证账号。")
        case "EDITOR_VERSION_UNAVAILABLE": return L("Steam 提供的版本尚未适配，请更新 Mirage 后重试。")
        case "OWNERSHIP_UNAVAILABLE": return L("暂时无法验证购买资格，请稍后重试。")
        case "EDITOR_PLAN_EXPIRED": return L("安装信息已过期，请重新检查。")
        case "EDITOR_SUPPORT_MISSING": return L("编辑器组件信息不可用，请更新 Mirage 后重试。")
        case "EDITOR_RUNTIME_DOWNLOAD": return L("编辑器运行组件下载失败，请重试。已下载的内容会保留。")
        case "EDITOR_ARCHITECTURE": return L("编辑器运行组件与此 Mac 的芯片架构不匹配。")
        case "EDITOR_SYSTEM_VERSION": return L("Apple Silicon 编辑器需要 macOS 26.5 或更高版本。")
        case "EDITOR_SPACE": return L("可用磁盘空间不足，请释放空间后重试。")
        case "EDITOR_BUSY": return L("请先保存工程并退出编辑器，再进行此操作。")
        case "EDITOR_FILES": return L("编辑器文件校验失败，请修复安装。")
        case "VALIDATION_FAILED": return L("下载内容校验失败，请重试")
        default: return detail?.isEmpty == false ? detail : L("编辑器操作失败，请重试。")
        }
    }
}

struct EditorRuntimeArchive: Codable, Sendable {
    let url: URL
    let sha256: String
    let bytes: Int64
}

struct EditorCatalog: Decodable, Sendable {
    struct Compatibility: Codable, Sendable {
        let version: String
        let branch: String
        let sourceHashes: [String: String]
    }
    let schema: Int
    let identifier: String
    let architecture: String
    let installedBytes: Int64
    let compatibility: Compatibility
    let files: [String: String]
    let links: [String: String]
    let minimumMacOS: String?
    let rosetta: Bool?
    let archive: EditorRuntimeArchive?
}

struct EditorDiskUsage: Sendable {
    var installed: Int64 = 0
    var reclaimable: Int64 = 0
    var uninstallable: Int64 = 0
}

enum EditorInstallation {
    #if arch(arm64)
    static let architecture = "arm64"
    #else
    static let architecture = "x86_64"
    #endif
    #if MIRAGE_EDITOR_TESTING
    static let root = URL(fileURLWithPath: ProcessInfo.processInfo.environment["MIRAGE_EDITOR_TEST_ROOT"]!, isDirectory: true)
    #else
    static let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Mirage/Editor", isDirectory: true)
    #endif
    static var active: URL { root.appendingPathComponent("active", isDirectory: true) }
    static var downloads: URL { root.appendingPathComponent("downloads", isDirectory: true) }
    static var runtimeDownloads: URL { root.appendingPathComponent("runtime-downloads", isDirectory: true) }
    static var runtimeCache: URL { root.appendingPathComponent("runtime-cache", isDirectory: true) }
    static var application: URL { active.appendingPathComponent("Mirage Editor.app", isDirectory: true) }
    static var runtime: URL { active.appendingPathComponent("runtime", isDirectory: true) }
    static var bundledSupport: URL? {
        #if MIRAGE_EDITOR_TESTING
        let base = URL(fileURLWithPath: ProcessInfo.processInfo.environment["MIRAGE_EDITOR_TEST_SUPPORT"]!, isDirectory: true)
        #else
        guard let base = Bundle.main.resourceURL?.appendingPathComponent("EditorSupport", isDirectory: true) else { return nil }
        #endif
        let selected = base.appendingPathComponent(architecture, isDirectory: true)
        return FileManager.default.fileExists(atPath: selected.appendingPathComponent("catalog.json").path) ? selected : base
    }

    static var support: URL? {
        guard let bundledSupport else { return nil }
        guard let catalog = try? catalog(), let archive = catalog.archive else { return bundledSupport }
        return runtimeCache.appendingPathComponent(archive.sha256).appendingPathComponent("EditorSupport")
    }

    static func catalog() throws -> EditorCatalog {
        guard let bundledSupport, let data = try? Data(contentsOf: bundledSupport.appendingPathComponent("catalog.json")) else {
            throw EditorServiceError(code: "EDITOR_SUPPORT_MISSING")
        }
        let catalog = try JSONDecoder().decode(EditorCatalog.self, from: data)
        guard catalog.schema == 1, !catalog.identifier.isEmpty else { throw EditorServiceError(code: "EDITOR_FILES") }
        try validateCompatibility(catalog)
        return catalog
    }

    static func validateCompatibility(_ catalog: EditorCatalog, hostArchitecture: String = architecture,
                                      systemVersion: OperatingSystemVersion = ProcessInfo.processInfo.operatingSystemVersion) throws {
        guard catalog.architecture == hostArchitecture else { throw EditorServiceError(code: "EDITOR_ARCHITECTURE") }
        if let archive = catalog.archive {
            guard archive.url.scheme == "https", archive.url.host == "github.com",
                  archive.url.user == nil, archive.url.password == nil,
                  archive.url.path.hasPrefix("/laobamac/MirageWallpaper/releases/download/"),
                  archive.sha256.range(of: "^[a-f0-9]{64}$", options: .regularExpression) != nil,
                  archive.bytes > 0, archive.bytes <= 10 * 1024 * 1024 * 1024,
                  !catalog.files.isEmpty else { throw EditorServiceError(code: "EDITOR_FILES") }
        }
        if hostArchitecture == "arm64" {
            guard catalog.rosetta == false, let minimum = catalog.minimumMacOS else { throw EditorServiceError(code: "EDITOR_FILES") }
            let parts = minimum.split(separator: ".", omittingEmptySubsequences: false).compactMap { Int($0) }
            guard (2...3).contains(parts.count), parts.allSatisfy({ $0 >= 0 }),
                  parts.count == minimum.split(separator: ".", omittingEmptySubsequences: false).count else { throw EditorServiceError(code: "EDITOR_FILES") }
            let required = parts + Array(repeating: 0, count: 3 - parts.count)
            let current = [systemVersion.majorVersion, systemVersion.minorVersion, systemVersion.patchVersion]
            guard !required.lexicographicallyPrecedes([26, 5, 0]) else { throw EditorServiceError(code: "EDITOR_FILES") }
            guard !current.lexicographicallyPrecedes(required) else { throw EditorServiceError(code: "EDITOR_SYSTEM_VERSION") }
        }
    }

    static var isInstalled: Bool {
        FileManager.default.fileExists(atPath: active.appendingPathComponent("receipt.json").path) &&
            FileManager.default.isExecutableFile(atPath: application.appendingPathComponent("Contents/MacOS/Launcher").path)
    }

    static func contained(_ relative: String, in directory: URL) throws -> URL {
        guard !relative.hasPrefix("/"), !relative.contains("\\"), !relative.split(separator: "/").contains("..") else {
            throw EditorServiceError(code: "EDITOR_FILES")
        }
        let result = directory.appendingPathComponent(relative).standardizedFileURL
        guard result.path.hasPrefix(directory.standardizedFileURL.path + "/") else { throw EditorServiceError(code: "EDITOR_FILES") }
        return result
    }

    static func validateRoot() throws {
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        for url in [root.deletingLastPathComponent(), root, active, downloads, runtimeDownloads, runtimeCache, root.appendingPathComponent("staging"), root.appendingPathComponent("previous"), root.appendingPathComponent("Projects"), root.appendingPathComponent("UserData")] {
            if (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true {
                throw EditorServiceError(code: "EDITOR_FILES")
            }
        }
    }

    static func availableBytes() -> Int64 {
        let ancestor = FileManager.default.fileExists(atPath: root.path) ? root : root.deletingLastPathComponent()
        return (try? ancestor.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
    }

    static func size(_ directory: URL) -> Int64 {
        let keys: [URLResourceKey] = [.isSymbolicLinkKey, .isRegularFileKey, .totalFileAllocatedSizeKey, .fileAllocatedSizeKey]
        guard let enumerator = FileManager.default.enumerator(at: directory, includingPropertiesForKeys: keys, options: []) else { return 0 }
        var total: Int64 = 0
        for case let url as URL in enumerator {
            guard let values = try? url.resourceValues(forKeys: Set(keys)) else { continue }
            if values.isSymbolicLink == true { continue }
            if values.isRegularFile == true { total += Int64(values.totalFileAllocatedSize ?? values.fileAllocatedSize ?? 0) }
        }
        return total
    }

    static let cachePaths = ["downloads", "runtime-downloads", "runtime-cache", "staging", "previous", "active/runtime/logs",
                             "UserData/editor/base/Default/Cache", "UserData/editor/base/Default/Code Cache",
                             "UserData/editor/base/Default/GPUCache", "UserData/editor/base/ShaderCache"]

    static func usage() -> EditorDiskUsage {
        EditorDiskUsage(installed: size(active), reclaimable: cachePaths.reduce(0) { $0 + size(root.appendingPathComponent($1)) },
                        uninstallable: (["active", "downloads", "runtime-downloads", "runtime-cache", "staging", "previous"] + cachePaths.filter { $0.hasPrefix("UserData/") }).reduce(0) { $0 + size(root.appendingPathComponent($1)) })
    }

    static func clean(uninstall: Bool) throws {
        try validateRoot()
        let fm = FileManager.default
        for relative in uninstall ? ["active", "previous", "staging"] : ["previous", "staging"] {
            try stopPrefix(root.appendingPathComponent(relative + "/runtime"))
        }
        for relative in uninstall ? ["active", "previous", "staging", "downloads", "runtime-downloads", "runtime-cache"] + cachePaths.filter({ $0.hasPrefix("UserData/") }) : cachePaths {
            let url = try contained(relative, in: root)
            // Refuse an intermediate symlink; Wine's own project links are never traversed.
            var parent = url.deletingLastPathComponent()
            while parent.path != root.path {
                if (try? parent.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true { throw EditorServiceError(code: "EDITOR_FILES") }
                parent.deleteLastPathComponent()
            }
            if fm.fileExists(atPath: url.path) { try fm.removeItem(at: url) }
        }
    }

    static func hash(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var digest = SHA256()
        while let data = try handle.read(upToCount: 1024 * 1024), !data.isEmpty {
            try Task.checkCancellation()
            digest.update(data: data)
        }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func verifySupport(_ directory: URL, catalog: EditorCatalog) throws {
        let resolved = directory.resolvingSymlinksInPath().path + "/"
        for (name, expected) in catalog.files {
            let file = try contained(name, in: directory)
            guard file.resolvingSymlinksInPath().path.hasPrefix(resolved),
                  (try file.resourceValues(forKeys: [.isSymbolicLinkKey])).isSymbolicLink != true,
                  try hash(file) == expected else { throw EditorServiceError(code: "EDITOR_FILES") }
        }
        for (name, expected) in catalog.links {
            let file = try contained(name, in: directory)
            guard !expected.hasPrefix("/"), file.resolvingSymlinksInPath().path.hasPrefix(resolved),
                  try FileManager.default.destinationOfSymbolicLink(atPath: file.path) == expected else {
                throw EditorServiceError(code: "EDITOR_FILES")
            }
        }
    }

    private struct Patch: Decodable {
        struct Operation: Decodable { let offset: Int?; let count: Int?; let data: String? }
        let path: String
        let sourceSHA256: String
        let resultSHA256: String
        let operations: [Operation]
    }

    static func verifyInstalled(catalog: EditorCatalog) throws {
        try validateRoot()
        var binaries = ["Mirage Editor.app/Contents/MacOS/Launcher", "runtime/MetalInterop.dylib",
                        "runtime/Mirage Project Picker.app/Contents/MacOS/ProjectPicker",
                        "runtime/Mirage WE Editor.app/Contents/MacOS/wine",
                        "runtime/Mirage WE Editor.app/Contents/Resources/wine/bin/wine",
                        "runtime/Mirage WE Editor.app/Contents/Resources/wine/bin/wineserver",
                        "runtime/adaptations.json"]
        if catalog.architecture == "arm64" {
            let engine = "runtime/Mirage WE Editor.app/Contents/Resources/wine/lib/wine/"
            binaries += [engine + "aarch64-unix/ntdll.so", engine + "aarch64-unix/libarm64ecfex.so",
                         engine + "aarch64-windows/xtajit64.dll", "runtime/native-build.json"]
        }
        for relative in binaries {
            let url = try contained(relative, in: active)
            guard url.resolvingSymlinksInPath().path.hasPrefix(active.resolvingSymlinksInPath().path + "/"),
                  let expected = catalog.files[relative], try hash(url) == expected else { throw EditorServiceError(code: "EDITOR_FILES") }
        }
        let patches = try JSONDecoder().decode([Patch].self, from: Data(contentsOf: runtime.appendingPathComponent("adaptations.json")))
        for patch in patches {
            let url = try contained(patch.path, in: runtime.appendingPathComponent("windows/WallpaperEngine"))
            guard try hash(url) == patch.resultSHA256 else { throw EditorServiceError(code: "EDITOR_FILES") }
        }
        for name in graphicsLibraries(architecture: catalog.architecture) {
            guard try hash(runtime.appendingPathComponent("prefix-devel/drive_c/windows/system32/" + name)) == catalog.files["runtime/overrides/" + name] else {
                throw EditorServiceError(code: "EDITOR_FILES")
            }
        }
    }

    static func assemble(catalog: EditorCatalog) throws {
        try validateRoot()
        guard let support else { throw EditorServiceError(code: "EDITOR_SUPPORT_MISSING") }
        let fm = FileManager.default
        let staging = root.appendingPathComponent("staging", isDirectory: true)
        let previous = root.appendingPathComponent("previous", isDirectory: true)
        try stopPrefix(staging.appendingPathComponent("runtime"))
        if fm.fileExists(atPath: staging.path) { try fm.removeItem(at: staging) }
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        for (path, expected) in catalog.files {
            try Task.checkCancellation()
            let source = try contained(path, in: support)
            guard try hash(source) == expected else { throw EditorServiceError(code: "EDITOR_FILES") }
            let target = try contained(path, in: staging)
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.copyItem(at: source, to: target)
        }
        for (path, destination) in catalog.links {
            let target = try contained(path, in: staging)
            let resolved = target.deletingLastPathComponent().appendingPathComponent(destination).standardizedFileURL
            guard !destination.hasPrefix("/"), resolved.path.hasPrefix(staging.path + "/") else { throw EditorServiceError(code: "EDITOR_FILES") }
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.createSymbolicLink(atPath: target.path, withDestinationPath: destination)
        }
        let runtime = staging.appendingPathComponent("runtime", isDirectory: true)
        let windows = runtime.appendingPathComponent("windows", isDirectory: true)
        try fm.createDirectory(at: windows, withIntermediateDirectories: true)
        let we = windows.appendingPathComponent("WallpaperEngine", isDirectory: true)
        try fm.copyItem(at: downloads, to: we)
        let patches = try JSONDecoder().decode([Patch].self, from: Data(contentsOf: runtime.appendingPathComponent("adaptations.json")))
        for patch in patches {
            try Task.checkCancellation()
            let target = try contained(patch.path, in: we)
            guard try hash(target) == patch.sourceSHA256 else { throw EditorServiceError(code: "EDITOR_VERSION_UNAVAILABLE") }
            let source = try Data(contentsOf: target)
            var result = Data()
            for operation in patch.operations {
                if let offset = operation.offset, let count = operation.count {
                    guard offset >= 0, count >= 0, offset <= source.count, count <= source.count - offset else { throw EditorServiceError(code: "EDITOR_FILES") }
                    result.append(source[offset..<(offset + count)])
                } else if let data = operation.data, let decoded = Data(base64Encoded: data) { result.append(decoded) }
                else { throw EditorServiceError(code: "EDITOR_FILES") }
            }
            try result.write(to: target, options: .atomic)
            guard try hash(target) == patch.resultSHA256 else { throw EditorServiceError(code: "EDITOR_FILES") }
        }
        for (name, relative) in ["standalone.js": "ui/dist/scripts/mirage-standalone.js", "macos.css": "ui/dist/styles/macos.css"] {
            let target = we.appendingPathComponent(relative)
            if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
            try fm.copyItem(at: runtime.appendingPathComponent(name), to: target)
        }
        let index = we.appendingPathComponent("ui/dist/index.html")
        var html = try String(contentsOf: index, encoding: .utf8)
        guard html.contains("</head>"), !html.contains("mirage-standalone.js") else { throw EditorServiceError(code: "EDITOR_FILES") }
        html = html.replacingOccurrences(of: "</head>", with: "<link rel=\"stylesheet\" href=\"styles/macos.css\"><script src=\"scripts/mirage-session.js\"></script><script src=\"scripts/mirage-standalone.js\"></script></head>")
        try html.write(to: index, atomically: true, encoding: .utf8)
        let projects = root.appendingPathComponent("Projects", isDirectory: true)
        try fm.createDirectory(at: projects, withIntermediateDirectories: true)
        let myProjects = we.appendingPathComponent("projects/myprojects", isDirectory: true)
        try fm.createDirectory(at: myProjects.deletingLastPathComponent(), withIntermediateDirectories: true)
        if fm.fileExists(atPath: myProjects.path) { try fm.removeItem(at: myProjects) }
        try fm.createSymbolicLink(at: myProjects, withDestinationURL: projects)
        let userData = root.appendingPathComponent("UserData", isDirectory: true)
        let oldProfile = self.runtime.appendingPathComponent("windows/WallpaperEngine/ui/uicache")
        if !fm.fileExists(atPath: userData.path), fm.fileExists(atPath: oldProfile.path),
           (try? oldProfile.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true {
            try fm.copyItem(at: oldProfile, to: userData)
        }
        try fm.createDirectory(at: userData, withIntermediateDirectories: true)
        let profile = we.appendingPathComponent("ui/uicache")
        if fm.fileExists(atPath: profile.path) { try fm.removeItem(at: profile) }
        try fm.createSymbolicLink(at: profile, withDestinationURL: userData)
        try initializePrefix(runtime, architecture: catalog.architecture)
        try Task.checkCancellation()
        for name in ["user.reg", "userdef.reg", "system.reg"] {
            let file = runtime.appendingPathComponent("prefix-devel/" + name)
            let source = try String(contentsOf: file, encoding: .utf8)
            let oldWindows = staging.path.replacingOccurrences(of: "/", with: "\\\\")
            let newWindows = active.path.replacingOccurrences(of: "/", with: "\\\\")
            let updated = source.replacingOccurrences(of: staging.path, with: active.path)
                .replacingOccurrences(of: oldWindows, with: newWindows)
            try updated.write(to: file, atomically: true, encoding: .utf8)
        }
        let receipt = try JSONSerialization.data(withJSONObject: ["identifier": catalog.identifier, "version": catalog.compatibility.version])
        try receipt.write(to: staging.appendingPathComponent("receipt.json"), options: .atomic)
        if fm.fileExists(atPath: previous.path) { try fm.removeItem(at: previous) }
        if fm.fileExists(atPath: active.path) { try fm.moveItem(at: active, to: previous) }
        do { try fm.moveItem(at: staging, to: active) }
        catch {
            if !fm.fileExists(atPath: active.path), fm.fileExists(atPath: previous.path) { try? fm.moveItem(at: previous, to: active) }
            throw error
        }
    }

    private static func stopPrefix(_ runtime: URL) throws {
        // An installed editor remains self-contained after its download cache is cleared.
        let installedServer = runtime.appendingPathComponent("Mirage WE Editor.app/Contents/Resources/wine/bin/wineserver")
        let executable = FileManager.default.isExecutableFile(atPath: installedServer.path) ? installedServer :
            support?.appendingPathComponent("runtime/Mirage WE Editor.app/Contents/Resources/wine/bin/wineserver")
        guard let executable else { return }
        let prefix = runtime.appendingPathComponent("prefix-devel")
        guard prefix.resolvingSymlinksInPath().path.hasPrefix(root.resolvingSymlinksInPath().path + "/") else { throw EditorServiceError(code: "EDITOR_FILES") }
        guard FileManager.default.isExecutableFile(atPath: executable.path),
              FileManager.default.fileExists(atPath: prefix.path) else { return }
        for arguments in [["-k"], ["-w"]] {
            let process = Process()
            process.executableURL = executable
            process.arguments = arguments
            process.environment = ["WINEPREFIX": prefix.path, "PATH": "/usr/bin:/bin:/usr/sbin:/sbin"]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            let deadline = Date().addingTimeInterval(10)
            while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
            if process.isRunning { process.terminate(); throw EditorServiceError(code: "EDITOR_BUSY") }
            guard process.terminationStatus == 0 else { throw EditorServiceError(code: "EDITOR_BUSY") }
        }
    }

    private static func graphicsLibraries(architecture: String) -> [String] {
        // Native Wine's DXGI initializes OpenGL even when D3D11 uses DXVK.
        // Use DXVK's matching DXGI on ARM64 so enumeration stays on Vulkan.
        ["d3d11.dll", "d3d10core.dll", "mirage_dxvk.dll"] + (architecture == "arm64" ? ["dxgi.dll"] : [])
    }

    private static func initializePrefix(_ runtime: URL, architecture: String) throws {
        let fm = FileManager.default
        let engine = runtime.appendingPathComponent("Mirage WE Editor.app/Contents/Resources/wine")
        let prefix = runtime.appendingPathComponent("prefix-devel")
        let logDirectory = runtime.appendingPathComponent("logs")
        try fm.createDirectory(at: logDirectory, withIntermediateDirectories: true)
        let log = logDirectory.appendingPathComponent("installation.log")
        fm.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600])
        let output = try FileHandle(forWritingTo: log)
        defer { try? output.close() }
        var environment = ProcessInfo.processInfo.environment
        environment["WINEPREFIX"] = prefix.path
        environment["WINEARCH"] = "win64"
        environment["WINEDEBUG"] = "-all"
        // The editor uses its own CEF runtime. Wine's optional Mono/Gecko
        // installers must not block initialization behind an unseen dialog.
        environment["WINEDLLOVERRIDES"] = "winemenubuilder.exe=d;mscoree,mshtml="
        environment["DYLD_INSERT_LIBRARIES"] = nil
        environment["WINELOADER"] = nil
        #if arch(arm64)
        environment["DYLD_LIBRARY_PATH"] = engine.appendingPathComponent("lib/external").path
        environment["WINEDLLPATH"] = engine.appendingPathComponent("lib/wine").path
        #endif
        environment["PATH"] = engine.appendingPathComponent("bin").path + ":/usr/bin:/bin:/usr/sbin:/sbin"
        func run(_ binary: String, _ arguments: [String], cancellable: Bool = true) throws {
            let process = Process()
            process.executableURL = engine.appendingPathComponent("bin/" + binary)
            process.arguments = arguments
            process.environment = environment
            process.standardOutput = output; process.standardError = output
            try process.run()
            let deadline = Date().addingTimeInterval(90)
            while process.isRunning {
                if (cancellable && Task.isCancelled) || Date() > deadline {
                    process.terminate()
                    throw EditorServiceError(code: "EDITOR_INITIALIZATION", detail: L("编辑器环境准备已中止，请重试。"))
                }
                Thread.sleep(forTimeInterval: 0.05)
            }
            guard process.terminationStatus == 0 else { throw EditorServiceError(code: "EDITOR_INITIALIZATION", detail: L("编辑器环境初始化失败，可在管理窗口查看日志。")) }
        }
        defer {
            try? run("wineserver", ["-k"], cancellable: false)
            try? run("wineserver", ["-w"], cancellable: false)
        }
        try run("wineboot", ["--init"])
        let mount = prefix.appendingPathComponent("dosdevices/m:")
        try fm.createSymbolicLink(atPath: mount.path, withDestinationPath: "../../windows")
        try run("wine", ["reg", "add", "HKCU\\Software\\WallpaperEngine", "/v", "installPath", "/d", "M:\\WallpaperEngine\\wallpaper64.exe", "/f"])
        for name in graphicsLibraries(architecture: architecture) {
            let target = prefix.appendingPathComponent("drive_c/windows/system32/" + name)
            if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
            try fm.copyItem(at: runtime.appendingPathComponent("overrides/" + name), to: target)
        }
    }
}
