// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.

import AppKit
import Network
import UniformTypeIdentifiers

// Authenticated loopback IPC for native project selection / 原生工程选择通信。
final class NativeBridge: NSObject, NSApplicationDelegate {
    let root = Bundle.main.bundleURL.deletingLastPathComponent()
    let token = UUID().uuidString + UUID().uuidString
    let queue = DispatchQueue(label: "cn.laobamac.Mirage.WEEditor.Bridge")
    var listener: NWListener?
    var choosing = false
    var appearanceObserver: NSObjectProtocol?
    var appearanceWaiters: [UUID: NWConnection] = [:]
    #if MIRAGE_MANAGED_EDITOR
    private var editorLifetime: DispatchSourceProcess?
    #endif
    var appearance: String {
        NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? "dark" : "light"
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        #if MIRAGE_MANAGED_EDITOR
        let parent = getppid()
        guard parent > 1 else { NSApp.terminate(nil); return }
        let lifetime = DispatchSource.makeProcessSource(identifier: parent, eventMask: .exit, queue: .main)
        lifetime.setEventHandler { NSApp.terminate(nil) }
        editorLifetime = lifetime
        lifetime.resume()
        #endif
        appearanceObserver = DistributedNotificationCenter.default().addObserver(forName: NSNotification.Name("AppleInterfaceThemeChangedNotification"), object: nil, queue: .main) { [weak self] _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { self?.publishAppearance() }
        }
        do {
            let parameters = NWParameters.tcp
            parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
            let server = try NWListener(using: parameters)
            listener = server
            server.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                if case .ready = state, let port = server.port {
                    let state: [String: Any] = ["endpoint": "http://127.0.0.1:\(port.rawValue)", "token": self.token]
                    do {
                        let data = try JSONSerialization.data(withJSONObject: state)
                        try data.write(to: self.root.appendingPathComponent("bridge-session.json"), options: .atomic)
                        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: self.root.appendingPathComponent("bridge-session.json").path)
                    } catch { DispatchQueue.main.async { NSApp.terminate(nil) } }
                }
            }
            server.newConnectionHandler = { [weak self] connection in
                guard let self else { return }
                connection.start(queue: self.queue)
                self.receive(connection, data: Data())
            }
            server.start(queue: queue)
        } catch { NSApp.terminate(nil) }
    }

    func publishAppearance() {
        let waiting = appearanceWaiters
        appearanceWaiters.removeAll()
        for connection in waiting.values { reply(connection, object: ["appearance": appearance]) }
    }

    func watchAppearance(_ connection: NWConnection, previous: String?) {
        if previous != appearance { reply(connection, object: ["appearance": appearance]); return }
        let id = UUID()
        appearanceWaiters[id] = connection
        connection.stateUpdateHandler = { [weak self] state in
            if case .cancelled = state { DispatchQueue.main.async { self?.appearanceWaiters.removeValue(forKey: id) } }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 25) { [weak self] in
            guard let self, let waiting = self.appearanceWaiters.removeValue(forKey: id) else { return }
            self.reply(waiting, object: ["appearance": self.appearance])
        }
    }

    func receive(_ connection: NWConnection, data: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] next, _, done, error in
            guard let self else { return }
            var buffer = data
            if let next { buffer.append(next) }
            if buffer.count > 32768 { connection.cancel(); return }
            if let boundary = buffer.range(of: Data("\r\n\r\n".utf8)),
               let header = String(data: buffer[..<boundary.lowerBound], encoding: .utf8) {
                let contentLength = header.components(separatedBy: "\r\n").first(where: { $0.lowercased().hasPrefix("content-length:") })
                    .flatMap { Int($0.split(separator: ":", maxSplits: 1)[1].trimmingCharacters(in: .whitespaces)) } ?? 0
                guard contentLength >= 0 && contentLength <= 16384 else { connection.cancel(); return }
                if buffer.count >= boundary.upperBound + contentLength, let request = String(data: buffer, encoding: .utf8) {
                    self.handle(connection, request: request)
                } else if !done && error == nil { self.receive(connection, data: buffer) }
                else { connection.cancel() }
            } else if !done && error == nil { self.receive(connection, data: buffer) }
            else { connection.cancel() }
        }
    }

    func reply(_ connection: NWConnection, status: String = "200 OK", object: [String: Any] = [:]) {
        let body = (try? JSONSerialization.data(withJSONObject: object)) ?? Data("{}".utf8)
        let headers = "HTTP/1.1 \(status)\r\nContent-Type: application/json\r\nContent-Length: \(body.count)\r\nAccess-Control-Allow-Origin: http://wpx.internal\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type, X-Mirage-Appearance\r\nAccess-Control-Allow-Private-Network: true\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
        var data = Data(headers.utf8)
        data.append(body)
        connection.send(content: data, completion: .contentProcessed { _ in connection.cancel() })
    }

    func handle(_ connection: NWConnection, request: String) {
        let lines = request.components(separatedBy: "\r\n")
        let first = lines.first ?? ""
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[String(line[..<colon]).lowercased()] = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
        }
        if let origin = headers["origin"], origin != "http://wpx.internal" {
            reply(connection, status: "403 Forbidden"); return
        }
        if first.hasPrefix("OPTIONS ") { reply(connection); return }
        guard headers["authorization"] == "Bearer " + token else {
            reply(connection, status: "403 Forbidden"); return
        }
        if first.hasPrefix("POST /appearance ") {
            DispatchQueue.main.async { self.watchAppearance(connection, previous: headers["x-mirage-appearance"]) }
            return
        }
        if first.hasPrefix("POST /window/zoom ") {
            DispatchQueue.main.async {
                DistributedNotificationCenter.default().postNotificationName(NSNotification.Name("cn.laobamac.Mirage.Editor.Zoom"), object: self.token, userInfo: nil, deliverImmediately: true)
                self.reply(connection)
            }
            return
        }
        if first.hasPrefix("POST /about ") {
            DispatchQueue.main.async {
                let language = Locale.preferredLanguages.first ?? "en"
                let chinese = language.hasPrefix("zh")
                let traditional = language.contains("Hant") || language.contains("TW") || language.contains("HK")
                let alert = NSAlert()
                alert.messageText = "Mirage Editor"
                let version = chinese ? (traditional ? "版本 0.1 · 預覽版" : "版本 0.1 · 预览版") : "Version 0.1 · Preview"
                let description = chinese ? (traditional ? "獨立壁紙編輯器" : "独立壁纸编辑器") : "Standalone wallpaper editor"
                alert.informativeText = "\(version)\n\n\(description)\n\nCopyright © 2026 王孝慈.\nAll rights reserved."
                alert.icon = NSImage(contentsOf: self.root.appendingPathComponent("MirageEditor.icns"))
                alert.addButton(withTitle: chinese ? (traditional ? "好" : "好") : "OK")
                NSApp.activate(ignoringOtherApps: true)
                alert.runModal()
                self.reply(connection)
                NSRunningApplication.runningApplications(withBundleIdentifier: "cn.laobamac.Mirage.WEEditor.Editor").first?.activate(options: [.activateAllWindows])
            }
            return
        }
        if first.hasPrefix("POST /shutdown ") {
            reply(connection)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { NSApp.terminate(nil) }
            return
        }
        guard first.hasPrefix("POST /open-project ") else {
            reply(connection, status: "404 Not Found"); return
        }
        DispatchQueue.main.async { self.chooseProject(connection) }
    }

    func chooseProject(_ connection: NWConnection) {
        guard !choosing else { reply(connection, status: "409 Conflict"); return }
        choosing = true
        defer { choosing = false }
        let language = Locale.preferredLanguages.first ?? "en"
        let chinese = language.hasPrefix("zh")
        let traditional = language.contains("Hant") || language.contains("TW") || language.contains("HK")
        let panel = NSOpenPanel()
        panel.title = chinese ? (traditional ? "開啟壁紙工程" : "打开壁纸工程") : "Open Wallpaper Project"
        panel.message = chinese ? (traditional ? "選擇工程資料夾或 project.json" : "选择工程文件夹或 project.json") : "Choose a project folder or project.json"
        panel.prompt = chinese ? (traditional ? "開啟" : "打开") : "Open"
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.json]
        NSApp.activate(ignoringOtherApps: true)
        var result: [String: Any] = ["cancelled": true]
        if panel.runModal() == .OK, let selection = panel.url {
            var isDirectory: ObjCBool = false
            _ = FileManager.default.fileExists(atPath: selection.path, isDirectory: &isDirectory)
            let project = isDirectory.boolValue ? selection.appendingPathComponent("project.json") : selection
            do {
                let data = try Data(contentsOf: project)
                guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                      object["file"] is String else { throw CocoaError(.fileReadCorruptFile) }
                result = ["file": "Z:" + project.path, "cancelled": false]
            } catch {
                result = ["error": chinese ? (traditional ? "所選檔案不是有效的壁紙工程。" : "所选文件不是有效的壁纸工程。") : "The selected file is not a valid wallpaper project."]
            }
        }
        reply(connection, object: result)
        NSRunningApplication.runningApplications(withBundleIdentifier: "cn.laobamac.Mirage.WEEditor.Editor").first?.activate(options: [.activateAllWindows])
    }
}

let app = NSApplication.shared
let delegate = NativeBridge()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
