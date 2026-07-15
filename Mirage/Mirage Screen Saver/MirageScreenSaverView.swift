import AVFoundation
import AppKit
import Darwin
import ScreenSaver
import UniformTypeIdentifiers
import WebKit

private struct MirageSaverConfiguration {
    let title: String
    let kind: String
    let renderDirectory: URL
    let entryURL: URL
    let entryRelativePath: String
    let overlays: [URL]
    let properties: [String: Any]
    let rawProperties: [String: Any]
    let fps: Int
    let fillMode: String

    static func load() -> Self? {
        let home: URL
        if let record = getpwuid(getuid()), let path = String(validatingUTF8: record.pointee.pw_dir) {
            home = URL(fileURLWithPath: path, isDirectory: true)
        } else {
            home = FileManager.default.homeDirectoryForCurrentUser
        }
        let url = home
            .appendingPathComponent("Library/Application Support/Mirage/screensaver.json")
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (object["version"] as? Int) == 1,
              let kind = object["kind"] as? String,
              let renderPath = object["renderDirectory"] as? String,
              let entryPath = object["entryPath"] as? String else { return nil }
        let renderDirectory = URL(fileURLWithPath: renderPath, isDirectory: true)
        let entryURL = URL(fileURLWithPath: entryPath)
        guard FileManager.default.fileExists(atPath: entryURL.path) else { return nil }
        return Self(
            title: object["title"] as? String ?? "Mirage",
            kind: kind,
            renderDirectory: renderDirectory,
            entryURL: entryURL,
            entryRelativePath: entryURL.path.hasPrefix(renderDirectory.path + "/")
                ? String(entryURL.path.dropFirst(renderDirectory.path.count + 1))
                : entryURL.lastPathComponent,
            overlays: (object["assetOverlays"] as? [String] ?? []).map { URL(fileURLWithPath: $0, isDirectory: true) },
            properties: object["properties"] as? [String: Any] ?? [:],
            rawProperties: object["rawProperties"] as? [String: Any] ?? [:],
            fps: max(10, min(object["fps"] as? Int ?? 30, 60)),
            fillMode: object["fillMode"] as? String ?? "cover"
        )
    }
}

private final class MirageSceneLibrary {
    typealias Create = @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafePointer<CChar>?,
        UInt32, UInt32, UInt32
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
              let createSymbol = dlsym(handle, "MirageSceneSaverCreate"),
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

private final class MirageWallpaperSchemeHandler: NSObject, WKURLSchemeHandler {
    let base: URL
    let overlays: [URL]

    init(base: URL, overlays: [URL]) {
        self.base = base
        self.overlays = overlays
    }

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        guard let requestURL = urlSchemeTask.request.url,
              let relative = requestURL.path.removingPercentEncoding?.trimmingCharacters(in: CharacterSet(charactersIn: "/")),
              !relative.isEmpty else {
            urlSchemeTask.didFailWithError(URLError(.badURL))
            return
        }
        let roots = relative.lowercased() == "project.json" ? [base] : overlays + [base]
        for root in roots {
            let normalizedRoot = root.standardizedFileURL.resolvingSymlinksInPath()
            let candidate = normalizedRoot.appendingPathComponent(relative).standardizedFileURL.resolvingSymlinksInPath()
            guard candidate.path.hasPrefix(normalizedRoot.path + "/") else { continue }
            guard let data = try? Data(contentsOf: candidate) else { continue }
            let mime = UTType(filenameExtension: candidate.pathExtension)?.preferredMIMEType ?? "application/octet-stream"
            let response = URLResponse(url: requestURL, mimeType: mime, expectedContentLength: data.count, textEncodingName: nil)
            urlSchemeTask.didReceive(response)
            urlSchemeTask.didReceive(data)
            urlSchemeTask.didFinish()
            return
        }
        urlSchemeTask.didFailWithError(URLError(.fileDoesNotExist))
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {}
}

@objc(MirageScreenSaverView)
final class MirageScreenSaverView: ScreenSaverView {
    private var player: AVPlayer?
    private var playerLayer: AVPlayerLayer?
    private var webView: WKWebView?
    private var schemeHandler: MirageWallpaperSchemeHandler?
    private var endObserver: NSObjectProtocol?
    private var messageLabel: NSTextField?
    private var configuration: MirageSaverConfiguration?
    private var sceneLibrary: MirageSceneLibrary?
    private var sceneEngine: UnsafeMutableRawPointer?

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        loadWallpaper()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        loadWallpaper()
    }

    deinit {
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        player?.pause()
        webView?.stopLoading()
        if let sceneEngine { sceneLibrary?.destroy(sceneEngine) }
    }

    private func loadWallpaper() {
        guard let configuration = MirageSaverConfiguration.load() else {
            showMessage("请先在 Mirage 设置中选择屏保壁纸")
            return
        }
        self.configuration = configuration
        animationTimeInterval = 1.0 / Double(configuration.fps)
        switch configuration.kind {
        case "video": loadVideo(configuration)
        case "web": loadWeb(configuration)
        case "scene": loadScene(configuration)
        default: showMessage("不支持的壁纸格式")
        }
    }

