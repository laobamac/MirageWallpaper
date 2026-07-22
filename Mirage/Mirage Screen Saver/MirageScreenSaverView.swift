import AVFoundation
import AppKit
import Darwin
import OSLog
import ScreenSaver
import UniformTypeIdentifiers
import WebKit

private let screenSaverLogger = Logger(
    subsystem: "cn.laobamac.Mirage.ScreenSaver",
    category: "Rendering"
)

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
    let language: String

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
            fillMode: object["fillMode"] as? String ?? "cover",
            language: object["language"] as? String ?? Locale.preferredLanguages.first ?? "en"
        )
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
        UInt32, UInt32, UInt32, UInt32, UInt32
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

    private func sendFile(_ candidate: URL, for requestURL: URL, task: WKURLSchemeTask) -> Bool {
        guard let data = try? Data(contentsOf: candidate) else { return false }
        let mime = UTType(filenameExtension: candidate.pathExtension)?.preferredMIMEType ?? "application/octet-stream"
        let response = URLResponse(url: requestURL, mimeType: mime, expectedContentLength: data.count, textEncodingName: nil)
        task.didReceive(response)
        task.didReceive(data)
        task.didFinish()
        return true
    }

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        guard let requestURL = urlSchemeTask.request.url,
              let relative = requestURL.path.removingPercentEncoding?.trimmingCharacters(in: CharacterSet(charactersIn: "/")),
              !relative.isEmpty else {
            urlSchemeTask.didFailWithError(URLError(.badURL))
            return
        }

        if relative == "__mirage_local",
           let requestedPath = URLComponents(url: requestURL, resolvingAgainstBaseURL: false)?
            .queryItems?.first(where: { $0.name == "path" })?.value,
           (requestedPath as NSString).isAbsolutePath {
            let candidate = URL(fileURLWithPath: requestedPath).standardizedFileURL.resolvingSymlinksInPath()
            for rootURL in overlays + [base] {
                let root = rootURL.standardizedFileURL.resolvingSymlinksInPath()
                if candidate.path == root.path || candidate.path.hasPrefix(root.path + "/") {
                    if sendFile(candidate, for: requestURL, task: urlSchemeTask) { return }
                    break
                }
            }
            urlSchemeTask.didFailWithError(URLError(.fileDoesNotExist))
            return
        }

        let roots = relative.lowercased() == "project.json" ? [base] : overlays + [base]
        for root in roots {
            let normalizedRoot = root.standardizedFileURL.resolvingSymlinksInPath()
            let candidate = normalizedRoot.appendingPathComponent(relative).standardizedFileURL.resolvingSymlinksInPath()
            guard candidate.path.hasPrefix(normalizedRoot.path + "/") else { continue }
            if sendFile(candidate, for: requestURL, task: urlSchemeTask) { return }
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
    private var didLoadWallpaper = false
    private var hostReportedSize = CGSize.zero

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
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
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        player?.pause()
        webView?.stopLoading()
        if let sceneEngine { sceneLibrary?.destroy(sceneEngine) }
    }

    private func localized(_ key: String) -> String {
        MirageSaverLocalization.string(key, language: configuration?.language)
    }

    private func loadWallpaper() {
        guard !didLoadWallpaper, window != nil else { return }
        layoutSubtreeIfNeeded()
        normalizeFullScreenBoundsIfNeeded()
        didLoadWallpaper = true
        guard let configuration = MirageSaverConfiguration.load() else {
            showMessage(MirageSaverLocalization.string("请先在 Mirage 设置中选择屏保壁纸"))
            return
        }
        self.configuration = configuration
        animationTimeInterval = 1.0 / Double(configuration.fps)
        switch configuration.kind {
        case "video": loadVideo(configuration)
        case "web": loadWeb(configuration)
        case "scene": loadScene(configuration)
        default: showMessage(localized("不支持的壁纸格式"))
        }
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
        let engine = assets.path.withCString { assetsPath in
            configuration.entryURL.path.withCString { packagePath in
                json.withCString { properties in
                    library.create(viewPointer, assetsPath, packagePath, properties,
                                   renderWidth, renderHeight,
                                   fixedDrawableWidth, fixedDrawableHeight,
                                   UInt32(configuration.fps))
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
        hostReportedSize = bounds.size
        guard let screen = window?.screen ?? NSScreen.main else { return }
        let logicalSize = screen.frame.size
        guard logicalSize.width > 0, logicalSize.height > 0 else { return }

        let physicalSize = displayPixelSize() ?? CGSize(
            width: logicalSize.width * screen.backingScaleFactor,
            height: logicalSize.height * screen.backingScaleFactor
        )
        let matchesPhysicalPixels = approximatelyEqual(bounds.size, physicalSize)
        let matchesLogicalPoints = approximatelyEqual(bounds.size, logicalSize)
        guard matchesPhysicalPixels, !matchesLogicalPoints else { return }

        // macOS 26's legacyScreenSaver ViewBridge can give a full-screen view
        // the display's physical pixel dimensions as its logical bounds even
        // though the remote container is sized in points. Normalize only that
        // exact full-display signature; small System Settings previews and
        // ordinary point-sized views are left untouched.
        bounds = NSRect(origin: bounds.origin, size: logicalSize)
        layoutSubtreeIfNeeded()
    }

    private func approximatelyEqual(_ lhs: CGSize, _ rhs: CGSize) -> Bool {
        let tolerance: CGFloat = 1
        return abs(lhs.width - rhs.width) <= tolerance
            && abs(lhs.height - rhs.height) <= tolerance
    }

    private func displayPixelSize() -> CGSize? {
        guard let screen = window?.screen ?? NSScreen.main else { return nil }
        let scale = screen.backingScaleFactor
        let size = screen.frame.size
        guard size.width > 0, size.height > 0, scale > 0 else { return nil }
        // A CAMetalLayer renders in AppKit backing pixels. This can differ from
        // CGDisplayMode.pixelWidth on a scaled display, so derive it from the
        // screen's stable logical frame and backing scale.
        return CGSize(width: size.width * scale, height: size.height * scale)
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
          function __mirageLocalAssetURL(raw){
            if(typeof raw!=='string'||raw.slice(0,5).toLowerCase()!=='file:')return raw;
            try{
              var u=new URL(raw),p=decodeURIComponent(u.pathname||'');
              while(p.length>1&&p.charAt(0)==='/'&&p.charAt(1)==='/')p=p.slice(1);
              return 'mirage-wallpaper://wallpaper/__mirage_local?path='+encodeURIComponent(p);
            }catch(e){return raw;}
          }
          function __mirageRewriteCssURLs(value){
            if(typeof value!=='string')return value;
            return value.replace(/file:[^'")]+/gi,function(url){return __mirageLocalAssetURL(url.trim());});
          }
          function __mirageWrapCssProperty(name){
            try{
              var d=Object.getOwnPropertyDescriptor(CSSStyleDeclaration.prototype,name);
              if(d&&d.get&&d.set)Object.defineProperty(CSSStyleDeclaration.prototype,name,{
                configurable:d.configurable,enumerable:d.enumerable,get:d.get,
                set:function(v){d.set.call(this,__mirageRewriteCssURLs(v));}
              });
            }catch(e){}
          }
          ['background','backgroundImage','cssText'].forEach(__mirageWrapCssProperty);
          try{
            var nativeSetProperty=CSSStyleDeclaration.prototype.setProperty;
            CSSStyleDeclaration.prototype.setProperty=function(name,value,priority){
              return nativeSetProperty.call(this,name,__mirageRewriteCssURLs(value),priority);
            };
          }catch(e){}
          function __mirageWrapURLProperty(proto,name){
            try{
              var d=Object.getOwnPropertyDescriptor(proto,name);
              if(d&&d.get&&d.set)Object.defineProperty(proto,name,{
                configurable:d.configurable,enumerable:d.enumerable,get:d.get,
                set:function(v){d.set.call(this,__mirageLocalAssetURL(v));}
              });
            }catch(e){}
          }
          [[HTMLImageElement.prototype,'src'],[HTMLMediaElement.prototype,'src'],
           [HTMLSourceElement.prototype,'src']].forEach(function(x){__mirageWrapURLProperty(x[0],x[1]);});
          try{
            var nativeSetAttribute=Element.prototype.setAttribute;
            Element.prototype.setAttribute=function(name,value){
              var n=String(name).toLowerCase();
              if(n==='src'||n==='poster')value=__mirageLocalAssetURL(value);
              else if(n==='style')value=__mirageRewriteCssURLs(value);
              return nativeSetAttribute.call(this,name,value);
            };
          }catch(e){}
          function __mirageRewriteElementAsset(element,name){
            try{
              var value=element.getAttribute(name);
              if(!value||value.toLowerCase().indexOf('file:')<0)return;
              var rewritten=name==='style'?__mirageRewriteCssURLs(value):__mirageLocalAssetURL(value);
              if(rewritten!==value)nativeSetAttribute.call(element,name,rewritten);
            }catch(e){}
          }
          function __mirageInstallLocalAssetObserver(){
            if(!document.documentElement||window.__mirageLocalAssetObserver)return;
            var observer=new MutationObserver(function(records){
              records.forEach(function(record){__mirageRewriteElementAsset(record.target,record.attributeName);});
            });
            observer.observe(document.documentElement,{subtree:true,attributes:true,attributeFilter:['style','src','poster']});
            window.__mirageLocalAssetObserver=observer;
          }
          if(document.documentElement)__mirageInstallLocalAssetObserver();
          document.addEventListener('DOMContentLoaded',__mirageInstallLocalAssetObserver,{once:true});
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
        loadWallpaper()
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
