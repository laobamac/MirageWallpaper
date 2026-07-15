#import "WebRendererEngine.h"
#import "WallpaperManifest.h"
#import "WRAudioTap.h"
#import "WRURLSchemeHandler.h"

#import <WebKit/WebKit.h>

// WE JS API shim, installed at document-start (≈ CEF OnContextCreated).
// Engine entrypoints (called via evaluateJavaScript):
//   __wr_applyProps(obj)   → wallpaperPropertyListener.applyUserProperties
//   __wr_setPaused(bool)   → wallpaperPropertyListener.setPaused + state
//   __wr_setFps(int)       → requestAnimationFrame throttle
//   __wr_applyMute(bool)   → registered audio streams .muted
//   __wr_pauseStreams()    → registered audio streams .pause()
//   __wr_resumeStreams()   → resume streams the host paused
//   __wr_pushAudio([128])  → wallpaperRegisterAudioListener callbacks
static NSString *const kShimJS = @"\
(function(){\
  if (window.__wr_installed) return;\
  window.__wr_installed = true;\
  /* Chrome-compat: many WE wallpapers were authored against Chromium and \
     feature-sniff for window.chrome. WebKit lacks it; stub it. */\
  try { window.chrome = window.chrome || { runtime: {} }; } catch(e) {}\
  /* WE exposes file properties as filesystem paths. Legacy wallpapers often \
     prepend file:/// in page script, which WKWebView blocks for a custom-scheme \
     document. Rewrite those subresources through the allow-listed handler. */\
  function __wr_localAssetURL(raw){\
    if(typeof raw!=='string'||raw.slice(0,5).toLowerCase()!=='file:')return raw;\
    try {\
      var u=new URL(raw),p=decodeURIComponent(u.pathname||'');\
      while(p.length>1&&p.charAt(0)==='/'&&p.charAt(1)==='/')p=p.slice(1);\
      return 'we-wallpaper://wallpaper/__mirage_local?path='+encodeURIComponent(p);\
    } catch(e) { return raw; }\
  }\
  function __wr_rewriteCssURLs(value){\
    if(typeof value!=='string')return value;\
    return value.replace(/file:[^'\")]+/gi,function(url){return __wr_localAssetURL(url.trim());});\
  }\
  function __wr_wrapCssProperty(name){\
    try {\
      var d=Object.getOwnPropertyDescriptor(CSSStyleDeclaration.prototype,name);\
      if(d&&d.get&&d.set)Object.defineProperty(CSSStyleDeclaration.prototype,name,{\
        configurable:d.configurable,enumerable:d.enumerable,get:d.get,\
        set:function(v){d.set.call(this,__wr_rewriteCssURLs(v));}\
      });\
    } catch(e) {}\
  }\
  ['background','backgroundImage','cssText'].forEach(__wr_wrapCssProperty);\
  try {\
    var __wr_nativeSetProperty=CSSStyleDeclaration.prototype.setProperty;\
    CSSStyleDeclaration.prototype.setProperty=function(name,value,priority){\
      return __wr_nativeSetProperty.call(this,name,__wr_rewriteCssURLs(value),priority);\
    };\
  } catch(e) {}\
  function __wr_wrapURLProperty(proto,name){\
    try {\
      var d=Object.getOwnPropertyDescriptor(proto,name);\
      if(d&&d.get&&d.set)Object.defineProperty(proto,name,{\
        configurable:d.configurable,enumerable:d.enumerable,get:d.get,\
        set:function(v){d.set.call(this,__wr_localAssetURL(v));}\
      });\
    } catch(e) {}\
  }\
  [[HTMLImageElement.prototype,'src'],[HTMLMediaElement.prototype,'src'],\
   [HTMLSourceElement.prototype,'src']].forEach(function(x){__wr_wrapURLProperty(x[0],x[1]);});\
  try {\
    var __wr_nativeSetAttribute=Element.prototype.setAttribute;\
    Element.prototype.setAttribute=function(name,value){\
      var n=String(name).toLowerCase();\
      if(n==='src'||n==='poster')value=__wr_localAssetURL(value);\
      else if(n==='style')value=__wr_rewriteCssURLs(value);\
      return __wr_nativeSetAttribute.call(this,name,value);\
    };\
  } catch(e) {}\
  function __wr_rewriteElementAsset(element,name){\
    try {\
      var value=element.getAttribute(name);\
      if(!value||value.toLowerCase().indexOf('file:')<0)return;\
      var rewritten=name==='style'?__wr_rewriteCssURLs(value):__wr_localAssetURL(value);\
      if(rewritten!==value)__wr_nativeSetAttribute.call(element,name,rewritten);\
    } catch(e) {}\
  }\
  function __wr_installLocalAssetObserver(){\
    if(!document.documentElement||window.__wr_localAssetObserver)return;\
    var observer=new MutationObserver(function(records){\
      records.forEach(function(record){__wr_rewriteElementAsset(record.target,record.attributeName);});\
    });\
    observer.observe(document.documentElement,{subtree:true,attributes:true,attributeFilter:['style','src','poster']});\
    window.__wr_localAssetObserver=observer;\
  }\
  if(document.documentElement)__wr_installLocalAssetObserver();\
  document.addEventListener('DOMContentLoaded',__wr_installLocalAssetObserver,{once:true});\
  window.wallpaperEngine_paused = false;\
  var __streams = [];\
  window.wallpaperRegisterAudioStream = function(el){\
    if (el && __streams.indexOf(el) < 0) __streams.push(el);\
    return el;\
  };\
  window.wallpaperRemoveAudioStream = function(el){\
    var i = __streams.indexOf(el); if (i >= 0) __streams.splice(i,1);\
  };\
  window.__wr_applyMute = function(m){\
    for (var i=0;i<__streams.length;i++){ try { __streams[i].muted = !!m; } catch(e){} }\
  };\
  window.__wr_pauseStreams = function(){\
    for (var i=0;i<__streams.length;i++){\
      try {\
        var s = __streams[i];\
        if (!s.paused) { s.__wr_wasPlaying = true; s.pause(); }\
        else s.__wr_wasPlaying = false;\
      } catch(e){}\
    }\
  };\
  window.__wr_resumeStreams = function(){\
    for (var i=0;i<__streams.length;i++){\
      try {\
        var s = __streams[i];\
        if (s.__wr_wasPlaying) { s.__wr_wasPlaying = false; var p = s.play(); if (p && p.catch) p.catch(function(){}); }\
      } catch(e){}\
    }\
  };\
  var __listeners = [];\
  window.wallpaperRegisterAudioListener = function(cb){\
    if (typeof cb === 'function') __listeners.push(cb);\
  };\
  window.wallpaperRemoveAudioListener = function(cb){\
    var i = __listeners.indexOf(cb); if (i >= 0) __listeners.splice(i,1);\
  };\
  window.__wr_pushAudio = function(arr){\
    for (var i=0;i<__listeners.length;i++){ try { __listeners[i](arr); } catch(e){} }\
  };\
  /* A real host pause must stop page-owned clocks as well as WE callbacks.\
     Keep a single rAF wrapper installed: replacing it for fps throttling used\
     to silently remove pause support. */\
  var __paused=false, __fps=0, __rafSerial=1, __rafPending={}, __rafNative={}, __rafDelay={};\
  var __nativeRaf=(window.requestAnimationFrame||function(cb){return setTimeout(function(){cb(performance.now());},16);}).bind(window);\
  var __nativeCancel=(window.cancelAnimationFrame||clearTimeout).bind(window);\
  var __nativeSetTimeout=window.setTimeout.bind(window), __nativeClearTimeout=window.clearTimeout.bind(window);\
  function __runRaf(id, cb, stamp){\
    if (!__rafPending[id]) return;\
    if (__paused) { __rafNative[id]=0; __rafDelay[id]=0; return; }\
    if (__fps>0&&__fps<60) window.__wr_lastRaf=performance.now();\
    delete __rafPending[id]; delete __rafNative[id]; cb(stamp);\
  }\
  function __scheduleRaf(id){\
    var cb=__rafPending[id]; if (!cb||__paused) return;\
    var interval=__fps>0&&__fps<60?1000/__fps:0;\
    if (interval) {\
      var now=performance.now(), wait=window.__wr_lastRaf?Math.max(0,interval-(now-window.__wr_lastRaf)):0;\
      __rafDelay[id]=__nativeSetTimeout(function(){\
        delete __rafDelay[id]; __runRaf(id,cb,performance.now());\
      },wait);\
    } else {\
      __rafNative[id]=__nativeRaf(function(t){__runRaf(id,cb,t);});\
    }\
  }\
  window.requestAnimationFrame=function(cb){\
    var id=__rafSerial++; __rafPending[id]=cb; __scheduleRaf(id); return id;\
  };\
  window.cancelAnimationFrame=function(id){\
    if (__rafNative[id]) __nativeCancel(__rafNative[id]);\
    if (__rafDelay[id]) __nativeClearTimeout(__rafDelay[id]);\
    delete __rafNative[id]; delete __rafDelay[id]; delete __rafPending[id];\
  };\
  window.__wr_setFps=function(fps){ __fps=(!isFinite(fps)||fps<=0||fps>=60)?0:fps; window.__wr_lastRaf=0; };\
  var __timerSerial=1, __timers={};\
  var __nativeSetInterval=window.setInterval.bind(window), __nativeClearInterval=window.clearInterval.bind(window);\
  function __scheduleTimer(id){\
    var t=__timers[id]; if (!t||__paused) return;\
    t.due=Date.now()+t.remaining;\
    t.native=__nativeSetTimeout(function(){\
      var current=__timers[id]; if (!current) return; current.native=0;\
      if (__paused) { current.remaining=Math.max(0,current.due-Date.now()); return; }\
      if (!current.repeat) delete __timers[id];\
      try { current.fn.apply(window,current.args); } catch(e){ setTimeout(function(){throw e;},0); }\
      if (current.repeat&&__timers[id]) { current.remaining=current.delay; __scheduleTimer(id); }\
    },t.remaining);\
  }\
  function __makeTimer(fn,delay,repeat,args){\
    if (typeof fn!=='function') return repeat?__nativeSetInterval(fn,delay):__nativeSetTimeout(fn,delay);\
    var id=__timerSerial++, ms=Math.max(0,Number(delay)||0);\
    __timers[id]={fn:fn,args:args,delay:ms,remaining:ms,repeat:repeat,native:0,due:0}; __scheduleTimer(id); return id;\
  }\
  window.setTimeout=function(fn,delay){return __makeTimer(fn,delay,false,Array.prototype.slice.call(arguments,2));};\
  window.setInterval=function(fn,delay){return __makeTimer(fn,delay,true,Array.prototype.slice.call(arguments,2));};\
  window.clearTimeout=window.clearInterval=function(id){\
    var t=__timers[id]; if (t) { if(t.native) __nativeClearTimeout(t.native); delete __timers[id]; return; } __nativeClearTimeout(id); __nativeClearInterval(id);\
  };\
  function __allMedia(){\
    var a=__streams.slice(); try { var m=document.querySelectorAll('audio,video'); for(var i=0;i<m.length;i++) if(a.indexOf(m[i])<0)a.push(m[i]); }catch(e){} return a;\
  }\
  function __pauseMedia(){ var a=__allMedia(); for(var i=0;i<a.length;i++)try{var s=a[i];s.__wr_wasPlaying=!s.paused;if(s.__wr_wasPlaying)s.pause();}catch(e){} }\
  function __resumeMedia(){ var a=__allMedia(); for(var i=0;i<a.length;i++)try{var s=a[i];if(s.__wr_wasPlaying){s.__wr_wasPlaying=false;var p=s.play();if(p&&p.catch)p.catch(function(){});}}catch(e){} }\
  function __setCssPaused(p){\
    var root=document.documentElement; if(!root)return;\
    if(!document.getElementById('__wr_pause_style')){var style=document.createElement('style');style.id='__wr_pause_style';style.textContent='html.__wr-paused *,html.__wr-paused *::before,html.__wr-paused *::after{-webkit-animation-play-state:paused!important;animation-play-state:paused!important;}';(document.head||root).appendChild(style);}\
    if(p)root.classList.add('__wr-paused');else root.classList.remove('__wr-paused');\
  }\
  window.__wr_pauseStreams=__pauseMedia; window.__wr_resumeStreams=__resumeMedia;\
  var __wr_pendingProps={},__wr_propsTimer=0,__wr_propsDelay=25;\
  function __wr_flushProps(){\
    __wr_propsTimer=0;\
    var listener=window.wallpaperPropertyListener;\
    if(listener&&typeof listener.applyUserProperties==='function'){\
      try {\
        var applied=__wr_pendingProps;\
        listener.applyUserProperties(applied);__wr_pendingProps={};__wr_propsDelay=25;\
        return;\
      }\
      catch(e){ console.error('WebRenderer applyUserProperties:',e); }\
    }\
    if(Object.keys(__wr_pendingProps).length){\
      __wr_propsTimer=__nativeSetTimeout(__wr_flushProps,__wr_propsDelay);\
      __wr_propsDelay=Math.min(250,__wr_propsDelay*2);\
    }\
  }\
  window.__wr_applyProps=function(props){\
    if(!props||typeof props!=='object')return;\
    Object.keys(props).forEach(function(key){__wr_pendingProps[key]=props[key];});\
    if(!__wr_propsTimer)__wr_flushProps();\
  };\
  window.__wr_applySnapshot=function(props,generation){\
    if(!props||typeof props!=='object')return -1;\
    var listener=window.wallpaperPropertyListener;\
    if(!listener||typeof listener.applyUserProperties!=='function')return 0;\
    try {\
      if(__wr_propsTimer){__nativeClearTimeout(__wr_propsTimer);__wr_propsTimer=0;}\
      __wr_pendingProps={};\
      listener.applyUserProperties(props);\
      if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.wrProperties)\
        window.webkit.messageHandlers.wrProperties.postMessage({generation:String(generation||'snapshot'),count:Object.keys(props).length});\
      return 1;\
    } catch(e) { console.error('WebRenderer applyUserProperties:',e); return -1; }\
  };\
  window.__wr_setPaused = function(p){\
    p=!!p; if (__paused===p) return; __paused=p; window.wallpaperEngine_paused=p;\
    __setCssPaused(p);\
    if(p){\
      Object.keys(__rafNative).forEach(function(id){if(__rafNative[id])__nativeCancel(__rafNative[id]);__rafNative[id]=0;});\
      Object.keys(__rafDelay).forEach(function(id){if(__rafDelay[id])__nativeClearTimeout(__rafDelay[id]);__rafDelay[id]=0;});\
      Object.keys(__timers).forEach(function(id){var t=__timers[id];if(t.native){__nativeClearTimeout(t.native);t.native=0;t.remaining=Math.max(0,t.due-Date.now());}});\
      __pauseMedia();\
    }else{\
      Object.keys(__rafPending).forEach(function(id){__scheduleRaf(id);});\
      Object.keys(__timers).forEach(function(id){__scheduleTimer(id);});\
      __resumeMedia();\
    }\
    try {\
      if (window.wallpaperPropertyListener && typeof window.wallpaperPropertyListener.setPaused === 'function')\
        window.wallpaperPropertyListener.setPaused(p);\
    } catch(e){ console.error('WebRenderer setPaused:', e); }\
  };\
  /* Synthetic mouse-event dispatch — used by WRDesktopInputForwarder to feed \
     the page real desktop clicks/moves (the wallpaper window sits below \
     Finder's desktop window and never receives them directly). \
     4th arg `buttons` maps to MouseEvent.buttons (0=none, 1=left held). */\
  window.__wr_dispatchMouse = function(type, x, y, buttons){\
    try {\
      var btn = (type === 'mousedown' || type === 'mouseup' || type === 'click') ? 0 : -1;\
      var el = document.elementFromPoint(x, y) || document.body;\
      el.dispatchEvent(new MouseEvent(type, {\
        bubbles: true, cancelable: true, view: window, clientX: x, clientY: y,\
        button: (btn >= 0 ? btn : 0), buttons: (buttons || 0)\
      }));\
    } catch(e){ console.error('WebRenderer dispatchMouse:', e); }\
  };\
  /* Pipe console.* to native (≈ OWE ClientHandler::OnConsoleMessage). */\
  if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wrConsole) {\
    var __oc = window.console || {};\
    var __wrap = function(level, orig){\
      return function(){\
        try {\
          var args = Array.prototype.slice.call(arguments);\
          var msg = args.map(function(a){\
            try { return (typeof a === 'object') ? JSON.stringify(a) : String(a); } catch(e){ return String(a); }\
          }).join(' ');\
          window.webkit.messageHandlers.wrConsole.postMessage({type: level, message: msg});\
        } catch(e){}\
        if (typeof orig === 'function') { try { orig.apply(window.console, args); } catch(e){} }\
      };\
    };\
    window.console = {\
      log: __wrap('log', __oc.log), info: __wrap('info', __oc.info),\
      warn: __wrap('warn', __oc.warn), error: __wrap('error', __oc.error),\
      debug: __wrap('debug', __oc.debug)\
    };\
    window.onerror = function(msg, src, line, col){\
      try { window.webkit.messageHandlers.wrConsole.postMessage({type:'error', message:'onerror: '+msg+' ('+src+':'+line+':'+col+')'}); } catch(e){}\
      return false;\
    };\
  }\
})();";

