// VideoWallpaper — desktop wallpaper host for Wallpaper Engine video wallpapers.
//
// Usage:
//   VideoWallpaper <wallpaper-dir> [--screen N] [--volume 0..1] [--muted]
//                  [--fill cover|contain|stretch] [--run-seconds N]

#import <AppKit/AppKit.h>
#import <CoreGraphics/CGWindowLevel.h>

#import "ControlChannel.h"
#import "VideoManifest.h"
#import "VideoRendererEngine.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <unistd.h>

struct WallpaperArgs {
    const char *workshop = nullptr;
    int screen = 0;
    CGDirectDisplayID displayID = 0;
    float volume = 1.0f;
    BOOL muted = NO;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
    BOOL controlStdin = NO;
    BOOL deferredShow = NO;
    BOOL loadFromMemory = NO;
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
    fprintf(stderr, "VideoWallpaper: parent process %d exited; shutting down\n", (int)parent);
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
// stops draining the pipe fills the 64 KiB buffer and freezes playback, so the
// descriptor is switched to non-blocking and events are DROPPED rather than
// allowed to stall the renderer. The all-or-nothing guarantee for such a write
// only holds up to PIPE_BUF — 512 bytes on Darwin, NOT the 64 KiB pipe
// capacity. A longer line (video-error carries AVFoundation's error text, which
// runs past 400 bytes once an NSOSStatusErrorDomain string is folded in) can
// come back SHORT, and a truncated JSON line desyncs the parent's line parser
// for every event after it. Payloads are therefore clamped below PIPE_BUF.
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
    // Error text is attacker-influenced and unbounded, so every string value is
    // clamped before serialising; that also bounds the line itself.
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
        fprintf(stderr, "VideoWallpaper: dropping oversized event line (%lu bytes)\n",
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

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <wallpaper-dir> [options]\n\n"
        "Options:\n"
        "  --screen N             screen index to cover (default 0 = main)\n"
        "  --display-id N         Core Graphics display ID to cover\n"
        "  --volume 0..1          audio volume (default 1.0)\n"
        "  --muted                start muted\n"
        "  --fill MODE            cover | contain | stretch (default cover)\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
        "  --deferred-show        keep the window hidden until an activate command\n"
        "  --load-from-memory     keep the video bytes in memory\n"
        "  --run-seconds N        exit after N seconds (test helper)\n"
        "  -h, --help             show this help\n",
        argv0);
}

static BOOL ParseFillMode(const char *value, VRVideoFillMode &out) {
    if (strcmp(value, "cover") == 0) {
        out = VRVideoFillModeCover;
        return YES;
    }
    if (strcmp(value, "contain") == 0 || strcmp(value, "fit") == 0) {
        out = VRVideoFillModeContain;
        return YES;
    }
    if (strcmp(value, "stretch") == 0) {
        out = VRVideoFillModeStretch;
        return YES;
    }
    return NO;
}

static BOOL ParseArgs(int argc, char **argv, WallpaperArgs &out) {
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        auto take = [&](int &i, const char *opt) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            PrintUsage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--screen") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.screen = atoi(v);
        } else if (strcmp(arg, "--display-id") == 0) {
            const char *v = take(i, arg); if (!v) return NO;
            char *end = nullptr;
            unsigned long value = strtoul(v, &end, 10);
            if (end == v || *end != '\0' || value == 0 || value > UINT32_MAX) return NO;
            out.displayID = (CGDirectDisplayID)value;
        } else if (strcmp(arg, "--volume") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.volume = strtof(v, nullptr);
        } else if (strcmp(arg, "--muted") == 0) {
            out.muted = YES;
        } else if (strcmp(arg, "--fill") == 0) {
            const char *v = take(i, arg); if (!v || !ParseFillMode(v, out.fillMode)) return NO;
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.runSeconds = atoi(v);
        } else if (strcmp(arg, "--control-stdin") == 0) {
            out.controlStdin = YES;
        } else if (strcmp(arg, "--deferred-show") == 0) {
            out.deferredShow = YES;
        } else if (strcmp(arg, "--load-from-memory") == 0) {
            out.loadFromMemory = YES;
        } else if (arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg);
            return NO;
        } else {
            if (out.workshop == nullptr) out.workshop = arg;
            else {
                fprintf(stderr, "unexpected positional argument: %s\n", arg);
                return NO;
            }
        }
    }
    if (out.workshop == nullptr) {
        PrintUsage(argv[0]);
        return NO;
    }
    if (out.screen < 0) out.screen = 0;
    if (out.volume < 0.0f) out.volume = 0.0f;
    if (out.volume > 1.0f) out.volume = 1.0f;
    if (out.runSeconds < 0) out.runSeconds = 0;
    return YES;
}

