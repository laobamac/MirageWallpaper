//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import ImageIO
import SwiftUI

// Loads Workshop preview images with a two-tier cache (decoded NSImage in
// memory, original bytes on disk) and ImageIO downsampling. This replaces the
// plain SwiftUI `AsyncImage`, which re-downloads on every tab switch and never
// downsamples — the two causes of the blurry, stuttering card previews.
final class WorkshopImageLoader {
    static let shared = WorkshopImageLoader()

    // Decoded, downsampled images keyed by "url#pixelBucket". Bounded so the
    // browse/discover grids cannot grow memory without limit.
    private let memory: NSCache<NSString, NSImage> = {
        let cache = NSCache<NSString, NSImage>()
        cache.countLimit = 400
        cache.totalCostLimit = 160 * 1024 * 1024
        return cache
    }()

    private let ioQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.workshopImage", qos: .userInitiated, attributes: .concurrent)

    private let session: URLSession = {
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 40
        configuration.httpMaximumConnectionsPerHost = 6
        return URLSession(configuration: configuration)
    }()

    private let diskDirectory: URL = {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/WorkshopImageCache")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }()

    // Bucket the target to multiples of 64px so a handful of card sizes share
    // cache entries instead of fragmenting per pixel.
    private func maxPixel(for size: CGSize, scale: CGFloat) -> Int {
        let raw = Double(max(size.width, size.height)) * Double(max(scale, 1))
        let bucket = (raw / 64).rounded(.up) * 64
        return max(64, min(Int(bucket), 2048))
    }

    private func memoryKey(_ url: URL, _ px: Int) -> NSString {
        "\(url.absoluteString)#\(px)" as NSString
    }

    // Stable filename (SHA256) — unlike Swift's per-process-seeded String.hash,
    // this survives relaunches so the disk cache actually hits.
    private func diskURL(for url: URL) -> URL {
        let digest = SHA256.hash(data: Data(url.absoluteString.utf8))
            .map { String(format: "%02x", $0) }.joined()
        return diskDirectory.appending(path: digest)
    }

    private func cost(_ image: NSImage) -> Int {
        guard let rep = image.representations.first else { return 1 }
        return max(1, rep.pixelsWide * rep.pixelsHigh * 4)
    }

    /// Synchronous memory-cache probe so an already-decoded image shows without
    /// a placeholder flash.
    func cachedImage(url: URL, targetSize: CGSize, scale: CGFloat) -> NSImage? {
        memory.object(forKey: memoryKey(url, maxPixel(for: targetSize, scale: scale)))
    }

    func load(url: URL, targetSize: CGSize, scale: CGFloat,
              completion: @escaping (NSImage?) -> Void) {
        let px = maxPixel(for: targetSize, scale: scale)
        let key = memoryKey(url, px)
        if let image = memory.object(forKey: key) {
            completion(image)
            return
        }
        ioQueue.async { [weak self] in
            guard let self else { return }
            let disk = self.diskURL(for: url)
            if let data = try? Data(contentsOf: disk), let image = Self.downsample(data, maxPixel: px) {
                self.memory.setObject(image, forKey: key, cost: self.cost(image))
                DispatchQueue.main.async { completion(image) }
                return
            }
            self.session.dataTask(with: URLRequest(url: url)) { [weak self] data, response, _ in
                guard let self else { return }
                let ok = (response as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? true
                guard ok, let data, !data.isEmpty else {
                    DispatchQueue.main.async { completion(nil) }
                    return
                }
                try? data.write(to: disk, options: .atomic)
                let image = Self.downsample(data, maxPixel: px)
                if let image {
                    self.memory.setObject(image, forKey: key, cost: self.cost(image))
                }
                DispatchQueue.main.async { completion(image) }
            }.resume()
        }
    }

    private static func downsample(_ data: Data, maxPixel: Int) -> NSImage? {
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions) else {
            return NSImage(data: data)
        }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
            return NSImage(data: data)
        }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }
}

// A drop-in preview image that fills the space its parent gives it, staying
// sharp (downsampled to the displayed size) and never re-downloading across
// tab switches. Recycled cells discard stale in-flight loads via a token.
struct WorkshopImage: View {
    let url: URL?
    var contentMode: ContentMode = .fill

    @State private var image: NSImage?
    @State private var failed = false
    @State private var boxSize: CGSize = .zero
    @State private var loadToken: UInt64 = 0

    var body: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.10))
            .overlay {
                if let image {
                    Image(nsImage: image)
                        .resizable()
                        .interpolation(.high)
                        .aspectRatio(contentMode: contentMode)
                } else if failed {
                    Image(systemName: "photo")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                } else {
                    ProgressView()
                        .controlSize(.small)
                }
            }
            .clipped()
            .background(
                GeometryReader { proxy in
                    Color.clear
                        .onAppear {
                            boxSize = proxy.size
                            load()
                        }
                        .onChange(of: proxy.size) { _, newValue in
                            guard abs(newValue.width - boxSize.width) > 1 ||
                                  abs(newValue.height - boxSize.height) > 1 else { return }
                            boxSize = newValue
                            load()
                        }
                }
            )
            .onChange(of: url) { _, _ in
                image = nil
                failed = false
                load()
            }
    }

    private func load() {
        guard let url, boxSize.width > 1, boxSize.height > 1 else { return }
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        if let cached = WorkshopImageLoader.shared.cachedImage(url: url, targetSize: boxSize, scale: scale) {
            image = cached
            failed = false
            return
        }
        loadToken &+= 1
        let token = loadToken
        let requestedURL = url
        WorkshopImageLoader.shared.load(url: requestedURL, targetSize: boxSize, scale: scale) { loaded in
            guard token == loadToken, requestedURL == url else { return }
            if let loaded {
                image = loaded
                failed = false
            } else {
                failed = true
            }
        }
    }
}
