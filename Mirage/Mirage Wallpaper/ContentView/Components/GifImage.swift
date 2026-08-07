//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import ImageIO
import SwiftUI

struct GifImage: NSViewRepresentable {
    private static let imageCache: NSCache<NSString, NSImage> = {
        let cache = NSCache<NSString, NSImage>()
        cache.countLimit = 80
        cache.totalCostLimit = 256 * 1024 * 1024
        return cache
    }()

    // Decoding animated images (full-frame) is expensive, so it runs on a
    // shared background queue and never blocks the main thread inside
    // updateNSView.
    private static let decodeQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.gif.decode", qos: .userInitiated, attributes: .concurrent)

    var gifName: String?
    var gifUrl: URL?

    var isResizable: Bool = false
    var contentMode: ContentMode = .fill

    var animates: Bool

    init(_ gifName: String, animates: Bool = true) {
        self.gifName = gifName
        self.animates = animates
    }

    init(contentsOf url: URL, animates: Bool = true) {
        self.gifUrl = url
        self.animates = animates
    }

    final class Coordinator {
        // Identity of the image currently shown (or being loaded) in the view.
        var imageIdentity: String?
        // Monotonic token so a slow background decode that finishes after the
        // cell was recycled to another wallpaper is discarded.
        var loadToken: UInt64 = 0
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSImageView {
        let nsView = NSImageView()

        nsView.canDrawSubviewsIntoLayer = true
        nsView.imageScaling = .scaleProportionallyUpOrDown
        nsView.animates = animates

        updateImage(nsView, coordinator: context.coordinator)

        return nsView
    }

    func updateNSView(_ nsView: NSImageView, context: Context) {
        nsView.animates = animates
        updateImage(nsView, coordinator: context.coordinator)
        updateModifiers(nsView)
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSImageView, context: Context) -> CGSize? {
        if !self.isResizable {
            return nsView.sizeThatFits(nsView.frame.size)
        } else {
            guard let width = proposal.width, let height = proposal.height else { return nil }
            return CGSize(width: width, height: height)
        }
    }

    private func updateModifiers(_ nsView: NSImageView) {
        if self.isResizable {
            switch self.contentMode {
            case .fill:
                nsView.imageScaling = .scaleAxesIndependently
            case .fit:
                nsView.imageScaling = .scaleProportionallyUpOrDown
            }
        }
    }

    private var sourceURL: URL? {
        if let gifUrl { return gifUrl }
        if let gifName { return Bundle.main.url(forResource: gifName, withExtension: "gif") }
        return nil
    }

    private func updateImage(_ imageView: NSImageView, coordinator: Coordinator) {
        guard let url = sourceURL else {
            coordinator.loadToken &+= 1
            coordinator.imageIdentity = nil
            imageView.image = nil
            return
        }
        // Identity is derived only from the URL and the animate flag — no
        // per-update filesystem stat. Wallpaper preview files are effectively
        // immutable for a given path, so this is safe and avoids a stat storm
        // when the whole grid rebuilds.
        let identity = url.path + (animates ? "#animated" : "#static")
        guard coordinator.imageIdentity != identity else { return }
        coordinator.imageIdentity = identity
        coordinator.loadToken &+= 1
        let token = coordinator.loadToken
        let key = identity as NSString

        if let cached = Self.imageCache.object(forKey: key) {
            apply(cached, to: imageView)
            return
        }

        let animates = self.animates
        Self.decodeQueue.async {
            guard let image = Self.loadImage(at: url, animates: animates) else { return }
            if animates {
                (image.representations.first as? NSBitmapImageRep)?.setProperty(.loopCount, withValue: 0)
            }
            Self.imageCache.setObject(image, forKey: key, cost: Self.decodedCost(of: image, animates: animates))
            DispatchQueue.main.async {
                // Discard the result if the cell was recycled to a different
                // wallpaper while we were decoding.
                guard coordinator.loadToken == token,
                      coordinator.imageIdentity == identity else { return }
                self.apply(image, to: imageView)
            }
        }
    }

    private func apply(_ image: NSImage, to imageView: NSImageView) {
        imageView.animates = animates
        imageView.image = image
    }

