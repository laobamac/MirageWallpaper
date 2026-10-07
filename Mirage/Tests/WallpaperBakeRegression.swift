//
//  WallpaperBakeRegression.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import CryptoKit
import Foundation

@main
struct WallpaperBakeRegression {
    enum Cancelled: Error { case requested }

    static func main() throws {
        let directory = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let pipe = Pipe()
        let received = DispatchSemaphore(value: 0)
        let writerDone = DispatchSemaphore(value: 0)
        let first = Data("{\"event\":\"progress\",\"completed\":1,\"total\":3,\"label\":\"烘焙\"}\n".utf8)
        DispatchQueue.global().async {
            pipe.fileHandleForWriting.write(first.prefix(first.count - 4))
            Thread.sleep(forTimeInterval: 0.05)
            pipe.fileHandleForWriting.write(first.suffix(4))
            precondition(received.wait(timeout: .now() + 2) == .success, "Progress was buffered until EOF")
            pipe.fileHandleForWriting.write(Data("{\"event\":\"progress\",\"completed\":2,\"total\":3}\n{\"event\":\"complete\"}".utf8))
            try! pipe.fileHandleForWriting.close()
            writerDone.signal()
        }
        var events: [[String: Any]] = []
        try WallpaperBakeIO.readLines(from: pipe.fileHandleForReading, check: {}) { line in
            let event = try JSONSerialization.jsonObject(with: line) as! [String: Any]
            events.append(event)
            if events.count == 1 {
                precondition(event["label"] as? String == "烘焙")
                received.signal()
            }
        }
        precondition(writerDone.wait(timeout: .now() + 2) == .success)
        try pipe.fileHandleForReading.close()
        precondition(events.count == 3)
        precondition(events.last?["event"] as? String == "complete")
        precondition(WallpaperBakeProgress(event: events[0])?.fraction == 1.0 / 3.0)
        precondition(WallpaperBakeProgress(event: events[2]) == nil)
        precondition(WallpaperBakeProgress(event: ["completed": Double.nan, "total": 1]) == nil)
        precondition(WallpaperBakeProgress(event: ["completed": 1.0, "total": 0.0]) == nil)
        precondition(WallpaperBakeProgress(completed: 0, total: nil, unit: "files").fraction == nil)

        let oversized = directory.appendingPathComponent("oversized")
        try Data(repeating: 65, count: 1_048_577).write(to: oversized)
        let handle = try FileHandle(forReadingFrom: oversized)
        do {
            try WallpaperBakeIO.readLines(from: handle, check: {}, consume: { _ in })
            preconditionFailure("Oversized event accepted")
        } catch let error as CocoaError {
            precondition(error.code == .fileReadCorruptFile)
        }
        try handle.close()
        try FileManager.default.removeItem(at: oversized)

        let a = directory.appendingPathComponent("a")
        let b = directory.appendingPathComponent("b")
        let data = Data(repeating: 42, count: 3 * 1_048_576)
        try data.write(to: b)
        try data.write(to: a)
        try Data("hidden".utf8).write(to: directory.appendingPathComponent(".ignored"))
        var reports: [WallpaperBakeProgress] = []
        let digest = try WallpaperBakeIO.digest([directory], check: {}) { reports.append($0) }
        var expected = SHA256()
        for file in [a, b] {
            let canonical = realpath(file.path, nil)!
            defer { free(canonical) }
            expected.update(data: Data(String(cString: canonical).utf8))
            expected.update(data: Data([0]))
            expected.update(data: data)
        }
        precondition(digest == expected.finalize().map { String(format: "%02x", $0) }.joined())
        precondition(reports.first?.unit == "files" && reports.first?.fraction == nil)
        precondition(reports.last?.completed == Double(data.count * 2))
        precondition(reports.last?.fraction == 1)
        try Data("changed".utf8).write(to: a)
        let changed = try WallpaperBakeIO.digest([directory], check: {})
        precondition(changed != digest)
        var checks = 0
        do {
            _ = try WallpaperBakeIO.digest([directory], check: {
                checks += 1
                if checks == 3 { throw Cancelled.requested }
            })
            preconditionFailure("Cancellation was ignored during enumeration")
        } catch Cancelled.requested {}
        print("PASS: live progress, split UTF-8, batched events, EOF, malformed counts, size limit, digest compatibility, source changes, cancellation")
    }
}
