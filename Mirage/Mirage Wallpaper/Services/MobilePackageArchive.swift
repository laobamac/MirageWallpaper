import Darwin
import Foundation

/// Shared PKGM writer for file exports and device transfers.
enum MobilePackageArchive {
    struct Entry {
        enum Source {
            case file(URL, offset: UInt64, size: UInt64)
            case data(Data)
        }

        let name: String
        let source: Source

        var size: UInt64 {
            switch source {
            case .file(_, _, let size): return size
            case .data(let data): return UInt64(data.count)
            }
        }

        static func file(named name: String, at url: URL) throws -> Entry {
            let values = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, let size = values.fileSize, size >= 0 else {
                throw MobileMPKGExportError.cannotReadSource(name)
            }
            return Entry(name: name, source: .file(url, offset: 0, size: UInt64(size)))
        }
    }

    static func write(
        entries: [Entry],
        version: String,
        to outputURL: URL,
        progress: ((UInt64, UInt64) -> Void)? = nil
    ) throws {
        guard !entries.isEmpty, entries.count <= 100_000,
              version.range(of: #"^PKGM\d{4}$"#, options: .regularExpression) != nil else {
            throw MobileMPKGExportError.invalidProject
        }
        var names = Set<String>()
        var table: [(entry: Entry, name: Data, offset: UInt32)] = []
        var payloadSize: UInt64 = 0
        var headerSize = UInt64(8 + version.utf8.count)
        let destination = outputURL.standardizedFileURL
        for entry in entries {
            guard isSafeEntryName(entry.name), names.insert(entry.name.lowercased()).inserted else {
                throw MobileMPKGExportError.unsafeProjectPath
            }
            let name = Data(entry.name.utf8)
            guard name.count <= 1024, entry.size <= UInt64(UInt32.max),
                  payloadSize + entry.size <= UInt64(UInt32.max) else {
                throw MobileMPKGExportError.wallpaperTooLarge
            }
            if case .file(let url, _, _) = entry.source,
               url.standardizedFileURL.resolvingSymlinksInPath() == destination.resolvingSymlinksInPath() {
                throw MobileMPKGExportError.unsafeProjectPath
            }
            table.append((entry, name, UInt32(payloadSize)))
            payloadSize += entry.size
            headerSize += UInt64(12 + name.count)
        }
        // A file and a directory cannot share a path on the receiving device.
        for name in names {
            var components = name.split(separator: "/")
            while components.count > 1 {
                components.removeLast()
                guard !names.contains(components.joined(separator: "/")) else {
                    throw MobileMPKGExportError.unsafeProjectPath
                }
            }
        }
        guard headerSize + payloadSize <= UInt64(UInt32.max) else {
            throw MobileMPKGExportError.wallpaperTooLarge
        }

        let fm = FileManager.default
        try fm.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
        let temporary = destination.deletingLastPathComponent()
            .appending(path: ".\(destination.lastPathComponent).\(UUID().uuidString).tmp")
        guard fm.createFile(atPath: temporary.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        defer { try? fm.removeItem(at: temporary) }
        let output = try FileHandle(forWritingTo: temporary)
        var written: UInt64 = 0
        do {
            try output.write(contentsOf: word(UInt32(version.utf8.count)))
            try output.write(contentsOf: Data(version.utf8))
            try output.write(contentsOf: word(UInt32(table.count)))
            for item in table {
                try output.write(contentsOf: word(UInt32(item.name.count)))
                try output.write(contentsOf: item.name)
                try output.write(contentsOf: word(item.offset))
                try output.write(contentsOf: word(UInt32(item.entry.size)))
            }
            progress?(0, payloadSize)
            for item in table {
                switch item.entry.source {
                case .data(let data):
                    try output.write(contentsOf: data)
                    written += UInt64(data.count)
                    progress?(written, payloadSize)
                case .file(let url, let offset, let size):
                    let input = try FileHandle(forReadingFrom: url)
                    defer { try? input.close() }
                    try input.seek(toOffset: offset)
                    var remaining = size
                    while remaining > 0 {
                        let data = try input.read(upToCount: Int(min(remaining, 1024 * 1024))) ?? Data()
                        guard !data.isEmpty else {
                            throw MobileMPKGExportError.cannotReadSource(item.entry.name)
                        }
                        try output.write(contentsOf: data)
                        remaining -= UInt64(data.count)
                        written += UInt64(data.count)
                        progress?(written, payloadSize)
                    }
                }
            }
            try output.synchronize()
            try output.close()
        } catch {
            try? output.close()
            throw error
        }
        // Same-directory rename replaces atomically and keeps an existing export on failure.
        guard rename(temporary.path, destination.path) == 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }

    static func isSafeEntryName(_ name: String) -> Bool {
        guard !name.isEmpty, !name.contains("\0"), !name.contains("\\"), !name.hasPrefix("/") else {
            return false
        }
        return name.split(separator: "/", omittingEmptySubsequences: false)
            .allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
    }

    private static func word(_ value: UInt32) -> Data {
        var value = value.littleEndian
        return withUnsafeBytes(of: &value) { Data($0) }
    }
}
