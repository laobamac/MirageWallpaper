// WebWallpaper — desktop wallpaper host.
//
// Counterpart to OWE's waywallen/web_main.cpp and to SceneRenderer's
// Tools/SceneWallpaper. The window sits just below the desktop-icon layer so
// it reads as the desktop background, spans all spaces, stays put.
//
// Mouse interaction: the wallpaper window renders BELOW Finder's full-screen
// desktop window, which absorbs all desktop clicks — so the window itself is
// display-only (ignoresMouseEvents=YES). Real desktop mouse interaction is
// fed to the page by WRDesktopInputForwarder: a global mouse monitor
// synthesizes JS MouseEvents on the page for clicks on empty desktop, while
// clicks on desktop icons (Finder-position-cached) and on app windows are
// left untouched. Drag stays Finder's rubber-band. This preserves full icon
// + app interactivity while making the wallpaper click-reactive.
//
// Usage:
//   WebWallpaper <wallpaper-dir> [--fps N] [--volume 0..1] [--no-spectrum]
//                 [--screen N] [--run-seconds N]

#import <AppKit/AppKit.h>
#import <CoreGraphics/CGWindowLevel.h>

#import "ControlChannel.h"
#import "WallpaperManifest.h"
#import "WebRendererEngine.h"
#import "WRDesktopInputForwarder.h"

#include <string>
#include <vector>

struct WallpaperArgs {
    const char *workshop = nullptr;
    int   fps = 60;
    float volume = 1.0f;
    BOOL  spectrum = YES;
    BOOL  externalSpectrum = NO;
    int   screen = 0;
    int   runSeconds = 0;
    BOOL  diag = NO;
    BOOL  controlStdin = NO;
    BOOL  loadFromMemory = NO;
    std::vector<std::string> assetOverlays;
};

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <wallpaper-dir> [options]\n\n"
        "Options:\n"
        "  --fps N                target frame rate (default 60)\n"
        "  --volume 0..1          master volume (default 1.0)\n"
        "  --no-spectrum          disable audio-spectrum capture\n"
        "  --external-spectrum    receive spectrum from the control channel\n"
        "  --screen N             screen index to cover (default 0 = main)\n"
        "  --asset-overlay DIR    serve preset assets before base assets\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
        "  --load-from-memory     cache wallpaper resources in memory\n"
        "  --run-seconds N        exit after N seconds (test helper)\n"
        "  --diag                 test the click-forward path (synthetic click)\n"
        "  -h, --help             show this help\n",
        argv0);
}

static BOOL ParseArgs(int argc, char **argv, WallpaperArgs &out) {
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        auto take = [&](int &i, const char *opt) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", opt); return nullptr; }
            return argv[++i];
        };
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            PrintUsage(argv[0]); return false;
        } else if (strcmp(arg, "--fps") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.fps = atoi(v);
        } else if (strcmp(arg, "--volume") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.volume = strtof(v, nullptr);
        } else if (strcmp(arg, "--no-spectrum") == 0) {
            out.spectrum = NO;
        } else if (strcmp(arg, "--external-spectrum") == 0) {
            out.externalSpectrum = YES;
        } else if (strcmp(arg, "--screen") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.screen = atoi(v);
        } else if (strcmp(arg, "--asset-overlay") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.assetOverlays.emplace_back(v);
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.runSeconds = atoi(v);
        } else if (strcmp(arg, "--diag") == 0) {
            out.diag = YES;
        } else if (strcmp(arg, "--control-stdin") == 0) {
            out.controlStdin = YES;
        } else if (strcmp(arg, "--load-from-memory") == 0) {
            out.loadFromMemory = YES;
        } else if (arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg); return false;
        } else {
            if (out.workshop == nullptr) out.workshop = arg;
            else { fprintf(stderr, "unexpected positional argument: %s\n", arg); return false; }
        }
    }
    if (out.workshop == nullptr) { PrintUsage(argv[0]); return false; }
    if (out.fps < 0) out.fps = 0;
    if (out.volume < 0.0f) out.volume = 0.0f;
    if (out.volume > 1.0f) out.volume = 1.0f;
    if (out.screen < 0) out.screen = 0;
    return true;
}

