// Copyright © 2026 王孝慈. All rights reserved.
import Foundation

/// Shared preparation for file exports and wireless transfers.
enum MobileWallpaperExportService {
    private static let conversionQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.mobile-export", qos: .userInitiated, attributes: .concurrent)

    @MainActor
    static func export(_ wallpaper: WEWallpaper, to output: URL,
                       options: SceneMobileExportOptions, progressID: UUID) async throws {
        let snapshot = wallpaper.kind == .scene && options.preRendered != nil
            ? await WallpaperBakeService.mobileSnapshot(for: wallpaper) : nil
        let progress = MobileTransferProgressModel.shared
        // Coordinate mobile pre-renders with desktop baking so large jobs do not compete for GPU memory.
        let queue = wallpaper.kind == .scene && options.preRendered != nil
            ? WallpaperBakeService.renderQueue : conversionQueue
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                do {
                    try FileManager.default.createDirectory(at: output.deletingLastPathComponent(),
                                                            withIntermediateDirectories: true)
                    switch wallpaper.kind {
                    case .video:
                        try MobileMPKGExporter.export(wallpaper, to: output) { completed, total in
                            progress.updatePreparation(id: progressID, completedBytes: completed, totalBytes: total)
                        }
                    case .scene:
                        try SceneMobileMPKGExporter.export(wallpaper, to: output, options: options, snapshot: snapshot) {
                            progress.updateConversion(id: progressID, fraction: $0)
                        }
                    case .web, .unsupported:
                        throw MobileMPKGExportError.unsupportedWallpaperType(wallpaper.kind)
                    }
                    continuation.resume()
                } catch { continuation.resume(throwing: error) }
            }
        }
    }
}
