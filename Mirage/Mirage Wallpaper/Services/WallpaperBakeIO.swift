//
//  WallpaperBakeIO.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import CryptoKit
import Darwin
import Foundation

struct WallpaperBakeProgress: Sendable {
    let completed: Double
    let total: Double?
    let unit: String?

    var fraction: Double? {
        guard completed.isFinite, completed >= 0, let total, total.isFinite, total > 0 else { return nil }
        return min(1, completed / total)
    }

    init(completed: Double, total: Double?, unit: String? = nil) {
        self.completed = completed
        self.total = total
        self.unit = unit
    }

    init?(event: [String: Any]) {
        guard let completed = event["completed"] as? Double, completed.isFinite, completed >= 0 else { return nil }
        let total = event["total"] as? Double
        if let total, !total.isFinite || total <= 0 { return nil }
        self.init(completed: completed, total: total, unit: event["unit"] as? String)
    }
}

enum WallpaperBakeIO {
    static func readLines(from handle: FileHandle, check: () throws -> Void,
                          consume: (Data) throws -> Void) throws {
        var bytes = [UInt8](repeating: 0, count: 8192)
        var buffer = Data()
        while true {
            try check()
            let count = Darwin.read(handle.fileDescriptor, &bytes, bytes.count)
            if count < 0 {
                if errno == EINTR { continue }
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
            }
            try check()
            if count == 0 {
                if !buffer.isEmpty { try consume(buffer) }
                return
            }
            buffer.append(contentsOf: bytes.prefix(count))
            while let newline = buffer.firstIndex(of: 10) {
                let line = Data(buffer.prefix(upTo: newline))
                guard line.count <= 1_048_576 else { throw CocoaError(.fileReadCorruptFile) }
                buffer.removeSubrange(...newline)
                try consume(line)
            }
            guard buffer.count <= 1_048_576 else { throw CocoaError(.fileReadCorruptFile) }
        }
    }

    static func digest(_ roots: [URL], check: () throws -> Void,
                       progress: (WallpaperBakeProgress) -> Void = { _ in }) throws -> String {
        var files: [URL] = []
        var discovered = 0
        var total = 0.0
        var reported = ProcessInfo.processInfo.systemUptime
        progress(WallpaperBakeProgress(completed: 0, total: nil, unit: "files"))
        for root in roots {
            try check()
            let candidates: [URL]
            if try root.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true {
                var enumerationError: Error?
                guard let enumerator = FileManager.default.enumerator(at: root,
                    includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey], options: [.skipsHiddenFiles],
                    errorHandler: { _, error in enumerationError = error; return false }) else {
                    throw CocoaError(.fileReadUnknown)
                }
                var entries: [URL] = []
                for case let file as URL in enumerator {
                    try check()
                    entries.append(file)
                    discovered += 1
                    let now = ProcessInfo.processInfo.systemUptime
                    if now - reported >= 0.1 {
                        progress(WallpaperBakeProgress(completed: Double(discovered), total: nil, unit: "files"))
                        reported = now
                    }
                }
                if let enumerationError { throw enumerationError }
                candidates = entries.sorted { $0.path < $1.path }
            } else {
                candidates = [root]
                discovered += 1
            }
            for file in candidates {
                try check()
                let values = try file.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                guard values.isRegularFile == true else { continue }
                files.append(file)
                total += Double(values.fileSize ?? 0)
            }
        }
        var hash = SHA256()
        var completed = 0.0
        progress(WallpaperBakeProgress(completed: completed, total: total, unit: "bytes"))
        for file in files {
            try check()
            hash.update(data: Data(file.path.utf8)); hash.update(data: Data([0]))
            let handle = try FileHandle(forReadingFrom: file)
            defer { try? handle.close() }
            while let data = try handle.read(upToCount: 1_048_576), !data.isEmpty {
                try check()
                hash.update(data: data)
                completed += Double(data.count)
                let now = ProcessInfo.processInfo.systemUptime
                if now - reported >= 0.1 {
                    progress(WallpaperBakeProgress(completed: completed, total: total, unit: "bytes"))
                    reported = now
                }
            }
        }
        try check()
        progress(WallpaperBakeProgress(completed: completed, total: total, unit: "bytes"))
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
