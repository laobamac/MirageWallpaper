import AVFoundation
import AppKit
import Darwin
import OSLog
import ScreenSaver

private let screenSaverLogger = Logger(
    subsystem: "cn.laobamac.Mirage.ScreenSaver",
    category: "Rendering"
)

private struct MirageSaverConfiguration {
    let title: String
    let kind: String
    let entryURL: URL
    let playbackEntryURL: URL
    let fallbackEntryURL: URL?
    let rawProperties: [String: Any]
    let fps: Int
    let fillMode: String
    let position: WallpaperPosition
    let positionsByDisplay: [String: WallpaperPosition]
    let enableHDRVideo: Bool
    let loadFromMemory: Bool
    let language: String
    let speed: Float
    let scriptStorage: [String: String]
    let hostApplicationPath: String?
    let identity: Data

    static func load(displayKey: String? = nil) -> Self? {
        let home: URL
        if let record = getpwuid(getuid()), let path = String(validatingUTF8: record.pointee.pw_dir) {
            home = URL(fileURLWithPath: path, isDirectory: true)
        } else {
            home = FileManager.default.homeDirectoryForCurrentUser
        }
        let configurationName = Bundle(for: MirageScreenSaverView.self).bundleIdentifier
            == "cn.laobamac.Mirage.DynamicLockScreen"
            ? "dynamic-lock-screen-screensaver.json"
            : "screensaver.json"
        let url = home
            .appendingPathComponent("Library/Application Support/Mirage/\(configurationName)")
        guard let data = try? Data(contentsOf: url),
              let decoded = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (decoded["version"] as? Int) == 1,
              let kind = decoded["kind"] as? String,
              kind == "video" || kind == "scene",
              let entryPath = decoded["entryPath"] as? String else { return nil }
        var object = decoded
        if let displayKey,
           let snapshots = decoded["runtimeByDisplay"] as? [String: [String: Any]],
           let snapshot = snapshots[displayKey] {
            for key in ["rawProperties", "fillMode", "position", "speed", "scriptStorage"] {
                if let value = snapshot[key] { object[key] = value }
            }
        }
        if let displayKey, let positions = decoded["positionsByDisplay"] as? [String: [String: Any]],
           let position = positions[displayKey] { object["position"] = position }
        var identityObject = object
        identityObject.removeValue(forKey: "runtimeByDisplay")
        identityObject.removeValue(forKey: "positionsByDisplay")
        guard let identity = try? JSONSerialization.data(withJSONObject: identityObject, options: .sortedKeys) else { return nil }
        let rawSpeed = (object["speed"] as? NSNumber)?.floatValue ?? 1
        let speed = rawSpeed.isFinite && rawSpeed > 0 ? rawSpeed : 1
        let configuredEntryURL = URL(fileURLWithPath: entryPath)
        let entryURL: URL
        if FileManager.default.fileExists(atPath: configuredEntryURL.path) {
            entryURL = configuredEntryURL
        } else if kind == "scene" {
            let packageURL = configuredEntryURL.deletingPathExtension().appendingPathExtension("pkg")
            guard FileManager.default.fileExists(atPath: packageURL.path) else { return nil }
            entryURL = packageURL
        } else {
            return nil
        }
        let candidate = (object["playableEntryPath"] as? String).map(URL.init(fileURLWithPath:))
        let fallbackEntryURL: URL?
        if kind == "video", let candidate,
           FileManager.default.fileExists(atPath: candidate.path),
           Self.isCurrentCache(candidate, for: entryURL) {
            fallbackEntryURL = candidate
        } else {
            fallbackEntryURL = nil
        }
        return Self(
            title: object["title"] as? String ?? "Mirage",
            kind: kind,
            entryURL: entryURL,
            playbackEntryURL: entryURL,
            fallbackEntryURL: fallbackEntryURL,
            rawProperties: object["rawProperties"] as? [String: Any] ?? [:],
            fps: max(10, min(object["fps"] as? Int ?? 30, 60)),
            fillMode: object["fillMode"] as? String ?? "cover",
            position: WallpaperPosition(dictionary: object["position"] as? [String: Any]),
            positionsByDisplay: (object["positionsByDisplay"] as? [String: [String: Any]] ?? [:])
                .mapValues { WallpaperPosition(dictionary: $0) },
            enableHDRVideo: object["enableHDRVideo"] as? Bool ?? false,
            loadFromMemory: object["loadFromMemory"] as? Bool ?? false,
            language: object["language"] as? String ?? Locale.preferredLanguages.first ?? "en",
            speed: speed,
            scriptStorage: object["scriptStorage"] as? [String: String] ?? [:],
            hostApplicationPath: object["hostApplicationPath"] as? String,
            identity: identity
        )
    }