static NSString *const kDefaultUserAgent =
    @"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
     "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

@interface WebRendererEngine () <WKScriptMessageHandler>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, strong) WRAudioTap *audioTap;
@property (nonatomic, strong) NSTimer *audioTimer;
@property (nonatomic, strong) WRManifest *manifest;
@property (nonatomic, strong) WRURLSchemeHandler *schemeHandler;
@property (nonatomic, assign) BOOL didFinishLoad;
@property (nonatomic, strong) NSMutableArray<NSString *> *pendingJS;
@property (nonatomic, assign) float volume;
@property (nonatomic, assign) BOOL muted;
@property (nonatomic, strong) NSMutableDictionary<NSString *, id> *userPropertySnapshot;
@property (nonatomic, copy) NSString *userPropertyGeneration;
@property (nonatomic, assign) NSUInteger propertyApplySerial;
@property (nonatomic, assign) BOOL propertySnapshotApplied;
@end

@implementation WebRendererEngine {
    WREngineConfig _config;
}

+ (WREngineConfig)defaultConfig {
    WREngineConfig c;
    c.enableInspector = YES;
    c.enableAudioSpectrum = YES;
    c.enableAudioPlayback = YES;
    c.initialVolume = 1.0f;
    c.frameRate = 60;
    c.userAgent = nil;
    c.assetOverlayDirectories = nil;
    return c;
}

