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

    private static func loadImage(at url: URL, animates: Bool) -> NSImage? {
        guard !animates else { return NSImage(contentsOf: url) }
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 512
              ] as CFDictionary) else {
            return NSImage(contentsOf: url)
        }
        return NSImage(cgImage: thumbnail, size: .zero)
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