    private static func isCurrentCache(_ cache: URL, for source: URL) -> Bool {
        let keys: Set<URLResourceKey> = [.contentModificationDateKey]
        guard let sourceDate = try? source.resourceValues(forKeys: keys).contentModificationDate,
              let cacheDate = try? cache.resourceValues(forKeys: keys).contentModificationDate else { return false }
        return cacheDate >= sourceDate
    }
}

private enum MirageSaverLocalization {
    static func string(_ key: String, language: String? = nil) -> String {
        let preferred = (language ?? Locale.preferredLanguages.first ?? "en").lowercased()
        let resource: String
        if preferred.hasPrefix("zh-hant") || preferred.hasPrefix("zh-tw") || preferred.hasPrefix("zh-hk") {
            resource = "zh-Hant"
        } else if preferred.hasPrefix("zh") {
            resource = "zh-Hans"
        } else {
            resource = "en"
        }
        let bundle = Bundle(for: MirageScreenSaverView.self)
        guard let path = bundle.path(forResource: resource, ofType: "lproj"),
              let localizedBundle = Bundle(path: path) else { return key }
        return localizedBundle.localizedString(forKey: key, value: key, table: "Localizable")
    }
}

struct MirageSaverRenderSize {
    let drawableWidth: UInt32
    let drawableHeight: UInt32
    let renderWidth: UInt32
    let renderHeight: UInt32

    init?(size: CGSize, isPreview: Bool) {
        guard size.width.isFinite, size.height.isFinite,
              size.width > 0, size.height > 0,
              let width = UInt32(exactly: max(1, size.width.rounded())),
              let height = UInt32(exactly: max(1, size.height.rounded())) else { return nil }
        let scale = isPreview ? max(1, 500 / min(size.width, size.height)) : 1
        guard scale.isFinite else { return nil }
        let scaledWidth = size.width >= 8192 / scale ? 8192 : ceil(size.width * scale)
        let scaledHeight = size.height >= 8192 / scale ? 8192 : ceil(size.height * scale)
        guard let renderWidth = UInt32(exactly: max(1, scaledWidth)),
              let renderHeight = UInt32(exactly: max(1, scaledHeight)) else { return nil }
        drawableWidth = isPreview ? 0 : width
        drawableHeight = isPreview ? 0 : height
        self.renderWidth = renderWidth
        self.renderHeight = renderHeight
    }
}