- (instancetype)initWithFrame:(NSRect)frame config:(WREngineConfig)config {
    self = [super init];
    if (self) {
        _config = config;
        _pendingJS = [NSMutableArray array];
        _volume = config.initialVolume;
        _muted = (config.initialVolume <= 0.0f);
        _audioTap = [[WRAudioTap alloc] init];
        [self setupWebViewWithFrame:frame];
    }
    return self;
}

- (void)setupWebViewWithFrame:(NSRect)frame {
    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    WKUserContentController *ucc = [WKUserContentController new];
    WKUserScript *shim = [[WKUserScript alloc] initWithSource:kShimJS
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES];
    [ucc addUserScript:shim];
    [ucc addScriptMessageHandler:self name:@"wrConsole"];
    [ucc addScriptMessageHandler:self name:@"wrProperties"];
    cfg.userContentController = ucc;
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = YES;
    cfg.suppressesIncrementalRendering = NO;
    if (_config.enableAudioPlayback) {
        cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    }

    // Custom scheme = the WKWebView equivalent of CEF's --allow-file-access-from-files.
    _schemeHandler = [WRURLSchemeHandler new];
    [cfg setURLSchemeHandler:_schemeHandler forURLScheme:@"we-wallpaper"];

    _webView = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
    _webView.navigationDelegate = self;
    _webView.customUserAgent = (_config.userAgent.length > 0) ? _config.userAgent : kDefaultUserAgent;
    if (@available(macOS 13.0, *)) {
        _webView.inspectable = _config.enableInspector ? YES : NO;
    }
    _webView.configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
}

