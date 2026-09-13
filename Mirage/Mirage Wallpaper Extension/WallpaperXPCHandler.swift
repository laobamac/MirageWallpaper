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
    let wallpaperID: UUID?
    let context: CAContext
    let rootLayer: CALayer
    var renderer: MirageLockRenderer?
    let displayID: UInt32
    let configurationKey: String?
    let isPreview: Bool
    var isLocked: Bool

    init(id: UInt32, wallpaperID: UUID?, context: CAContext, rootLayer: CALayer,
         renderer: MirageLockRenderer?, displayID: UInt32, configurationKey: String?,
         isPreview: Bool, isLocked: Bool) {
        self.id = id
        self.wallpaperID = wallpaperID
        self.context = context
        self.rootLayer = rootLayer
        self.renderer = renderer
        self.displayID = displayID
        self.configurationKey = configurationKey
        self.isPreview = isPreview
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
    private var invalidated = false
    private let instanceID = UUID()
    private var settingsRequestID = UUID()
    private var configurationDigest: String?
    private var settingsReady = false
    private var settingsError: String?
    private var rendererErrors: [UInt32: String] = [:]
    private var readyContexts: Set<UInt32> = []
    private var renderedConfigurationDigest: String?

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
        }, MirageLockBridge.configurationNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().reloadPreviews()
        }, MirageLockBridge.previewNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().reloadDesktopFallbacks()
        }, MirageLockBridge.desktopFallbackNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().setLocked(true)
        }, MirageLockBridge.lockedNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().setLocked(false)
        }, MirageLockBridge.unlockedNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().scheduleWakeRecovery()
        }, MirageLockBridge.wakeNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().pauseForSleep()
        }, MirageLockBridge.sleepNotification as CFString, nil, .deliverImmediately)
        CFNotificationCenterAddObserver(center, retained, { _, object, _, _, _ in
            guard let object else { return }
            Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(object).takeUnretainedValue().reloadContexts()
        }, MirageLockBridge.probeNotification as CFString, nil, .deliverImmediately)
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
        let distributed = DistributedNotificationCenter.default()
        lockObservers.forEach { distributed.removeObserver($0) }
        let workspace = NSWorkspace.shared.notificationCenter
        sleepObservers.forEach { workspace.removeObserver($0) }
        wakeObservers.forEach { workspace.removeObserver($0) }
    }

    func invalidateAll() {
        let stop = { [self] in
            guard !invalidated else { return }
            invalidated = true
            settingsRequestID = UUID()
            agentProxy = nil
            if let observer {
                self.observer = nil
                CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetDarwinNotifyCenter(), observer)
                Unmanaged<MirageWallpaperXPCHandler>.fromOpaque(observer).release()
            }
            lock.lock()
            let values = Array(contexts.values)
            contexts.removeAll()
            lock.unlock()
            values.forEach { $0.renderer?.stop() }
            if let container = try? MirageLockBridge.containerURL() {
                try? FileManager.default.removeItem(at: MirageLockBridge.runtimeDirectory(in: container)
                    .appendingPathComponent("status-\(instanceID.uuidString).json"))
                MirageLockBridge.post(MirageLockBridge.statusNotification)
            }
        }
        if Thread.isMainThread { stop() } else { DispatchQueue.main.async(execute: stop) }
    }

    func connectionFailed(_ error: Error) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.invalidated else { return }
            self.settingsReady = false
            self.settingsError = error.localizedDescription
            self.publishHealth()
        }
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
        let isPreview = Self.field(named: "isPreview", in: request) as? Bool ?? false
        let configurationKey = (Self.field(named: "configuration", in: request) as? Data)
            .flatMap { String(data: $0, encoding: .utf8) }
        let work = {
            guard !self.invalidated else {
                reply(nil, MirageLockBridge.failure("Wallpaper connection is invalidated"))
                return
            }
            if !isPreview {
                if let presentationMode {
                    self.isLocked = presentationMode == "locked"
                } else if let locked = Self.currentScreenLockState() {
                    self.isLocked = locked
                }
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
            let identifier = self.contextID(from: id) ?? context.contextId
            let renderer: MirageLockRenderer?
            if isPreview {
                renderer = nil
            } else {
                guard let activeRenderer = self.renderer(
                    for: displayID, rootLayer: rootLayer, size: size,
                    scale: scale, locked: self.isLocked, contextID: identifier) else {
                    self.reloadSettings()
                    reply(nil, MirageLockBridge.failure("Lock screen configuration is unavailable"))
                    return
                }
                renderer = activeRenderer
            }
            let active = MirageLockContext(
                id: identifier, wallpaperID: Self.uuid(from: id), context: context, rootLayer: rootLayer,
                renderer: renderer, displayID: displayID, configurationKey: configurationKey,
                isPreview: isPreview, isLocked: self.isLocked)
            self.lock.lock()
            let previous = self.contexts[identifier]?.renderer
            self.contexts[identifier] = active
            self.lock.unlock()
            previous?.stop()
            if isPreview { self.updatePreview(active, configuration: Self.loadConfiguration()) }
            reply(remote, nil)
            self.reloadSettings()
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func update(withId id: Any?, request: Any?, reply: @escaping ((any Error)?) -> Void) {
        let mode = Self.enumCase(named: "presentationMode", in: request)
        let work = {
            if let mode, let identifier = self.contextID(from: id) {
                self.setLocked(mode == "locked", contextID: identifier)
            }
            reply(nil)
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func invalidate(withId id: Any?, reply: @escaping ((any Error)?) -> Void) {
        let work = {
            let identifier = self.contextID(from: id)
            self.lock.lock()
            let removed = identifier.flatMap { self.contexts.removeValue(forKey: $0) }
            self.lock.unlock()
            removed?.renderer?.stop()
            if let identifier {
                self.readyContexts.remove(identifier)
                self.rendererErrors.removeValue(forKey: identifier)
            }
            self.publishHealth()
            reply(nil)
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func snapshot(withId id: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        let work = {
            do {
                let stored = try MirageLockConfigurationStore.shared.load()
                let active = self.contextID(from: id).flatMap { self.contexts[$0] }
                let displayID = active.flatMap { self.display(for: $0, configuration: stored.configuration)?.displayID }
                guard let snapshot = MirageSnapshotProvider.makeSnapshot(
                    from: stored.configuration, displayID: displayID,
                    showWallpaper: active.map { $0.isPreview || $0.isLocked }) else {
                    throw MirageLockBridge.failure("Unable to create wallpaper snapshot")
                }
                reply(snapshot, nil)
            } catch {
                NSLog("[MirageLock] snapshot failed: %@", error.localizedDescription)
                reply(nil, error)
            }
        }
        if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
    }

    func provideSettingsViewModels(withContentTypes types: Any?, reply: @escaping (Any?, (any Error)?) -> Void) {
        do {
            let stored = try MirageLockConfigurationStore.shared.load()
            reply(try buildMirageSettingsViewModels(configuration: stored.configuration), nil)
            DispatchQueue.main.async { [weak self] in self?.reloadSettings() }
        } catch {
            connectionFailed(error)
            reply(nil, error)
        }
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
        guard !invalidated else { return }
        reloadSettings(invalidateSnapshots: true)
        lock.lock()
        let values = Array(contexts.values)
        lock.unlock()
        guard let stored = try? MirageLockConfigurationStore.shared.load() else { return }
        let digest = MirageLockBridge.digest(stored.data)
        guard renderedConfigurationDigest != digest else {
            publishHealth()
            return
        }
        renderedConfigurationDigest = digest
        let configuration = stored.configuration
        readyContexts.removeAll()
        rendererErrors.removeAll()
        values.forEach {
            $0.renderer?.stop()
            $0.renderer = nil
        }
        values.forEach { context in
            if context.isPreview {
                updatePreview(context, configuration: configuration)
                return
            }
            let renderer = self.renderer(
                for: context.displayID, rootLayer: context.rootLayer,
                size: context.rootLayer.bounds.size,
                scale: context.rootLayer.contentsScale,
                configuration: configuration,
                locked: context.isLocked, contextID: context.id)
            lock.lock()
            context.renderer = renderer
            lock.unlock()
        }
        publishHealth()
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
                context.renderer?.setLocked(false)
            }
        }
        agentProxy?.invalidateSnapshots { error in
            if let error { NSLog("[MirageLock] snapshot invalidation failed: %@", error.localizedDescription) }
        }
    }

    private func reloadPreviews() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.reloadPreviews() }
            return
        }
        guard !invalidated else { return }
        let configuration = Self.loadConfiguration()
        lock.lock()
        let previews = contexts.values.filter(\.isPreview)
        lock.unlock()
        previews.forEach { updatePreview($0, configuration: configuration) }
        reloadSettings(invalidateSnapshots: true)
    }

    private func display(for context: MirageLockContext,
                         configuration: MirageLockConfiguration?) -> MirageLockDisplayConfiguration? {
        guard let configuration else { return nil }
        return context.configurationKey.flatMap { configuration.displays[$0] }
            ?? configuration.displays["display-\(context.displayID)"]
            ?? configuration.displays.values.sorted(by: { $0.displayID < $1.displayID }).first
    }

    private func updatePreview(_ context: MirageLockContext, configuration: MirageLockConfiguration?) {
        let entry = configuration?.enabled == false ? nil : display(for: context, configuration: configuration)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        context.rootLayer.contents = MirageSnapshotProvider.previewImage(for: entry)
        context.rootLayer.contentsGravity = .resizeAspect
        context.rootLayer.masksToBounds = true
        CATransaction.commit()
        CATransaction.flush()
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
        readyContexts.removeAll()
        values.forEach { $0.renderer?.prepareForSleep() }
        publishHealth()
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

    private func reloadSettings(invalidateSnapshots: Bool = false) {
        guard !invalidated, let proxy = agentProxy else { return }
        let requestID = UUID()
        settingsRequestID = requestID
        settingsReady = false
        do {
            let stored = try MirageLockConfigurationStore.shared.load()
            configurationDigest = MirageLockBridge.digest(stored.data)
            let models = try buildMirageSettingsViewModels(configuration: stored.configuration)
            settingsError = nil
            proxy.updateSettingsViewModels(models) { [weak self] error in
                DispatchQueue.main.async {
                    guard let self, !self.invalidated, self.settingsRequestID == requestID else { return }
                    self.settingsReady = error == nil
                    self.settingsError = error?.localizedDescription
                    self.publishHealth()
                    if error == nil, invalidateSnapshots {
                        proxy.invalidateSnapshots { error in
                            if let error { NSLog("[MirageLock] preview invalidation failed: %@", error.localizedDescription) }
                        }
                    }
                }
            }
        } catch {
            configurationDigest = nil
            settingsError = error.localizedDescription
            NSLog("[MirageLock] settings refresh failed: %@", error.localizedDescription)
            publishHealth()
        }
    }

    private func publishHealth() {
        guard !invalidated else { return }
        do {
            let container = try MirageLockBridge.containerURL()
            let probe = try MirageLockBridge.readProbe(in: container)
            let identity = MirageLockRuntimeIdentity.current
            let report = MirageLockReport(
                probeID: probe.id, instanceID: instanceID, processID: ProcessInfo.processInfo.processIdentifier,
                extensionPath: identity.path, fingerprint: identity.fingerprint, version: identity.version,
                configurationDigest: configurationDigest, settingsReady: settingsReady,
                readyDisplayIDs: readyContexts.compactMap { contexts[$0]?.displayID }.sorted(),
                error: settingsError ?? rendererErrors.sorted(by: { $0.key < $1.key }).first?.value)
            try report.write(in: container)
        } catch {
            if (error as NSError).code != NSFileReadNoSuchFileError {
                NSLog("[MirageLock] status reporting failed: %@", error.localizedDescription)
            }
        }
    }

    private func setLocked(_ locked: Bool, contextID: UInt32? = nil) {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in
                self?.setLocked(locked, contextID: contextID)
            }
            return
        }
        let effectiveLocked = locked && (Self.loadConfiguration().map { $0.enabled != false } ?? false)
        lock.lock()
        let values: [MirageLockContext]
        if let contextID {
            values = contexts[contextID].map { [$0] } ?? []
        } else {
            isLocked = locked
            values = Array(contexts.values)
        }
        let renderers = values.filter { !$0.isPreview }
        renderers.forEach { $0.isLocked = locked }
        lock.unlock()
        renderers.forEach { $0.renderer?.setLocked(effectiveLocked) }
    }

    private func renderer(for displayID: UInt32, rootLayer: CALayer,
                                 size: CGSize, scale: CGFloat,
                                 configuration: MirageLockConfiguration? = nil,
                                 locked: Bool, contextID: UInt32) -> MirageLockRenderer? {
        let config = configuration ?? Self.loadConfiguration()
        guard let entry = config?.displays["display-\(displayID)"] ?? config?.displays.values.first else { return nil }
        return MirageLockRenderer(
            rootLayer: rootLayer, size: size, scale: scale,
            configuration: entry,
            locked: locked && config?.enabled != false,
            dynamicEnabled: config?.enabled != false,
            onStateChange: { [weak self] error in
                guard let self, !self.invalidated else { return }
                if let error {
                    self.rendererErrors[contextID] = error
                    self.readyContexts.remove(contextID)
                } else {
                    self.rendererErrors.removeValue(forKey: contextID)
                    self.readyContexts.insert(contextID)
                }
                self.publishHealth()
            })
    }

    private static func loadConfiguration() -> MirageLockConfiguration? {
        (try? MirageLockConfigurationStore.shared.load().configuration)
            ?? MirageLockConfigurationStore.shared.lastKnownConfiguration
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

    private func contextID(from value: Any?) -> UInt32? {
        guard let uuid = Self.uuid(from: value) else { return Self.uint32(from: value) }
        lock.lock()
        defer { lock.unlock() }
        return contexts.values.first { $0.wallpaperID == uuid }?.id
    }

    private static func uuid(from value: Any?) -> UUID? {
        value as? UUID ?? field(named: "id", in: value) as? UUID
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
        let displayID = field(named: "directDisplayID", in: request) as? UInt32
            ?? field(named: "displayID", in: request) as? UInt32
        let size = field(named: "size", in: request) as? CGSize
        let scale = (field(named: "scaleFactor", in: request) as? NSNumber).map { CGFloat($0.doubleValue) }
        return (displayID, size.flatMap { $0.width > 0 && $0.height > 0 ? $0 : nil },
                scale.flatMap { $0 > 0 ? $0 : nil })
    }

    private static func enumCase(named name: String, in value: Any?) -> String? {
        guard let found = field(named: name, in: value) else { return nil }
        let mirror = Mirror(reflecting: found)
        if mirror.displayStyle == .enum, let label = mirror.children.first?.label { return label }
        return String(describing: found).split(separator: "(").first.map(String.init)
    }

    private static func field(named name: String, in value: Any?, depth: Int = 0) -> Any? {
        guard let value, depth < 8 else { return nil }
        for child in Mirror(reflecting: value).children {
            if child.label == name {
                var result = child.value
                while Mirror(reflecting: result).displayStyle == .optional {
                    guard let wrapped = Mirror(reflecting: result).children.first else { return nil }
                    result = wrapped.value
                }
                return result
            }
            if let result = field(named: name, in: child.value, depth: depth + 1) { return result }
        }
        return nil
    }
}
