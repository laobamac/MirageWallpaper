//
//  UserTextureCache.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import CryptoKit
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum UserTextureCacheError: LocalizedError {
    case unsupportedImage
    case encodingFailed

    var errorDescription: String? {
        switch self {
        case .unsupportedImage: return L("无法读取所选图片")
        case .encodingFailed: return L("无法保存所选图片")
        }
    }
}

final class UserTextureCache {
    static let shared = UserTextureCache()

    private let fm = FileManager.default
    private let lock = NSLock()

    private var directory: URL {
        fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/UserTextureCache", directoryHint: .isDirectory)
    }

    func importImage(at source: URL) throws -> URL {
        let data = try Data(contentsOf: source, options: [.mappedIfSafe])
        let key = digest(data + Data("user-texture-v2".utf8))
        let destination = directory.appending(path: "\(key).png")
        lock.lock()
        defer { lock.unlock() }
        if fm.fileExists(atPath: destination.path) { return destination }
        guard let image = decodedImage(at: source) else {
            throw UserTextureCacheError.unsupportedImage
        }
        try write(image, to: destination)
        return destination
    }

    func shortcutIconPath(for target: String) -> String? {
        let expanded = (target as NSString).expandingTildeInPath
        guard (expanded as NSString).isAbsolutePath,
              fm.fileExists(atPath: expanded) else { return nil }
        let source = URL(fileURLWithPath: expanded).standardizedFileURL
        let values = try? source.resourceValues(forKeys: [.contentModificationDateKey])
        let stamp = values?.contentModificationDate?.timeIntervalSince1970 ?? 0
        let key = digest(Data("shortcut-icon-v2|\(source.path)|\(stamp)".utf8))
        let destination = directory.appending(path: "\(key).png")
        lock.lock()
        defer { lock.unlock() }
        if fm.fileExists(atPath: destination.path) { return destination.path }
        let icon = NSWorkspace.shared.icon(forFile: source.path)
        guard let image = renderedIcon(icon, size: 512) else { return nil }
        do {
            try write(image, to: destination)
            return destination.path
        } catch {
            return nil
        }
    }

    private func decodedImage(at url: URL) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        let count = CGImageSourceGetCount(source)
        guard count > 0 else { return nil }
        var bestIndex = 0
        var bestArea = 0
        for index in 0..<count {
            guard let properties = CGImageSourceCopyPropertiesAtIndex(source, index, nil)
                    as? [CFString: Any],
                  let width = properties[kCGImagePropertyPixelWidth] as? NSNumber,
                  let height = properties[kCGImagePropertyPixelHeight] as? NSNumber else { continue }
            let area = width.intValue * height.intValue
            if area > bestArea {
                bestArea = area
                bestIndex = index
            }
        }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: 2048
        ]
        return CGImageSourceCreateThumbnailAtIndex(source, bestIndex, options as CFDictionary)
            ?? CGImageSourceCreateImageAtIndex(source, bestIndex, nil)
    }

    private func renderedIcon(_ image: NSImage, size: Int) -> CGImage? {
        guard let representation = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: size,
            pixelsHigh: size,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bitmapFormat: .alphaFirst,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ) else { return nil }
        representation.size = NSSize(width: size, height: size)
        guard let context = NSGraphicsContext(bitmapImageRep: representation) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.imageInterpolation = .high
        image.draw(in: NSRect(x: 0, y: 0, width: size, height: size),
                   from: .zero,
                   operation: .copy,
                   fraction: 1)
        context.flushGraphics()
        NSGraphicsContext.restoreGraphicsState()
        return representation.cgImage
    }

    private func write(_ image: CGImage, to destination: URL) throws {
        let data = NSMutableData()
        guard let writer = CGImageDestinationCreateWithData(
            data, UTType.png.identifier as CFString, 1, nil) else {
            throw UserTextureCacheError.encodingFailed
        }
        CGImageDestinationAddImage(writer, image, nil)
        guard CGImageDestinationFinalize(writer) else {
            throw UserTextureCacheError.encodingFailed
        }
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        try (data as Data).write(to: destination, options: .atomic)
    }

    private func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}
