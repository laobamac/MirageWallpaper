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

    static func load() -> Self? {
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
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (object["version"] as? Int) == 1,
              let kind = object["kind"] as? String,
              kind == "video" || kind == "scene",
              let entryPath = object["entryPath"] as? String else { return nil }
        let entryURL = URL(fileURLWithPath: entryPath)
        guard FileManager.default.fileExists(atPath: entryURL.path) else { return nil }
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
            language: object["language"] as? String ?? Locale.preferredLanguages.first ?? "en"
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

private final class MirageSceneLibrary {
    typealias Create = @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafePointer<CChar>?,
        UInt32, UInt32, UInt32, UInt32, UInt32, UnsafePointer<CChar>?, Double, Double
    ) -> UnsafeMutableRawPointer?
    typealias SetPaused = @convention(c) (UnsafeMutableRawPointer?, Int32) -> Void
    typealias Destroy = @convention(c) (UnsafeMutableRawPointer?) -> Void

    let handle: UnsafeMutableRawPointer
    let create: Create
    let setPaused: SetPaused
    let destroy: Destroy

    init?(bundle: Bundle) {
        guard let frameworkDirectory = bundle.privateFrameworksURL else { return nil }
        let libraryURL = frameworkDirectory.appendingPathComponent("libMirageSceneSaver.dylib")
        guard let handle = dlopen(libraryURL.path, RTLD_NOW | RTLD_LOCAL),
              let createSymbol = dlsym(handle, "MirageSceneSaverCreateWithPosition"),
              let pauseSymbol = dlsym(handle, "MirageSceneSaverSetPaused"),
              let destroySymbol = dlsym(handle, "MirageSceneSaverDestroy") else {
            return nil
        }
        self.handle = handle
        create = unsafeBitCast(createSymbol, to: Create.self)
        setPaused = unsafeBitCast(pauseSymbol, to: SetPaused.self)
        destroy = unsafeBitCast(destroySymbol, to: Destroy.self)
    }

    deinit { dlclose(handle) }
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
    private var sceneLibrary: MirageSceneLibrary?
    private var sceneEngine: UnsafeMutableRawPointer?
    private var didLoadWallpaper = false
    private var isAnimatingWallpaper = false
    private var videoLoadTask: Task<Void, Never>?
    private var videoLoadID = UUID()
    private var hostReportedSize = CGSize.zero

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
        guard window != nil else { return }
        // legacyScreenSaver attaches the view before its final layout pass.
        // Defer one run-loop turn so the first Vulkan extent is derived from
        // the host's settled bounds/backing coordinate space.
        DispatchQueue.main.async { [weak self] in
            guard let self, self.window != nil else { return }
            self.loadWallpaper()
        }
    }

    deinit {
        player?.pause()
        looper?.disableLooping()
        player?.removeAllItems()
        looper = nil
        memoryAssetLoader = nil
        videoLoadTask?.cancel()
        if let sceneEngine { sceneLibrary?.destroy(sceneEngine) }
    }

    private func localized(_ key: String) -> String {
        MirageSaverLocalization.string(key, language: configuration?.language)
    }

    private func loadWallpaper() {
        guard !didLoadWallpaper, window != nil else { return }
        layoutSubtreeIfNeeded()
        normalizeFullScreenBoundsIfNeeded()
        layoutSubtreeIfNeeded()
        didLoadWallpaper = true
        guard let configuration = MirageSaverConfiguration.load() else {
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
        return configuration.positionsByDisplay[key] ?? .center
    }

    private func loadScene(_ configuration: MirageSaverConfiguration) {
        let bundle = Bundle(for: MirageScreenSaverView.self)
        guard let resources = bundle.resourceURL,
              let library = MirageSceneLibrary(bundle: bundle) else {
            showMessage(localized("场景屏保组件不可用"))
            return
        }
        let assets = resources.appendingPathComponent("assets", isDirectory: true)
        let icd = resources.appendingPathComponent("vulkan/icd.d/MoltenVK_icd.json")
        guard FileManager.default.fileExists(atPath: assets.path),
              FileManager.default.fileExists(atPath: icd.path) else {
            showMessage(localized("场景屏保资源不完整"))
            return
        }
        setenv("VK_ICD_FILENAMES", icd.path, 1)
        setenv("VK_DRIVER_FILES", icd.path, 1)
        let data = (try? JSONSerialization.data(withJSONObject: configuration.rawProperties)) ?? Data("{}".utf8)
        let json = String(data: data, encoding: .utf8) ?? "{}"
        // ScreenSaverView uses logical points while Vulkan renders pixels.
        // Ask AppKit for this view's actual backing rect so mixed-DPI displays
        // and System Settings' preview both retain the correct aspect ratio.
        let backingSize = convertToBacking(bounds).size
        let drawableSize = isPreview ? backingSize : displayPixelSize() ?? backingSize
        let drawableWidth = UInt32(max(1, drawableSize.width.rounded()))
        let drawableHeight = UInt32(max(1, drawableSize.height.rounded()))
        let fixedDrawableWidth = isPreview ? 0 : drawableWidth
        let fixedDrawableHeight = isPreview ? 0 : drawableHeight
        let previewScale = isPreview
            ? max(1, 500 / min(drawableSize.width, drawableSize.height))
            : 1
        let renderWidth = UInt32(min(ceil(drawableSize.width * previewScale), 8192))
        let renderHeight = UInt32(min(ceil(drawableSize.height * previewScale), 8192))
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"
        screenSaverLogger.notice(
            "MirageScreenSaver build=\(build, privacy: .public) preview=\(self.isPreview, privacy: .public) host=\(Int(self.hostReportedSize.width), privacy: .public)x\(Int(self.hostReportedSize.height), privacy: .public) points=\(Int(self.bounds.width), privacy: .public)x\(Int(self.bounds.height), privacy: .public) backing=\(Int(backingSize.width), privacy: .public)x\(Int(backingSize.height), privacy: .public) drawable=\(drawableWidth, privacy: .public)x\(drawableHeight, privacy: .public) render=\(renderWidth, privacy: .public)x\(renderHeight, privacy: .public)"
        )
        let viewPointer = Unmanaged.passUnretained(self).toOpaque()
        let position = position(for: configuration)
        let engine = assets.path.withCString { assetsPath in
            configuration.entryURL.path.withCString { packagePath in
                json.withCString { properties in
                    configuration.fillMode.withCString { fill in
                        library.create(viewPointer, assetsPath, packagePath, properties,
                                       renderWidth, renderHeight,
                                       fixedDrawableWidth, fixedDrawableHeight,
                                       UInt32(configuration.fps), fill, position.x, position.y)
                    }
                }
            }
        }
        guard let engine else {
            showMessage(localized("场景壁纸加载失败"))
            return
        }
        sceneLibrary = library
        sceneEngine = engine
    }

    private func normalizeFullScreenBoundsIfNeeded() {
        if hostReportedSize == .zero {
            hostReportedSize = bounds.size
        }
        guard !isPreview else { return }
        guard let screen = window?.screen ?? NSScreen.main else { return }
        guard screen.backingScaleFactor > 0 else { return }
        let backingSize = screen.convertRectToBacking(screen.frame).size
        let logicalSize = CGSize(
            width: backingSize.width / screen.backingScaleFactor,
            height: backingSize.height / screen.backingScaleFactor
        )
        guard logicalSize.width > 0, logicalSize.height > 0 else { return }
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
        guard size.width > 0, size.height > 0 else { return nil }
        return size
    }

    private func loadVideo(_ configuration: MirageSaverConfiguration) {
        videoLoadTask?.cancel()
        let loadID = UUID()
        videoLoadID = loadID
        videoLoadTask = Task { [weak self] in
            let candidates = [configuration.playbackEntryURL, configuration.fallbackEntryURL]
                .compactMap { $0 }
            var playableAsset: AVURLAsset?
            var playableLoader: MirageMemoryVideoAssetLoader?
            for url in candidates {
                let loader: MirageMemoryVideoAssetLoader?
                let asset: AVURLAsset
                if configuration.loadFromMemory {
                    do {
                        let candidateLoader = try MirageMemoryVideoAssetLoader(fileURL: url)
                        loader = candidateLoader
                        asset = candidateLoader.makeAsset()
                    } catch {
                        screenSaverLogger.error("In-memory video load failed: \(error.localizedDescription, privacy: .public)")
                        loader = nil
                        asset = AVURLAsset(url: url)
                    }
                } else {
                    loader = nil
                    asset = AVURLAsset(url: url)
                }
                guard let playable = try? await asset.load(.isPlayable), playable,
                      let duration = try? await asset.load(.duration),
                      duration.isNumeric, CMTimeCompare(duration, .zero) > 0,
                      let tracks = try? await asset.loadTracks(withMediaType: .video),
                      !tracks.isEmpty else { continue }
                var decodable = true
                for track in tracks {
                    guard let value = try? await track.load(.isDecodable), value else {
                        decodable = false
                        break
                    }
                }
                if decodable {
                    playableAsset = asset
                    playableLoader = loader
                    break
                }
            }
            guard let asset = playableAsset else {
                await MainActor.run {
                    guard let self, self.videoLoadID == loadID else { return }
                    self.showMessage(self.localized("此视频格式无法播放，请先在 Mirage 中播放一次以完成转换"))
                }
                return
            }
            guard !Task.isCancelled else { return }
            await MainActor.run {
                guard let self, self.videoLoadID == loadID else { return }
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
                if self.isAnimatingWallpaper { player.play() }
            }
        }
    }

    override func layout() {
        super.layout()
        normalizeFullScreenBoundsIfNeeded()
        guard let rootLayer = layer else { return }
        rootLayer.contentsScale = window?.backingScaleFactor ?? rootLayer.contentsScale
        videoLayout?.update(bounds: videoPresentationBounds)
        playerLayer?.contentsScale = rootLayer.contentsScale
        if let playerLayer, let configuration {
            applyVideoDynamicRange(to: playerLayer, enabled: configuration.enableHDRVideo)
        }
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
        loadWallpaper()
        player?.play()
        if let sceneEngine { sceneLibrary?.setPaused(sceneEngine, 0) }
    }

    override func stopAnimation() {
        isAnimatingWallpaper = false
        player?.pause()
        if let sceneEngine { sceneLibrary?.setPaused(sceneEngine, 1) }
        super.stopAnimation()
    }

    override func animateOneFrame() {
        normalizeFullScreenBoundsIfNeeded()
        videoLayout?.update(bounds: presentationBounds)
    }

    override var hasConfigureSheet: Bool { false }
}