// Non-key (no focus stealing), non-main. Mirrors SceneRendererWallpaperWindow.
@interface WebWallpaperWindow : NSWindow
@end
@implementation WebWallpaperWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface WebWallpaperAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) WebRendererEngine *engine;
@property (nonatomic, strong) WRDesktopInputForwarder *inputForwarder;
@property (nonatomic, strong) MirageControlChannel *control;
@end
@implementation WebWallpaperAppDelegate
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        WallpaperArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

        NSError *manifestErr = nil;
        WRManifest *manifest = [WRManifest loadFromDirectory:@(args.workshop) error:&manifestErr];
        if (manifest == nil) {
            fprintf(stderr, "WebWallpaper: %s\n",
                    manifestErr.localizedDescription.UTF8String ?: "failed to load project.json");
            return 2;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        WebWallpaperAppDelegate *delegate = [WebWallpaperAppDelegate new];
        [app setDelegate:delegate];
        [app finishLaunching];

        NSArray<NSScreen *> *screens = NSScreen.screens;
        NSScreen *screen = (args.screen < (int)screens.count) ? screens[args.screen] : NSScreen.mainScreen;
        if (screen == nil) screen = NSScreen.mainScreen;
        if (screen == nil) { fprintf(stderr, "WebWallpaper: no screen available\n"); return 1; }
        NSRect screenFrame = screen.frame;

        WREngineConfig cfg = [WebRendererEngine defaultConfig];
        cfg.enableInspector = NO;
        cfg.enableAudioSpectrum = args.spectrum && !args.externalSpectrum;
        cfg.initialVolume = args.volume;
        cfg.frameRate = args.fps;
        cfg.loadFromMemory = args.loadFromMemory;
        NSMutableArray<NSString *> *assetOverlays = [NSMutableArray arrayWithCapacity:args.assetOverlays.size()];
        for (const auto &path : args.assetOverlays) {
            NSString *overlay = [NSString stringWithUTF8String:path.c_str()];
            if (overlay != nil) [assetOverlays addObject:overlay];
        }
        cfg.assetOverlayDirectories = assetOverlays;

        WebRendererEngine *engine = [[WebRendererEngine alloc] initWithFrame:screenFrame config:cfg];
        delegate.engine = engine;
        engine.audioSpectrumDemandHandler = ^(BOOL needed) {
            fprintf(stdout, "{\"event\":\"audio-demand\",\"needed\":%s}\n",
                    needed ? "true" : "false");
            fflush(stdout);
        };

        WebWallpaperWindow *window = [[WebWallpaperWindow alloc]
            initWithContentRect:screenFrame
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        window.title = manifest.title.length ? manifest.title : @"WebWallpaper";
        window.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) - 1;
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorIgnoresCycle;
        window.opaque = YES;
        window.backgroundColor = NSColor.blackColor;
        window.hasShadow = NO;
        // The wallpaper renders below Finder's full-screen desktop window, which
        // absorbs all desktop clicks. So the wallpaper window itself is display-
        // only; real desktop mouse interaction is fed to the page by
        // WRDesktopInputForwarder (global mouse monitor → JS synthesis), which
        // preserves icon clicks (left to Finder) and app-window clicks.
        window.ignoresMouseEvents = YES;
        window.acceptsMouseMovedEvents = NO;
        window.releasedWhenClosed = NO;
        window.canHide = NO;

        window.contentView = engine.webView;
        engine.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        delegate.window = window;
        [window orderFrontRegardless];

        [engine openWallpaper:manifest];
        [engine startAudioSpectrum];

        // Forward desktop mouse clicks/moves to the page (icons + app windows
        // stay fully interactive with Finder / their owner).
        delegate.inputForwarder = [[WRDesktopInputForwarder alloc] initWithWebView:engine.webView screen:screen];
        [delegate.inputForwarder start];

        // Live control channel: Mirage.app pipes JSON commands on stdin.
        if (args.controlStdin) {
            WebRendererEngine *eng = engine;
            delegate.control = [[MirageControlChannel alloc]
                initWithHandler:^(NSDictionary *cmd) {
                    NSString *name = cmd[@"cmd"];
                    id value = cmd[@"value"];
                    if ([name isEqualToString:@"setProperty"]) {
                        NSString *key = cmd[@"key"];
                        if ([key isKindOfClass:[NSString class]] && value != nil) {
                            // WE property listener expects {key:{value:...}}.
                            [eng applyUserProperty:key value:@{@"value": value}];
                        }
                    } else if ([name isEqualToString:@"setProperties"]) {
                        NSDictionary *values = [cmd[@"values"] isKindOfClass:[NSDictionary class]] ? cmd[@"values"] : nil;
                        NSString *generation = [cmd[@"generation"] isKindOfClass:[NSString class]] ? cmd[@"generation"] : @"snapshot";
                        if (values != nil) {
                            fprintf(stderr, "WebRenderer: received property snapshot generation=%s count=%ld\n",
                                    generation.UTF8String ?: "snapshot", (long)values.count);
                            [eng applyUserProperties:values generation:generation];
                        }
                    } else if ([name isEqualToString:@"pause"]) {
                        [eng setPaused:YES];
                        [delegate.inputForwarder setPaused:YES];
                    } else if ([name isEqualToString:@"resume"] || [name isEqualToString:@"play"]) {
                        [eng setPaused:NO];
                        [delegate.inputForwarder setPaused:NO];
                    } else if ([name isEqualToString:@"volume"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setVolume:[value floatValue]];
                    } else if ([name isEqualToString:@"muted"]) {
                        if ([value isKindOfClass:[NSNumber class]]) {
                            [eng setMuted:[value boolValue]];
                        }
                    } else if ([name isEqualToString:@"fps"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setFrameRate:[value intValue]];
                    } else if ([name isEqualToString:@"audioSpectrum"]) {
                        NSArray *data = [cmd[@"data"] isKindOfClass:[NSArray class]] ? cmd[@"data"] : nil;
                        if (data.count == 128) [eng pushAudioSpectrum:data];
                    }
                }
                onEOF:^{
                    [NSApp terminate:nil];
                }];
            [delegate.control start];
        }

        if (args.diag) {
            WKWebView *dw = engine.webView;
            // After 5s (page loaded + properties applied), test the dispatch
            // path: read clickCount, synthesize a click at #player-container's
            // center, read clickCount again. If it increments, the JS synthesis
            // works and any remaining issue is in click detection.
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                NSString *probe1 = @"(function(){"
                    "var pc=document.querySelector('#player-container');"
                    "var r=pc?pc.getBoundingClientRect():null;"
                    "var cx=r?(r.left+r.width/2):window.innerWidth/2;"
                    "var cy=r?(r.top+r.height/2):window.innerHeight/2;"
                    "var before=(typeof window.clickCount==='number')?window.clickCount:-1;"
                    "window.__wr_dispatchMouse('click', cx, cy);"
                    "return JSON.stringify({before:before, hasPC:!!pc, cx:Math.round(cx), cy:Math.round(cy),"
                    "pcRect: r?{l:r.left|0,t:r.top|0,w:r.width|0,h:r.height|0}:null, innerW:window.innerWidth, innerH:window.innerHeight});"
                    "})();";
                [dw evaluateJavaScript:probe1 completionHandler:^(id res, NSError *err) {
                    fprintf(stderr, "WebRenderer DIAG-1: %s\n",
                            err ? err.localizedDescription.UTF8String : ([res isKindOfClass:[NSString class]] ? [res UTF8String] : "(null)"));
                    // Re-read clickCount after the handler runs.
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                                   dispatch_get_main_queue(), ^{
                        [dw evaluateJavaScript:@"(typeof window.clickCount==='number')?window.clickCount:-1"
                                 completionHandler:^(id res2, NSError *err2) {
                            fprintf(stderr, "WebRenderer DIAG-2 clickCount after: %s\n",
                                    err2 ? err2.localizedDescription.UTF8String : ([[res2 description] UTF8String]));
                        }];
                    });
                }];
            });
        }

        if (args.runSeconds > 0) {
            int secs = args.runSeconds;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)secs * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                [NSApp stop:nil];
                NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                 location:NSZeroPoint modifierFlags:0
                                                 timestamp:0 windowNumber:0 context:nil
                                                 subtype:0 data1:0 data2:0];
                [NSApp postEvent:ev atStart:NO];
            });
        }

        [app run];

        [delegate.inputForwarder stop];
        [engine stopAudioSpectrum];
    }
    return 0;
}
