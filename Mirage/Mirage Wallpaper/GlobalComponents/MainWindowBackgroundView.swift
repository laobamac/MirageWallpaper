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
    private var tintView: MainWindowTintView?
    private var contentConstraints: [NSLayoutConstraint] = []
    private var accessibilityObserver: NSObjectProtocol?
    private var blurEnabled: Bool
    private var appliedBlurEnabled: Bool?

    init(hostedView: NSView, blurEnabled: Bool) {
        self.hostedView = hostedView
        self.blurEnabled = blurEnabled
        super.init(frame: hostedView.frame)
        hostedView.translatesAutoresizingMaskIntoConstraints = false
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

    override func viewWillMove(toWindow newWindow: NSWindow?) {
        NSLayoutConstraint.deactivate(contentConstraints)
        contentConstraints.removeAll()
        super.viewWillMove(toWindow: newWindow)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let guide = window?.contentLayoutGuide {
            contentConstraints = [NSLayoutConstraint.Attribute.leading, .trailing, .top, .bottom].map {
                NSLayoutConstraint(item: hostedView, attribute: $0, relatedBy: .equal,
                                   toItem: guide, attribute: $0, multiplier: 1, constant: 0)
            }
            NSLayoutConstraint.activate(contentConstraints)
        }
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

                let tint = MainWindowTintView(frame: bounds)
                tint.autoresizingMask = [.width, .height]
                addSubview(tint, positioned: .above, relativeTo: effect)
                tintView = tint
            }
        } else {
            tintView?.removeFromSuperview()
            tintView = nil
            effectView?.removeFromSuperview()
            effectView = nil
        }
        window.isOpaque = !enabled
        window.backgroundColor = enabled ? .clear : .windowBackgroundColor
        window.invalidateShadow()
    }
}

private final class MainWindowTintView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.withAlphaComponent(0.12).setFill()
        dirtyRect.fill()
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}