#pragma mark - Open wallpaper

- (void)openWallpaper:(WRManifest *)manifest {
    _manifest = manifest;
    _didFinishLoad = NO;
    _propertyApplySerial += 1;
    _propertySnapshotApplied = NO;
    _userPropertySnapshot = nil;
    _userPropertyGeneration = nil;
    [_pendingJS removeAllObjects];

    _schemeHandler.baseDirectory = manifest.workshopDir;
    _schemeHandler.overlayDirectories = _config.assetOverlayDirectories ?: @[];
    NSString *entry = manifest.entryHTML ?: @"index.html";
    NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"we-wallpaper://wallpaper/%@", entry]];
    fprintf(stderr, "WebRenderer: loading %s\n", entry.UTF8String ?: "index.html");
    [_webView loadRequest:[NSURLRequest requestWithURL:url]];
}

#pragma mark - JS helpers

- (NSString *)jsLiteralForObject:(id)obj {
    if (obj == nil || obj == [NSNull null]) return @"null";
    if ([obj isKindOfClass:[NSString class]]) {
        NSData *d = [NSJSONSerialization dataWithJSONObject:@[obj] options:0 error:nil];
        if (d == nil) return @"null";
        NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        return [s substringWithRange:NSMakeRange(1, s.length - 2)];
    }
    if ([obj isKindOfClass:[NSNumber class]]) {
        if (strcmp([obj objCType], @encode(BOOL)) == 0 ||
            strcmp([obj objCType], @encode(bool)) == 0) {
            return [obj boolValue] ? @"true" : @"false";
        }
        return [obj description];
    }
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"null";
}

