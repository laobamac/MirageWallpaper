//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import Foundation
import ImageIO

@_silgen_name("MirageCopyNowPlayingJSON")
private func MirageCopyNowPlayingJSON() -> UnsafeMutablePointer<CChar>?

@_silgen_name("MirageFreeNowPlayingJSON")
private func MirageFreeNowPlayingJSON(_ value: UnsafeMutablePointer<CChar>)

final class NowPlayingService {
    static let shared = NowPlayingService()

    var onUpdate: (([String: Any]) -> Void)?

    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.now-playing", qos: .utility)
    private var timer: DispatchSourceTimer?
    private var lastState = -1
    private var lastArtworkURL = ""
    private var previousArtworkURL = ""
    private var lastMediaIdentity = ""

    private init() {}

    func setEnabled(_ enabled: Bool) {
        queue.async { [weak self] in
            guard let self else { return }
            if enabled {
                guard timer == nil else { return }
                let source = DispatchSource.makeTimerSource(queue: queue)
                source.schedule(deadline: .now(), repeating: 2)
                source.setEventHandler { [weak self] in self?.poll() }
                timer = source
                source.resume()
            } else {
                timer?.cancel()
                timer = nil
                lastState = -1
                lastArtworkURL = ""
                previousArtworkURL = ""
                lastMediaIdentity = ""
            }
        }
    }

    private func poll() {
        guard let data = currentPayloadData() else {
            publishStopped()
            return
        }
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let title = object["title"] as? String, !title.isEmpty else {
            publishStopped()
            return
        }
        let playing = (object["playing"] as? NSNumber)?.boolValue == true
        let identity = mediaIdentity(object)
        if identity != lastMediaIdentity {
            previousArtworkURL = lastArtworkURL
            lastArtworkURL = ""
            lastMediaIdentity = identity
        }
        var payload: [String: Any] = [
            "state": playing ? 1 : 2,
            "title": title,
            "artist": object["artist"] as? String ?? "",
            "album": object["album"] as? String ?? "",
            "albumArtist": object["albumArtist"] as? String ?? "",
            "position": (object["position"] as? NSNumber)?.doubleValue ?? 0,
            "duration": (object["duration"] as? NSNumber)?.doubleValue ?? 0
        ]
        if let encoded = object["artworkData"] as? String,
           let artwork = Data(base64Encoded: encoded), !artwork.isEmpty,
           let image = persistArtwork(artwork, mimeType: object["artworkMimeType"] as? String) {
            if image.url != lastArtworkURL {
                if !lastArtworkURL.isEmpty {
                    previousArtworkURL = lastArtworkURL
                }
                lastArtworkURL = image.url
            }
            payload["artURL"] = lastArtworkURL
            payload["previousArtURL"] = previousArtworkURL
            payload["primaryColor"] = image.colors[0]
            payload["secondaryColor"] = image.colors[1]
            payload["tertiaryColor"] = image.colors[2]
            payload["textColor"] = image.colors[3]
            payload["highContrastColor"] = image.colors[4]
        } else {
            payload["artURL"] = lastArtworkURL
            payload["previousArtURL"] = previousArtworkURL
        }
        lastState = playing ? 1 : 2
        onUpdate?(payload)
    }

