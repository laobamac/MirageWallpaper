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

#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/event.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

struct WallpaperArgs {
    const char *workshop = nullptr;
    int   fps = 60;
    float volume = 1.0f;
    BOOL  spectrum = YES;
    BOOL  externalSpectrum = NO;
    int   screen = 0;
    CGDirectDisplayID displayID = 0;
    int   runSeconds = 0;
    BOOL  diag = NO;
    BOOL  controlStdin = NO;
    BOOL  deferredShow = NO;
    BOOL  loadFromMemory = NO;
    WRNetworkPolicy networkPolicy = WRNetworkPolicyObserve;
    std::vector<std::string> assetOverlays;
};

// Orphan guard: if Mirage.app crashes or is force-quit, a renderer left behind
// keeps drawing a full-screen desktop-level window the user cannot close.
// EVFILT_PROC/NOTE_EXIT on the parent is the reliable macOS signal; the
// getppid() poll afterwards covers the case where kqueue registration fails
// (e.g. the parent already exited) and doubles as the fallback.
static void *MirageParentWatchdogMain(void *context) {
    pid_t parent = (pid_t)(intptr_t)context;
    int kq = kqueue();
    if (kq >= 0) {
        struct kevent change;
        EV_SET(&change, (uintptr_t)parent, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
        if (kevent(kq, &change, 1, NULL, 0, NULL) == 0) {
            struct kevent event;
            for (;;) {
                int n = kevent(kq, NULL, 0, &event, 1, NULL);
                if (n > 0) break;                    // parent exited
                if (n < 0 && errno != EINTR) break;  // fall through to polling
            }
        }
        close(kq);
    }
    while (getppid() == parent) {
        sleep(1);
    }
    fprintf(stderr, "WebWallpaper: parent process %d exited; shutting down\n", (int)parent);
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
    // If the main thread is wedged, do not leave an undismissable window up.
    sleep(2);
    _exit(0);
    return NULL;
}

static void MirageStartParentWatchdog(void) {
    pid_t parent = getppid();
    if (parent <= 1) return; // already orphaned / no parent to watch
    pthread_t thread;
    if (pthread_create(&thread, NULL, MirageParentWatchdogMain,
                       (void *)(intptr_t)parent) == 0) {
        pthread_detach(thread);
    }
}

// Events are emitted from the main thread. With a blocking stdout a parent that
// stops draining the pipe fills the 64 KiB buffer and freezes the wallpaper, so
// the descriptor is switched to non-blocking and events are DROPPED rather than
// allowed to stall the renderer. The all-or-nothing guarantee for such a write
// only holds up to PIPE_BUF — 512 bytes on Darwin, NOT the 64 KiB pipe
// capacity. A longer line can come back SHORT, and a truncated JSON line
// desyncs the parent's line parser for every event after it. Payloads are
// therefore clamped below PIPE_BUF. Kept in sync with VideoWallpaper.mm.
static const size_t kMirageMaxEventLine = PIPE_BUF; // 512 on Darwin
static const NSUInteger kMirageMaxEventStringBytes = 256;

static void MirageMakeStdoutNonBlocking(void) {
    int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (flags != -1) fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
}

// Cuts on a composed-character boundary: slicing UTF-16 units blindly can split
// a surrogate pair and yield a string NSJSONSerialization refuses to encode.
static NSString *MirageClampEventString(NSString *value) {
    if ([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= kMirageMaxEventStringBytes) {
        return value;
    }
    NSUInteger cut = MIN(value.length, kMirageMaxEventStringBytes);
    while (cut > 0) {
        cut = [value rangeOfComposedCharacterSequenceAtIndex:cut - 1].location;
        NSString *head = [value substringToIndex:cut];
        if ([head lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= kMirageMaxEventStringBytes - 3) {
            return [head stringByAppendingString:@"..."];
        }
    }
    return @"...";
}

static void MirageEmitEvent(NSDictionary *event) {
    // Page-supplied text is attacker-influenced and unbounded, so every string
    // value is clamped before serialising; that also bounds the line itself.
    NSMutableDictionary *clamped = [event mutableCopy];
    for (id key in event) {
        NSString *value = event[key];
        if (![value isKindOfClass:[NSString class]]) continue;
        clamped[key] = MirageClampEventString(value);
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:clamped options:0 error:nil];
    if (data == nil) return;
    if (data.length + 1 > kMirageMaxEventLine) {
        // JSON escaping can still expand a clamped payload past PIPE_BUF.
        // Dropping the event whole is the only outcome that cannot corrupt the
        // stream for the events that follow it.
        fprintf(stderr, "WebWallpaper: dropping oversized event line (%lu bytes)\n",
                (unsigned long)data.length);
        return;
    }
    NSMutableData *line = [data mutableCopy];
    [line appendBytes:"\n" length:1];
    const uint8_t *cursor = (const uint8_t *)line.bytes;
    size_t remaining = line.length;
    while (remaining > 0) {
        ssize_t written = write(STDOUT_FILENO, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        break; // EAGAIN at offset 0 (parent not draining) or error: drop it
    }
    if (remaining > 0 && remaining < line.length) {
        // Unreachable for a line <= PIPE_BUF, but a half-written line would
        // corrupt every event after it: terminate it so the parent resyncs.
        ssize_t ignored = write(STDOUT_FILENO, "\n", 1);
        (void)ignored;
    }
}

static BOOL ParseNetworkPolicy(const char *value, WRNetworkPolicy &out) {
    if (strcmp(value, "block") == 0)   { out = WRNetworkPolicyBlock;   return YES; }
    if (strcmp(value, "observe") == 0) { out = WRNetworkPolicyObserve; return YES; }
    if (strcmp(value, "allow") == 0)   { out = WRNetworkPolicyAllow;   return YES; }
    fprintf(stderr, "unknown --network-policy value: %s (block|observe|allow)\n", value);
    return NO;
}

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <wallpaper-dir> [options]\n\n"
        "Options:\n"
        "  --fps N                target frame rate (default 60)\n"
        "  --volume 0..1          master volume (default 1.0)\n"
        "  --no-spectrum          disable audio-spectrum capture\n"
        "  --external-spectrum    receive spectrum from the control channel\n"
        "  --screen N             screen index to cover (default 0 = main)\n"
        "  --display-id N         Core Graphics display ID to cover\n"
        "  --network-policy MODE  block | observe | allow (default observe).\n"
        "                         observe logs every remote request the page makes\n"
        "                         without blocking it; block denies everything but\n"
        "                         the wallpaper's own we-wallpaper: resources\n"
        "  --asset-overlay DIR    serve preset assets before base assets\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
        "  --deferred-show        keep the window hidden until an activate command\n"
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
        } else if (strcmp(arg, "--display-id") == 0) {
            const char *v = take(i, arg); if (!v) return false;
            char *end = nullptr;
            unsigned long value = strtoul(v, &end, 10);
            if (end == v || *end != '\0' || value == 0 || value > UINT32_MAX) return false;
            out.displayID = (CGDirectDisplayID)value;
        } else if (strncmp(arg, "--network-policy=", 17) == 0) {
            if (!ParseNetworkPolicy(arg + 17, out.networkPolicy)) return false;
        } else if (strcmp(arg, "--network-policy") == 0) {
            const char *v = take(i, arg);
            if (!v || !ParseNetworkPolicy(v, out.networkPolicy)) return false;
        } else if (strcmp(arg, "--asset-overlay") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.assetOverlays.emplace_back(v);
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char *v = take(i, arg); if (!v) return false; out.runSeconds = atoi(v);
        } else if (strcmp(arg, "--diag") == 0) {
            out.diag = YES;
        } else if (strcmp(arg, "--control-stdin") == 0) {
            out.controlStdin = YES;
        } else if (strcmp(arg, "--deferred-show") == 0) {
            out.deferredShow = YES;
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
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen *)screen { return frameRect; }
@end

@interface WebWallpaperAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) WebRendererEngine *engine;
@property (nonatomic, strong) WRDesktopInputForwarder *inputForwarder;
@property (nonatomic, strong) MirageControlChannel *control;
@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, assign) BOOL deferredShow;
@property (nonatomic, assign) BOOL prepared;
@property (nonatomic, assign) BOOL windowActivated;
@property (nonatomic, assign) BOOL playbackPaused;
@property (nonatomic, assign) NSUInteger visibilityEpoch;
- (void)observeScreenParameterChanges;
- (void)contentDidBecomeReady;
- (void)activateWindow;
- (void)deactivateWindow;
- (void)prepareActivationForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts;
- (void)beginActivationFailureForEpoch:(NSUInteger)epoch;
- (void)confirmActivationFailureForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts;
- (void)syncInputForwarder;
@end
@implementation WebWallpaperAppDelegate

static NSScreen *MirageScreenForDisplayID(CGDirectDisplayID displayID) {
    for (NSScreen *screen in NSScreen.screens) {
        NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
        if (number != nil && number.unsignedIntValue == displayID) return screen;
    }
    return nil;
}

static BOOL MirageNearlyEqual(CGFloat lhs, CGFloat rhs) {
    return std::abs(lhs - rhs) <= 0.5;
}

static BOOL MirageRectMatches(CGRect lhs, CGRect rhs) {
    return MirageNearlyEqual(CGRectGetMinX(lhs), CGRectGetMinX(rhs)) &&
           MirageNearlyEqual(CGRectGetMinY(lhs), CGRectGetMinY(rhs)) &&
           MirageNearlyEqual(CGRectGetWidth(lhs), CGRectGetWidth(rhs)) &&
           MirageNearlyEqual(CGRectGetHeight(lhs), CGRectGetHeight(rhs));
}

static BOOL MirageNSRectMatches(NSRect lhs, NSRect rhs) {
    return MirageNearlyEqual(NSMinX(lhs), NSMinX(rhs)) &&
           MirageNearlyEqual(NSMinY(lhs), NSMinY(rhs)) &&
           MirageNearlyEqual(NSWidth(lhs), NSWidth(rhs)) &&
           MirageNearlyEqual(NSHeight(lhs), NSHeight(rhs));
}

static BOOL MirageWindowServerState(NSWindow *window, CGRect *bounds,
                                    BOOL *onscreen, double *alpha) {
    if (window == nil || window.windowNumber <= 0) return NO;
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionIncludingWindow, (CGWindowID)window.windowNumber);
    if (windows == NULL || CFArrayGetCount(windows) != 1) {
        if (windows != NULL) CFRelease(windows);
        return NO;
    }
    CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windows, 0);
    CFDictionaryRef encoded = (CFDictionaryRef)CFDictionaryGetValue(info, kCGWindowBounds);
    BOOL hasBounds = encoded != NULL && CGRectMakeWithDictionaryRepresentation(encoded, bounds);
    CFBooleanRef visible = (CFBooleanRef)CFDictionaryGetValue(info, kCGWindowIsOnscreen);
    *onscreen = visible != NULL && CFBooleanGetValue(visible);
    CFNumberRef opacity = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowAlpha);
    *alpha = 0.0;
    if (opacity != NULL) CFNumberGetValue(opacity, kCFNumberDoubleType, alpha);
    CFRelease(windows);
    return hasBounds;
}

