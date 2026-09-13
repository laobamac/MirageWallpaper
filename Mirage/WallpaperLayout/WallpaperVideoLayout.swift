//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AVFoundation
import QuartzCore

final class WallpaperVideoLayout {
    private let layer: AVPlayerLayer
    private let fillMode: String
    private let position: WallpaperPosition
    private var bounds: CGRect
    private var playerObservation: NSKeyValueObservation?
    private var sizeObservation: NSKeyValueObservation?
    private var presentationSize = CGSize.zero

    init(layer: AVPlayerLayer, bounds: CGRect, fillMode: String, position: WallpaperPosition) {
        self.layer = layer
        self.bounds = bounds
        self.fillMode = fillMode
        self.position = position
        layer.autoresizingMask = []
        layer.videoGravity = .resizeAspect
        playerObservation = layer.player?.observe(\.currentItem, options: [.initial, .new]) { [weak self] player, _ in
            DispatchQueue.main.async {
                self?.observeSize(of: player.currentItem)
            }
        }
        update(bounds: bounds)
    }

    private func observeSize(of item: AVPlayerItem?) {
        sizeObservation = item?.observe(\.presentationSize, options: [.initial, .new]) { [weak self] _, _ in
            DispatchQueue.main.async {
                guard let self else { return }
                self.update(bounds: self.bounds)
            }
        }
        update(bounds: bounds)
    }

    func update(bounds: CGRect) {
        self.bounds = bounds
        if let source = layer.player?.currentItem?.presentationSize, source.width > 0, source.height > 0 {
            presentationSize = source
        }
        let layout = WallpaperLayout(source: presentationSize, bounds: bounds, fillMode: fillMode,
                                     position: position,
                                     flipped: layer.superlayer?.isGeometryFlipped ?? false)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer.videoGravity = fillMode == "stretch" ? .resize : .resizeAspect
        layer.frame = layout.frame
        CATransaction.commit()
    }
}
