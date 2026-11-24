//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import ImageIO
import SwiftUI

private struct MirageContentActiveKey: EnvironmentKey {
    static let defaultValue = true
}

extension EnvironmentValues {
    var mirageContentActive: Bool {
        get { self[MirageContentActiveKey.self] }
        set { self[MirageContentActiveKey.self] = newValue }
    }
}

private final class WorkshopImageDiskCache {
    struct Policy {
        var maximumBytes: Int64 = 512 * 1024 * 1024
        var preferredBytes: Int64 = 384 * 1024 * 1024
        var maximumAge: TimeInterval = 30 * 24 * 60 * 60
    }

    private struct Entry {
        let size: Int64
        var lastUse: Date
    }

    private let directory: URL
    private let policy: Policy
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.workshopImage.disk", qos: .utility)
    private var entries: [String: Entry] = [:]
    private var totalBytes: Int64 = 0
    private var indexed = false
    private var pendingUsage: Set<String> = []
    private var usageFlushScheduled = false
    private var timer: DispatchSourceTimer?

    init(directory: URL, policy: Policy = Policy()) {
        self.directory = directory
        self.policy = policy
    }

    deinit {
        timer?.cancel()
    }

    func startMaintenance() {
        queue.async { [weak self] in
            guard let self, self.timer == nil else { return }
            self.ensureIndex()
            let timer = DispatchSource.makeTimerSource(queue: self.queue)
            timer.schedule(deadline: .now() + 3600, repeating: 3600, leeway: .seconds(60))
            timer.setEventHandler { [weak self] in self?.maintain() }
            self.timer = timer
            timer.resume()
        }
    }

    func read(for url: URL, completion: @escaping (Data?) -> Void) {
        queue.async { [self] in
            ensureIndex()
            let name = name(for: url)
            guard isCacheDirectory, refreshEntry(name), let entry = entries[name] else {
                completion(nil)
                return
            }
            let now = Date()
            guard entry.size > 0, now.timeIntervalSince(entry.lastUse) < policy.maximumAge else {
                remove(name)
                completion(nil)
                return
            }
            guard let data = try? Data(contentsOf: fileURL(name)), !data.isEmpty else {
                refreshEntry(name)
                completion(nil)
                return
            }
            recordUsage(name, at: now)
            completion(data)
        }
    }

    func recordUsage(for url: URL) {
        queue.async { [self] in
            ensureIndex()
            recordUsage(name(for: url), at: Date())
        }
    }

    func store(_ data: Data, for url: URL, shouldStore: @escaping () -> Bool) {
        queue.async { [self] in
            guard shouldStore(), !data.isEmpty, Int64(data.count) <= policy.maximumBytes else { return }
            ensureIndex()
            do {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                guard isCacheDirectory else { return }
                let name = name(for: url)
                do {
                    guard let entry = try entry(at: fileURL(name)) else { return }
                    update(name, with: entry)
                } catch {
                    guard isMissingFile(error) else { return }
                    forget(name)
                }
                trim(incomingBytes: Int64(data.count), replacing: name)
                guard shouldStore(), projectedBytes(incomingBytes: Int64(data.count), replacing: name)
                    <= policy.maximumBytes else { return }
                try data.write(to: fileURL(name), options: .atomic)
                pendingUsage.remove(name)
                update(name, with: Entry(size: Int64(data.count), lastUse: Date()))
            } catch {}
        }
    }

    func discard(_ data: Data, for url: URL, shouldDiscard: @escaping () -> Bool) {
        queue.async { [self] in
            guard shouldDiscard() else { return }
            ensureIndex()
            let name = name(for: url)
            guard isCacheDirectory, refreshEntry(name),
                  let stored = try? Data(contentsOf: fileURL(name)), stored == data else { return }
            remove(name)
        }
    }

