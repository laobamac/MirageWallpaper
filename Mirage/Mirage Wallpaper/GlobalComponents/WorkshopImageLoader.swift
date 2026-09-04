//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import ImageIO

struct WorkshopImageAnimationDescriptor: Hashable, Sendable {
    let url: URL
    let sourceVersion: String?
    let identity: String
}

struct WorkshopImageAnimation: @unchecked Sendable, Equatable {
    let image: NSImage
    let identity: String

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.identity == rhs.identity
    }
}

struct WorkshopLoadedImage: @unchecked Sendable {
    let image: NSImage
    let animationDescriptor: WorkshopImageAnimationDescriptor?
}

actor WorkshopImageLoader {
    static let shared = WorkshopImageLoader()

    private struct SourceKey: Hashable, Sendable {
        let url: URL
        let sourceVersion: String?
    }

    private struct ImageKey: Hashable, Sendable {
        let source: SourceKey
        let generation: UUID
        let maxPixel: Int
    }

    private struct SourcePayload: @unchecked Sendable {
        let generation = UUID()
        let data: Data
        let analysis: AnimationAnalysis
        let fallbackImage: NSImage?
        let animationDescriptor: WorkshopImageAnimationDescriptor?
        let expiresAt: Date
    }

    private struct StaticPayload: @unchecked Sendable {
        let image: NSImage
        let expiresAt: Date
    }

    private struct SourceTask: Sendable {
        let id = UUID()
        let task: Task<SourcePayload, Error>
        var waiters: Set<UUID> = []
    }

    private final class BlockingResultBox<Value: Sendable>: @unchecked Sendable {
        private let lock = NSLock()
        private var result: Result<Value, Error>?

        func store(_ result: Result<Value, Error>) {
            lock.lock()
            self.result = result
            lock.unlock()
        }

        func take() -> Result<Value, Error>? {
            lock.lock()
            defer { lock.unlock() }
            return result
        }
    }

    private final class SourceCacheEntry: NSObject {
        let payload: SourcePayload

        init(_ payload: SourcePayload) {
            self.payload = payload
        }
    }

    private final class StaticCacheEntry: NSObject {
        let payload: StaticPayload

        init(_ payload: StaticPayload) {
            self.payload = payload
        }
    }

    private enum AnimationAnalysis: Sendable {
        case invalidSource
        case staticImage
        case safe
        case unsafe

        var isValidSource: Bool {
            if case .invalidSource = self { return false }
            return true
        }

        var shouldSampleFrames: Bool {
            switch self {
            case .safe, .unsafe:
                return true
            case .invalidSource, .staticImage:
                return false
            }
        }
    }

    private enum LoadError: Error {
        case invalidData
        case invalidResponse
        case sourceTooLarge
    }

    private static let maximumSourceBytes = 64 * 1024 * 1024
    private static let maximumDecodedAnimationBytes = 64 * 1024 * 1024
    private static let maximumSourcePixels = 256 * 1024 * 1024
    private static let maximumCacheLifetime: TimeInterval = 6 * 60 * 60

    private let sourceMemory: NSCache<NSString, SourceCacheEntry> = {
        let cache = NSCache<NSString, SourceCacheEntry>()
        cache.countLimit = 120
        cache.totalCostLimit = 160 * 1024 * 1024
        return cache
    }()

    private let staticMemory: NSCache<NSString, StaticCacheEntry> = {
        let cache = NSCache<NSString, StaticCacheEntry>()
        cache.countLimit = 400
        cache.totalCostLimit = 160 * 1024 * 1024
        return cache
    }()

    private let session: URLSession
    private var sourceTasks: [SourceKey: SourceTask] = [:]

    // ImageIO and AppKit decoding are synchronous. Keep them off the
    // cooperative executor and bound their concurrency separately.
    private static let blockingWorkQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "cn.laobamac.Mirage.workshopImage.blocking"
        queue.qualityOfService = .userInitiated
        queue.maxConcurrentOperationCount = 4
        return queue
    }()

    init(session: URLSession = WorkshopImageLoader.makeSession()) {
        self.session = session
    }

    private static func makeSession() -> URLSession {
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 40
        configuration.httpMaximumConnectionsPerHost = 6
        configuration.urlCache = URLCache(
            memoryCapacity: 32 * 1024 * 1024,
            diskCapacity: 256 * 1024 * 1024
        )
        return URLSession(configuration: configuration)
    }

    static func maxPixel(for size: CGSize, scale: CGFloat) -> Int {
        let dimension = Double(max(size.width, size.height))
        guard dimension.isFinite, dimension > 0 else { return 64 }

        let requestedScale = Double(scale)
        let safeScale = requestedScale.isFinite ? max(requestedScale, 1) : 1
        let raw = dimension * safeScale
        guard raw.isFinite else { return 2048 }

        let bucket = (raw / 64).rounded(.up) * 64
        return Int(min(max(bucket, 64), 2048))
    }

    func load(url: URL, maxPixel: Int) async throws -> WorkshopLoadedImage {
        try Task.checkCancellation()
        let sourceVersion = try await Self.sourceVersion(for: url)
        try Task.checkCancellation()

        let sourceKey = SourceKey(url: url, sourceVersion: sourceVersion)
        let source = try await loadSource(for: sourceKey)
        try Task.checkCancellation()
        let imageKey = ImageKey(
            source: sourceKey,
            generation: source.generation,
            maxPixel: Self.normalizedMaxPixel(maxPixel)
        )
        let staticPayload = try await loadStatic(for: imageKey, source: source)
        try Task.checkCancellation()
        return WorkshopLoadedImage(
            image: staticPayload.image,
            animationDescriptor: source.animationDescriptor
        )
    }

    func loadAnimation(
        _ descriptor: WorkshopImageAnimationDescriptor
    ) async throws -> WorkshopImageAnimation {
        try Task.checkCancellation()
        let sourceKey = SourceKey(
            url: descriptor.url,
            sourceVersion: descriptor.sourceVersion
        )
        let source = try await loadSource(for: sourceKey)
        try Task.checkCancellation()
        guard source.animationDescriptor == descriptor else {
            throw LoadError.invalidData
        }
        return try await loadPreparedAnimation(descriptor, source: source)
    }

    private func loadSource(for key: SourceKey) async throws -> SourcePayload {
        let cacheKey = sourceMemoryKey(for: key)
        if let cached = sourceMemory.object(forKey: cacheKey) {
            if cached.payload.expiresAt > Date() {
                return cached.payload
            }
            sourceMemory.removeObject(forKey: cacheKey)
        }

        let waiterID = UUID()
        let entry: SourceTask
        if var pending = sourceTasks[key] {
            pending.waiters.insert(waiterID)
            sourceTasks[key] = pending
            entry = pending
        } else {
            let session = session
            let task = Task.detached(priority: .userInitiated) {
                try await Self.fetchSource(key: key, session: session)
            }
            var pending = SourceTask(task: task)
            pending.waiters.insert(waiterID)
            sourceTasks[key] = pending
            entry = pending
        }

        let result = await withTaskCancellationHandler {
            await entry.task.result
        } onCancel: {
            Task {
                await self.cancelSourceWaiter(
                    waiterID,
                    key: key,
                    taskID: entry.id
                )
            }
        }
        if sourceTasks[key]?.id == entry.id {
            sourceTasks.removeValue(forKey: key)
            if case .success(let payload) = result, payload.expiresAt > Date() {
                sourceMemory.setObject(
                    SourceCacheEntry(payload),
                    forKey: cacheKey,
                    cost: sourceCost(payload)
                )
            }
        }
        try Task.checkCancellation()
        return try result.get()
    }

    private func loadStatic(
        for key: ImageKey,
        source: SourcePayload
    ) async throws -> StaticPayload {
        let cacheKey = staticMemoryKey(for: key)
        if let cached = staticMemory.object(forKey: cacheKey) {
            if cached.payload.expiresAt > Date() {
                return cached.payload
            }
            staticMemory.removeObject(forKey: cacheKey)
        }

        let payload = try await Self.performBlocking {
            try Self.prepareStatic(source: source, maxPixel: key.maxPixel)
        }
        if payload.expiresAt > Date() {
            staticMemory.setObject(
                StaticCacheEntry(payload),
                forKey: cacheKey,
                cost: imageCost(payload.image)
            )
        }
        try Task.checkCancellation()
        return payload
    }

    private func loadPreparedAnimation(
        _ descriptor: WorkshopImageAnimationDescriptor,
        source: SourcePayload
    ) async throws -> WorkshopImageAnimation {
        try await Self.performBlocking {
            try Self.prepareAnimation(descriptor: descriptor, source: source)
        }
    }

    private func cancelSourceWaiter(
        _ waiterID: UUID,
        key: SourceKey,
        taskID: UUID
    ) {
        guard var entry = sourceTasks[key], entry.id == taskID,
              entry.waiters.remove(waiterID) != nil else { return }
        if entry.waiters.isEmpty {
            sourceTasks.removeValue(forKey: key)
            entry.task.cancel()
        } else {
            sourceTasks[key] = entry
        }
    }

    private func sourceMemoryKey(for key: SourceKey) -> NSString {
        "\(key.url.absoluteString)#\(key.sourceVersion ?? "remote")" as NSString
    }

    private func staticMemoryKey(for key: ImageKey) -> NSString {
        "\(key.source.url.absoluteString)#\(key.generation)#\(key.maxPixel)" as NSString
    }

    private func sourceCost(_ payload: SourcePayload) -> Int {
        payload.data.count + (payload.fallbackImage.map(imageCost) ?? 0)
    }

    private func imageCost(_ image: NSImage) -> Int {
        Self.imageCost(image)
    }

    private static func imageCost(_ image: NSImage) -> Int {
        guard let representation = image.representations.first else { return 1 }
        let (pixels, pixelOverflow) = representation.pixelsWide
            .multipliedReportingOverflow(by: representation.pixelsHigh)
        let (bytes, byteOverflow) = pixels.multipliedReportingOverflow(by: 4)
        guard !pixelOverflow, !byteOverflow else { return 1 }
        return max(1, bytes)
    }

    private static func normalizedMaxPixel(_ maxPixel: Int) -> Int {
        let bounded = min(max(maxPixel, 64), 2048)
        return ((bounded - 1) / 64 + 1) * 64
    }

    private static func sourceVersion(for url: URL) async throws -> String? {
        guard url.isFileURL else { return nil }
        return try await performBlocking {
            let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
            guard let size = (attributes[.size] as? NSNumber)?.uint64Value else {
                throw LoadError.invalidData
            }
            let modified = (attributes[.modificationDate] as? Date)?
                .timeIntervalSinceReferenceDate ?? 0
            let fileNumber = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value ?? 0
            return "\(size)#\(modified)#\(fileNumber)"
        }
    }

    private static func fetchSource(
        key: SourceKey,
        session: URLSession
    ) async throws -> SourcePayload {
        try Task.checkCancellation()
        if key.url.isFileURL {
            return try await performBlocking {
                let data = try readSourceData(at: key.url)
                return try prepareSource(
                    key: key,
                    data: data,
                    expiresAt: .distantFuture
                )
            }
        }

        let (data, response) = try await session.data(for: URLRequest(url: key.url))
        try Task.checkCancellation()
        try validate(response: response)
        guard !data.isEmpty else { throw LoadError.invalidData }
        guard data.count <= maximumSourceBytes else { throw LoadError.sourceTooLarge }
        let expiresAt = cacheExpiration(for: response)
        return try await performBlocking {
            return try prepareSource(key: key, data: data, expiresAt: expiresAt)
        }
    }

    private static func readSourceData(at url: URL) throws -> Data {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        guard let size = (attributes[.size] as? NSNumber)?.uint64Value, size > 0 else {
            throw LoadError.invalidData
        }
        guard size <= UInt64(maximumSourceBytes) else {
            throw LoadError.sourceTooLarge
        }
        let data = try Data(contentsOf: url, options: .mappedIfSafe)
        guard !data.isEmpty else { throw LoadError.invalidData }
        guard data.count <= maximumSourceBytes else { throw LoadError.sourceTooLarge }
        return data
    }

    private static func validate(response: URLResponse) throws {
        if let response = response as? HTTPURLResponse,
           !(200..<300).contains(response.statusCode) {
            throw LoadError.invalidResponse
        }
    }

    private static func cacheExpiration(for response: URLResponse) -> Date {
        let now = Date()
        guard let response = response as? HTTPURLResponse else {
            return now.addingTimeInterval(maximumCacheLifetime)
        }

        let directives = response.value(forHTTPHeaderField: "Cache-Control")?
            .split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() }
            ?? []
        if directives.contains("no-store") || directives.contains("no-cache") ||
            response.value(forHTTPHeaderField: "Vary")?.trimmingCharacters(
                in: .whitespacesAndNewlines
            ) == "*" ||
            response.value(forHTTPHeaderField: "Pragma")?.lowercased() == "no-cache" {
            return .distantPast
        }
        if let directive = directives.first(where: { $0.hasPrefix("max-age=") }),
           let seconds = TimeInterval(directive.dropFirst("max-age=".count)) {
            let age = TimeInterval(response.value(forHTTPHeaderField: "Age") ?? "") ?? 0
            let remaining = max(seconds - max(age, 0), 0)
            return now.addingTimeInterval(min(remaining, maximumCacheLifetime))
        }
        if let value = response.value(forHTTPHeaderField: "Expires") {
            let formatter = DateFormatter()
            formatter.locale = Locale(identifier: "en_US_POSIX")
            formatter.timeZone = TimeZone(secondsFromGMT: 0)
            formatter.dateFormat = "EEE',' dd MMM yyyy HH':'mm':'ss z"
            if let expiration = formatter.date(from: value) {
                return min(expiration, now.addingTimeInterval(maximumCacheLifetime))
            }
        }
        if directives.contains("must-revalidate") {
            return .distantPast
        }
        return now.addingTimeInterval(maximumCacheLifetime)
    }

    private static func prepareSource(
        key: SourceKey,
        data: Data,
        expiresAt: Date
    ) throws -> SourcePayload {
        let analysis = animationAnalysis(data)
        if analysis.isValidSource {
            let descriptor: WorkshopImageAnimationDescriptor?
            if case .safe = analysis {
                let identity = SHA256.hash(data: data)
                    .map { String(format: "%02x", $0) }
                    .joined()
                descriptor = WorkshopImageAnimationDescriptor(
                    url: key.url,
                    sourceVersion: key.sourceVersion,
                    identity: identity
                )
            } else {
                descriptor = nil
            }
            return SourcePayload(
                data: data,
                analysis: analysis,
                fallbackImage: nil,
                animationDescriptor: descriptor,
                expiresAt: expiresAt
            )
        }

        guard let fallbackImage = appKitFallbackImage(data),
              isReasonableFallbackImage(fallbackImage) else {
            throw LoadError.invalidData
        }
        return SourcePayload(
            data: data,
            analysis: .staticImage,
            fallbackImage: fallbackImage,
            animationDescriptor: nil,
            expiresAt: expiresAt
        )
    }

    private static func prepareStatic(
        source: SourcePayload,
        maxPixel: Int
    ) throws -> StaticPayload {
        guard let image = downsample(
            source.data,
            fallbackImage: source.fallbackImage,
            maxPixel: maxPixel,
            sampleAnimatedFrames: source.analysis.shouldSampleFrames
        ) else {
            throw LoadError.invalidData
        }
        return StaticPayload(image: image, expiresAt: source.expiresAt)
    }

    private static func prepareAnimation(
        descriptor: WorkshopImageAnimationDescriptor,
        source: SourcePayload
    ) throws -> WorkshopImageAnimation {
        guard let image = NSImage(data: source.data), image.isValid else {
            throw LoadError.invalidData
        }
        return WorkshopImageAnimation(
            image: image,
            identity: descriptor.identity
        )
    }

    private static func animationAnalysis(_ data: Data) -> AnimationAnalysis {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else {
            return .invalidSource
        }
        let frameCount = CGImageSourceGetCount(source)
        guard frameCount > 0 else { return .invalidSource }

        let sourceProperties = CGImageSourceCopyProperties(source, nil) as? [CFString: Any]
        let frameProperties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil)
            as? [CFString: Any]
        guard let (width, height) = pixelDimensions(in: sourceProperties)
                ?? pixelDimensions(in: frameProperties) else {
            return .invalidSource
        }
        let (pixels, pixelOverflow) = width.multipliedReportingOverflow(by: height)
        guard !pixelOverflow, pixels <= maximumSourcePixels else {
            return .invalidSource
        }
        guard frameCount > 1 else { return .staticImage }

        let (framePixels, frameOverflow) = pixels.multipliedReportingOverflow(by: frameCount)
        let (decodedBytes, byteOverflow) = framePixels.multipliedReportingOverflow(by: 4)
        guard !frameOverflow, !byteOverflow,
              decodedBytes <= maximumDecodedAnimationBytes else {
            return .unsafe
        }
        return .safe
    }

    private static func pixelDimensions(
        in properties: [CFString: Any]?
    ) -> (Int, Int)? {
        guard let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
              width > 0, height > 0 else { return nil }
        return (width, height)
    }

    private static func downsample(
        _ data: Data,
        fallbackImage: NSImage?,
        maxPixel: Int,
        sampleAnimatedFrames: Bool
    ) -> NSImage? {
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions) else {
            return downsampleFallback(
                fallbackImage ?? appKitFallbackImage(data),
                maxPixel: maxPixel
            )
        }
        let frameIndex = sampleAnimatedFrames ? representativeFrameIndex(in: source) : 0
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(
            source,
            frameIndex,
            options as CFDictionary
        ) else {
            return downsampleFallback(
                fallbackImage ?? appKitFallbackImage(data),
                maxPixel: maxPixel
            )
        }
        return NSImage(
            cgImage: cgImage,
            size: NSSize(width: cgImage.width, height: cgImage.height)
        )
    }

    private static func appKitFallbackImage(_ data: Data) -> NSImage? {
        guard let image = NSImage(data: data), image.isValid else { return nil }
        return image
    }

    private static func isReasonableFallbackImage(_ image: NSImage) -> Bool {
        for representation in image.representations {
            let width = representation.pixelsWide
            let height = representation.pixelsHigh
            guard width > 0, height > 0 else { continue }
            let (pixels, overflow) = width.multipliedReportingOverflow(by: height)
            return !overflow && pixels <= maximumSourcePixels
        }
        return image.size.width > 0 && image.size.height > 0
    }

    private static func downsampleFallback(_ image: NSImage?, maxPixel: Int) -> NSImage? {
        guard let image else { return nil }
        let sourceSize = image.size
        guard sourceSize.width > 0, sourceSize.height > 0 else { return image }

        let scale = min(
            1,
            CGFloat(maxPixel) / max(sourceSize.width, sourceSize.height)
        )
        let targetSize = NSSize(
            width: max(1, sourceSize.width * scale),
            height: max(1, sourceSize.height * scale)
        )
        var proposedRect = NSRect(origin: .zero, size: targetSize)
        guard let cgImage = image.cgImage(
            forProposedRect: &proposedRect,
            context: nil,
            hints: nil
        ) else { return image }
        return NSImage(cgImage: cgImage, size: targetSize)
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
            guard let frame = CGImageSourceCreateThumbnailAtIndex(
                source,
                index,
                options as CFDictionary
            ) else { continue }
            if !isNearBlack(frame) {
                return index
            }
        }
        return 0
    }

    private static func performBlocking<T: Sendable>(
        _ work: @escaping @Sendable () throws -> T
    ) async throws -> T {
        try Task.checkCancellation()
        let result = BlockingResultBox<T>()
        let operation = BlockOperation {
            result.store(Result { try work() })
        }
        let value: T = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                operation.completionBlock = {
                    continuation.resume(
                        with: result.take() ?? .failure(CancellationError())
                    )
                }
                blockingWorkQueue.addOperation(operation)
            }
        } onCancel: {
            operation.cancel()
        }
        try Task.checkCancellation()
        return value
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