private final class MirageSceneLibrary {
    typealias Create = @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafePointer<CChar>?,
        UInt32, UInt32, UInt32, UInt32, UInt32, UnsafePointer<CChar>?, Double, Double, UnsafePointer<CChar>?
    ) -> UnsafeMutableRawPointer?
    typealias SetPaused = @convention(c) (UnsafeMutableRawPointer?, Int32) -> Void
    typealias FirstFrame = @convention(c) (UnsafeMutableRawPointer?) -> Void
    typealias SetFirstFrame = @convention(c) (UnsafeMutableRawPointer?, FirstFrame?, UnsafeMutableRawPointer?) -> Void
    typealias Destroy = @convention(c) (UnsafeMutableRawPointer?) -> Void

    let handle: UnsafeMutableRawPointer
    let create: Create
    let setPaused: SetPaused
    let setFirstFrame: SetFirstFrame
    let destroy: Destroy

    init?(libraryURL: URL) {
        guard let handle = dlopen(libraryURL.path, RTLD_NOW | RTLD_LOCAL) else { return nil }
        guard let createSymbol = dlsym(handle, "MirageSceneSaverCreateWithRuntime"),
              let pauseSymbol = dlsym(handle, "MirageSceneSaverSetPaused"),
              let firstFrameSymbol = dlsym(handle, "MirageSceneDesktopSetFirstFrameCallback"),
              let destroySymbol = dlsym(handle, "MirageSceneSaverDestroy") else {
            dlclose(handle)
            return nil
        }
        self.handle = handle
        create = unsafeBitCast(createSymbol, to: Create.self)
        setPaused = unsafeBitCast(pauseSymbol, to: SetPaused.self)
        setFirstFrame = unsafeBitCast(firstFrameSymbol, to: SetFirstFrame.self)
        destroy = unsafeBitCast(destroySymbol, to: Destroy.self)
    }

    deinit { dlclose(handle) }
}

private final class MirageSaverSceneSession {
    private final class Request: @unchecked Sendable {
        private let lock = NSLock()
        private var cancelled = false
        private var firstFrameAction: (() -> Void)?

        func onFirstFrame(_ action: @escaping () -> Void) {
            lock.lock()
            firstFrameAction = action
            lock.unlock()
        }

        func signalFirstFrame() {
            lock.lock()
            let action = firstFrameAction
            firstFrameAction = nil
            lock.unlock()
            DispatchQueue.main.async { [self] in
                if !isCancelled { action?() }
            }
        }

        var isCancelled: Bool {
            lock.lock()
            defer { lock.unlock() }
            return cancelled
        }

        func cancel() {
            lock.lock()
            cancelled = true
            lock.unlock()
        }
    }

    private final class Resources {
        let view: Unmanaged<NSView>
        let request: Request
        var library: MirageSceneLibrary?
        var engine: UnsafeMutableRawPointer?

        init(view: NSView, request: Request) {
            self.view = .passRetained(view)
            self.request = request
        }

        deinit {
            let view = view, library = library, engine = engine, request = request
            MirageSaverSceneSession.queue.async {
                withExtendedLifetime(request) {
                    if let library, let engine {
                        library.setFirstFrame(engine, nil, nil)
                        library.destroy(engine)
                    }
                }
                DispatchQueue.main.async { view.release() }
            }
        }
    }

    private static let queue = DispatchQueue(label: "cn.laobamac.Mirage.ScreenSaver.Scene", qos: .userInitiated)
    private let request = Request()
    private var resources: Resources?
    private var paused = true