// Evaluate now if loaded, else queue and replay on didFinishNavigation.
- (void)eval:(NSString *)script {
    if (_didFinishLoad) {
        [_webView evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
            (void)result;
            if (error != nil) {
                fprintf(stderr, "WebRenderer: JavaScript control failed: %s\n",
                        error.localizedDescription.UTF8String ?: "unknown error");
            }
        }];
    } else {
        [_pendingJS addObject:script];
    }
}

- (void)flushPendingJS {
    NSArray *pending = [_pendingJS copy];
    [_pendingJS removeAllObjects];
    for (NSString *s in pending) {
        [self eval:s];
    }
}

#pragma mark - WE API

- (void)beginPropertySnapshotApplication {
    if (!_didFinishLoad || _userPropertySnapshot == nil) return;
    _propertySnapshotApplied = NO;
    NSUInteger serial = ++_propertyApplySerial;
    [self applyPropertySnapshotWithSerial:serial attempt:0];
}

- (void)applyPropertySnapshotWithSerial:(NSUInteger)serial attempt:(NSUInteger)attempt {
    if (!_didFinishLoad || serial != _propertyApplySerial || _userPropertySnapshot == nil) return;
    NSString *json = [self jsLiteralForObject:_userPropertySnapshot];
    NSString *generationJSON = [self jsLiteralForObject:_userPropertyGeneration ?: @"snapshot"];
    NSString *script = [NSString stringWithFormat:@"__wr_applySnapshot(%@, %@);", json, generationJSON];
    __weak WebRendererEngine *weakSelf = self;
    [_webView evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
        __strong WebRendererEngine *self = weakSelf;
        if (self == nil || serial != self.propertyApplySerial || !self.didFinishLoad) return;
        if (error != nil) {
            fprintf(stderr, "WebRenderer: property snapshot failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown error");
            return;
        }
        NSInteger status = [result respondsToSelector:@selector(integerValue)] ? [result integerValue] : 0;
        if (status == 1) {
            self.propertySnapshotApplied = YES;
            return;
        }
        if (status < 0) {
            fprintf(stderr, "WebRenderer: wallpaper rejected property snapshot generation=%s\n",
                    self.userPropertyGeneration.UTF8String ?: "snapshot");
            return;
        }
        if (attempt >= 480) {
            fprintf(stderr, "WebRenderer: property listener did not become ready for generation=%s\n",
                    self.userPropertyGeneration.UTF8String ?: "snapshot");
            return;
        }
        NSUInteger delayStep = MIN(attempt, (NSUInteger)4);
        NSTimeInterval delay = MIN(0.25, 0.025 * (1u << delayStep));
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [self applyPropertySnapshotWithSerial:serial attempt:attempt + 1];
        });
    }];
}