- (BOOL)normalizeGeometry {
    NSScreen *screen = MirageScreenForDisplayID(self.displayID);
    if (screen == nil || NSWidth(screen.frame) <= 0 || NSHeight(screen.frame) <= 0) return NO;
    if (!MirageNSRectMatches(self.window.frame, screen.frame)) {
        [self.window setFrame:screen.frame display:YES];
    }
    NSView *content = self.window.contentView;
    if (content == nil) return NO;
    content.frame = NSMakeRect(0, 0, NSWidth(screen.frame), NSHeight(screen.frame));
    [content layoutSubtreeIfNeeded];
    [self.inputForwarder updateScreen:screen];
    CGRect bounds = CGRectZero;
    BOOL onscreen = NO;
    double alpha = 0.0;
    return MirageNSRectMatches(self.window.frame, screen.frame) &&
           MirageWindowServerState(self.window, &bounds, &onscreen, &alpha) &&
           MirageRectMatches(bounds, CGDisplayBounds(self.displayID));
}

- (BOOL)visibleStateMatches:(BOOL)visible {
    if (visible && ![self normalizeGeometry]) return NO;
    if (!visible && (self.window.isVisible || self.window.alphaValue > 0.001)) return NO;
    CGRect bounds = CGRectZero;
    BOOL onscreen = NO;
    double alpha = 0.0;
    if (!MirageWindowServerState(self.window, &bounds, &onscreen, &alpha)) return !visible;
    if (visible) {
        return self.window.isVisible && onscreen && self.window.alphaValue >= 0.999 && alpha >= 0.999;
    }
    return !self.window.isVisible && !onscreen && self.window.alphaValue <= 0.001 && alpha <= 0.001;
}

