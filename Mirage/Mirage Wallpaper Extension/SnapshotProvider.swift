//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import CoreGraphics
import Foundation
@preconcurrency import IOSurface
import ImageIO

enum MirageSnapshotProvider {
    static func makeSnapshot(from configuration: MirageLockConfiguration?, displayID: UInt32? = nil,
                             showWallpaper: Bool? = nil) -> AnyObject? {
        guard let configuration,
              let display = displayID.flatMap({ configuration.displays["display-\($0)"] })
                ?? configuration.displays.values.sorted(by: { $0.displayID < $1.displayID }).first else { return nil }
        let image: CGImage?
        if (showWallpaper ?? currentScreenLockState()) && configuration.enabled != false {
            image = previewImage(for: display)
        } else {
            image = display.desktopFallbackPath.flatMap(loadImage)
                ?? systemFallbackImage()
        }
        guard let image else { return nil }
        return makeSnapshot(from: image)
    }

    static func previewImage(for display: MirageLockDisplayConfiguration?) -> CGImage? {
        if let image = display?.renderedPreviewPath.flatMap(loadImage) { return image }
        let fallback = Bundle.main.url(forResource: "thumbnail", withExtension: "png")
            ?? URL(fileURLWithPath: "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/SidebarDisplay.icns")
        return loadImage(fallback.path)
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