    private var isCacheDirectory: Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: directory.path)
        return attributes?[.type] as? FileAttributeType == .typeDirectory
    }

    private func name(for url: URL) -> String {
        SHA256.hash(data: Data(url.absoluteString.utf8))
            .map { String(format: "%02x", $0) }.joined()
    }

    private func fileURL(_ name: String) -> URL {
        directory.appending(path: name)
    }

    private func isMissingFile(_ error: Error) -> Bool {
        let error = error as NSError
        return error.domain == NSCocoaErrorDomain
            && (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError)
    }

    private func entry(at url: URL) throws -> Entry? {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        guard attributes[.type] as? FileAttributeType == .typeRegular,
              let size = attributes[.size] as? NSNumber else { return nil }
        return Entry(size: size.int64Value, lastUse: attributes[.modificationDate] as? Date ?? .distantPast)
    }

    @discardableResult
    private func refreshEntry(_ name: String) -> Bool {
        do {
            guard let entry = try entry(at: fileURL(name)) else {
                forget(name)
                return false
            }
            update(name, with: entry)
            return true
        } catch {
            if isMissingFile(error) { forget(name) }
            return false
        }
    }

    private func update(_ name: String, with entry: Entry) {
        var entry = entry
        if let previous = entries[name] {
            totalBytes -= previous.size
            entry.lastUse = max(entry.lastUse, previous.lastUse)
        }
        entries[name] = entry
        totalBytes += entry.size
    }

    private func forget(_ name: String) {
        if let entry = entries.removeValue(forKey: name) { totalBytes -= entry.size }
        pendingUsage.remove(name)
    }

    @discardableResult
    private func remove(_ name: String) -> Bool {
        guard isCacheDirectory, refreshEntry(name) else { return entries[name] == nil }
        do {
            try FileManager.default.removeItem(at: fileURL(name))
            forget(name)
            return true
        } catch {
            if isMissingFile(error) {
                forget(name)
                return true
            }
            return false
        }
    }

    private func recordUsage(_ name: String, at date: Date) {
        guard var entry = entries[name] else { return }
        entry.lastUse = date
        entries[name] = entry
        pendingUsage.insert(name)
        guard !usageFlushScheduled else { return }
        usageFlushScheduled = true
        queue.asyncAfter(deadline: .now() + 60) { [weak self] in
            guard let self else { return }
            self.usageFlushScheduled = false
            self.flushUsage()
        }
    }

    private func flushUsage() {
        guard isCacheDirectory else { return }
        for name in pendingUsage {
            guard refreshEntry(name), let entry = entries[name] else { continue }
            do {
                try FileManager.default.setAttributes([.modificationDate: entry.lastUse],
                                                      ofItemAtPath: fileURL(name).path)
                pendingUsage.remove(name)
            } catch {}
        }
    }

    private func ensureIndex() {
        guard !indexed else { return }
        indexed = true
        maintain()
    }

    private func maintain() {
        flushUsage()
        guard isCacheDirectory else {
            entries.removeAll()
            totalBytes = 0
            pendingUsage.removeAll()
            return
        }
        guard let files = try? FileManager.default.contentsOfDirectory(at: directory,
                                                                      includingPropertiesForKeys: nil) else { return }
        var found: [String: Entry] = [:]
        for file in files {
            let name = file.lastPathComponent
            guard name.utf8.count == 64, name.utf8.allSatisfy({
                (48...57).contains($0) || (97...102).contains($0)
            }) else { continue }
            do {
                guard var entry = try entry(at: file) else { continue }
                if let previous = entries[name] { entry.lastUse = max(entry.lastUse, previous.lastUse) }
                found[name] = entry
            } catch {
                if !isMissingFile(error), let previous = entries[name] { found[name] = previous }
            }
        }
        entries = found
        totalBytes = entries.values.reduce(0) { $0 + $1.size }
        pendingUsage.formIntersection(entries.keys)
        let now = Date()
        for (name, entry) in entries where entry.size == 0
            || now.timeIntervalSince(entry.lastUse) >= policy.maximumAge {
            remove(name)
        }
        trim()
    }

    private func projectedBytes(incomingBytes: Int64, replacing name: String?) -> Int64 {
        totalBytes - (name.flatMap { entries[$0]?.size } ?? 0) + incomingBytes
    }

    private func trim(incomingBytes: Int64 = 0, replacing name: String? = nil) {
        guard projectedBytes(incomingBytes: incomingBytes, replacing: name) > policy.maximumBytes else { return }
        let oldest = entries.filter { $0.key != name }.sorted {
            $0.value.lastUse == $1.value.lastUse ? $0.key < $1.key : $0.value.lastUse < $1.value.lastUse
        }
        for (candidate, _) in oldest {
            guard projectedBytes(incomingBytes: incomingBytes, replacing: name) > policy.preferredBytes else { break }
            remove(candidate)
        }
    }
}