- (void)confirmVisible:(BOOL)visible epoch:(NSUInteger)epoch attempts:(NSInteger)attempts {
    if (epoch != self.visibilityEpoch) return;
    if ([self visibleStateMatches:visible]) {
        if (visible) {
            // Keep WebKit's host barrier engaged while the alpha-zero window is
            // registered and its target-display geometry is validated. Only a
            // WindowServer-visible window may resume media, and `activated` is
            // not true until WebKit confirms that paired resume completed.
            __weak WebWallpaperAppDelegate *weakSelf = self;
            [self.engine setHostMediaPlaybackSuspended:NO completion:^{
                WebWallpaperAppDelegate *strongSelf = weakSelf;
                if (strongSelf == nil || epoch != strongSelf.visibilityEpoch) return;
                if (![strongSelf visibleStateMatches:YES]) {
                    [strongSelf beginActivationFailureForEpoch:epoch];
                    return;
                }
                strongSelf.windowActivated = YES;
                [strongSelf syncInputForwarder];
                MirageEmitEvent(@{ @"event": @"activated" });
            }];
            return;
        }
        self.windowActivated = visible;
        [self syncInputForwarder];
        MirageEmitEvent(@{ @"event": @"deactivated" });
        return;
    }
    if (attempts <= 0) {
        if (visible) {
            [self beginActivationFailureForEpoch:epoch];
        }
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        [self confirmVisible:visible epoch:epoch attempts:attempts - 1];
    });
}