    init(view: NSView, bundle: Bundle, configuration: MirageSaverConfiguration,
         size: MirageSaverRenderSize, position: WallpaperPosition,
         onFirstFrame: @escaping () -> Void = {}, completion: @escaping (String?) -> Void) {
        let request = request
        request.onFirstFrame(onFirstFrame)
        let resources = Resources(view: view, request: request)
        let data = (try? JSONSerialization.data(withJSONObject: configuration.rawProperties, options: .sortedKeys)) ?? Data("{}".utf8)
        let properties = String(data: data, encoding: .utf8) ?? "{}"
        let runtimeData = try? JSONSerialization.data(withJSONObject: [
            "speed": configuration.speed, "scriptStorage": configuration.scriptStorage
        ], options: .sortedKeys)
        let runtimeJSON = runtimeData.flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        Self.queue.async { [weak self] in
            let failure: String? = autoreleasepool {
                guard !request.isCancelled else { return nil }
                guard let host = MirageHostApplication.locate(configuredPath: configuration.hostApplicationPath, component: bundle) else {
                    return "场景屏保资源不完整"
                }
                guard let library = MirageSceneLibrary(libraryURL: host.sceneLibraryURL) else {
                    return "场景屏保组件不可用"
                }
                resources.library = library
                let assets = host.assetsURL
                let icd = host.vulkanICDURL
                guard !request.isCancelled else { return nil }
                setenv("VK_ICD_FILENAMES", icd.path, 1)
                setenv("VK_DRIVER_FILES", icd.path, 1)
                resources.engine = assets.path.withCString { assetsPath in
                    configuration.entryURL.path.withCString { packagePath in
                        properties.withCString { properties in
                            configuration.fillMode.withCString { fill in
                                runtimeJSON.withCString { runtime in
                                    library.create(resources.view.toOpaque(), assetsPath, packagePath, properties,
                                                   size.renderWidth, size.renderHeight,
                                                   size.drawableWidth, size.drawableHeight,
                                                   UInt32(configuration.fps), fill, position.x, position.y, runtime)
                                }
                            }
                        }
                    }
                }
                if let engine = resources.engine {
                    library.setFirstFrame(engine, { pointer in
                        guard let pointer else { return }
                        Unmanaged<Request>.fromOpaque(pointer).takeUnretainedValue().signalFirstFrame()
                    }, Unmanaged.passUnretained(request).toOpaque())
                }
                return resources.engine == nil ? "场景壁纸加载失败" : nil
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, !request.isCancelled else { return }
                if resources.engine != nil {
                    self.resources = resources
                    self.setPaused(self.paused)
                }
                completion(failure)
            }
        }
    }

    func setPaused(_ paused: Bool) {
        self.paused = paused
        if let resources, let engine = resources.engine {
            resources.library?.setPaused(engine, paused ? 1 : 0)
        }
    }

    func stop() {
        request.cancel()
        setPaused(true)
        resources = nil
    }

    deinit {
        request.cancel()
        if let resources, let engine = resources.engine {
            resources.library?.setPaused(engine, 1)
        }
    }
}

@objc(MirageScreenSaverView)
final class MirageScreenSaverView: ScreenSaverView {
    private var player: AVQueuePlayer?
    private var looper: AVPlayerLooper?
    private var memoryAssetLoader: MirageMemoryVideoAssetLoader?
    private var playerLayer: AVPlayerLayer?
    private var videoLayout: WallpaperVideoLayout?
    private var messageLabel: NSTextField?
    private var configuration: MirageSaverConfiguration?
    private var sceneSession: MirageSaverSceneSession?
    private var sceneView: NSView?
    private var wallpaperLoadWorkItem: DispatchWorkItem?
    private var didLoadWallpaper = false
    private var isAnimatingWallpaper = false
    private var isWaitingForLayout = false
    private var videoLoadTask: Task<Void, Never>?
    private var videoLoadID = UUID()
    private var hostReportedSize = CGSize.zero
    private var configurationObserver: NSObjectProtocol?
    private var videoReadyObservation: NSKeyValueObservation?
    private var finishReload: (() -> Void)?
    private var configurationReload: DispatchWorkItem?
    private var configurationRequestID = UUID()

    private var currentDisplayKey: String? {
        guard !isPreview, let screen = window?.screen,
              let displayID = (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
        else { return nil }
        return wallpaperDisplayKey(displayID)
    }

    private func observeConfiguration() {
        guard configurationObserver == nil else { return }
        configurationObserver = DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name("cn.laobamac.Mirage.screensaver.configurationChanged"),
            object: nil, queue: .main) { [weak self] notification in
                guard let self else { return }
                let target = Bundle(for: MirageScreenSaverView.self).bundleIdentifier
                    == "cn.laobamac.Mirage.DynamicLockScreen" ? "lock" : "saver"
                guard notification.object as? String == target else { return }
                self.reloadConfiguration()
            }
    }