final class WorkshopImageLoader {
    static let shared = WorkshopImageLoader()

    struct Source: Hashable {
        let url: URL
        var directory: URL?
        var relativePath: String?

        var key: String {
            if let directory, let relativePath {
                return directory.absoluteString + "#" + relativePath
            }
            return url.absoluteString
        }

        func resolvedURL() -> URL? {
            if let directory, let relativePath {
                return PathContainment.containedURL(relativePath, in: directory)
            }
            return url
        }
    }

    struct Variant: Hashable {
        let pixels: Int
        let animated: Bool
    }

    final class Result {
        let image: NSImage
        let animation: NSImage?
        let cost: Int

        init(image: NSImage, animation: NSImage?, cost: Int) {
            self.image = image
            self.animation = animation
            self.cost = cost
        }
    }

    private struct Consumer {
        let variant: Variant
        var priority: Int
        let completion: (Result?) -> Void
    }

    private final class Request: @unchecked Sendable {
        let source: Source
        var consumers: [UUID: Consumer] = [:]
        var task: URLSessionDataTask?
        var data: Data?
        var active = false
        var decoding = false
        var cachesToDisk = false
        private let lock = NSLock()
        private var cancelled = false

        init(source: Source) { self.source = source }

        var priority: Int { consumers.values.map(\.priority).max() ?? 0 }

        var isCancelled: Bool {
            lock.lock()
            defer { lock.unlock() }
            return cancelled
        }

        func cancel() {
            lock.lock()
            cancelled = true
            lock.unlock()
            task?.cancel()
        }
    }