- (void)applyUserProperty:(NSString *)key value:(id)value {
    if (_userPropertySnapshot == nil) {
        NSDictionary *base = _manifest.userProperties ?: @{};
        _userPropertySnapshot = [base mutableCopy];
        _userPropertyGeneration = @"manifest";
    }
    _userPropertySnapshot[key] = value;
    if (!_didFinishLoad) return;
    if (!_propertySnapshotApplied) {
        [self beginPropertySnapshotApplication];
        return;
    }
    NSString *payload = [self jsLiteralForObject:@{key: value}];
    [self eval:[NSString stringWithFormat:@"__wr_applyProps(%@);", payload]];
}

- (void)applyUserProperties:(NSDictionary<NSString *,id> *)properties generation:(NSString *)generation {
    _userPropertySnapshot = [properties mutableCopy];
    _userPropertyGeneration = generation.length > 0 ? [generation copy] : @"snapshot";
    [self beginPropertySnapshotApplication];
}

- (void)setPaused:(BOOL)paused {
    [self eval:[NSString stringWithFormat:@"__wr_setPaused(%@);", paused ? @"true" : @"false"]];
}

- (void)setVolume:(float)volume {
    _volume = volume;
    [self applyUserProperty:@"audio" value:@{@"value": @(volume)}];
    BOOL effectiveMuted = _muted || volume <= 0.0f;
    [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
}

- (void)setMuted:(BOOL)muted {
    _muted = muted;
    BOOL effectiveMuted = muted || _volume <= 0.0f;
    [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
}

- (void)setFrameRate:(int)fps {
    [self eval:[NSString stringWithFormat:@"__wr_setFps(%d);", fps]];
}

#pragma mark - Audio spectrum

- (void)startAudioSpectrum {
    if (!_config.enableAudioSpectrum || _audioTimer != nil) return;
    __weak WebRendererEngine *weakSelf = self;
    [_audioTap startWithCompletion:^(BOOL ok, NSString *msg) {
        __strong WebRendererEngine *s = weakSelf;
        if (!s) return;
        if (!ok) {
            fprintf(stderr, "WebRenderer: audio spectrum disabled (%s)\n",
                    msg ? msg.UTF8String : "unknown");
            return;
        }
        if (getenv("WR_DEBUG")) {
            fprintf(stderr, "WebRenderer: audio spectrum tap running\n");
        }
        s->_audioTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/30.0 repeats:YES
            block:^(NSTimer *t) { (void)t; [s tickAudio]; }];
        [[NSRunLoop mainRunLoop] addTimer:s->_audioTimer forMode:NSRunLoopCommonModes];
    }];
}

- (void)stopAudioSpectrum {
    [_audioTimer invalidate];
    _audioTimer = nil;
    [_audioTap stop];
}

- (void)tickAudio {
    float bins[64];
    if (![_audioTap copySpectrum:bins count:64]) return;

    // WE contract: 128 floats — [0..63]=L, [64..127]=R. Mono source duplicated
    // into both halves (matches OWE's WebViewer with wavsen's mono output).
    NSMutableString *arr = [NSMutableString stringWithCapacity:128 * 8];
    [arr appendString:@"__wr_pushAudio(["];
    char buf[32];
    for (int i = 0; i < 64; ++i) {
        if (i) [arr appendString:@","];
        snprintf(buf, sizeof(buf), "%.4f", bins[i]);
        [arr appendFormat:@"%s", buf];
    }
    for (int i = 0; i < 64; ++i) {
        [arr appendString:@","];
        snprintf(buf, sizeof(buf), "%.4f", bins[i]);
        [arr appendFormat:@"%s", buf];
    }
    [arr appendString:@"]);"];
    [_webView evaluateJavaScript:arr completionHandler:nil];
}

#pragma mark - WKNavigationDelegate

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    _didFinishLoad = YES;
    fprintf(stderr, "WebRenderer: navigation finished url=%s; injecting user properties\n",
            webView.URL.absoluteString.UTF8String ?: "unknown");

    if (_userPropertySnapshot == nil) {
        _userPropertySnapshot = [(_manifest.userProperties ?: @{}) mutableCopy];
        _userPropertyGeneration = @"manifest";
    }
    if (_config.initialVolume < 1.0f || _muted) {
        _userPropertySnapshot[@"audio"] = @{ @"value": @(_volume) };
        BOOL effectiveMuted = _muted || _volume <= 0.0f;
        [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
    }
    [self beginPropertySnapshotApplication];
    if (_config.frameRate > 0 && _config.frameRate < 60) {
        [self setFrameRate:_config.frameRate];
    }
    [self flushPendingJS];
}

- (void)webView:(WKWebView *)webView
        decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction
                        decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    (void)webView;
    NSString *scheme = navigationAction.request.URL.scheme.lowercaseString;
    // Allow our scheme + local/about/data; cancel external page-level nav so a
    // wallpaper can't yank the window off to the web. Sub-resources unaffected.
    if ([scheme isEqualToString:@"we-wallpaper"] || [scheme isEqualToString:@"file"] ||
        [scheme isEqualToString:@"about"] || [scheme isEqualToString:@"data"]) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    fprintf(stderr, "WebRenderer: blocked external navigation to %s\n",
            navigationAction.request.URL.absoluteString.UTF8String ?: "");
    decisionHandler(WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation;
    _didFinishLoad = NO;
    _propertyApplySerial += 1;
    fprintf(stderr, "WebRenderer: navigation failed: %s\n", error.localizedDescription.UTF8String ?: "");
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation;
    _didFinishLoad = NO;
    _propertyApplySerial += 1;
    fprintf(stderr, "WebRenderer: provisional load failed: %s\n", error.localizedDescription.UTF8String ?: "");
}

- (void)userContentController:(WKUserContentController *)ucc didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)ucc;
    if ([message.name isEqualToString:@"wrProperties"]) {
        NSDictionary *body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : nil;
        fprintf(stderr, "WebRenderer: applied property snapshot generation=%s count=%ld\n",
                [body[@"generation"] description].UTF8String ?: "unknown",
                (long)[body[@"count"] integerValue]);
        return;
    }
    if (![message.name isEqualToString:@"wrConsole"]) return;
    NSDictionary *body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : nil;
    NSString *type = body[@"type"] ?: @"log";
    NSString *text = body[@"message"] ?: @"";
    fprintf(stderr, "WebRenderer [%s] %s\n", type.UTF8String ?: "log", text.UTF8String ?: "");
}

@end
