//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import ImageIO
import SwiftUI

final class WorkshopImageLoader {
    static let shared = WorkshopImageLoader()

    private let memory: NSCache<NSString, NSImage> = {
        let cache = NSCache<NSString, NSImage>()
        cache.countLimit = 400
        cache.totalCostLimit = 160 * 1024 * 1024
        return cache
    }()

    private let dataMemory: NSCache<NSString, NSData> = {
        let cache = NSCache<NSString, NSData>()
        cache.countLimit = 400
        cache.totalCostLimit = 160 * 1024 * 1024
        return cache
    }()

    private let animatedFlags: NSCache<NSString, NSNumber> = {
        let cache = NSCache<NSString, NSNumber>()
        cache.countLimit = 4000
        return cache
    }()

    private let ioQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.workshopImage", qos: .userInitiated, attributes: .concurrent)
    private let ioLimiter = DispatchSemaphore(value: 4)

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

    private func maxPixel(for size: CGSize, scale: CGFloat) -> Int {
        let raw = Double(max(size.width, size.height)) * Double(max(scale, 1))
        let bucket = (raw / 64).rounded(.up) * 64
        return max(64, min(Int(bucket), 2048))
    }

    private func memoryKey(_ url: URL, _ px: Int) -> NSString {
        "\(url.absoluteString)#\(px)" as NSString
    }

    private func diskURL(for url: URL) -> URL {
        let digest = SHA256.hash(data: Data(url.absoluteString.utf8))
            .map { String(format: "%02x", $0) }.joined()
        return diskDirectory.appending(path: digest)
    }

    private func cost(_ image: NSImage) -> Int {
        guard let rep = image.representations.first else { return 1 }
        return max(1, rep.pixelsWide * rep.pixelsHigh * 4)
    }

    func cachedImage(url: URL, targetSize: CGSize, scale: CGFloat) -> NSImage? {
        memory.object(forKey: memoryKey(url, maxPixel(for: targetSize, scale: scale)))
    }

    private func cachedAnimatedFlag(_ url: URL) -> Bool? {
        animatedFlags.object(forKey: url.absoluteString as NSString)?.boolValue
    }

    private func setAnimatedFlag(_ value: Bool, for url: URL) {
        animatedFlags.setObject(NSNumber(value: value), forKey: url.absoluteString as NSString)
    }

    func load(url: URL, targetSize: CGSize, scale: CGFloat,
              completion: @escaping (NSImage?, Data?) -> Void) {
        let px = maxPixel(for: targetSize, scale: scale)
        let key = memoryKey(url, px)
        let dataKey = url.absoluteString as NSString
        if let image = memory.object(forKey: key), let animated = cachedAnimatedFlag(url) {
            guard animated else {
                completion(image, nil)
                return
            }
            if let cachedData = dataMemory.object(forKey: dataKey) {
                completion(image, cachedData as Data)
                return
            }
        }
        ioQueue.async { [weak self] in
            guard let self else { return }
            self.ioLimiter.wait()
            defer { self.ioLimiter.signal() }
            if url.isFileURL {
                guard let data = try? Data(contentsOf: url, options: .mappedIfSafe),
                      !data.isEmpty,
                      let image = Self.downsample(data, maxPixel: px) else {
                    DispatchQueue.main.async { completion(nil, nil) }
                    return
                }
                let animated = Self.isAnimated(data)
                self.setAnimatedFlag(animated, for: url)
                self.store(data: data, image: image, dataKey: dataKey,
                           imageKey: key, animated: animated)
                DispatchQueue.main.async {
                    completion(image, animated ? data : nil)
                }
                return
            }
            let disk = self.diskURL(for: url)
            if let data = try? Data(contentsOf: disk), !data.isEmpty,
               let image = Self.downsample(data, maxPixel: px) {
                let animated = Self.isAnimated(data)
                self.setAnimatedFlag(animated, for: url)
                self.store(data: data, image: image, dataKey: dataKey,
                           imageKey: key, animated: animated)
                DispatchQueue.main.async {
                    completion(image, animated ? data : nil)
                }
                return
            }
            let semaphore = DispatchSemaphore(value: 0)
            self.session.dataTask(with: URLRequest(url: url)) { [weak self] data, response, _ in
                defer { semaphore.signal() }
                guard let self else { return }
                let ok = (response as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? true
                guard ok, let data, !data.isEmpty else {
                    DispatchQueue.main.async { completion(nil, nil) }
                    return
                }
                try? data.write(to: disk, options: .atomic)
                let image = Self.downsample(data, maxPixel: px)
                let animated = Self.isAnimated(data)
                self.setAnimatedFlag(animated, for: url)
                if let image {
                    self.store(data: data, image: image, dataKey: dataKey,
                               imageKey: key, animated: animated)
                }
                DispatchQueue.main.async {
                    completion(image, animated ? data : nil)
                }
            }.resume()
            semaphore.wait()
        }
    }

    private func store(data: Data, image: NSImage, dataKey: NSString,
                       imageKey: NSString, animated: Bool) {
        if animated {
            dataMemory.setObject(data as NSData, forKey: dataKey, cost: data.count)
        }
        memory.setObject(image, forKey: imageKey, cost: cost(image))
    }

    private static func isAnimated(_ data: Data) -> Bool {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return false }
        return CGImageSourceGetCount(source) > 1
    }