@interface VideoWallpaperWindow : NSWindow
@end

@implementation VideoWallpaperWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen *)screen { return frameRect; }
@end

@interface VideoWallpaperAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) VRVideoRendererEngine *engine;
@property (nonatomic, strong) MirageControlChannel *control;
@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, assign) BOOL deferredShow;
@property (nonatomic, assign) BOOL prepared;
@property (nonatomic, assign) NSUInteger visibilityEpoch;
- (void)observeScreenParameterChanges;
- (void)contentDidBecomeReady;
- (void)activateWindow;
- (void)deactivateWindow;
- (void)prepareActivationForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts;
- (void)beginActivationFailureForEpoch:(NSUInteger)epoch;
- (void)confirmActivationFailureForEpoch:(NSUInteger)epoch attempts:(NSInteger)attempts;
@end

@implementation VideoWallpaperAppDelegate

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

static BOOL MirageSizeMatches(NSSize lhs, NSSize rhs) {
    return MirageNearlyEqual(lhs.width, rhs.width) &&
           MirageNearlyEqual(lhs.height, rhs.height);
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
    CGRect bounds = CGRectZero;
    BOOL onscreen = NO;
    double alpha = 0.0;
    return MirageNSRectMatches(self.window.frame, screen.frame) &&
           MirageSizeMatches(content.bounds.size, screen.frame.size) &&
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
        MirageEmitEvent(@{ @"event": visible ? @"activated" : @"deactivated" });
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
    [self.engine pause];
    self.window.alphaValue = 0.0;
    [self.window orderOut:nil];
    [self confirmActivationFailureForEpoch:epoch attempts:200];
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
    [self.engine pause];
    self.window.alphaValue = 0.0;
    [self.window orderOut:nil];
    [self confirmVisible:NO epoch:epoch attempts:200];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self.engine pause];
}

