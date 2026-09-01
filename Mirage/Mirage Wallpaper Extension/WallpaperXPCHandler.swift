//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation
import QuartzCore

private final class MirageLockContext {
    let id: UInt32
    let context: CAContext
    let rootLayer: CALayer
    var renderer: MirageLockRenderer?
    let displayID: UInt32
    var isLocked: Bool

    init(id: UInt32, context: CAContext, rootLayer: CALayer,
         renderer: MirageLockRenderer?, displayID: UInt32, isLocked: Bool) {
        self.id = id
        self.context = context
        self.rootLayer = rootLayer
        self.renderer = renderer
        self.displayID = displayID
        self.isLocked = isLocked
    }
}

final class MirageWallpaperXPCHandler: NSObject, WallpaperExtensionXPCProtocol {
    var agentProxy: WallpaperExtensionProxyXPCProtocol?
    private var contexts: [UInt32: MirageLockContext] = [:]
    private let lock = NSLock()
    private var observer: UnsafeMutableRawPointer?
    private var lockObservers: [NSObjectProtocol] = []
    private var sleepObservers: [NSObjectProtocol] = []
    private var wakeObservers: [NSObjectProtocol] = []
    private var wakeRecoveryScheduled = false
    private var isLocked = false

    override init() {
        super.init()
        if let locked = Self.currentScreenLockState() {
            isLocked = locked
        }
        let retained = Unmanaged.passRetained(self).toOpaque()
        observer = retained
        let center = CFNotificationCenterGetDarwinNotifyCenter()
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().reloadContexts()
        }, "cn.laobamac.Mirage.dynamicLockScreen.configurationChanged" as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().reloadDesktopFallbacks()
        }, "cn.laobamac.Mirage.dynamicLockScreen.desktopFallbackChanged" as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().setLocked(true)
        }, "cn.laobamac.Mirage.dynamicLockScreen.locked" as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().setLocked(false)
        }, "cn.laobamac.Mirage.dynamicLockScreen.unlocked" as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().scheduleWakeRecovery()
        }, "cn.laobamac.Mirage.dynamicLockScreen.wake" as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().pauseForSleep()
        }, "cn.laobamac.Mirage.dynamicLockScreen.sleep" as CFString, nil, .deliverImmediately)
        let distributed = DistributedNotificationCenter.default()
        lockObservers = [
            distributed.addObserver(forName: NSNotification.Name("com.apple.screenIsLocked"), object: nil, queue: .main) { [weak self] _ in
                self?.setLocked(true)
            },
            distributed.addObserver(forName: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil, queue: .main) { [weak self] _ in
                self?.setLocked(false)
            }
        ]
        let workspace = NSWorkspace.shared.notificationCenter
        sleepObservers = [
            workspace.addObserver(forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: .main) { [weak self] _ in
                self?.pauseForSleep()
            }
        ]
        wakeObservers = [
            workspace.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
                self?.scheduleWakeRecovery()
            },
            workspace.addObserver(forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: .main) { [weak self] _ in
                self?.scheduleWakeRecovery()
            }
        ]
    }

    deinit {
        if let observer {
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.configurationChanged" as CFString), nil)
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.desktopFallbackChanged" as CFString), nil)
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.locked" as CFString), nil)
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.unlocked" as CFString), nil)
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.wake" as CFString), nil)
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer, CFNotificationName("cn.laobamac.Mirage.dynamicLockScreen.sleep" as CFString), nil)
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(observer).release()
        }
        let distributed = DistributedNotificationCenter.default()
        lockObservers.forEach { distributed.removeObserver($0) }
        let workspace = NSWorkspace.shared.notificationCenter
        sleepObservers.forEach { workspace.removeObserver($0) }
        wakeObservers.forEach { workspace.removeObserver($0) }
        invalidateAll()
    }

    func invalidateAll() {
        lock.lock()
        let values = Array(contexts.values)
        contexts.removeAll()
        lock.unlock()
        let stop = {
            values.forEach { $0.renderer?.stop() }
        }
        if Thread.isMainThread { stop() } else { DispatchQueue.main.async(execute: stop) }
    }

    func acquire(withId id: Any?, request: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        guard #available(macOS 26.0, *) else {
            reply(nil, NSError(domain: "MirageWallpaperExtension", code: 1))
            return
        }
        let geometry = Self.geometry(from: request)
        let displayID = geometry.displayID ?? Self.firstDisplayID()
        guard let displayID else {
            reply(nil, NSError(domain: "MirageWallpaperExtension", code: 2))
            return
        }
        let size = geometry.size ?? CGSize(width: CGDisplayBounds(displayID).width, height: CGDisplayBounds(displayID).height)
        let scale = geometry.scale ?? 1
        let presentationMode = Self.enumCase(named: "presentationMode", in: request)
        let work = {
            if let presentationMode {
                self.isLocked = presentationMode == "locked"
            } else if let locked = Self.currentScreenLockState() {
                self.isLocked = locked
            }
            let options: [String: Any] = ["displayId": NSNumber(value: displayID)]
            guard let context = CAContext.perform(NSSelectorFromString("remoteContextWithOptions:"), with: options)?.takeUnretainedValue() as? CAContext,
                  context.contextId != 0,
                  let remote = Self.remoteContextObject(context.contextId) else {
                reply(nil, NSError(domain: "MirageWallpaperExtension", code: 3))
                return
            }
            let rootLayer = CALayer()
            rootLayer.frame = CGRect(origin: .zero, size: size)
            rootLayer.contentsScale = scale
            rootLayer.backgroundColor = NSColor.black.cgColor
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            context.layer = rootLayer
            CATransaction.commit()
            CATransaction.flush()
            let renderer = Self.renderer(
                for: displayID, rootLayer: rootLayer, size: size,
                scale: scale, locked: self.isLocked)
            let active = MirageLockContext(
                id: context.contextId, context: context, rootLayer: rootLayer,
                renderer: renderer, displayID: displayID, isLocked: self.isLocked)
            self.lock.lock()
            let previous = self.contexts[context.contextId]?.renderer
            self.contexts[context.contextId] = active
            self.lock.unlock()
            previous?.stop()
            reply(remote, nil)
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func update(withId id: Any?, request: Any?, reply: @escaping ((any Error)?) -> Void) {
        if let mode = Self.enumCase(named: "presentationMode", in: request) {
            setLocked(mode == "locked", contextID: Self.uint32(from: id))
        }
        reply(nil)
    }

    func invalidate(withId id: Any?, reply: @escaping ((any Error)?) -> Void) {
        let identifier = Self.uint32(from: id)
        let work = {
            self.lock.lock()
            let removed = identifier.flatMap { self.contexts.removeValue(forKey: $0) }
            self.lock.unlock()
            removed?.renderer?.stop()
            reply(nil)
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func snapshot(withId id: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        let work = {
            reply(MirageSnapshotProvider.makeSnapshot(from: Self.loadConfiguration()), nil)
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func provideSettingsViewModels(withContentTypes types: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        reply(buildMirageSettingsViewModels(), nil)
    }

    func addChoiceRequest(withChoiceRequest request: Any?, onBehalfOfProcess process: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        reply(nil, nil)
    }

    func removeChoiceRequest(withChoiceRequest request: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func selectedChoicesDidChange(for id: Any?, reply: @escaping ((any Error)?) -> Void) { reloadContexts(); reply(nil) }
    func invokeContextMenuAction(withMenuItemID menuItemID: Any?, groupItemID: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func isChoiceDownloaded(with choiceID: Any?, reply: @escaping (Bool, (any Error)?) -> Void) { reply(true, nil) }
    func download(withChoiceID choiceID: Any?, reply: @escaping ((any Error)?) -> Void) -> Any? { reply(nil); return nil }
    func pauseDownload(for choiceID: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func cancelDownload(for choiceID: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func resumeDownload(for choiceID: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func removeDownload(for choiceID: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func migrateSelectedChoice(for id: Any?, reply: @escaping (Any?, (any Error)?) -> Void) { reply(nil, nil) }
    func migrate(from: Any?, to: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func skipShuffledContent(withId id: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }
    func canSkipShuffledContent(withId id: Any?, reply: @escaping (Bool, (any Error)?) -> Void) { reply(false, nil) }
    func handleDebugRequest(for request: Any?, reply: @escaping (Any?, (any Error)?) -> Void) { reply(nil, nil) }
    func handleNotification(withNamed name: Any?, reply: @escaping ((any Error)?) -> Void) { reply(nil) }

    private func reloadContexts() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.reloadContexts() }
            return
        }
        reloadSettings()
        lock.lock()
        let values = Array(contexts.values)
        lock.unlock()
        guard let configuration = Self.loadConfiguration() else {
            values.forEach {
                $0.isLocked = false
                $0.renderer?.setLocked(false)
            }
            return
        }
        values.forEach {
            $0.renderer?.stop()
            $0.renderer = nil
        }
        values.forEach { context in
            let renderer = Self.renderer(
                for: context.displayID, rootLayer: context.rootLayer,
                size: context.rootLayer.bounds.size,
                scale: context.rootLayer.contentsScale,
                configuration: configuration,
                locked: context.isLocked && configuration.enabled != false)
            lock.lock()
            context.renderer = renderer
            lock.unlock()
        }
    }

    private func reloadDesktopFallbacks() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.reloadDesktopFallbacks() }
            return
        }
        guard let configuration = Self.loadConfiguration() else { return }
        lock.lock()
        let values = Array(contexts.values)
        lock.unlock()
        values.forEach { context in
            let entry = configuration.displays["display-\(context.displayID)"]
                ?? configuration.displays.values.first
            context.renderer?.updateDesktopFallback(path: entry?.desktopFallbackPath)
            if configuration.enabled == false {
                context.isLocked = false
                context.renderer?.setLocked(false)
            }
        }
        agentProxy?.invalidateSnapshots { _ in }
    }

    private func scheduleWakeRecovery() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.scheduleWakeRecovery() }
            return
        }
        guard !wakeRecoveryScheduled else { return }
        wakeRecoveryScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            guard let self else { return }
            self.wakeRecoveryScheduled = false
            self.recoverAfterWake()
        }
    }

    private func pauseForSleep() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.pauseForSleep() }
            return
        }
        lock.lock()
        let values = Array(contexts.values)
        lock.unlock()
        NSLog("[MirageLock] display sleep: pausing %lu contexts", values.count)
        values.forEach { $0.renderer?.prepareForSleep() }
    }

    private func recoverAfterWake() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.recoverAfterWake() }
            return
        }
        lock.lock()
        let values = Array(contexts.values)
        lock.unlock()
        NSLog("[MirageLock] display wake: recovering %lu contexts", values.count)
        values.forEach { context in
            context.renderer?.recoverAfterWake()
            context.context.layer = context.rootLayer
        }
        agentProxy?.invalidateSnapshots { _ in }
    }

    private func reloadSettings() {
        guard let models = buildMirageSettingsViewModels(),
              let proxy = agentProxy else { return }
        proxy.updateSettingsViewModels(models) { _ in }
    }

    private func setLocked(_ locked: Bool, contextID: UInt32? = nil) {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in
                self?.setLocked(locked, contextID: contextID)
            }
            return
        }
        let effectiveLocked = locked && Self.loadConfiguration()?.enabled != false
        isLocked = effectiveLocked
        lock.lock()
        let values = contextID.flatMap { identifier in
            contexts[identifier].map { [$0] }
        } ?? Array(contexts.values)
        values.forEach { $0.isLocked = effectiveLocked }
        lock.unlock()
        values.forEach { $0.renderer?.setLocked(effectiveLocked) }
    }

    private static func renderer(for displayID: UInt32, rootLayer: CALayer,
                                 size: CGSize, scale: CGFloat,
                                 configuration: MirageLockConfiguration? = nil,
                                 locked: Bool) -> MirageLockRenderer? {
        let config = configuration ?? loadConfiguration()
        guard let entry = config?.displays["display-\(displayID)"] ?? config?.displays.values.first else { return nil }
        return MirageLockRenderer(
            rootLayer: rootLayer, size: size, scale: scale,
            configuration: entry,
            locked: locked && config?.enabled != false,
            dynamicEnabled: config?.enabled != false)
    }

    private static func loadConfiguration() -> MirageLockConfiguration? {
        guard let container = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: "group.cn.laobamac.Mirage"),
              let data = try? Data(contentsOf: container.appendingPathComponent("dynamic-lock-screen.json")) else { return nil }
        return try? JSONDecoder().decode(MirageLockConfiguration.self, from: data)
    }

    private static func remoteContextObject(_ id: UInt32) -> AnyObject? {
        guard let cls = objc_getClass("WallpaperRemoteContextXPC") as? AnyClass,
              let raw = class_createInstance(cls, 0) else { return nil }
        let object = raw as AnyObject
        if object.responds(to: NSSelectorFromString("setBox:")) || object.responds(to: NSSelectorFromString("setContextId:")) {
            object.setValue(NSNumber(value: id), forKey: "box")
            return object
        }
        guard let ivar = class_getInstanceVariable(cls, "box") else { return object }
        let offset = ivar_getOffset(ivar)
        guard offset + MemoryLayout<UInt32>.size <= class_getInstanceSize(cls) else { return object }
        Unmanaged.passUnretained(object).toOpaque().advanced(by: offset).storeBytes(of: id, as: UInt32.self)
        return object
    }

    private static func firstDisplayID() -> UInt32? {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return nil }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetActiveDisplayList(count, &ids, &count) == .success else { return nil }
        return ids.first
    }

    private static func currentScreenLockState() -> Bool? {
        guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return nil }
        for key in ["CGSSessionScreenIsLocked", "kCGSSessionScreenIsLocked"] {
            if let value = session[key] as? NSNumber { return value.boolValue }
            if let value = session[key] as? Bool { return value }
        }
        return false
    }

    private static func uint32(from value: Any?) -> UInt32? {
        guard let value else { return nil }
        if let number = value as? NSNumber { return number.uint32Value }
        func find(_ value: Any, depth: Int) -> UInt32? {
            guard depth < 5 else { return nil }
            if let number = value as? NSNumber { return number.uint32Value }
            let mirror = Mirror(reflecting: value)
            for child in mirror.children {
                if ["box", "contextId", "contextID"].contains(child.label ?? ""),
                   let number = child.value as? NSNumber {
                    return number.uint32Value
                }
                if let found = find(child.value, depth: depth + 1) { return found }
            }
            return nil
        }
        return find(value, depth: 0)
    }

    private static func geometry(from request: Any?) -> (displayID: UInt32?, size: CGSize?, scale: CGFloat?) {
        var descriptions: [String] = []
        func collect(_ value: Any, depth: Int) {
            guard depth < 3, descriptions.count < 32 else { return }
            descriptions.append(String(describing: value))
            Mirror(reflecting: value).children.forEach { collect($0.value, depth: depth + 1) }
        }
        if let request { collect(request, depth: 0) }
        func number(after marker: String) -> Double? {
            for text in descriptions {
                guard let range = text.range(of: marker) else { continue }
                let suffix = text[range.upperBound...].drop(while: { $0 == " " })
                let token = suffix.prefix { $0.isNumber || $0 == "." || $0 == "-" }
                if let value = Double(token) { return value }
            }
            return nil
        }
        let displayID = number(after: "directDisplayID: ").flatMap { UInt32(exactly: Int($0)) }
            ?? number(after: "displayID: ").flatMap { UInt32(exactly: Int($0)) }
        let width = number(after: "width: ").map { CGFloat($0) }
        let height = number(after: "height: ").map { CGFloat($0) }
        let scale = number(after: "scaleFactor: ").flatMap { $0 > 0 ? CGFloat($0) : nil }
        let size = width.flatMap { w in height.flatMap { h in
            w > 0 && h > 0 ? CGSize(width: w, height: h) : nil
        } }
        return (displayID, size, scale)
    }

    private static func enumCase(named name: String, in value: Any?) -> String? {
        guard let value else { return nil }
        func find(_ value: Any, depth: Int) -> Any? {
            guard depth < 6 else { return nil }
            let mirror = Mirror(reflecting: value)
            for child in mirror.children {
                if child.label == name { return child.value }
                if let found = find(child.value, depth: depth + 1) { return found }
            }
            return nil
        }
        guard let found = find(value, depth: 0) else { return nil }
        let mirror = Mirror(reflecting: found)
        if mirror.displayStyle == .enum, let label = mirror.children.first?.label { return label }
        return String(describing: found).split(separator: "(").first.map(String.init)
    }
}