    private static func downsample(_ data: Data, maxPixel: Int) -> NSImage? {
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions) else {
            return NSImage(data: data)
        }
        let frameIndex = representativeFrameIndex(in: source)
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, frameIndex, options as CFDictionary) else {
            return NSImage(data: data)
        }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }

    private static func representativeFrameIndex(in source: CGImageSource) -> Int {
        let count = CGImageSourceGetCount(source)
        guard count > 1 else { return 0 }

        let sampleCount = min(count, 6)
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
}

struct WorkshopImage: View {
    let url: URL?
    var contentMode: ContentMode = .fill
    var isAnimating = false
    var isLoadingEnabled = true

    @Environment(\.displayScale) private var displayScale
    @State private var image: NSImage?
    @State private var animationData: Data?
    @State private var failed = false
    @State private var boxSize: CGSize = .zero
    @State private var loadToken: UInt64 = 0
    @State private var pendingLoad = false

    var body: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.10))
            .overlay {
                if let image {
                    Image(nsImage: image)
                        .resizable()
                        .interpolation(.high)
                        .aspectRatio(contentMode: contentMode)
                    if isAnimating, let animationData, let url {
                        WorkshopAnimatedImage(
                            data: animationData,
                            identity: url.absoluteString,
                            contentMode: contentMode
                        )
                    }
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
                animationData = nil
                failed = false
                load()
            }
            .onChange(of: isLoadingEnabled) { _, enabled in
                if enabled && pendingLoad {
                    load()
                }
            }
            .onDisappear {
                loadToken &+= 1
                animationData = nil
            }
    }

    private func load() {
        guard let url, boxSize.width > 1, boxSize.height > 1 else { return }
        let scale = displayScale
        if let cached = WorkshopImageLoader.shared.cachedImage(url: url, targetSize: boxSize, scale: scale) {
            image = cached
            failed = false
            pendingLoad = false
            if !isAnimating { return }
        }
        guard isLoadingEnabled else {
            pendingLoad = true
            return
        }
        pendingLoad = false
        loadToken &+= 1
        let token = loadToken
        let requestedURL = url
        WorkshopImageLoader.shared.load(url: requestedURL, targetSize: boxSize, scale: scale) { loaded, animatedData in
            guard token == loadToken, requestedURL == url else { return }
            if let loaded {
                image = loaded
                animationData = animatedData
                failed = false
            } else {
                failed = true
            }
        }
    }
}

private struct WorkshopAnimatedImage: NSViewRepresentable {
    let data: Data
    let identity: String
    let contentMode: ContentMode

    final class Coordinator {
        var identity: String?
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> WorkshopAnimatedNSView {
        let view = WorkshopAnimatedNSView()
        updateNSView(view, context: context)
        return view
    }

    func updateNSView(_ view: WorkshopAnimatedNSView, context: Context) {
        view.contentMode = contentMode
        if context.coordinator.identity != identity {
            context.coordinator.identity = identity
            view.image = NSImage(data: data)
        }
    }

    static func dismantleNSView(_ view: WorkshopAnimatedNSView, coordinator: Coordinator) {
        view.image = nil
        coordinator.identity = nil
    }
}

private final class WorkshopAnimatedNSView: NSView {
    private let imageView = NSImageView()
    var contentMode: ContentMode = .fill {
        didSet { needsLayout = true }
    }
    var image: NSImage? {
        get { imageView.image }
        set {
            imageView.image = newValue
            imageView.animates = newValue != nil
            needsLayout = true
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.masksToBounds = true
        imageView.imageScaling = .scaleProportionallyUpOrDown
        imageView.imageAlignment = .alignCenter
        imageView.autoresizingMask = []
        addSubview(imageView)
    }

    required init?(coder: NSCoder) {
        nil
    }

    override func layout() {
        super.layout()
        guard contentMode == .fill, let size = imageView.image?.size,
              size.width > 0, size.height > 0,
              bounds.width > 0, bounds.height > 0 else {
            imageView.frame = bounds
            return
        }
        let scale = max(bounds.width / size.width, bounds.height / size.height)
        let width = size.width * scale
        let height = size.height * scale
        imageView.frame = NSRect(
            x: (bounds.width - width) / 2,
            y: (bounds.height - height) / 2,
            width: width,
            height: height
        )
    }
}
