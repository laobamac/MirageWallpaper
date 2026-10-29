// Copyright © 2026 王孝慈. All rights reserved.
import Foundation
import CryptoKit

func L(_ key: String, _ arguments: CVarArg...) -> String { key }

final class FixtureProtocol: URLProtocol, @unchecked Sendable {
    final class State: @unchecked Sendable {
        let lock = NSLock()
        var handler: (@Sendable (FixtureProtocol) -> Void)?
    }
    static let state = State()
    static func respond(_ handler: @escaping @Sendable (FixtureProtocol) -> Void) {
        state.lock.lock(); state.handler = handler; state.lock.unlock()
    }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.state.lock.lock(); let handler = Self.state.handler; Self.state.lock.unlock()
        handler!(self)
    }
    override func stopLoading() {}
    func send(_ data: Data, status: Int = 200, headers: [String: String] = [:], finish: Bool = true) {
        var headers = headers
        if headers["Content-Length"] == nil { headers["Content-Length"] = String(data.count) }
        let response = HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers)!
        client!.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client!.urlProtocol(self, didLoad: data)
        if finish { client!.urlProtocolDidFinishLoading(self) }
    }
}

@main struct EditorRuntimeDownloadTests {
    static let url = URL(string: "https://github.com/laobamac/MirageWallpaper/releases/download/test/runtime.tar.gz")!
    static func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func log(_ message: String) { FileHandle.standardError.write(Data((message + "\n").utf8)) }
    static func expectContents(_ file: URL, _ expected: Data) throws {
        let actual = try Data(contentsOf: file)
        guard actual == expected else {
            throw NSError(domain: "Unexpected transfer contents: \(actual.count) bytes, expected \(expected.count)", code: 1)
        }
    }
    static func configuration() -> URLSessionConfiguration {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [FixtureProtocol.self]
        return configuration
    }
    static func main() async throws {
        let fm = FileManager.default
        let root = EditorInstallation.root
        try EditorInstallation.validateRoot()
        let bytes = Data("a complete resumable runtime download".utf8)
        let archive = EditorRuntimeArchive(url: url, sha256: digest(bytes), bytes: Int64(bytes.count))
        let partial = root.appendingPathComponent("transfer.partial")
        try bytes.prefix(7).write(to: partial)
        FixtureProtocol.respond { request in
            precondition(request.request.value(forHTTPHeaderField: "Range") == "bytes=7-")
            request.send(bytes.dropFirst(7), status: 206,
                         headers: ["Content-Range": "bytes 7-\(bytes.count - 1)/\(bytes.count)"])
        }
        try await EditorArchiveTransfer(archive: archive, partial: partial, configuration: configuration()) { _, _ in }.download()
        try expectContents(partial, bytes)
        log("Resumed range passed")

        try bytes.prefix(4).write(to: partial)
        FixtureProtocol.respond { request in request.send(bytes) }
        try await EditorArchiveTransfer(archive: archive, partial: partial, configuration: configuration()) { _, _ in }.download()
        try expectContents(partial, bytes)
        log("Ignored Range restart passed")

        try bytes.prefix(4).write(to: partial)
        FixtureProtocol.respond { request in
            request.send(bytes.dropFirst(4), status: 206, headers: ["Content-Range": "bytes 0-\(bytes.count - 5)/\(bytes.count)"])
        }
        do {
            try await EditorArchiveTransfer(archive: archive, partial: partial, configuration: configuration()) { _, _ in }.download()
            fatalError("Accepted a mismatched resumed range")
        } catch is EditorServiceError {}
        try expectContents(partial, bytes.prefix(4))
        log("Invalid range rejection passed")

        try Data().write(to: partial)
        let cancellationBytes = Data(repeating: 42, count: 128 * 1024)
        let cancellationArchive = EditorRuntimeArchive(url: url, sha256: digest(cancellationBytes), bytes: Int64(cancellationBytes.count))
        FixtureProtocol.respond { request in
            request.send(cancellationBytes.prefix(64 * 1024), headers: ["Content-Length": String(cancellationBytes.count)], finish: false)
        }
        let (updates, signal) = AsyncStream<Void>.makeStream()
        let transfer = try EditorArchiveTransfer(archive: cancellationArchive, partial: partial, configuration: configuration()) { count, _ in
            if count > 0 { signal.yield() }
        }
        let download = Task { try await transfer.download() }
        for await _ in updates { break }
        signal.finish()
        download.cancel()
        do { try await download.value; fatalError("Ignored cancellation") } catch is CancellationError {}
        try expectContents(partial, cancellationBytes.prefix(64 * 1024))
        print("HTTP resume, ignored Range, invalid Range and cancellation passed.")

        let fixture = root.appendingPathComponent("fixture/EditorSupport/runtime")
        try fm.createDirectory(at: fixture, withIntermediateDirectories: true)
        let value = Data("verified runtime".utf8)
        try value.write(to: fixture.appendingPathComponent("value"))
        try fm.createSymbolicLink(atPath: fixture.appendingPathComponent("alias").path, withDestinationPath: "value")
        let tar = root.appendingPathComponent("fixture.tar.gz")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        process.arguments = ["-czf", tar.path, "-C", root.appendingPathComponent("fixture").path, "EditorSupport"]
        try process.run(); process.waitUntilExit(); precondition(process.terminationStatus == 0)
        let tarBytes = try Data(contentsOf: tar)
        var manifest: [String: Any] = ["schema": 1, "identifier": "fixture", "architecture": EditorInstallation.architecture,
            "installedBytes": value.count, "compatibility": ["version": "2.8.42", "branch": "public", "sourceHashes": [:]],
            "files": ["runtime/value": digest(value)], "links": ["runtime/alias": "value"],
            "minimumMacOS": "26.5", "rosetta": false,
            "archive": ["url": url.absoluteString, "sha256": digest(tarBytes), "bytes": tarBytes.count]]
        let catalog = try JSONDecoder().decode(EditorCatalog.self, from: JSONSerialization.data(withJSONObject: manifest))
        FixtureProtocol.respond { request in request.send(tarBytes) }
        try await EditorRuntimeDownload.prepare(catalog: catalog, configuration: configuration()) { _ in }
        let installed = EditorInstallation.runtimeCache.appendingPathComponent(digest(tarBytes) + "/EditorSupport")
        try EditorInstallation.verifySupport(installed, catalog: catalog)
        FixtureProtocol.respond { _ in fatalError("Fetched an already cached runtime") }
        try await EditorRuntimeDownload.prepare(catalog: catalog, configuration: configuration()) { _ in }
        try Data("tampered".utf8).write(to: installed.appendingPathComponent("runtime/value"))
        try await EditorRuntimeDownload.prepare(catalog: catalog, configuration: configuration()) { _ in }
        try EditorInstallation.verifySupport(installed, catalog: catalog)

        manifest["archive"] = ["url": url.absoluteString, "sha256": String(repeating: "a", count: 64), "bytes": tarBytes.count]
        let invalid = try JSONDecoder().decode(EditorCatalog.self, from: JSONSerialization.data(withJSONObject: manifest))
        FixtureProtocol.respond { request in request.send(tarBytes) }
        do {
            try await EditorRuntimeDownload.prepare(catalog: invalid, configuration: configuration()) { _ in }
            fatalError("Installed a runtime with an invalid archive hash")
        } catch is EditorServiceError {}
        precondition(!fm.fileExists(atPath: EditorInstallation.runtimeCache.appendingPathComponent(String(repeating: "a", count: 64)).path))
        print("Archive integrity, extraction, offline reuse and damaged-cache repair passed.")
        let environment = ProcessInfo.processInfo.environment
        if let archivePath = environment["MIRAGE_RUNTIME_TEST_ARCHIVE"], let manifestPath = environment["MIRAGE_RUNTIME_TEST_MANIFEST"] {
            let metadata = try Data(contentsOf: URL(fileURLWithPath: manifestPath))
            let runtime = try JSONDecoder().decode(EditorCatalog.self, from: metadata)
            let bundled = EditorInstallation.bundledSupport!
            try fm.createDirectory(at: bundled, withIntermediateDirectories: true)
            try metadata.write(to: bundled.appendingPathComponent("catalog.json"))
            try fm.copyItem(at: URL(fileURLWithPath: archivePath),
                            to: EditorInstallation.runtimeDownloads.appendingPathComponent(runtime.archive!.sha256 + ".tar.gz"))
            FixtureProtocol.respond { _ in fatalError("Offline payload verification attempted a network request") }
            try await EditorRuntimeDownload.prepare(catalog: runtime, configuration: configuration()) { _ in }
            try EditorInstallation.verifySupport(EditorInstallation.support!, catalog: runtime)
            print("Full signed runtime archive and manifest verified offline.")
        }
    }
}
