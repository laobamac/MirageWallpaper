import AppKit
import Foundation

// This runner compiles the production log service without the app lifecycle.
func L(_ value: String) -> String { value }

@main
@MainActor
struct LogSafetyRegression {
    static func require(_ value: Bool, _ message: String) throws {
        if !value { throw NSError(domain: message, code: 1) }
    }

    static func export(_ service: MirageLogService, to url: URL) async throws -> URL {
        try await withCheckedThrowingContinuation { continuation in
            service.export(to: url) { continuation.resume(with: $0) }
        }
    }

    static func main() async throws {
        let root = FileManager.default.temporaryDirectory.appending(path: "MirageLogSafety-\(UUID())")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appending(path: "Logs")
        let service = MirageLogService(logDirectory: directory, capturesStandardStreams: false)
        service.start()
        let samples = [#"{"password":"fake-json-secret","steamAPIKey":"fake-steam-key"}"#,
                       "Authorization: Bearer fake-bearer-secret",
                       "Authorization: Basic ZmFrZS1iYXNpYw==", "Cookie: first=fake-cookie; second=fake-cookie-two",
                       #"{"Authorization":"Bearer fake-header-secret"}"#,
                       #"{"pin":"fake-pin","private_key":"fake-private","aesKey":"fake-aes"}"#, "password='fake quoted secret'", "token=fake-query-secret&other=keep"]
        samples.forEach { service.append($0) }
        let line = Data("中文 🐱 password=fake-split-secret\n".utf8)
        for byte in line { service.consume(Data([byte]), source: "stderr") }
        service.consume(Data("unfinished 中文".utf8), source: "stdout")
        service.consume(Data(), source: "stdout")
        service.consume(Data((String(repeating: "x", count: 70_000) + "password=fake-large-secret\n").utf8), source: "stderr")
        let destination = root.appending(path: "export.log")
        _ = try await export(service, to: destination)
        let text = try String(contentsOf: destination, encoding: .utf8)
        for secret in ["fake-json-secret", "fake-steam-key", "fake-bearer-secret", "fake-header-secret",
                       "fake-pin", "fake-private", "fake-aes", "fake quoted secret", "fake-query-secret", "fake-split-secret", "fake-large-secret"] {
            try require(!text.contains(secret), "Secret survived redaction: \(secret)")
        }
        try require(!text.contains("ZmFrZS1iYXNpYw==") && !text.contains("fake-cookie"), "Basic auth or Cookie header leaked")
        try require(text.contains("中文 🐱") && text.contains("unfinished 中文"), "Split UTF-8 or EOF tail was lost")
        try require(text.contains("other=keep") && text.contains("<redacted>"), "Redaction removed unrelated data")
        try require(text.contains("oversized log line omitted"), "Oversized stream line was not bounded")
        try require(service.visibleText.isEmpty, "Hidden logger published UI text")
        let permissions = try FileManager.default.attributesOfItem(atPath: destination.path)[.posixPermissions] as? NSNumber
        try require(permissions?.intValue == 0o600, "Export was readable by other local users")
        do {
            _ = try await export(service, to: root.appending(path: "missing/export.log"))
            throw NSError(domain: "Invalid export unexpectedly succeeded", code: 1)
        } catch {
            try require(service.lastError != nil, "Export failure was hidden")
        }
        try require(try String(contentsOf: destination, encoding: .utf8) == text, "Failed export changed existing output")
        let activeFile = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)[0]
        try FileManager.default.removeItem(at: activeFile)
        do {
            _ = try await export(service, to: destination)
            throw NSError(domain: "Missing source unexpectedly succeeded", code: 1)
        } catch {
            try require(try String(contentsOf: destination, encoding: .utf8) == text, "Read failure destroyed the previous export")
        }
        print("PASS: JSON/header redaction, split UTF-8, EOF, oversized lines, hidden UI and export errors")

        let rotatingDirectory = root.appending(path: "Rotating")
        let rotating = MirageLogService(logDirectory: rotatingDirectory, capturesStandardStreams: false,
                                       maximumFileBytes: 1024, maximumSessionFiles: 3)
        rotating.start()
        for index in 0..<50 { rotating.append("rotation-\(index) " + String(repeating: "z", count: 200)) }
        let rotatingExport = root.appending(path: "rotating.log")
        _ = try await export(rotating, to: rotatingExport)
        let files = try FileManager.default.contentsOfDirectory(at: rotatingDirectory, includingPropertiesForKeys: [.fileSizeKey])
        try require(files.count == 3, "Session retention exceeded its file count")
        for file in files {
            try require(try file.resourceValues(forKeys: [.fileSizeKey]).fileSize! <= 1024, "A log segment exceeded its byte limit")
        }
        let rotated = try String(contentsOf: rotatingExport, encoding: .utf8)
        try require(!rotated.contains("rotation-0 ") && rotated.contains("rotation-49 "), "Rotation did not retain the newest logs")
        let positions = (43..<50).compactMap { rotated.range(of: "rotation-\($0) ")?.lowerBound }
        try require(positions.count >= 3 && positions == positions.sorted(), "Export reordered retained segments")
        print("PASS: bounded disk segments, oldest-first eviction and chronological export")

        let historyDirectory = root.appending(path: "History")
        try FileManager.default.createDirectory(at: historyDirectory, withIntermediateDirectories: true)
        let old = historyDirectory.appending(path: "Session-old.log")
        FileManager.default.createFile(atPath: old.path, contents: Data())
        let oldHandle = try FileHandle(forWritingTo: old)
        try oldHandle.truncate(atOffset: 101 * 1024 * 1024)
        try oldHandle.close()
        let unrelated = historyDirectory.appending(path: "keep.txt")
        try Data("keep".utf8).write(to: unrelated)
        let history = MirageLogService(logDirectory: historyDirectory, capturesStandardStreams: false)
        history.start()
        _ = try await export(history, to: root.appending(path: "history.log"))
        try require(!FileManager.default.fileExists(atPath: old.path) && FileManager.default.fileExists(atPath: unrelated.path),
                    "History budget either retained oversized logs or deleted unrelated files")
        let autoDirectory = root.appending(path: "Automatic")
        try FileManager.default.createDirectory(at: autoDirectory, withIntermediateDirectories: true)
        let oldAuto = autoDirectory.appending(path: "Mirage-Auto-\(UUID().uuidString).log")
        try Data().write(to: oldAuto)
        let autoHandle = try FileHandle(forWritingTo: oldAuto)
        try autoHandle.truncate(atOffset: 101 * 1024 * 1024)
        try autoHandle.close()
        let manual = autoDirectory.appending(path: "Mirage-Auto-user-export.log")
        try Data("keep".utf8).write(to: manual)
        let automatic = MirageLogService(logDirectory: root.appending(path: "AutoSource"), capturesStandardStreams: false,
                                         automaticSaveDirectory: autoDirectory)
        automatic.start()
        automatic.append("automatic retention")
        let automaticURL = automatic.saveAutomatically()!
        try require(!FileManager.default.fileExists(atPath: oldAuto.path), "automatic copies exceeded budget")
        try require(FileManager.default.fileExists(atPath: manual.path), "deleted a manual export")
        try require(automatic.saveAutomatically() == automaticURL, "automatic saves accumulated duplicate copies")
        print("PASS: automatic-copy retention and manual-export preservation")
        print("PASS: historical disk budget and unrelated file preservation")
    }
}