    private func currentPayloadData() -> Data? {
        if #unavailable(macOS 15.4) {
            if let pointer = MirageCopyNowPlayingJSON() {
                let data = Data(bytes: pointer, count: strlen(pointer))
                MirageFreeNowPlayingJSON(pointer)
                return data
            }
        }
        guard let helper = Bundle.main.resourceURL?
            .appendingPathComponent("NowPlaying/libMirageNowPlaying.dylib"),
              FileManager.default.isReadableFile(atPath: helper.path) else { return nil }
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/perl")
        process.arguments = [
            "-MDynaLoader",
            "-e",
            "$h=DynaLoader::dl_load_file($ARGV[0],0) or exit 2;"
                + "$s=DynaLoader::dl_find_symbol($h,'MiragePrintNowPlayingJSON') or exit 3;"
                + "$f=DynaLoader::dl_install_xsub('main::mirage_now_playing',$s);"
                + "mirage_now_playing();",
            helper.path
        ]
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do { try process.run() } catch { return nil }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0,
              let text = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty, text != "null" else { return nil }
        return Data(text.utf8)
    }

    private func publishStopped() {
        guard lastState != 0 else { return }
        let previous = lastArtworkURL
        lastState = 0
        lastArtworkURL = ""
        previousArtworkURL = ""
        lastMediaIdentity = ""
        onUpdate?([
            "state": 0,
            "title": "",
            "artist": "",
            "album": "",
            "albumArtist": "",
            "position": 0,
            "duration": 0,
            "artURL": "",
            "previousArtURL": previous
        ])
    }

    private func mediaIdentity(_ object: [String: Any]) -> String {
        let title = object["title"] as? String ?? ""
        let artist = object["artist"] as? String ?? ""
        let album = object["album"] as? String ?? ""
        let albumArtist = object["albumArtist"] as? String ?? ""
        return [title, artist, album, albumArtist]
            .joined(separator: "\u{1f}")
    }

    private func persistArtwork(_ data: Data, mimeType: String?)
        -> (url: String, colors: [[Double]])? {
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let ext = mimeType?.lowercased().contains("png") == true ? "png" : "jpg"
        let directory = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Mirage/NowPlaying", isDirectory: true)
        let url = directory.appendingPathComponent("\(digest).\(ext)")
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: url.path) {
                try data.write(to: url, options: .atomic)
            }
        } catch {
            return nil
        }
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceThumbnailMaxPixelSize: 48,
                kCGImageSourceCreateThumbnailWithTransform: true
              ] as CFDictionary) else { return nil }
        return (url.path, palette(image))
    }

    private func palette(_ image: CGImage) -> [[Double]] {
        let side = 32
        var pixels = [UInt8](repeating: 0, count: side * side * 4)
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: &pixels, width: side, height: side, bitsPerComponent: 8,
                bytesPerRow: side * 4, space: space,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return fallbackPalette() }
        context.interpolationQuality = .medium
        context.draw(image, in: CGRect(x: 0, y: 0, width: side, height: side))
        var buckets: [Int: (count: Int, r: Double, g: Double, b: Double, score: Double)] = [:]
        for index in stride(from: 0, to: pixels.count, by: 4) where pixels[index + 3] > 96 {
            let r = Double(pixels[index]) / 255
            let g = Double(pixels[index + 1]) / 255
            let b = Double(pixels[index + 2]) / 255
            let key = (Int(r * 15) << 8) | (Int(g * 15) << 4) | Int(b * 15)
            let saturation = max(r, g, b) - min(r, g, b)
            var value = buckets[key] ?? (0, 0, 0, 0, 0)
            value.count += 1
            value.r += r
            value.g += g
            value.b += b
            value.score += 0.35 + saturation
            buckets[key] = value
        }
        let ranked = buckets.values.sorted { $0.score > $1.score }.map {
            [$0.r / Double($0.count), $0.g / Double($0.count), $0.b / Double($0.count)]
        }
        var selected: [[Double]] = []
        for color in ranked where selected.count < 3 {
            if selected.allSatisfy({ distance($0, color) > 0.16 }) { selected.append(color) }
        }
        guard let primary = selected.first ?? ranked.first else { return fallbackPalette() }
        while selected.count < 3 {
            let factor = selected.count == 1 ? 1.25 : 0.65
            selected.append(primary.map { min(max($0 * factor, 0), 1) })
        }
        let luminance = primary[0] * 0.2126 + primary[1] * 0.7152 + primary[2] * 0.0722
        let text = luminance > 0.5 ? [0.0, 0.0, 0.0] : [1.0, 1.0, 1.0]
        let contrast = luminance > 0.35 ? [0.0, 0.0, 0.0] : [1.0, 1.0, 1.0]
        return selected + [text, contrast]
    }

    private func distance(_ lhs: [Double], _ rhs: [Double]) -> Double {
        sqrt(zip(lhs, rhs).reduce(0) { $0 + pow($1.0 - $1.1, 2) })
    }

    private func fallbackPalette() -> [[Double]] {
        [[1, 1, 1], [0.3, 0.3, 0.3], [0.6, 0.6, 0.6], [0, 0, 0], [1, 1, 1]]
    }
}
