// Copyright © 2026 王孝慈. All rights reserved.
import CoreFoundation
import Foundation

/// Reads explicit timing metadata; procedural scripts without a declared cycle use a bounded fallback.
enum SceneMobileTiming {
    struct Summary {
        var loops: [Double] = []
        var oneShots: [Double] = []
        var cameraDuration = 0.0
        var references = Set<String>()
    }

    static func summary(of value: Any) -> Summary {
        var result = Summary()
        func visit(_ value: Any, depth: Int, field: String = "") {
            guard depth <= 64 else { return }
            if let object = value as? [String: Any] {
                if object["visible"] as? Bool == false { return }
                if let curve = object["animation"] as? [String: Any] {
                    let options = curve["options"] as? [String: Any] ?? [:]
                    if options["startpaused"] as? Bool != true {
                        let declaredFPS = number(options["fps"]) ?? 30
                        let fps = declaredFPS > 0 ? declaredFPS : 30
                        let lastFrame = ["c0", "c1", "c2"].flatMap { curve[$0] as? [[String: Any]] ?? [] }
                            .compactMap { number($0["frame"]) }.max() ?? 0
                        let length = max(number(options["length"]) ?? 0, lastFrame)
                        if fps > 0, length > 0 {
                            let mode = options["mode"] as? String ?? ""
                            let duration = length / fps
                            if mode == "mirror" { result.loops.append(duration * 2) }
                            else if ["loop", "repeat"].contains(mode) || options["wraploop"] as? Bool == true {
                                result.loops.append(duration)
                            } else { result.oneShots.append(duration) }
                        }
                    }
                }
                // The renderer concatenates camera path tracks and plays their timestamps at 1 Hz.
                if let paths = object["paths"] as? [[String: Any]] {
                    for path in paths {
                        guard let transforms = path["transforms"] as? [[String: Any]], !transforms.isEmpty else { continue }
                        let declared = number(path["duration"]) ?? 0
                        let last = transforms.compactMap { number($0["timestamp"]) }.max() ?? 0
                        result.cameraDuration += declared > 0 ? declared : max(1, last)
                    }
                }
                for (key, child) in object where !["animation", "script", "scriptproperties"].contains(key) {
                    visit(child, depth: depth + 1, field: key)
                }
            } else if let array = value as? [Any] {
                for child in array { visit(child, depth: depth + 1, field: field) }
            } else if let name = value as? String, name.count <= 1024,
                      !name.hasPrefix("_rt_"), !name.contains("\n") {
                let suffix = (name as NSString).pathExtension.lowercased()
                if ["json", "tex", "mp4", "mov", "m4v"].contains(suffix) ||
                    ["image", "material", "texture", "textures", "model", "file"].contains(field) {
                    result.references.insert(name)
                }
            }
        }
        visit(value, depth: 0)
        return result
    }

    static func duration(loops: [Double], oneShots: [Double], fps: Int, speed: Double) -> Double {
        guard [24, 30, 60].contains(fps), speed.isFinite, (0.1...4).contains(speed) else { return 30 }
        let frameTime = speed / Double(fps)
        let limit = 600 * speed
        let periods = Array(Set(loops.filter { $0.isFinite && $0 > 0 }.map { max(frameTime, $0) })).sorted()
        let longestOneShot = min(limit, oneShots.filter { $0.isFinite && $0 > 0 }.max() ?? 0)
        guard !periods.isEmpty || longestOneShot > 0 else { return 30 }
        let minimum = max(speed, longestOneShot)
        var duration = minimum
        if let longest = periods.last {
            if longest >= limit { return 600 }
            duration = max(minimum, longest)
            let first = max(1, Int(ceil(minimum / longest)))
            let last = Int(floor(limit / longest))
            // Find a common cycle before applying speed or rounding to output frames.
            // Quantizing each period first creates artificial, very long cycles at speeds such as 1.1.
            if first <= last {
                for multiplier in first...last {
                    let candidate = Double(multiplier) * longest
                    if periods.allSatisfy({ period in
                        abs(candidate - (candidate / period).rounded() * period) <= frameTime / 2 + 1e-9
                    }) {
                        duration = candidate
                        break
                    }
                }
            }
        }
        let frames = min(600 * fps, max(fps, Int((duration / frameTime).rounded()),
            Int(ceil(longestOneShot / frameTime))))
        return Double(frames) / Double(fps)
    }

