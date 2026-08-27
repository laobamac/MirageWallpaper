//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import ImageIO
import SwiftUI

actor WorkshopImageLoader {
    static let shared = WorkshopImageLoader()

    // Images are fully prepared before publication and are never mutated by the loader.
    struct LoadedImage: @unchecked Sendable {
        let image: NSImage
        let animationData: Data?
    }

    private struct RequestKey: Hashable, Sendable {
        let url: URL
        let maxPixel: Int
    }

    private struct PreparedImage: @unchecked Sendable {
        let image: NSImage
        let animationData: Data?
        let sourceData: Data
    }

    private struct PendingLoad {
        let generationID: UUID
        let task: Task<Void, Never>
        var subscribers: [UUID: CheckedContinuation<LoadedImage, Error>]
    }

    private enum LoadError: Error {
        case invalidData
        case invalidResponse
    }

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

    private let session: URLSession
    private let diskDirectory: URL
    private var pendingLoads: [RequestKey: PendingLoad] = [:]

    // File I/O and ImageIO are synchronous. Keep them off the cooperative
    // executor and cap their concurrency independently from URLSession.
    private static let blockingWorkQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "cn.laobamac.Mirage.workshopImage.blocking"
        queue.qualityOfService = .userInitiated
        queue.maxConcurrentOperationCount = 4
        return queue
    }()

    private static func makeSession() -> URLSession {
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 40
        configuration.httpMaximumConnectionsPerHost = 6
        return URLSession(configuration: configuration)
    }

    private static func makeDiskDirectory() -> URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/WorkshopImageCache")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    init(
        session: URLSession = WorkshopImageLoader.makeSession(),
        diskDirectory: URL = WorkshopImageLoader.makeDiskDirectory()
    ) {
        self.session = session
        self.diskDirectory = diskDirectory
    }

    static func maxPixel(for size: CGSize, scale: CGFloat) -> Int {
        let raw = Double(max(size.width, size.height)) * Double(max(scale, 1))
        let bucket = (raw / 64).rounded(.up) * 64
        return max(64, min(Int(bucket), 2048))
    }

    private func memoryKey(_ url: URL, _ px: Int) -> NSString {
        "\(url.absoluteString)#\(px)" as NSString
    }

    private static func diskURL(for url: URL, in diskDirectory: URL) -> URL {
        let digest = SHA256.hash(data: Data(url.absoluteString.utf8))
            .map { String(format: "%02x", $0) }.joined()
        return diskDirectory.appending(path: digest)
    }

    private func cost(_ image: NSImage) -> Int {
        guard let rep = image.representations.first else { return 1 }
        return max(1, rep.pixelsWide * rep.pixelsHigh * 4)
    }

    private func cachedAnimatedFlag(_ url: URL) -> Bool? {
        animatedFlags.object(forKey: url.absoluteString as NSString)?.boolValue
    }

    private func setAnimatedFlag(_ value: Bool, for url: URL) {
        animatedFlags.setObject(NSNumber(value: value), forKey: url.absoluteString as NSString)
    }

    func load(url: URL, targetSize: CGSize, scale: CGFloat) async throws -> LoadedImage {
        try await load(
            url: url,
            maxPixel: Self.maxPixel(for: targetSize, scale: scale)
        )
    }

    func load(url: URL, maxPixel: Int) async throws -> LoadedImage {
        try Task.checkCancellation()

        let requestKey = RequestKey(url: url, maxPixel: maxPixel)
        let imageKey = memoryKey(url, maxPixel)
        let dataKey = url.absoluteString as NSString
        if let image = memory.object(forKey: imageKey),
           let animated = cachedAnimatedFlag(url) {
            guard animated else {
                return LoadedImage(image: image, animationData: nil)
            }
            if let cachedData = dataMemory.object(forKey: dataKey) {
                return LoadedImage(image: image, animationData: cachedData as Data)
            }
        }

        let subscriberID = UUID()
        let loaded = try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                subscribe(
                    subscriberID: subscriberID,
                    to: requestKey,
                    continuation: continuation
                )
            }
        } onCancel: {
            Task {
                await self.cancelSubscriber(subscriberID, from: requestKey)
            }
        }
        try Task.checkCancellation()
        return loaded
    }

    private func subscribe(
        subscriberID: UUID,
        to key: RequestKey,
        continuation: CheckedContinuation<LoadedImage, Error>
    ) {
        if var pending = pendingLoads[key] {
            pending.subscribers[subscriberID] = continuation
            pendingLoads[key] = pending
            return
        }

        let requestID = UUID()
        let session = session
        let diskDirectory = diskDirectory
        let task = Task.detached(priority: .userInitiated) {
            let result: Result<PreparedImage, Error>
            do {
                let prepared = try await Self.fetch(
                    key: key,
                    session: session,
                    diskDirectory: diskDirectory
                )
                result = .success(prepared)
            } catch {
                result = .failure(error)
            }
            await self.finish(key: key, requestID: requestID, result: result)
        }
        pendingLoads[key] = PendingLoad(
            generationID: requestID,
            task: task,
            subscribers: [subscriberID: continuation]
        )
    }

    private func cancelSubscriber(_ subscriberID: UUID, from key: RequestKey) {
        guard var pending = pendingLoads[key],
              let continuation = pending.subscribers.removeValue(forKey: subscriberID) else {
            return
        }

        continuation.resume(throwing: CancellationError())
        if pending.subscribers.isEmpty {
            pendingLoads.removeValue(forKey: key)
            pending.task.cancel()
        } else {
            pendingLoads[key] = pending
        }
    }

    private func finish(
        key: RequestKey,
        requestID: UUID,
        result: Result<PreparedImage, Error>
    ) {
        // Cache a completed success even when cancellation removed the last
        // subscriber before this actor turn. Generation matching only guards
        // continuation fan-out, not cache publication.
        if case .success(let prepared) = result {
            let animated = prepared.animationData != nil
            setAnimatedFlag(animated, for: key.url)
            store(
                data: prepared.sourceData,
                image: prepared.image,
                dataKey: key.url.absoluteString as NSString,
                imageKey: memoryKey(key.url, key.maxPixel),
                animated: animated
            )
        }

        // A fully cancelled generation may have been replaced for the same key.
        guard let pending = pendingLoads[key],
              pending.generationID == requestID else { return }
        pendingLoads.removeValue(forKey: key)

        let subscriberResult: Result<LoadedImage, Error>
        switch result {
        case .success(let prepared):
            subscriberResult = .success(
                LoadedImage(image: prepared.image, animationData: prepared.animationData)
            )
        case .failure(let error):
            subscriberResult = .failure(error)
        }

        for continuation in pending.subscribers.values {
            continuation.resume(with: subscriberResult)
        }
    }

    private static func fetch(
        key: RequestKey,
        session: URLSession,
        diskDirectory: URL
    ) async throws -> PreparedImage {
        try Task.checkCancellation()

        if key.url.isFileURL {
            let data = try await performBlocking {
                try Data(contentsOf: key.url, options: .mappedIfSafe)
            }
            try Task.checkCancellation()
            return try await performBlocking {
                try prepare(data: data, maxPixel: key.maxPixel)
            }
        }

        let disk = diskURL(for: key.url, in: diskDirectory)
        if let prepared = try? await performBlocking({
            guard let data = try? Data(contentsOf: disk), !data.isEmpty else {
                throw LoadError.invalidData
            }
            return try prepare(data: data, maxPixel: key.maxPixel)
        }) {
            try Task.checkCancellation()
            return prepared
        }

        let (data, response) = try await session.data(for: URLRequest(url: key.url))
        try Task.checkCancellation()
        let ok = (response as? HTTPURLResponse).map {
            (200..<300).contains($0.statusCode)
        } ?? true
        guard ok else { throw LoadError.invalidResponse }
        guard !data.isEmpty else { throw LoadError.invalidData }

        try Task.checkCancellation()
        return try await performBlocking {
            try? data.write(to: disk, options: .atomic)
            return try prepare(data: data, maxPixel: key.maxPixel)
        }
    }

    private static func performBlocking<T>(
        _ work: @escaping () throws -> T
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            blockingWorkQueue.addOperation {
                do {
                    continuation.resume(returning: try work())
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private static func prepare(data: Data, maxPixel: Int) throws -> PreparedImage {
        guard !data.isEmpty, let image = downsample(data, maxPixel: maxPixel) else {
            throw LoadError.invalidData
        }
        return PreparedImage(
            image: image,
            animationData: safeAnimationData(data),
            sourceData: data
        )
    }

    private func store(data: Data, image: NSImage, dataKey: NSString,
                       imageKey: NSString, animated: Bool) {
        if animated {
            dataMemory.setObject(data as NSData, forKey: dataKey, cost: data.count)
        }
        memory.setObject(image, forKey: imageKey, cost: cost(image))
    }

    private static func safeAnimationData(_ data: Data) -> Data? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        let frameCount = CGImageSourceGetCount(source)
        guard frameCount > 1,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
              width > 0, height > 0 else { return nil }
        let (pixels, pixelOverflow) = width.multipliedReportingOverflow(by: height)
        let (framePixels, frameOverflow) = pixels.multipliedReportingOverflow(by: frameCount)
        let (decodedBytes, byteOverflow) = framePixels.multipliedReportingOverflow(by: 4)
        guard !pixelOverflow, !frameOverflow, !byteOverflow,
              decodedBytes <= 64 * 1024 * 1024 else { return nil }
        return data
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
    private struct RequestIdentity: Hashable {
        let url: URL?
        let maxPixel: Int
        let hasUsableSize: Bool
        let isLoadingEnabled: Bool
    }

    let url: URL?
    var contentMode: ContentMode = .fill
    var isAnimating = false
    var isLoadingEnabled = true

    @Environment(\.displayScale) private var displayScale
    @State private var image: NSImage?
    @State private var animationData: Data?
    @State private var failed = false
    @State private var boxSize: CGSize = .zero

    private var requestIdentity: RequestIdentity {
        RequestIdentity(
            url: url,
            maxPixel: WorkshopImageLoader.maxPixel(for: boxSize, scale: displayScale),
            hasUsableSize: boxSize.width > 1 && boxSize.height > 1,
            isLoadingEnabled: isLoadingEnabled
        )
    }

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
                        }
                        .onChange(of: proxy.size) { _, newValue in
                            guard abs(newValue.width - boxSize.width) > 1 ||
                                  abs(newValue.height - boxSize.height) > 1 else { return }
                            boxSize = newValue
                        }
                }
            )
            .onChange(of: url) { _, _ in
                image = nil
                animationData = nil
                failed = false
            }
            .task(id: requestIdentity) {
                await load(requestIdentity)
            }
            .onDisappear {
                animationData = nil
            }
    }

    @MainActor
    private func load(_ request: RequestIdentity) async {
        guard request.hasUsableSize, request == requestIdentity else { return }
        guard let requestedURL = request.url else {
            image = nil
            animationData = nil
            failed = true
            return
        }
        guard request.isLoadingEnabled else { return }

        do {
            let loaded = try await WorkshopImageLoader.shared.load(
                url: requestedURL,
                maxPixel: request.maxPixel
            )
            guard !Task.isCancelled, request == requestIdentity else { return }
            image = loaded.image
            animationData = loaded.animationData
            failed = false
        } catch is CancellationError {
            return
        } catch {
            guard !Task.isCancelled, request == requestIdentity else { return }
            failed = true
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