// The window frame was captured once at launch, so a resolution change,
// display rearrangement or unplug/replug left it stale.
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
    if (NSEqualRects(self.window.frame, frame)) return;
    fprintf(stderr, "VideoWallpaper: screen parameters changed; resizing to %.0fx%.0f at (%.0f,%.0f)\n",
            NSWidth(frame), NSHeight(frame), NSMinX(frame), NSMinY(frame));
    [self.window setFrame:frame display:YES];
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

        NSError *manifestError = nil;
        VRVideoManifest *manifest = [VRVideoManifest loadFromDirectory:@(args.workshop)
                                                                  error:&manifestError];
        if (manifest == nil) {
            fprintf(stderr, "VideoWallpaper: %s\n",
                    manifestError.localizedDescription.UTF8String ?: "failed to load project.json");
            return 2;
        }

        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        VideoWallpaperAppDelegate *delegate = [VideoWallpaperAppDelegate new];
        app.delegate = delegate;
        [app finishLaunching];

        NSArray<NSScreen *> *screens = NSScreen.screens;
        NSScreen *screen = args.displayID != 0
            ? MirageScreenForDisplayID(args.displayID)
            : ((args.screen >= 0 && args.screen < (int)screens.count) ? screens[args.screen] : nil);
        if (screen == nil) {
            fprintf(stderr, "VideoWallpaper: no screen available\n");
            return 1;
        }
        NSNumber *screenNumber = screen.deviceDescription[@"NSScreenNumber"];
        if (screenNumber == nil) return 1;
        CGDirectDisplayID displayID = screenNumber.unsignedIntValue;

        VRVideoEngineConfig config = [VRVideoRendererEngine defaultConfig];
        config.fillMode = args.fillMode;
        config.initialVolume = args.volume;
        // A deferred candidate must decode while remaining inaudible. Mirage
        // replays the user's actual mute/volume policy only after activation.
        config.muted = args.deferredShow ? YES : args.muted;
        config.autoplay = YES;
        config.loadFromMemory = args.loadFromMemory;

        NSRect screenFrame = screen.frame;
        NSRect contentFrame = NSMakeRect(0, 0, NSWidth(screenFrame), NSHeight(screenFrame));
        VRVideoRendererEngine *engine = [[VRVideoRendererEngine alloc] initWithFrame:contentFrame
                                                                              config:config];
        // Queueing succeeds for undecodable files, so failures are reported
        // asynchronously; install the handlers before opening the wallpaper.
        engine.videoDidEndBlock = ^{
            MirageEmitEvent(@{ @"event": @"video-did-end" });
        };
        engine.videoDidFailBlock = ^(NSString *message) {
            MirageEmitEvent(@{ @"event": @"video-error",
                               @"message": message.length > 0 ? message : @"unknown error" });
        };
        engine.videoTranscodeProgressBlock = ^(double fraction, BOOL done) {
            MirageEmitEvent(@{ @"event": @"video-transcoding",
                               @"progress": @(fraction),
                               @"done": @(done) });
        };
        delegate.engine = engine;
        delegate.displayID = displayID;
        delegate.deferredShow = args.deferredShow;
        __weak VideoWallpaperAppDelegate *weakDelegate = delegate;
        engine.firstFrameReadyBlock = ^{
            [weakDelegate contentDidBecomeReady];
        };

        VideoWallpaperWindow *window = [[VideoWallpaperWindow alloc]
            initWithContentRect:screenFrame
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        window.title = manifest.title.length ? manifest.title : @"VideoWallpaper";
        window.level = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) - 1;
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorIgnoresCycle;
        window.opaque = YES;
        window.backgroundColor = NSColor.blackColor;
        window.alphaValue = args.deferredShow ? 0.0 : 1.0;
        window.hasShadow = NO;
        window.ignoresMouseEvents = YES;
        window.acceptsMouseMovedEvents = NO;
        window.releasedWhenClosed = NO;
        window.canHide = NO;
        window.contentView = engine;
        delegate.window = window;

        // Install every lifecycle callback and the window target before
        // playback starts. A very short local video can otherwise yield its
        // first decoded frame before the delegate is able to validate it.
        NSError *openError = nil;
        if (![engine openWallpaper:manifest error:&openError]) {
            fprintf(stderr, "VideoWallpaper: %s\n",
                    openError.localizedDescription.UTF8String ?: "failed to open video");
            return 3;
        }
        [window orderFrontRegardless];
        [delegate observeScreenParameterChanges];

        // Live control channel: Mirage.app pipes JSON commands on stdin.
        if (args.controlStdin) {
            VRVideoRendererEngine *eng = engine;
            delegate.control = [[MirageControlChannel alloc]
                initWithHandler:^(NSDictionary *cmd) {
                    NSString *name = cmd[@"cmd"];
                    id value = cmd[@"value"];
                    if ([name isEqualToString:@"activate"]) {
                        [delegate activateWindow];
                    } else if ([name isEqualToString:@"deactivate"]) {
                        [delegate deactivateWindow];
                    } else if ([name isEqualToString:@"pause"]) {
                        [eng pause];
                    } else if ([name isEqualToString:@"resume"] || [name isEqualToString:@"play"]) {
                        [eng play];
                    } else if ([name isEqualToString:@"speed"]) {
                        if ([value isKindOfClass:[NSNumber class]]) {
                            [eng setPlaybackRate:[value floatValue]];
                        }
                    } else if ([name isEqualToString:@"power"]) {
                        NSString *state = [cmd[@"state"] isKindOfClass:[NSString class]]
                            ? cmd[@"state"] : nil;
                        if ([state isEqualToString:@"pause"]) {
                            [eng pause];
                        } else if ([state isEqualToString:@"run"]
                                   || [state isEqualToString:@"throttle"]) {
                            [eng play];
                        }
                    } else if ([name isEqualToString:@"volume"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setVolume:[value floatValue]];
                    } else if ([name isEqualToString:@"muted"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setMuted:[value boolValue]];
                    } else if ([name isEqualToString:@"fillmode"]) {
                        if ([value isKindOfClass:[NSString class]]) {
                            VRVideoFillMode mode;
                            if (ParseFillMode([value UTF8String], mode)) [eng setFillMode:mode];
                        }
                    } else if ([name isEqualToString:@"snapshot"]) {
                        // Mirage.app wants a still of the live frame for the
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
                    // setProperty: video wallpapers have no live shader props; ignored.
                }
                onEOF:^{
                    [NSApp terminate:nil];
                }];
            [delegate.control start];
        }

        if (args.runSeconds > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)args.runSeconds * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                             [NSApp terminate:nil];
                           });
        }

        [app run];
    }
    return 0;
}