- (void)beginActivationFailureForEpoch:(NSUInteger)epoch {
    if (epoch != self.visibilityEpoch) return;
    [self.engine setMuted:YES];
    [self.engine setPaused:YES];
    self.playbackPaused = YES;
    self.windowActivated = NO;
    [self syncInputForwarder];
    __weak WebWallpaperAppDelegate *weakSelf = self;
    [self.engine setHostMediaPlaybackSuspended:YES completion:^{
        WebWallpaperAppDelegate *strongSelf = weakSelf;
        if (strongSelf == nil || epoch != strongSelf.visibilityEpoch) return;
        strongSelf.window.alphaValue = 0.0;
        [strongSelf.window orderOut:nil];
        [strongSelf confirmActivationFailureForEpoch:epoch attempts:200];
    }];
}

- (void)confirmActivationFailureForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts {
    if (epoch != self.visibilityEpoch) return;
    if ([self visibleStateMatches:NO]) {
        MirageEmitEvent(@{ @"event": @"activation-failed" });
        return;
    }
    if (attempts <= 0) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        [self confirmActivationFailureForEpoch:epoch attempts:attempts - 1];
    });
}

- (void)syncInputForwarder {
    if (self.windowActivated && !self.playbackPaused) {
        [self.inputForwarder start];
    } else {
        [self.inputForwarder stop];
    }
}

- (void)contentDidBecomeReady {
    if (self.prepared) return;
    self.prepared = YES;
    MirageEmitEvent(@{ @"event": @"prepared" });
}

