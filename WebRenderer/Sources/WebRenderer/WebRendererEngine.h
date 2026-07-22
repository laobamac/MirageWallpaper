#pragma once

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN

@class WRManifest;
@class WRAudioTap;

typedef struct {
    BOOL enableInspector;            // webView.inspectable — Safari Web Inspector
    BOOL enableAudioSpectrum;        // start WRAudioTap for wallpaperRegisterAudioListener
    BOOL enableAudioPlayback;        // allow media autoplay with sound
    float initialVolume;             // master volume 0..1 (applied via "audio" property)
    int  frameRate;                  // target fps (0 or ≥60 ⇒ no throttle)
    BOOL loadFromMemory;             // cache wallpaper resource bytes for process lifetime
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

// Injects a requestAnimationFrame throttle shim when fps < 60 (no native
// equivalent of CEF's SetWindowlessFrameRate).
- (void)setFrameRate:(int)fps;

- (void)startAudioSpectrum;
- (void)stopAudioSpectrum;
- (void)pushAudioSpectrum:(NSArray<NSNumber *> *)spectrum;

@end

NS_ASSUME_NONNULL_END
