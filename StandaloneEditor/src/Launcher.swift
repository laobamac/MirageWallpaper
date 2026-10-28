// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.

import AppKit
import Foundation
import Darwin

let root = Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("runtime", isDirectory: true)
let files = FileManager.default
#if MIRAGE_MANAGED_EDITOR
let base = root.appendingPathComponent("Mirage WE Editor.app/Contents/Resources/wine")
#else
let base = root.appendingPathComponent("engines/Wine Devel.app/Contents/Resources/wine")
#endif
func path(_ relative: String) -> String { root.appendingPathComponent(relative).path }
#if arch(arm64)
let wineUnixArchitecture = "aarch64-unix"
#else
let wineUnixArchitecture = "x86_64-unix"
#endif
func systemLanguage() -> String {
    for language in Locale.preferredLanguages {
        let tag = language.lowercased()
        if tag.hasPrefix("zh") { return tag.contains("hant") || tag.contains("-tw") || tag.contains("-hk") || tag.contains("-mo") ? "zh-cht" : "zh-chs" }
        let locale = root.appendingPathComponent("windows/WallpaperEngine/locale")
        if files.fileExists(atPath: locale.appendingPathComponent("ui_\(tag).json").path) { return tag }
        let prefix = "ui_" + tag.split(separator: "-")[0] + "-"
        if let match = (try? files.contentsOfDirectory(atPath: locale.path))?.sorted().first(where: { $0.hasPrefix(prefix) && $0.hasSuffix(".json") }) { return String(match.dropFirst(3).dropLast(5)) }
    }
    return "en-us"
}
func run() throws -> Never {
    #if MIRAGE_MANAGED_EDITOR
    let launchArguments = CommandLine.arguments
    guard let index = launchArguments.firstIndex(of: "--launch-ticket"), index + 1 < launchArguments.count,
          UUID(uuidString: launchArguments[index + 1]) != nil else {
        NSWorkspace.shared.open(URL(string: "mirage://editor")!)
        exit(0)
    }
    let ticket = root.appendingPathComponent("launch-" + launchArguments[index + 1] + ".json")
    let ticketDescriptor = Darwin.open(ticket.path, O_RDONLY | O_NOFOLLOW)
    guard ticketDescriptor >= 0, flock(ticketDescriptor, LOCK_EX | LOCK_NB) == 0 else { throw POSIXError(.EACCES) }
    let handle = FileHandle(fileDescriptor: ticketDescriptor, closeOnDealloc: true)
    let permit = try JSONSerialization.jsonObject(with: handle.readToEnd() ?? Data()) as? [String: Any]
    try files.removeItem(at: ticket)
    try handle.close()
    guard let expiry = permit?["expires"] as? Double, expiry > Date().timeIntervalSince1970,
          expiry < Date().timeIntervalSince1970 + 60,
          permit?["runtime"] as? String == root.standardizedFileURL.path else { throw POSIXError(.EACCES) }
    #endif
    // Reopening the launcher must activate the editor, not start a second CEF
    // instance against the same profile and replace its native bridge session.
    if let existing = NSRunningApplication.runningApplications(withBundleIdentifier: "cn.laobamac.Mirage.WEEditor.Editor")
        .first(where: { $0.processIdentifier != getpid() && !$0.isTerminated }) {
        existing.activate(options: [.activateAllWindows])
        exit(0)
    }
    let descriptor = root.appendingPathComponent("bridge-session.json")
    if let data = try? Data(contentsOf: descriptor), let bridge = try? JSONSerialization.jsonObject(with: data) as? [String: String],
       let endpoint = bridge["endpoint"], let token = bridge["token"], let url = URL(string: endpoint + "/shutdown") {
        var request = URLRequest(url: url, timeoutInterval: 2)
        request.httpMethod = "POST"; request.setValue("Bearer " + token, forHTTPHeaderField: "Authorization")
        let completed = DispatchSemaphore(value: 0)
        URLSession.shared.dataTask(with: request) { _, _, _ in completed.signal() }.resume()
        _ = completed.wait(timeout: .now() + 3)
    }
    try? files.removeItem(at: descriptor)
    try files.createDirectory(atPath: path("logs"), withIntermediateDirectories: true)
    let bridge = Process()
    bridge.executableURL = root.appendingPathComponent("Mirage Project Picker.app/Contents/MacOS/ProjectPicker")
    let bridgeLog = path("logs/bridge-native.log")
    if !files.fileExists(atPath: bridgeLog) { files.createFile(atPath: bridgeLog, contents: nil) }
    let bridgeOutput = try FileHandle(forWritingTo: URL(fileURLWithPath: bridgeLog))
    bridgeOutput.seekToEndOfFile(); bridge.standardOutput = bridgeOutput; bridge.standardError = bridgeOutput
    try bridge.run()
    let deadline = Date().addingTimeInterval(8)
    while !files.fileExists(atPath: descriptor.path) {
        guard bridge.isRunning && Date() < deadline else { throw NSError(domain: "MirageEditor", code: 1, userInfo: [NSLocalizedDescriptionKey:"The native project picker did not start."]) }
        Thread.sleep(forTimeInterval: 0.05)
    }
    let bridgeState = try JSONSerialization.jsonObject(with: Data(contentsOf: descriptor))
    if let state = bridgeState as? [String: String], let token = state["token"] {
        setenv("MIRAGE_EDITOR_SESSION_TOKEN", token, 1)
    }
    let defaults = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("standalone-defaults.json")))
    let language = systemLanguage()
    let session: [String: Any] = ["language":language, "bridge":bridgeState, "defaults":defaults]
    let data = try JSONSerialization.data(withJSONObject: session, options: [.sortedKeys])
    let source = "// Copyright © 2026 王孝慈. All rights reserved.\nwindow.mirageEditorSession = " + String(decoding:data,as:UTF8.self) + ";\n"
    try source.write(toFile:path("windows/WallpaperEngine/ui/dist/scripts/mirage-session.js"),atomically:true,encoding:.utf8)
    for cache in ["Cache", "Code Cache"] { try? files.removeItem(atPath:path("windows/WallpaperEngine/ui/uicache/editor/base/Default/" + cache)) }
    let environment = [
        "WINEPREFIX":path("prefix-devel"), "WINEARCH":"win64", "WINEDEBUG":"fixme-all",
        "WINEDLLOVERRIDES":"winemenubuilder.exe=d;d3d11,d3d10core=n,b;dxgi,vulkan-1=b",
        "DXVK_CONFIG_FILE":"Z:" + path("dxvk.conf"), "DXVK_LOG_PATH":path("logs"), "DXVK_LOG_LEVEL":"error", "MVK_CONFIG_LOG_LEVEL":"1",
        "VK_DRIVER_FILES":base.appendingPathComponent("lib/wine/\(wineUnixArchitecture)/vulkan/icd.d/MoltenVK_icd.json").path,
        "WINELOADER":path("Mirage WE Editor.app/Contents/MacOS/wine"),
        "MIRAGE_WE_EDITOR_LOADER":path("Mirage WE Editor.app/Contents/MacOS/wine"),
        "DYLD_INSERT_LIBRARIES":path("MetalInterop.dylib"),
        "PATH":base.appendingPathComponent("bin").path + ":/usr/bin:/bin:/usr/sbin:/sbin"
    ]
    for (key,value) in environment { setenv(key,value,1) }
    #if arch(arm64)
    setenv("DYLD_LIBRARY_PATH", base.appendingPathComponent("lib/external").path, 1)
    #endif
    let log = Darwin.open(path("logs/editor.log"),O_WRONLY|O_CREAT|O_APPEND,0o600)
    if log >= 0 { dup2(log,STDOUT_FILENO); dup2(log,STDERR_FILENO); close(log) }
    guard chdir(path("windows/WallpaperEngine/bin")) == 0 else { throw POSIXError(.ENOENT) }
    let arguments = [path("Mirage WE Editor.app/Contents/MacOS/wine"), "M:\\WallpaperEngine\\bin\\wallpaperui.exe",
        "-weuimain", "-window", "editor", "-buildv", "20000800000042", "-skin", "mirage", "-language", language, "-steamlanguage", language,
        "--in-process-gpu", "--disable-gpu-driver-bug-workarounds", "--use-cmd-decoder=validating", "--use-angle=d3d11", "--disable-software-rasterizer", "--disable-direct-composition", "--ignore-gpu-blocklist",
        "--enable-gpu-rasterization", "--disable-background-networking", "--disable-component-update", "--disable-sync",
        "--disable-features=CalculateNativeWinOcclusion,RemoveRedirectionBitmap"]
    let pointers = arguments.map { strdup($0) } + [nil]
    pointers.withUnsafeBufferPointer { _ = execv(arguments[0], $0.baseAddress!) }
    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
}
do { try run() } catch {
    NSApplication.shared.setActivationPolicy(.accessory)
    let alert = NSAlert()
    let language = systemLanguage()
    alert.messageText = language == "zh-chs" ? "编辑器启动失败" : language == "zh-cht" ? "編輯器啟動失敗" : "Unable to Start Editor"
    alert.informativeText = error.localizedDescription
    alert.runModal()
    exit(1)
}