- (void)activateWindow {
    if (!self.prepared) {
        self.visibilityEpoch += 1;
        [self beginActivationFailureForEpoch:self.visibilityEpoch];
        return;
    }
    self.visibilityEpoch += 1;
    NSUInteger epoch = self.visibilityEpoch;
    self.window.alphaValue = 0.0;
    [self.window orderFrontRegardless];
    [self prepareActivationForEpoch:epoch attempts:200];
}

- (void)prepareActivationForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts {
    if (epoch != self.visibilityEpoch) return;
    if ([self normalizeGeometry]) {
        self.window.alphaValue = 1.0;
        [self.window displayIfNeeded];
        [self confirmVisible:YES epoch:epoch attempts:200];
        return;
    }
    if (attempts <= 0) {
        [self beginActivationFailureForEpoch:epoch];
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        [self prepareActivationForEpoch:epoch attempts:attempts - 1];
    });
}

- (void)deactivateWindow {
    self.visibilityEpoch += 1;
    NSUInteger epoch = self.visibilityEpoch;
    [self.engine setMuted:YES];
    [self.engine setPaused:YES];
    self.playbackPaused = YES;
    self.windowActivated = NO;
    [self syncInputForwarder];
    __weak WebWallpaperAppDelegate *weakSelf = self;
    [self.engine setHostMediaPlaybackSuspended:YES completion:^{
        WebWallpaperAppDelegate *strongSelf = weakSelf;
        if (strongSelf == nil || epoch != strongSelf.visibilityEpoch) return;
        strongSelf.window.alphaValue = 0.0;
        [strongSelf.window orderOut:nil];
        [strongSelf confirmVisible:NO epoch:epoch attempts:200];
    }];
}

// Resolution changes, display rearrangement and unplug/replug all leave the
// window (and the input forwarder's coordinate normalization) on a stale
// frame, because both captured screen.frame once at launch.
- (void)observeScreenParameterChanges {
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(screenParametersDidChange:)
                                               name:NSApplicationDidChangeScreenParametersNotification
                                             object:nil];
}

