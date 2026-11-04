//
//  MainWindowBackgroundView.swift
//  Mirage Wallpaper
//
//  Created on 2026/11/05.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit

final class MainWindowBackgroundView: NSView {
    private let hostedView: NSView
    private var effectView: NSVisualEffectView?
    private var accessibilityObserver: NSObjectProtocol?
    private var blurEnabled: Bool
    private var appliedBlurEnabled: Bool?

    init(hostedView: NSView, blurEnabled: Bool) {
        self.hostedView = hostedView
        self.blurEnabled = blurEnabled
        super.init(frame: hostedView.frame)
        hostedView.frame = bounds
        hostedView.autoresizingMask = [.width, .height]
        addSubview(hostedView)
        accessibilityObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.applyBackground()
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        if let accessibilityObserver {
            NSWorkspace.shared.notificationCenter.removeObserver(accessibilityObserver)
        }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        appliedBlurEnabled = nil
        applyBackground()
    }

    func setBlurEnabled(_ enabled: Bool) {
        guard blurEnabled != enabled else { return }
        blurEnabled = enabled
        applyBackground()
    }

    private func applyBackground() {
        guard let window else { return }
        let enabled = blurEnabled && !NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency
        guard appliedBlurEnabled != enabled else { return }
        appliedBlurEnabled = enabled
        if enabled {
            if effectView == nil {
                let effect = NSVisualEffectView(frame: bounds)
                effect.autoresizingMask = [.width, .height]
                effect.material = .underWindowBackground
                effect.blendingMode = .behindWindow
                effect.state = .active
                addSubview(effect, positioned: .below, relativeTo: hostedView)
                effectView = effect
            }
        } else {
            effectView?.removeFromSuperview()
            effectView = nil
        }
        window.isOpaque = !enabled
        window.backgroundColor = enabled ? .clear : .windowBackgroundColor
        window.titlebarAppearsTransparent = !enabled
        window.invalidateShadow()
    }
}
