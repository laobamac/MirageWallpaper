//
//  DesktopWallpaperNotchMask.swift
//  Mirage Wallpaper
//
//  Created by 王孝慈 on 2026/11/26.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum DesktopWallpaperNotchMask {
    static func write(from sourceURL: URL, to destinationURL: URL,
                      screenHeight: CGFloat, topInset: CGFloat) throws -> URL {
        guard screenHeight.isFinite, screenHeight > 0,
              topInset.isFinite, topInset >= 0, topInset <= screenHeight else {
            throw CocoaError(.coderInvalidValue)
        }
        guard topInset > 0 else { return sourceURL }
        guard sourceURL.standardizedFileURL != destinationURL.standardizedFileURL,
              let source = CGImageSourceCreateWithURL(sourceURL as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
              image.width > 0, image.height > 0 else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let colorSpace = image.colorSpace.flatMap { $0.model == .rgb ? $0 : nil }
            ?? CGColorSpace(name: CGColorSpace.sRGB)
        let bitsPerComponent = image.bitsPerComponent > 8 ? 16 : 8
        guard let colorSpace,
              let context = CGContext(data: nil, width: image.width, height: image.height,
                                      bitsPerComponent: bitsPerComponent, bytesPerRow: 0,
                                      space: colorSpace,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            throw CocoaError(.coderInvalidValue)
        }
        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        let insetPixels = min(height, ceil(height * (topInset / screenHeight)))
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        context.setBlendMode(.copy)
        context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
        context.fill(CGRect(x: 0, y: height - insetPixels, width: width, height: insetPixels))
        let data = NSMutableData()
        guard let masked = context.makeImage(),
              let destination = CGImageDestinationCreateWithData(
                data, UTType.png.identifier as CFString, 1, nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        CGImageDestinationAddImage(destination, masked, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw CocoaError(.fileWriteUnknown)
        }
        try (data as Data).write(to: destinationURL, options: .atomic)
        return destinationURL
    }
}