    private func reloadConfiguration() {
        guard isAnimatingWallpaper else { return }
        configurationReload?.cancel()
        let request = UUID()
        configurationRequestID = request
        let displayKey = currentDisplayKey
        let work = DispatchWorkItem { [weak self] in
            let value = MirageSaverConfiguration.load(displayKey: displayKey)
            DispatchQueue.main.async { [weak self] in
                guard let self, self.configurationRequestID == request, self.isAnimatingWallpaper,
                      let value, value.identity != self.configuration?.identity else { return }
                self.preserveWallpaperForReload()
                self.releaseWallpaper(preservingReload: true)
                self.loadWallpaper(value)
            }
        }
        configurationReload = work
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 0.2, execute: work)
    }

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.masksToBounds = true
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { releaseWallpaper() }
        else { scheduleWallpaperLoad() }
    }

    deinit {
        finishReload?()
        videoReadyObservation?.invalidate()
        configurationReload?.cancel()
        if let configurationObserver { DistributedNotificationCenter.default().removeObserver(configurationObserver) }
        player?.pause()
        looper?.disableLooping()
        player?.removeAllItems()
        looper = nil
        playerLayer?.player = nil
        memoryAssetLoader = nil
        videoLoadTask?.cancel()
        wallpaperLoadWorkItem?.cancel()
        sceneSession?.stop()
    }

    private func localized(_ key: String) -> String {
        MirageSaverLocalization.string(key, language: configuration?.language)
    }

    private func scheduleWallpaperLoad() {
        guard isAnimatingWallpaper, !didLoadWallpaper, window != nil,
              wallpaperLoadWorkItem == nil else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            defer { self.wallpaperLoadWorkItem = nil }
            self.loadWallpaper()
        }
        wallpaperLoadWorkItem = work
        DispatchQueue.main.async(execute: work)
    }

    private func loadWallpaper(_ replacement: MirageSaverConfiguration? = nil) {
        guard isAnimatingWallpaper, !didLoadWallpaper, window != nil else { return }
        layoutSubtreeIfNeeded()
        normalizeFullScreenBoundsIfNeeded()
        layoutSubtreeIfNeeded()
        guard hasValidBounds,
              MirageSaverRenderSize(size: convertToBacking(bounds).size, isPreview: isPreview) != nil else {
            if !isWaitingForLayout {
                screenSaverLogger.info("Waiting for valid screen saver layout: \(self.bounds.width, privacy: .public)x\(self.bounds.height, privacy: .public)")
                isWaitingForLayout = true
            }
            return
        }
        isWaitingForLayout = false
        didLoadWallpaper = true
        guard let configuration = replacement ?? MirageSaverConfiguration.load(displayKey: currentDisplayKey) else {
            showMessage(MirageSaverLocalization.string("请先在 Mirage 设置中选择屏保壁纸"))
            return
        }
        self.configuration = configuration
        animationTimeInterval = 1.0 / Double(configuration.fps)
        switch configuration.kind {
        case "video": loadVideo(configuration)
        case "scene": loadScene(configuration)
        default: showMessage(localized("不支持的壁纸格式"))
        }
    }

    private func position(for configuration: MirageSaverConfiguration) -> WallpaperPosition {
        guard !isPreview,
              let screen = window?.screen,
              let displayID = (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value,
              let key = wallpaperDisplayKey(displayID) else { return configuration.position }
        return configuration.positionsByDisplay[key] ?? configuration.position
    }

    private func loadScene(_ configuration: MirageSaverConfiguration) {
        let bundle = Bundle(for: MirageScreenSaverView.self)
        let backingSize = convertToBacking(bounds).size
        let drawableSize = isPreview ? backingSize : displayPixelSize() ?? backingSize
        guard let size = MirageSaverRenderSize(size: drawableSize, isPreview: isPreview) else {
            didLoadWallpaper = false
            return
        }
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"
        screenSaverLogger.notice(
            "MirageScreenSaver build=\(build, privacy: .public) preview=\(self.isPreview, privacy: .public) host=\(self.hostReportedSize.width, privacy: .public)x\(self.hostReportedSize.height, privacy: .public) points=\(self.bounds.width, privacy: .public)x\(self.bounds.height, privacy: .public) backing=\(backingSize.width, privacy: .public)x\(backingSize.height, privacy: .public) drawable=\(drawableSize.width, privacy: .public)x\(drawableSize.height, privacy: .public) render=\(size.renderWidth, privacy: .public)x\(size.renderHeight, privacy: .public)"
        )
        let view = NSView(frame: bounds)
        view.autoresizingMask = [.width, .height]
        view.alphaValue = finishReload == nil ? 1 : 0
        addSubview(view)
        sceneView = view
        sceneSession = MirageSaverSceneSession(view: view, bundle: bundle, configuration: configuration,
                                              size: size, position: position(for: configuration),
                                              onFirstFrame: { [weak self] in self?.finishConfigurationReload() }) { [weak self] failure in
            guard let self else { return }
            if let failure {
                screenSaverLogger.error("Scene screen saver initialization failed: \(failure, privacy: .public)")
                self.showMessage(self.localized(failure))
            } else {
                screenSaverLogger.info("Scene screen saver initialized")
            }
        }
        sceneSession?.setPaused(!isAnimatingWallpaper)
    }

    private func normalizeFullScreenBoundsIfNeeded() {
        if hostReportedSize == .zero {
            hostReportedSize = bounds.size
        }
        guard !isPreview else { return }
        guard let screen = window?.screen ?? NSScreen.main else { return }
        guard screen.backingScaleFactor.isFinite, screen.backingScaleFactor > 0 else { return }
        let backingSize = screen.convertRectToBacking(screen.frame).size
        let logicalSize = CGSize(
            width: backingSize.width / screen.backingScaleFactor,
            height: backingSize.height / screen.backingScaleFactor
        )
        guard logicalSize.width.isFinite, logicalSize.height.isFinite,
              logicalSize.width > 0, logicalSize.height > 0 else { return }
        if !approximatelyEqual(bounds.size, logicalSize) {
            var normalizedBounds = bounds
            normalizedBounds.size = logicalSize
            bounds = normalizedBounds
        }
        if !approximatelyEqual(frame.size, logicalSize) {
            var normalizedFrame = frame
            normalizedFrame.size = logicalSize
            frame = normalizedFrame
        }
    }

    private func approximatelyEqual(_ lhs: CGSize, _ rhs: CGSize) -> Bool {
        let tolerance: CGFloat = 1
        return abs(lhs.width - rhs.width) <= tolerance
            && abs(lhs.height - rhs.height) <= tolerance
    }

    private func displayPixelSize() -> CGSize? {
        guard let screen = window?.screen ?? NSScreen.main else { return nil }
        let size = screen.convertRectToBacking(screen.frame).size
        guard size.width.isFinite, size.height.isFinite,
              size.width > 0, size.height > 0 else { return nil }
        return size
    }

    private var hasValidBounds: Bool {
        bounds.width.isFinite && bounds.height.isFinite && bounds.width > 0 && bounds.height > 0
    }

    private func loadVideo(_ configuration: MirageSaverConfiguration) {
        videoLoadTask?.cancel()
        let loadID = UUID()
        videoLoadID = loadID
        videoLoadTask = Task.detached(priority: .userInitiated) { [weak self] in
            let candidates = [configuration.playbackEntryURL, configuration.fallbackEntryURL]
                .compactMap { $0 }
            var playableAsset: AVURLAsset?
            var playableLoader: MirageMemoryVideoAssetLoader?
            for url in candidates {
                guard !Task.isCancelled else { return }
                let loader: MirageMemoryVideoAssetLoader?
                let asset: AVURLAsset
                if configuration.loadFromMemory {
                    do {
                        let candidateLoader = try MirageMemoryVideoAssetLoader(fileURL: url, isCancelled: { Task.isCancelled })
                        loader = candidateLoader
                        asset = candidateLoader.makeAsset()
                    } catch {
                        guard !Task.isCancelled else { return }
                        screenSaverLogger.error("In-memory video load failed: \(error.localizedDescription, privacy: .public)")
                        loader = nil
                        asset = AVURLAsset(url: url)
                    }
                } else {
                    loader = nil
                    asset = AVURLAsset(url: url)
                }
                let decodable = try? await withTaskCancellationHandler {
                    try Task.checkCancellation()
                    guard try await asset.load(.isPlayable) else { return false }
                    try Task.checkCancellation()
                    let duration = try await asset.load(.duration)
                    guard duration.isNumeric, CMTimeCompare(duration, .zero) > 0 else { return false }
                    try Task.checkCancellation()
                    let tracks = try await asset.loadTracks(withMediaType: .video)
                    guard !tracks.isEmpty else { return false }
                    for track in tracks {
                        try Task.checkCancellation()
                        guard try await track.load(.isDecodable) else { return false }
                    }
                    return true
                } onCancel: {
                    asset.cancelLoading()
                }
                guard !Task.isCancelled else { return }
                if decodable == true {
                    playableAsset = asset
                    playableLoader = loader
                    break
                }
            }
            guard !Task.isCancelled else { return }
            guard let asset = playableAsset else {
                await MainActor.run { [weak self] in
                    guard let self, self.videoLoadID == loadID,
                          self.isAnimatingWallpaper, self.window != nil else { return }
                    self.videoLoadTask = nil
                    self.showMessage(self.localized("此视频格式无法播放，请先在 Mirage 中播放一次以完成转换"))
                }
                return
            }
            guard !Task.isCancelled else { return }
            await MainActor.run { [weak self, playableLoader] in
                guard let self, self.videoLoadID == loadID,
                      self.isAnimatingWallpaper, self.window != nil else { return }
                self.videoLoadTask = nil
                let item = AVPlayerItem(asset: asset)
                let player = AVQueuePlayer()
                player.automaticallyWaitsToMinimizeStalling = true
                player.isMuted = true
                let looper = AVPlayerLooper(player: player, templateItem: item)
                let playerLayer = AVPlayerLayer(player: player)
                playerLayer.frame = self.presentationBounds
                self.applyVideoDynamicRange(to: playerLayer, enabled: configuration.enableHDRVideo)
                self.layer?.addSublayer(playerLayer)
                self.videoLayout = WallpaperVideoLayout(
                    layer: playerLayer, bounds: self.presentationBounds,
                    fillMode: configuration.fillMode, position: self.position(for: configuration))
                self.player = player
                self.looper = looper
                self.memoryAssetLoader = playableLoader
                self.playerLayer = playerLayer
                playerLayer.opacity = self.finishReload == nil ? 1 : 0
                self.videoReadyObservation = playerLayer.observe(\.isReadyForDisplay, options: [.initial, .new]) { [weak self] layer, _ in
                    guard layer.isReadyForDisplay else { return }
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.videoLoadID == loadID else { return }
                        self.finishConfigurationReload()
                    }
                }
                if self.isAnimatingWallpaper { player.playImmediately(atRate: configuration.speed) }
            }
        }
    }

    override func layout() {
        super.layout()
        normalizeFullScreenBoundsIfNeeded()
        guard hasValidBounds else {
            releaseWallpaper()
            return
        }
        scheduleWallpaperLoad()
        guard let rootLayer = layer else { return }
        rootLayer.contentsScale = window?.backingScaleFactor ?? rootLayer.contentsScale
        videoLayout?.update(bounds: videoPresentationBounds)
        playerLayer?.contentsScale = rootLayer.contentsScale
        if let playerLayer, let configuration {
            applyVideoDynamicRange(to: playerLayer, enabled: configuration.enableHDRVideo)
        }
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        scheduleWallpaperLoad()
    }

    override func setBoundsSize(_ newSize: NSSize) {
        super.setBoundsSize(newSize)
        scheduleWallpaperLoad()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        needsLayout = true
        scheduleWallpaperLoad()
    }

    private func preserveWallpaperForReload() {
        guard finishReload == nil, sceneView != nil || playerLayer != nil else { return }
        let oldSession = sceneSession, oldView = sceneView
        let oldPlayer = player, oldLayer = playerLayer, oldLooper = looper, oldLoader = memoryAssetLoader
        oldSession?.setPaused(true)
        oldPlayer?.pause()
        sceneSession = nil
        sceneView = nil
        player = nil
        playerLayer = nil
        looper = nil
        memoryAssetLoader = nil
        videoLayout = nil
        finishReload = {
            oldSession?.stop()
            oldView?.removeFromSuperview()
            withExtendedLifetime(oldLoader) {
                oldLooper?.disableLooping()
                oldLayer?.player = nil
                oldPlayer?.removeAllItems()
                oldLayer?.removeFromSuperlayer()
            }
        }
    }

    private func finishConfigurationReload() {
        sceneView?.alphaValue = 1
        playerLayer?.opacity = 1
        finishReload?()
        finishReload = nil
    }

    private func releaseWallpaper(preservingReload: Bool = false) {
        if !preservingReload {
            finishReload?()
            finishReload = nil
        }
        videoReadyObservation?.invalidate()
        videoReadyObservation = nil
        if didLoadWallpaper {
            screenSaverLogger.info("Releasing screen saver resources, preview=\(self.isPreview, privacy: .public)")
        }
        if !isAnimatingWallpaper || window == nil { isWaitingForLayout = false }
        wallpaperLoadWorkItem?.cancel()
        wallpaperLoadWorkItem = nil
        videoLoadID = UUID()
        videoLoadTask?.cancel()
        videoLoadTask = nil
        player?.pause()
        looper?.disableLooping()
        playerLayer?.player = nil
        player?.removeAllItems()
        videoLayout = nil
        playerLayer?.removeFromSuperlayer()
        playerLayer = nil
        looper = nil
        player = nil
        memoryAssetLoader = nil
        sceneSession?.stop()
        sceneSession = nil
        sceneView?.removeFromSuperview()
        sceneView = nil
        messageLabel?.removeFromSuperview()
        messageLabel = nil
        configuration = nil
        didLoadWallpaper = false
        hostReportedSize = .zero
    }

    private var videoPresentationBounds: CGRect {
        presentationBounds
    }

    private var presentationBounds: CGRect {
        CGRect(origin: .zero, size: bounds.size)
    }

    private func applyVideoDynamicRange(to playerLayer: AVPlayerLayer, enabled: Bool) {
        let screen = window?.screen ?? NSScreen.main
        let useHDR = enabled && (screen?.maximumPotentialExtendedDynamicRangeColorComponentValue ?? 1) > 1
        if #available(macOS 26.0, *) {
            let range: CALayer.DynamicRange = useHDR ? .constrainedHigh : .standard
            layer?.preferredDynamicRange = range
            playerLayer.preferredDynamicRange = range
        } else {
            layer?.wantsExtendedDynamicRangeContent = useHDR
            playerLayer.wantsExtendedDynamicRangeContent = useHDR
        }
    }

    private func showMessage(_ text: String) {
        messageLabel?.removeFromSuperview()
        let label = NSTextField(labelWithString: text)
        label.textColor = .secondaryLabelColor
        label.font = .systemFont(ofSize: 18, weight: .medium)
        label.alignment = .center
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
            label.leadingAnchor.constraint(greaterThanOrEqualTo: leadingAnchor, constant: 24),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -24)
        ])
        messageLabel = label
    }

    override func startAnimation() {
        super.startAnimation()
        isAnimatingWallpaper = true
        observeConfiguration()
        reloadConfiguration()
        scheduleWallpaperLoad()
        player?.playImmediately(atRate: configuration?.speed ?? 1)
        sceneSession?.setPaused(false)
    }

    override func stopAnimation() {
        configurationRequestID = UUID()
        configurationReload?.cancel()
        configurationReload = nil
        isAnimatingWallpaper = false
        releaseWallpaper()
        super.stopAnimation()
    }

    override func animateOneFrame() {
        normalizeFullScreenBoundsIfNeeded()
        videoLayout?.update(bounds: presentationBounds)
    }

    override var hasConfigureSheet: Bool { false }
}