    private func loadScene(_ configuration: MirageSaverConfiguration) {
        let bundle = Bundle(for: MirageScreenSaverView.self)
        guard let resources = bundle.resourceURL,
              let library = MirageSceneLibrary(bundle: bundle) else {
            showMessage("场景屏保组件不可用")
            return
        }
        let assets = resources.appendingPathComponent("assets", isDirectory: true)
        let icd = resources.appendingPathComponent("vulkan/icd.d/MoltenVK_icd.json")
        guard FileManager.default.fileExists(atPath: assets.path),
              FileManager.default.fileExists(atPath: icd.path) else {
            showMessage("场景屏保资源不完整")
            return
        }
        setenv("VK_ICD_FILENAMES", icd.path, 1)
        setenv("VK_DRIVER_FILES", icd.path, 1)
        let data = (try? JSONSerialization.data(withJSONObject: configuration.rawProperties)) ?? Data("{}".utf8)
        let json = String(data: data, encoding: .utf8) ?? "{}"
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        let width = UInt32(max(bounds.width * scale, 500))
        let height = UInt32(max(bounds.height * scale, 500))
        let viewPointer = Unmanaged.passUnretained(self).toOpaque()
        let engine = assets.path.withCString { assetsPath in
            configuration.entryURL.path.withCString { packagePath in
                json.withCString { properties in
                    library.create(viewPointer, assetsPath, packagePath, properties,
                                   width, height, UInt32(configuration.fps))
                }
            }
        }
        guard let engine else {
            showMessage("场景壁纸加载失败")
            return
        }
        sceneLibrary = library
        sceneEngine = engine
    }

    private func loadVideo(_ configuration: MirageSaverConfiguration) {
        let player = AVPlayer(url: configuration.entryURL)
        player.isMuted = true
        let playerLayer = AVPlayerLayer(player: player)
        playerLayer.frame = bounds
        playerLayer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        switch configuration.fillMode {
        case "contain": playerLayer.videoGravity = .resizeAspect
        case "stretch": playerLayer.videoGravity = .resize
        default: playerLayer.videoGravity = .resizeAspectFill
        }
        layer?.addSublayer(playerLayer)
        self.player = player
        self.playerLayer = playerLayer
        endObserver = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: player.currentItem, queue: .main
        ) { [weak player] _ in
            player?.seek(to: .zero)
            player?.play()
        }
    }

    private func loadWeb(_ configuration: MirageSaverConfiguration) {
        let webConfiguration = WKWebViewConfiguration()
        let controller = WKUserContentController()
        let propertiesData = (try? JSONSerialization.data(withJSONObject: configuration.properties)) ?? Data("{}".utf8)
        let propertiesJSON = String(data: propertiesData, encoding: .utf8) ?? "{}"
        let shim = """
        (function(){
          window.wallpaperEngine_paused=false;
          window.chrome=window.chrome||{runtime:{}};
          var retry=window.setTimeout.bind(window);
          var listeners=[];
          window.wallpaperRegisterAudioListener=function(cb){if(typeof cb==='function')listeners.push(cb);};
          window.wallpaperRemoveAudioListener=function(cb){var i=listeners.indexOf(cb);if(i>=0)listeners.splice(i,1);};
          window.wallpaperRegisterAudioStream=function(el){if(el)el.muted=true;return el;};
          var mediaPlay=HTMLMediaElement.prototype.play;
          HTMLMediaElement.prototype.play=function(){this.muted=true;return mediaPlay.apply(this,arguments);};
          window.__mirageProperties=\(propertiesJSON);
          window.__mirageApply=function(){
            var l=window.wallpaperPropertyListener;
            if(l&&typeof l.applyUserProperties==='function'){
              try{l.applyUserProperties(window.__mirageProperties);return;}catch(e){}
            }
            retry(window.__mirageApply,25);
          };
          document.addEventListener('DOMContentLoaded',window.__mirageApply,{once:true});
        })();
        """
        controller.addUserScript(WKUserScript(source: shim, injectionTime: .atDocumentStart, forMainFrameOnly: true))
        webConfiguration.userContentController = controller
        webConfiguration.websiteDataStore = .nonPersistent()
        webConfiguration.mediaTypesRequiringUserActionForPlayback = []
        let handler = MirageWallpaperSchemeHandler(base: configuration.renderDirectory, overlays: configuration.overlays)
        webConfiguration.setURLSchemeHandler(handler, forURLScheme: "mirage-wallpaper")
        schemeHandler = handler

        let webView = WKWebView(frame: bounds, configuration: webConfiguration)
        webView.autoresizingMask = [.width, .height]
        webView.setValue(false, forKey: "drawsBackground")
        addSubview(webView)
        self.webView = webView

        var components = URLComponents()
        components.scheme = "mirage-wallpaper"
        components.host = "wallpaper"
        components.path = "/" + configuration.entryRelativePath
        if let url = components.url { webView.load(URLRequest(url: url)) }
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
        player?.play()
        webView?.evaluateJavaScript("window.wallpaperEngine_paused=false;if(window.wallpaperPropertyListener&&window.wallpaperPropertyListener.setPaused)window.wallpaperPropertyListener.setPaused(false);")
        if let sceneEngine { sceneLibrary?.setPaused(sceneEngine, 0) }
    }

    override func stopAnimation() {
        player?.pause()
        webView?.evaluateJavaScript("window.wallpaperEngine_paused=true;if(window.wallpaperPropertyListener&&window.wallpaperPropertyListener.setPaused)window.wallpaperPropertyListener.setPaused(true);")
        if let sceneEngine { sceneLibrary?.setPaused(sceneEngine, 1) }
        super.stopAnimation()
    }

    override func animateOneFrame() {}

    override var hasConfigureSheet: Bool { false }
}