- (void)screenParametersDidChange:(NSNotification *)note {
    (void)note;
    NSScreen *screen = MirageScreenForDisplayID(self.displayID);
    if (screen == nil) {
        [self.window orderOut:nil];
        [NSApp terminate:nil];
        return;
    }
    NSRect frame = screen.frame;
    if (NSWidth(frame) <= 0 || NSHeight(frame) <= 0) return;
    if (!NSEqualRects(self.window.frame, frame)) {
        fprintf(stderr, "WebWallpaper: screen parameters changed; resizing to %.0fx%.0f at (%.0f,%.0f)\n",
                NSWidth(frame), NSHeight(frame), NSMinX(frame), NSMinY(frame));
        [self.window setFrame:frame display:YES];
    }
    [self.inputForwarder updateScreen:screen];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        WallpaperArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

        MirageStartParentWatchdog();
        MirageMakeStdoutNonBlocking();

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
        NSScreen *screen = args.displayID != 0
            ? MirageScreenForDisplayID(args.displayID)
            : ((args.screen >= 0 && args.screen < (int)screens.count) ? screens[args.screen] : nil);
        if (screen == nil) { fprintf(stderr, "WebWallpaper: no screen available\n"); return 1; }
        NSNumber *screenNumber = screen.deviceDescription[@"NSScreenNumber"];
        if (screenNumber == nil) return 1;
        CGDirectDisplayID displayID = screenNumber.unsignedIntValue;
        NSRect screenFrame = screen.frame;

        WREngineConfig cfg = [WebRendererEngine defaultConfig];
        cfg.enableInspector = NO;
        cfg.enableAudioSpectrum = args.spectrum && !args.externalSpectrum;
        cfg.initiallySuspendsMediaPlayback = args.deferredShow;
        cfg.initialVolume = args.deferredShow ? 0.0f : args.volume;
        cfg.frameRate = args.fps;
        cfg.loadFromMemory = args.loadFromMemory;
        cfg.networkPolicy = args.networkPolicy;
        NSMutableArray<NSString *> *assetOverlays = [NSMutableArray arrayWithCapacity:args.assetOverlays.size()];
        for (const auto &path : args.assetOverlays) {
            NSString *overlay = [NSString stringWithUTF8String:path.c_str()];
            if (overlay != nil) [assetOverlays addObject:overlay];
        }
        cfg.assetOverlayDirectories = assetOverlays;

        NSRect contentFrame = NSMakeRect(0, 0, NSWidth(screenFrame), NSHeight(screenFrame));
        WebRendererEngine *engine = [[WebRendererEngine alloc] initWithFrame:contentFrame config:cfg];
        delegate.engine = engine;
        delegate.displayID = displayID;
        delegate.deferredShow = args.deferredShow;
        delegate.windowActivated = !args.deferredShow;
        delegate.playbackPaused = NO;
        engine.audioSpectrumDemandHandler = ^(BOOL needed) {
            MirageEmitEvent(@{ @"event": @"audio-demand", @"needed": @(needed) });
        };
        __weak WebWallpaperAppDelegate *weakDelegate = delegate;
        engine.contentReadyHandler = ^{
            [weakDelegate contentDidBecomeReady];
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
        window.alphaValue = args.deferredShow ? 0.0 : 1.0;
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
        if (!args.deferredShow) [delegate.inputForwarder start];

        [delegate observeScreenParameterChanges];

        // Live control channel: Mirage.app pipes JSON commands on stdin.
        if (args.controlStdin) {
            WebRendererEngine *eng = engine;
            delegate.control = [[MirageControlChannel alloc]
                initWithHandler:^(NSDictionary *cmd) {
                    NSString *name = cmd[@"cmd"];
                    id value = cmd[@"value"];
                    if ([name isEqualToString:@"activate"]) {
                        [delegate activateWindow];
                    } else if ([name isEqualToString:@"deactivate"]) {
                        [delegate deactivateWindow];
                    } else if ([name isEqualToString:@"setProperty"]) {
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
                        delegate.playbackPaused = YES;
                        [delegate syncInputForwarder];
                    } else if ([name isEqualToString:@"resume"] || [name isEqualToString:@"play"]) {
                        [eng setPaused:NO];
                        delegate.playbackPaused = NO;
                        [delegate syncInputForwarder];
                    } else if ([name isEqualToString:@"power"]) {
                        // Authoritative playback state from the app. This window
                        // is canHide=NO + orderFrontRegardless, so AppKit never
                        // reports it occluded and this process cannot observe
                        // occlusion, lock or sleep itself — it only obeys.
                        NSString *state = [cmd[@"state"] isKindOfClass:[NSString class]]
                            ? cmd[@"state"] : nil;
                        if (state == nil) return;
                        BOOL shouldPause = [state isEqualToString:@"pause"];
                        if (!shouldPause && ![state isEqualToString:@"run"]
                            && ![state isEqualToString:@"throttle"]) {
                            return;
                        }
                        if (!shouldPause) {
                            id fps = cmd[@"fps"];
                            if ([fps isKindOfClass:[NSNumber class]]) {
                                [eng setFrameRate:[fps intValue]];
                            }
                        }
                        [eng setPaused:shouldPause];
                        delegate.playbackPaused = shouldPause;
                        [delegate syncInputForwarder];
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
                    } else if ([name isEqualToString:@"snapshot"]) {
                        // Mirage.app wants a still of the live page for the
                        // system desktop picture. Always answer, success or
                        // not, or the requester waits out its whole timeout.
                        NSString *path = [cmd[@"path"] isKindOfClass:[NSString class]]
                            ? cmd[@"path"] : nil;
                        NSString *token = [cmd[@"token"] isKindOfClass:[NSString class]]
                            ? cmd[@"token"] : @"";
                        if (path == nil) {
                            MirageEmitEvent(@{ @"event": @"snapshot-done",
                                               @"token": token, @"ok": @NO });
                        } else {
                            [eng takeSnapshotToPath:path completion:^(BOOL ok) {
                                MirageEmitEvent(@{ @"event": @"snapshot-done",
                                                   @"token": token, @"ok": @(ok) });
                            }];
                        }
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