    /// Above this decoded footprint an animated preview is not worth playing.
    /// A 1920×1080 GIF with 80 frames decodes to ~660 MB; sweeping the cursor
    /// across a grid used to do that once per cell.
    private static let maxAnimatedDecodedBytes = 64 * 1024 * 1024

    private static func loadImage(at url: URL, animates: Bool) -> NSImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return NSImage(contentsOf: url)
        }
        guard animates else {
            return downsampledStill(from: source) ?? NSImage(contentsOf: url)
        }

        // The animated hover path used to decode at full resolution
        // unconditionally. Play the animation only when its decoded size is
        // sane; otherwise fall back to the same downsampled still the cell
        // already shows when it is not hovered.
        let frameCount = max(CGImageSourceGetCount(source), 1)
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let width = (properties?[kCGImagePropertyPixelWidth] as? Int) ?? 0
        let height = (properties?[kCGImagePropertyPixelHeight] as? Int) ?? 0
        if width > 0, height > 0 {
            let decodedBytes = width * height * 4 * frameCount
            if decodedBytes > maxAnimatedDecodedBytes {
                return downsampledStill(from: source) ?? NSImage(contentsOf: url)
            }
        }
        return NSImage(contentsOf: url)
    }

    private static func downsampledStill(from source: CGImageSource) -> NSImage? {
        let frameIndex = representativeFrameIndex(in: source)
        guard let thumbnail = CGImageSourceCreateThumbnailAtIndex(source, frameIndex, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 512
        ] as CFDictionary) else {
            return nil
        }
        return NSImage(cgImage: thumbnail, size: .zero)
    }

    private static func representativeFrameIndex(in source: CGImageSource) -> Int {
        let count = CGImageSourceGetCount(source)
        guard count > 1 else { return 0 }

        let sampleCount = min(count, 32)
        let indexes = Set((0..<sampleCount).map { sample in
            sampleCount == 1 ? 0 : sample * (count - 1) / (sampleCount - 1)
        }).sorted()
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: 64
        ]

        for index in indexes {
            guard let frame = CGImageSourceCreateThumbnailAtIndex(source, index, options as CFDictionary) else {
                continue
            }
            if !isNearBlack(frame) {
                return index
            }
        }
        return 0
    }

    private static func isNearBlack(_ image: CGImage) -> Bool {
        let width = min(image.width, 64)
        let height = min(image.height, 64)
        guard width > 0, height > 0 else { return true }

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let rendered = pixels.withUnsafeMutableBytes { bytes -> Bool in
            guard let context = CGContext(
                data: bytes.baseAddress,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                    | CGBitmapInfo.byteOrder32Big.rawValue
            ) else { return false }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard rendered else { return true }

        var visiblePixels = 0
        var litPixels = 0
        var brightnessTotal = 0
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            guard pixels[offset + 3] > 8 else { continue }
            visiblePixels += 1
            let brightness = max(pixels[offset], pixels[offset + 1], pixels[offset + 2])
            brightnessTotal += Int(brightness)
            if brightness > 18 {
                litPixels += 1
            }
        }
        guard visiblePixels > 0 else { return true }
        return litPixels * 50 < visiblePixels && brightnessTotal < visiblePixels * 10
    }

    private static func decodedCost(of image: NSImage, animates: Bool) -> Int {
        guard let rep = image.representations.first else { return 1 }
        let frames = animates
            ? max((rep as? NSBitmapImageRep)?.value(forProperty: .frameCount) as? Int ?? 1, 1)
            : 1
        let bytes = Double(max(rep.pixelsWide, 1)) * Double(max(rep.pixelsHigh, 1)) *
            4.0 * Double(frames)
        return max(1, Int(min(bytes, Double(Int.max))))
    }

    func resizable(capInsets: EdgeInsets = EdgeInsets(), resizingMode: Image.ResizingMode = .stretch) -> Self {
        var view = self
        view.isResizable = true
        return view
    }

    func aspectRatio(_ aspectRatio: CGFloat? = nil, contentMode: ContentMode) -> Self {
        var view = self
        view.contentMode = contentMode
        return view
    }
}
