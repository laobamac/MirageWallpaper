//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AVFoundation
import CoreGraphics
import Foundation
@preconcurrency import IOSurface
import ImageIO

enum MirageSnapshotProvider {
    static func makeSnapshot(from configuration: MirageLockConfiguration?) -> AnyObject? {
        guard let display = configuration?.displays.values.first else { return nil }
        let locked = currentScreenLockState()
        let image: CGImage?
        if locked && configuration?.enabled != false {
            image = display.previewPath.flatMap(loadImage)
                ?? (display.kind == "video" ? loadVideoFrame(at: URL(fileURLWithPath: display.entryPath)) : nil)
        } else {
            image = display.desktopFallbackPath.flatMap(loadImage)
                ?? systemFallbackImage()
        }
        guard let image else { return nil }
        return makeSnapshot(from: image)
    }

    private static func currentScreenLockState() -> Bool {
        guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
        for key in ["CGSSessionScreenIsLocked", "kCGSSessionScreenIsLocked"] {
            if let value = session[key] as? NSNumber { return value.boolValue }
            if let value = session[key] as? Bool { return value }
        }
        return false
    }

    private static func systemFallbackImage() -> CGImage? {
        let directories = [
            URL(fileURLWithPath: "/System/Library/Desktop Pictures", isDirectory: true),
            URL(fileURLWithPath: "/System/Library/CoreServices", isDirectory: true)
        ]
        for directory in directories {
            let candidates = (try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: nil, options: .skipsHiddenFiles
            )) ?? []
            for candidate in candidates.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                if let image = loadImage(candidate.path) { return image }
            }
        }
        return nil
    }

    private static func loadImage(_ path: String) -> CGImage? {
        let url = URL(fileURLWithPath: path)
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(source, 0, nil)
    }

    private static func loadVideoFrame(at url: URL) -> CGImage? {
        let generator = AVAssetImageGenerator(asset: AVURLAsset(url: url))
        generator.appliesPreferredTrackTransform = true
        return try? generator.copyCGImage(at: .zero, actualTime: nil)
    }

    static func makeSnapshot(from image: CGImage) -> AnyObject? {
        let properties: [IOSurfacePropertyKey: any Sendable] = [
            .width: image.width,
            .height: image.height,
            .bytesPerElement: 4,
            .pixelFormat: 0x42475241
        ]
        guard let surface = IOSurface(properties: properties),
              let snapshotClass = NSClassFromString("WallpaperSnapshotXPC"),
              let storage = class_getInstanceVariable(snapshotClass, "rawValue"),
              ivar_getOffset(storage) >= MemoryLayout<UnsafeRawPointer>.size,
              ivar_getOffset(storage) + MemoryLayout<UnsafeRawPointer>.size <= class_getInstanceSize(snapshotClass),
              let instance = class_createInstance(snapshotClass, 0),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: surface.baseAddress,
                width: image.width,
                height: image.height,
                bitsPerComponent: 8,
                bytesPerRow: surface.bytesPerRow,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
              ) else { return nil }
        surface.lock(options: [], seed: nil)
        context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        surface.unlock(options: [], seed: nil)
        let retainedSurface = Unmanaged.passRetained(surface).toOpaque()
        Unmanaged.passUnretained(instance as AnyObject).toOpaque()
            .advanced(by: ivar_getOffset(storage))
            .storeBytes(of: retainedSurface, as: UnsafeMutableRawPointer.self)
        return instance as AnyObject
    }
}