    private static func number(_ value: Any?) -> Double? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              number.doubleValue.isFinite else { return nil }
        return number.doubleValue
    }

    struct TextureSource: Hashable {
        let url: URL
        let offset: UInt64
        let size: UInt64
    }

    /// Skip texture pixels and read sprite frame times or stream an embedded video to a temporary file.
    static func textureTiming(_ source: TextureSource, in staging: URL) throws -> (period: Double?, video: URL?) {
        let handle = try FileHandle(forReadingFrom: source.url)
        defer { try? handle.close() }
        guard source.offset <= UInt64.max - source.size else { throw CocoaError(.fileReadCorruptFile) }
        let end = source.offset + source.size
        try handle.seek(toOffset: source.offset)
        func read(_ count: Int) throws -> Data {
            let offset = try handle.offset()
            guard count >= 0, offset <= end, UInt64(count) <= end - offset,
                  let data = try handle.read(upToCount: count), data.count == count else {
                throw CocoaError(.fileReadCorruptFile)
            }
            return data
        }
        func integer() throws -> UInt32 {
            try read(4).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian }
        }
        func stamp(_ prefix: String) throws -> Int {
            let data = try read(9)
            guard data.last == 0, let name = String(data: data.dropLast(), encoding: .ascii),
                  name.hasPrefix(prefix), let version = Int(name.suffix(4)) else {
                throw CocoaError(.fileReadCorruptFile)
            }
            return version
        }
        guard try stamp("TEXV") == 5, try stamp("TEXI") == 1 else { throw CocoaError(.fileReadCorruptFile) }
        _ = try integer() // Format
        let flags = try integer()
        _ = try read(20) // Width, height, mapping dimensions, reserved
        let version = try stamp("TEXB")
        guard (1...4).contains(version) else { throw CocoaError(.fileReadCorruptFile) }
        let slots = try integer()
        guard (1...4096).contains(slots) else { throw CocoaError(.fileReadCorruptFile) }
        let imageType = version >= 3 ? try integer() : UInt32.max
        if version >= 4 { _ = try integer() }
        if flags & (1 << 2) == 0, imageType != UInt32.max, imageType != 100 { return (nil, nil) }
        for _ in 0..<slots {
            let mips = try integer()
            guard (1...64).contains(mips) else { throw CocoaError(.fileReadCorruptFile) }
            for _ in 0..<mips {
                _ = try read(8)
                let compressed = version >= 2 ? try integer() != 0 : false
                if version >= 2 { _ = try integer() }
                let size = UInt64(try integer()), start = try handle.offset()
                guard start <= end, size <= end - start else { throw CocoaError(.fileReadCorruptFile) }
                if !compressed, size >= 16, (imageType == UInt32.max || imageType == 100) {
                    let prefix = try read(16)
                    if prefix.subdata(in: 4..<8) == Data("ftyp".utf8) {
                        let video = staging.appendingPathComponent(UUID().uuidString + ".mp4")
                        var keepsVideo = false
                        defer { if !keepsVideo { try? FileManager.default.removeItem(at: video) } }
                        FileManager.default.createFile(atPath: video.path, contents: nil)
                        let output = try FileHandle(forWritingTo: video)
                        defer { try? output.close() }
                        try handle.seek(toOffset: start)
                        var remaining = size
                        while remaining > 0 {
                            let chunk = try read(Int(min(remaining, 1_048_576)))
                            try output.write(contentsOf: chunk)
                            remaining -= UInt64(chunk.count)
                        }
                        keepsVideo = true
                        return (nil, video)
                    }
                }
                try handle.seek(toOffset: start + size)
            }
        }
        guard flags & (1 << 2) != 0 else { return (nil, nil) }
        let spriteVersion = try stamp("TEXS")
        guard (1...3).contains(spriteVersion) else { throw CocoaError(.fileReadCorruptFile) }
        let count = try integer()
        guard (1...1_000_000).contains(count) else { throw CocoaError(.fileReadCorruptFile) }
        if spriteVersion >= 3 { _ = try read(8) }
        let tableStart = try handle.offset()
        guard tableStart <= end, UInt64(count) * 32 <= end - tableStart else { throw CocoaError(.fileReadCorruptFile) }
        var duration = 0.0, remaining = Int(count)
        while remaining > 0 {
            let batch = min(remaining, 4096)
            let data = try read(batch * 32)
            try data.withUnsafeBytes { bytes in
                for frame in 0..<batch {
                    let image = bytes.loadUnaligned(fromByteOffset: frame * 32, as: UInt32.self).littleEndian
                    guard image < slots else { continue } // The renderer skips invalid image IDs too.
                    let bits = bytes.loadUnaligned(fromByteOffset: frame * 32 + 4, as: UInt32.self).littleEndian
                    let seconds = Float(bitPattern: bits)
                    guard seconds.isFinite, seconds >= 0 else { throw CocoaError(.fileReadCorruptFile) }
                    duration += Double(seconds)
                }
            }
            remaining -= batch
        }
        return (duration > 0 ? duration : nil, nil)
    }
}
