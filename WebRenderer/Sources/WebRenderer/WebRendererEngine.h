#pragma once

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN

@class WRManifest;
@class WRAudioTap;

// Egress policy for the untrusted wallpaper page. The navigation delegate only
// gates NAVIGATIONS; fetch/XMLHttpRequest/WebSocket/<img src>/<script src> to
// arbitrary hosts are otherwise completely unrestricted, so a Workshop
// wallpaper can exfiltrate anything it can read.
typedef NS_ENUM(NSInteger, WRNetworkPolicy) {
    // Default. Does not block — logs every remote request to stderr so the
    // wallpaper corpus can be audited before the default is tightened.
    WRNetworkPolicyObserve = 0,
    // WKContentRuleList denying everything but we-wallpaper:/about:/data:/blob:.
    WRNetworkPolicyBlock,
    // Legacy behaviour: unrestricted, and silent about it.
    WRNetworkPolicyAllow,
};

typedef struct {
    BOOL enableInspector;            // webView.inspectable — Safari Web Inspector
    BOOL enableAudioSpectrum;        // start WRAudioTap for wallpaperRegisterAudioListener
    BOOL enableAudioPlayback;        // allow media autoplay with sound
    BOOL initiallySuspendsMediaPlayback; // host barrier completed before first navigation
    float initialVolume;             // master volume 0..1 (applied via "audio" property)
    int  frameRate;                  // target fps (0 or ≥60 ⇒ no throttle)
    BOOL loadFromMemory;             // cache wallpaper resource bytes for process lifetime
    WRNetworkPolicy networkPolicy;   // egress control for untrusted page script
    NSString *_Nullable userAgent;   // nil ⇒ Chrome-on-mac default
    NSArray<NSString *> *_Nullable assetOverlayDirectories;
} WREngineConfig;

// Owns a WKWebView and implements the Wallpaper Engine web-wallpaper host
// contract. The WKWebView counterpart to OWE's weweb::BrowserHost: where OWE
// drives CEF (Init/OpenWallpaper/ApplyUserProperty/SetPaused/SetFrameRate/
// ApplyVolume/PushAudioData), this drives WKWebView. WKWebView renders
// straight to its CoreAnimation layer, so no CEF OSR / DMA-BUF / Vulkan
// presenter — the host just embeds `webView` in a window.
//
// WE JS APIs are installed as a WKUserScript at document-start (≈ CEF
// OnContextCreated) and driven via evaluateJavaScript: (≈ ExecuteJavaScript);
// initial properties inject on didFinishNavigation: (≈ OnLoadEnd).
@interface WebRendererEngine : NSObject <WKNavigationDelegate>

+ (WREngineConfig)defaultConfig;

- (instancetype)initWithFrame:(NSRect)frame config:(WREngineConfig)config;

@property (nonatomic, strong, readonly) WKWebView *webView;
/// Called when page listener demand changes. The value is false while paused.
@property (nonatomic, copy, nullable) void (^audioSpectrumDemandHandler)(BOOL needed);
/// Called once after navigation has finished and WebKit has produced a composited
/// snapshot. Desktop hosts use this to keep replacement windows hidden until the
/// page can actually be presented.
@property (nonatomic, copy, nullable) void (^contentReadyHandler)(void);

// Load via we-wallpaper://wallpaper/<entry> (served by WRURLSchemeHandler).
- (void)openWallpaper:(WRManifest *)manifest;

// wallpaperPropertyListener.applyUserProperties({key: {value: ...}}).
- (void)applyUserProperty:(NSString *)key value:(id)value;
- (void)applyUserProperties:(NSDictionary<NSString *, id> *)properties generation:(NSString *)generation;

// Freezes page animation clocks, timers, CSS animations and playing media, then
// calls wallpaperPropertyListener.setPaused (the Wallpaper Engine contract).
- (void)setPaused:(BOOL)paused;

// Master volume: applies the "audio" property + mutes/unmutes registered streams.
- (void)setVolume:(float)volume;
- (void)setMuted:(BOOL)muted;

// Host-level media barrier. Unlike JavaScript muting, WebKit guarantees that a
// suspended page cannot resume HTML media or WebAudio until the paired NO call
// completes. Desktop lifecycle events must wait for this completion.
- (void)setHostMediaPlaybackSuspended:(BOOL)suspended
                            completion:(void (^ _Nullable)(void))completion;

// Injects a requestAnimationFrame throttle shim when fps < 60 (no native
// equivalent of CEF's SetWindowlessFrameRate).
- (void)setFrameRate:(int)fps;

- (void)startAudioSpectrum;
- (void)stopAudioSpectrum;
- (void)pushAudioSpectrum:(NSArray<NSNumber *> *)spectrum;

// Writes a still of what the page currently shows to `path` (HEIC, falling back
// to JPEG where no HEVC encoder exists). Mirage.app installs it as the system
// desktop picture so the menu bar and Dock tint match the wallpaper.
// `completion` runs on the main thread exactly once.
- (void)takeSnapshotToPath:(NSString *)path
                completion:(void (^)(BOOL ok))completion;

@end

NS_ASSUME_NONNULL_END