    private let memory: NSCache<NSString, Result> = {
        let cache = NSCache<NSString, Result>()
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
    private let ioQueue = DispatchQueue(label: "cn.laobamac.Mirage.workshopImage",
                                        qos: .userInitiated, attributes: .concurrent)
    private let session: URLSession
    private let diskCache: WorkshopImageDiskCache

    init(session: URLSession? = nil, cacheDirectory: URL? = nil) {
        if let session {
            self.session = session
        } else {
            let configuration = URLSessionConfiguration.default
            configuration.timeoutIntervalForRequest = 20
            configuration.timeoutIntervalForResource = 40
            configuration.httpMaximumConnectionsPerHost = 4
            configuration.urlCache = nil
            self.session = URLSession(configuration: configuration)
        }
        let directory = cacheDirectory ?? FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/WorkshopImageCache")
        diskCache = WorkshopImageDiskCache(directory: directory)
    }

    private var requests: [String: Request] = [:]
    private var owners: [UUID: Request] = [:]
    private var waiting: [Request] = []
    private var activeCount = 0

    static func pixels(for size: CGSize, scale: CGFloat) -> Int {
        let raw = Double(max(size.width, size.height)) * Double(max(scale, 1))
        return max(64, min(Int((raw / 64).rounded(.up) * 64), 2048))
    }

    private func memoryKey(_ source: Source, _ variant: Variant) -> NSString {
        "\(source.key)#\(variant.pixels)#\(variant.animated)" as NSString
    }

    func startCacheMaintenance() {
        diskCache.startMaintenance()
    }

    func cachedImage(source: Source, variant: Variant) -> Result? {
        guard let cached = memory.object(forKey: memoryKey(source, variant)) else { return nil }
        if !source.url.isFileURL { diskCache.recordUsage(for: source.url) }
        return cached
    }

    @discardableResult
    func load(source: Source, variant: Variant, priority: Int,
              completion: @escaping (Result?) -> Void) -> UUID {
        let token = UUID()
        if let cached = cachedImage(source: source, variant: variant) {
            completion(cached)
            return token
        }
        let request: Request
        if let existing = requests[source.key], !existing.isCancelled {
            request = existing
        } else {
            request = Request(source: source)
            requests[source.key] = request
            waiting.append(request)
        }
        request.consumers[token] = Consumer(variant: variant, priority: priority, completion: completion)
        owners[token] = request
        if request.data != nil && !request.decoding { decode(request) }
        startWaiting()
        return token
    }

    func promote(_ token: UUID, priority: Int) {
        guard let request = owners[token], var consumer = request.consumers[token] else { return }
        consumer.priority = priority
        request.consumers[token] = consumer
        startWaiting()
    }

    func cancel(_ token: UUID) {
        guard let request = owners.removeValue(forKey: token) else { return }
        request.consumers[token] = nil
        guard request.consumers.isEmpty else { return }
        request.cancel()
        if !request.active { finish(request) }
    }

    private func startWaiting() {
        while activeCount < 4 {
            waiting.removeAll { $0.isCancelled }
            guard let index = waiting.indices.max(by: {
                waiting[$0].priority < waiting[$1].priority
            }) else { return }
            let request = waiting[index]
            if request.priority == 0 && activeCount > 0 { return }
            waiting.remove(at: index)
            request.active = true
            activeCount += 1
            fetch(request)
        }
    }

    private func fetch(_ request: Request) {
        let source = request.source
        ioQueue.async { [self] in
            guard !request.isCancelled, let url = source.resolvedURL() else {
                DispatchQueue.main.async { self.fail(request) }
                return
            }
            if let cached = dataMemory.object(forKey: source.key as NSString) {
                if !url.isFileURL { diskCache.recordUsage(for: source.url) }
                DispatchQueue.main.async { self.receive(cached as Data, for: request) }
                return
            }
            if url.isFileURL {
                let data = try? Data(contentsOf: url, options: .mappedIfSafe)
                DispatchQueue.main.async { self.receive(data, for: request) }
                return
            }
            diskCache.read(for: source.url) { data in
                DispatchQueue.main.async {
                    guard !request.isCancelled else { self.finish(request); return }
                    if let data {
                        self.receive(data, for: request)
                        return
                    }
                    request.task = self.session.dataTask(with: url) { [self] data, response, _ in
                        let valid = (response as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? true
                        DispatchQueue.main.async {
                            request.cachesToDisk = true
                            self.receive(valid ? data : nil, for: request)
                        }
                    }
                    request.task?.resume()
                }
            }
        }
    }

    private func receive(_ data: Data?, for request: Request) {
        guard !request.isCancelled else { finish(request); return }
        guard let data, !data.isEmpty else { fail(request); return }
        dataMemory.setObject(data as NSData, forKey: request.source.key as NSString, cost: data.count)
        request.data = data
        decode(request)
    }

    private func decode(_ request: Request) {
        guard !request.isCancelled, !request.decoding, let data = request.data else { return }
        request.decoding = true
        let variants = Set(request.consumers.values.map(\.variant))
        let cachesToDisk = request.cachesToDisk
        ioQueue.async { [self] in
            var results: [Variant: Result] = [:]
            for variant in variants {
                guard !request.isCancelled else { break }
                autoreleasepool {
                    guard let image = Self.downsample(data, maxPixel: variant.pixels) else { return }
                    var animation: NSImage?
                    var animatedCost = 0
                    if variant.animated, let safeData = Self.safeAnimationData(data),
                       let animated = NSImage(data: safeData),
                       let rep = animated.representations.first as? NSBitmapImageRep {
                        rep.setProperty(.loopCount, withValue: 0)
                        let frames = max(rep.value(forProperty: .frameCount) as? Int ?? 1, 1)
                        animatedCost = rep.pixelsWide * rep.pixelsHigh * 4 * frames
                        animation = animated
                    }
                    let rep = image.representations.first
                    let cost = max(1, (rep?.pixelsWide ?? 1) * (rep?.pixelsHigh ?? 1) * 4 + animatedCost)
                    results[variant] = Result(image: image, animation: animation, cost: cost)
                }
            }
            if !request.isCancelled {
                if results.isEmpty {
                    dataMemory.removeObject(forKey: request.source.key as NSString)
                    if !request.source.url.isFileURL {
                        diskCache.discard(data, for: request.source.url) { !request.isCancelled }
                    }
                } else if cachesToDisk {
                    diskCache.store(data, for: request.source.url) { !request.isCancelled }
                }
            }
            DispatchQueue.main.async {
                request.decoding = false
                request.cachesToDisk = false
                guard !request.isCancelled else { self.finish(request); return }
                for (variant, result) in results {
                    self.memory.setObject(result, forKey: self.memoryKey(request.source, variant), cost: result.cost)
                }
                let completed = request.consumers.filter { variants.contains($0.value.variant) }
                for token in completed.keys {
                    guard let consumer = request.consumers.removeValue(forKey: token),
                          !request.isCancelled else { continue }
                    self.owners[token] = nil
                    consumer.completion(results[consumer.variant])
                }
                if request.consumers.isEmpty { self.finish(request) }
                else { self.decode(request) }
            }
        }
    }

    private func fail(_ request: Request) {
        let consumers = request.consumers
        request.consumers.removeAll()
        for token in consumers.keys { owners[token] = nil }
        finish(request)
        for consumer in consumers.values {
            if !request.isCancelled { consumer.completion(nil) }
        }
    }

    private func finish(_ request: Request) {
        if request.active {
            request.active = false
            activeCount -= 1
        }
        if requests[request.source.key] === request { requests[request.source.key] = nil }
        waiting.removeAll { $0 === request }
        startWaiting()
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
    private struct LoadRequest: Equatable {
        let source: WorkshopImageLoader.Source?
        let pixels: Int
        let contentActive: Bool
        let isLoadingEnabled: Bool
        let preloadsWhenInactive: Bool
        let isAnimating: Bool
    }

    private let source: WorkshopImageLoader.Source?
    var contentMode: ContentMode
    var isAnimating: Bool
    var isLoadingEnabled: Bool
    var preloadsWhenInactive: Bool
    var showsLoadingIndicator: Bool

    @Environment(\.displayScale) private var displayScale
    @Environment(\.mirageContentActive) private var contentActive
    @State private var image: NSImage?
    @State private var animation: NSImage?
    @State private var failed = false
    @State private var requestID: UUID?
    @State private var loadToken: UInt64 = 0
    @State private var loadedKey: String?
    @State private var loadedSource: WorkshopImageLoader.Source?

    init(url: URL?, contentMode: ContentMode = .fill, isAnimating: Bool = false,
         isLoadingEnabled: Bool = true, preloadsWhenInactive: Bool = false,
         showsLoadingIndicator: Bool = true) {
        source = url.map { WorkshopImageLoader.Source(url: $0) }
        self.contentMode = contentMode
        self.isAnimating = isAnimating
        self.isLoadingEnabled = isLoadingEnabled
        self.preloadsWhenInactive = preloadsWhenInactive
        self.showsLoadingIndicator = showsLoadingIndicator
    }

    init(wallpaper: WEWallpaper, contentMode: ContentMode = .fill, isAnimating: Bool = false,
         isLoadingEnabled: Bool = true, preloadsWhenInactive: Bool = false,
         showsLoadingIndicator: Bool = true) {
        source = wallpaper.project.preview.isEmpty ? nil : WorkshopImageLoader.Source(
            url: wallpaper.wallpaperDirectory.appending(path: wallpaper.project.preview),
            directory: wallpaper.wallpaperDirectory, relativePath: wallpaper.project.preview)
        self.contentMode = contentMode
        self.isAnimating = isAnimating
        self.isLoadingEnabled = isLoadingEnabled
        self.preloadsWhenInactive = preloadsWhenInactive
        self.showsLoadingIndicator = showsLoadingIndicator
    }

    var body: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.10))
            .overlay {
                if loadedSource == source, let image {
                    Image(nsImage: image)
                        .resizable()
                        .interpolation(.high)
                        .aspectRatio(contentMode: contentMode)
                    if contentActive && isLoadingEnabled && isAnimating, let animation, let source {
                        WorkshopAnimatedImage(image: animation, identity: source.key, contentMode: contentMode)
                    }
                } else if loadedSource == source && failed {
                    Image(systemName: "photo")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                } else if showsLoadingIndicator && source != nil {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: "photo")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                }
            }
            .clipped()
            .background(
                GeometryReader { proxy in
                    let pixels = proxy.size.width > 1 && proxy.size.height > 1
                        ? WorkshopImageLoader.pixels(for: proxy.size, scale: displayScale) : 0
                    let request = LoadRequest(
                        source: source, pixels: pixels, contentActive: contentActive,
                        isLoadingEnabled: isLoadingEnabled, preloadsWhenInactive: preloadsWhenInactive,
                        isAnimating: isAnimating)
                    Color.clear
                        .task(id: request) {
                            guard !Task.isCancelled else { return }
                            load(request)
                        }
                }
            )
            .onDisappear {
                cancel()
                image = nil
                animation = nil
            }
    }

    private func cancel() {
        if let requestID { WorkshopImageLoader.shared.cancel(requestID) }
        requestID = nil
        loadToken &+= 1
        loadedKey = nil
    }

    private func load(_ request: LoadRequest) {
        if loadedSource != request.source {
            cancel()
            loadedSource = request.source
            image = nil
            animation = nil
            failed = false
        }
        guard let source = request.source else {
            failed = true
            return
        }
        guard request.pixels > 0, request.contentActive &&
                (request.isLoadingEnabled || request.preloadsWhenInactive) else {
            cancel()
            animation = nil
            return
        }
        let variant = WorkshopImageLoader.Variant(
            pixels: request.pixels,
            animated: request.isAnimating && request.isLoadingEnabled)
        if !variant.animated { animation = nil }
        let key = "\(source.key)#\(variant.pixels)#\(variant.animated)"
        if loadedKey == key {
            if let requestID {
                WorkshopImageLoader.shared.promote(requestID, priority: request.isLoadingEnabled ? 1 : 0)
            }
            return
        }
        cancel()
        loadedKey = key
        if let cached = WorkshopImageLoader.shared.cachedImage(source: source, variant: variant) {
            image = cached.image
            animation = variant.animated ? cached.animation : nil
            failed = false
            return
        }
        let token = loadToken
        requestID = WorkshopImageLoader.shared.load(source: source, variant: variant,
                                                     priority: request.isLoadingEnabled ? 1 : 0) { result in
            guard loadToken == token else { return }
            requestID = nil
            if let result {
                image = result.image
                animation = variant.animated ? result.animation : nil
                failed = false
            } else {
                failed = true
                loadedKey = nil
            }
        }
    }
}

private struct WorkshopAnimatedImage: NSViewRepresentable {
    let image: NSImage
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
        if context.coordinator.identity != identity || view.image !== image {
            context.coordinator.identity = identity
            view.image = image
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
        didSet { if contentMode != oldValue { needsLayout = true } }
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
