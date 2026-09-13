//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import QuartzCore

final class PlaylistTransitionOverlay {
    static let shared = PlaylistTransitionOverlay()

    private var windows: [Int: NSWindow] = [:]
    private var generations: [Int: UUID] = [:]
    private let lock = NSLock()

    private init() {}

    func present(on screen: Int,
                 duration: TimeInterval,
                 kind: PlaylistTransitionKind,
                 apply: @escaping () -> Void) {
        let generation = UUID()
        lock.lock()
        generations[screen] = generation
        lock.unlock()
        DispatchQueue.main.async {
            self.performPresentation(screen: screen, duration: duration, kind: kind,
                                     generation: generation, apply: apply)
        }
    }

    func cancel(on screen: Int) {
        lock.lock()
        generations[screen] = nil
        let window = windows.removeValue(forKey: screen)
        lock.unlock()
        DispatchQueue.main.async { window?.orderOut(nil) }
    }

    private func isCurrent(_ generation: UUID, on screen: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return generations[screen] == generation
    }

    private func performPresentation(screen: Int,
                                     duration: TimeInterval,
                                     kind: PlaylistTransitionKind,
                                     generation: UUID,
                                     apply: @escaping () -> Void) {
        guard isCurrent(generation, on: screen) else { return }
        dismissWindow(on: screen)
        guard duration > 0.05, kind != .disabled,
              screen >= 0, screen < NSScreen.screens.count else {
            apply()
            dismissWindow(on: screen, generation: generation)
            return
        }
        let ns = NSScreen.screens[screen]
        let window = makeWindow(on: ns)
        let contentView = window.contentView!
        contentView.wantsLayer = true
        let layer = contentView.layer!
        layer.backgroundColor = NSColor.black.cgColor
        layer.opacity = 0.0
        window.orderFrontRegardless()

        lock.lock()
        guard generations[screen] == generation else {
            lock.unlock()
            window.orderOut(nil)
            return
        }
        windows[screen] = window
        lock.unlock()

        let fadeIn = CABasicAnimation(keyPath: "opacity")
        fadeIn.fromValue = 0.0
        fadeIn.toValue = 1.0
        fadeIn.duration = duration / 2
        fadeIn.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        fadeIn.fillMode = .forwards
        fadeIn.isRemovedOnCompletion = false
        layer.add(fadeIn, forKey: "fade-in")
        layer.opacity = 1.0

        DispatchQueue.main.asyncAfter(deadline: .now() + duration / 2) { [weak self] in
            guard let self, self.isCurrent(generation, on: screen) else { return }
            apply()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                guard self.isCurrent(generation, on: screen) else { return }
                let fadeOut = CABasicAnimation(keyPath: "opacity")
                fadeOut.fromValue = 1.0
                fadeOut.toValue = 0.0
                fadeOut.duration = duration / 2
                fadeOut.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                fadeOut.fillMode = .forwards
                fadeOut.isRemovedOnCompletion = false
                layer.add(fadeOut, forKey: "fade-out")
                layer.opacity = 0.0
                DispatchQueue.main.asyncAfter(deadline: .now() + duration / 2 + 0.05) {
                    self.dismissWindow(on: screen, generation: generation)
                }
            }
        }
    }

    private func dismissWindow(on screen: Int, generation: UUID? = nil) {
        lock.lock()
        if let generation, generations[screen] != generation {
            lock.unlock()
            return
        }
        if generation != nil { generations[screen] = nil }
        let window = windows.removeValue(forKey: screen)
        lock.unlock()
        window?.orderOut(nil)
    }

    private func makeWindow(on screen: NSScreen) -> NSWindow {
        let window = NSWindow(contentRect: screen.frame,
                              styleMask: .borderless,
                              backing: .buffered,
                              defer: false,
                              screen: screen)
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.desktopIconWindow)))
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        window.contentView = NSView(frame: NSRect(origin: .zero, size: screen.frame.size))
        return window
    }
}
