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
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct WallpaperArgs {
    const char *workshop = nullptr;
    int screen = 0;
    CGDirectDisplayID displayID = 0;
    float volume = 1.0f;
    BOOL muted = NO;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
    double positionX = 0.5;
    double positionY = 0.5;
    BOOL controlStdin = NO;
    BOOL deferredShow = NO;
    BOOL loadFromMemory = NO;
    BOOL hdrEnabled = NO;
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
        "  --position-x N         horizontal crop position 0..1 (default 0.5)\n"
        "  --position-y N         vertical crop position 0..1 (default 0.5)\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
        "  --deferred-show        keep the window hidden until an activate command\n"
        "  --load-from-memory     keep the video bytes in memory\n"
        "  --hdr                  enable HDR output on EDR-capable displays\n"
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
        } else if (strcmp(arg, "--position-x") == 0 || strcmp(arg, "--position-y") == 0) {
            const char *v = take(i, arg); if (!v) return NO;
            char *end = nullptr;
            double value = strtod(v, &end);
            if (end == v || *end != '\0' || !std::isfinite(value)) return NO;
            (strcmp(arg, "--position-x") == 0 ? out.positionX : out.positionY) = fmin(fmax(value, 0.0), 1.0);
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.runSeconds = atoi(v);
        } else if (strcmp(arg, "--control-stdin") == 0) {
            out.controlStdin = YES;
        } else if (strcmp(arg, "--deferred-show") == 0) {
            out.deferredShow = YES;
        } else if (strcmp(arg, "--load-from-memory") == 0) {
            out.loadFromMemory = YES;
        } else if (strcmp(arg, "--hdr") == 0) {
            out.hdrEnabled = YES;
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
    [self.engine updateDynamicRangeForScreen:screen];
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
    [self.engine updateDynamicRangeForScreen:screen];
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

#pragma mark - Cross-display playback sync

// Every display runs its own VideoWallpaper process with its own AVPlayer, so
// the only thing keeping their playheads together is a shared anchor. The
// parent seeds that anchor from a renderer's real playhead and sends it out,
// and each process maps it to a media position on its own.
//
// CLOCK_MONOTONIC_RAW instead of CFAbsoluteTimeGetCurrent: it reads the same in
// every process, keeps counting through sleep, and an NTP step cannot move
// every renderer's target at once.
static double MirageSyncNow(void) {
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9;
}

// The sync trace goes to a file, not to stderr. stderr is a pipe the parent
// only reads for fatal startup errors, so a trace written there is lost exactly
// while a wallpaper is playing. One file per display, truncated when it grows
// past a megabyte.
static char sSyncLogPath[PATH_MAX] = {0};

static void MirageSyncLogOpen(CGDirectDisplayID displayID, int screen) {
    snprintf(sSyncLogPath, sizeof(sSyncLogPath), "/tmp/mirage-sync-%u-%d.log",
             (unsigned)displayID, screen);
    FILE *probe = fopen(sSyncLogPath, "r");
    if (probe != NULL) {
        long size = fseek(probe, 0, SEEK_END) == 0 ? ftell(probe) : 0;
        fclose(probe);
        if (size > 1024 * 1024) {
            FILE *truncate = fopen(sSyncLogPath, "w");
            if (truncate != NULL) fclose(truncate);
        }
    }
}

static void MirageSyncLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void MirageSyncLog(const char *format, ...) {
    if (sSyncLogPath[0] == '\0') return;
    FILE *fp = fopen(sSyncLogPath, "a");
    if (fp == NULL) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(fp, "%.3f ", (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);
    fputc('\n', fp);
    fclose(fp);
}

// Tolerance and cooldown.
//
// The tolerance decides whether a tick touches the player at all, so it has to
// sit above one-frame sampling noise and below what looks out of step. 150 ms
// was too coarse: it is where two screens *start* to look wrong, so every
// settled state was allowed to be visibly late. The eye compares motion, and
// two frames of offset already reads as an echo.
//
// 65 ms is about two frames at 30 fps. The seek tolerance below is a frame, so
// chasing anything finer is pointless.
static const double kMirageSyncHold = 0.065;
// Wait this long after a correction before measuring again. A seek needs time
// to land, and measuring early reads the pre-seek playhead and stacks a second
// seek on top of the first.
static const double kMirageSyncLandingCooldown = 0.900;

// How a correction is applied: a rate trim, not a seek.
//
// A tolerant seek costs about 450 ms of playback here. The decoder flushes and
// re-primes, the picture sits still, and the screen ends up further behind than
// the offset it was meant to remove, so each seek made the next measurement
// worse and the loop fed itself until the budget cut it off.
//
// A trim costs one dropped frame and moves the picture smoothly instead. At
// 1.03x a 120 ms offset is gone in four seconds and nobody watching a wallpaper
// can tell.
//
// The gain keeps the close time the same for any offset: rate - 1 is
// proportional to the offset, so it shrinks geometrically in 1/gain seconds.
static const double kMirageTrimGain = 0.25;   // ~4 s to close, any offset
static const double kMirageTrimMax = 0.040;   // never more than ±4%
// Stop trimming below this. Under kMirageSyncHold, so a trim that starts at the
// tolerance does not oscillate around it.
static const double kMirageTrimStop = 0.030;
// How often a running trim is resized. Each resize assigns `rate`, and
// assigning on every tick is what made the corrected screen stutter: it forces
// AVFoundation to recompute playback timing, which costs a frame.
static const double kMirageTrimReeval = 2.000;
// Smallest change worth an assignment.
static const double kMirageTrimQuantum = 0.005;

static BOOL sTrimActive = NO;
static double sTrimScale = 1.0;
static double sTrimStartedAt = -1.0e9;

// Offset big enough that a seek is worth its 450 ms. Only a screen that really
// stopped gets here: a stall or a bad landing, not drift.
static const double kMirageHardSeekDrift = 0.250;

// One sample over tolerance is not a decision. The playhead reads a frame low
// across a loop swap and right after a seek, and acting on a single sample like
// that turns it into a visible seek.
static NSInteger sDriftStreak = 0;
static NSInteger sDriftStreakSign = 0;

// Set while the engine has not accepted the join yet (it refuses seeks until
// the first frame is decoded), so the next tick retries.
static BOOL sJoinPending = NO;
// True while the player is paused. Nothing can be corrected in this state: the
// engine refuses seeks and drops rate writes. Measuring drift here just spends
// the seek budget on seeks that never happen, and once the budget is gone this
// screen has none left exactly when it resumes. Corrections are held instead,
// and the first sync after the pause lifts lands like a join.
static BOOL sSyncPaused = NO;
// Set when the pause lifts: the next sync is a join, not a chase.
static BOOL sResumeJoin = NO;
// Previous tick's reference position, used to spot the reference crossing its
// own loop seam.
static double sLastSyncTarget = -1.0;
// Corrections stay off until this instant after a seam.
//
// Right after a seam the local player is not running at 1.0x: AVPlayerLooper is
// wiring in a fresh copy and its decoder is priming, so over the next second the
// playhead advances a little slower than real time. Measuring inside that window
// reads the slowdown as drift. A seek used to turn that into a jump; a trim
// turns it into a screen that speeds up to compensate for a slowdown that has
// already ended, which is just as wrong and lasts longer. The slowdown is
// transient and self-correcting, so the only useful response is to wait it out.
static double sSeamGraceUntil = -1.0e9;
static const double kMirageSeamGrace = 1.200;

// Playhead seen on the previous tick, used to notice a loop restart. One
// process drives one wallpaper window, so a file-scope value is enough.
static double sLastSyncPosition = -1.0;
// Monotonic time of the last seek. A seek needs time to land, and measuring too
// early reads the pre-seek playhead and stacks another seek on it.
static double sLastSyncLandingTime = -1.0e9;
// Seek budget. Only seeks count, because they are the corrections that cost
// something visible.
static double sSyncCorrectionWindowStart = -1.0e9;
static NSInteger sSyncCorrectionCount = 0;
static double sSyncSuspendUntil = -1.0e9;
static const double kMirageSyncBudgetWindow = 30.0;
static const NSInteger kMirageSyncBudgetCount = 8;
static const double kMirageSyncSuspendDuration = 45.0;

// Distance from `position` to `target` along the shorter arc of the loop.
// Positive means this renderer is behind and has to speed up.
static double MirageSyncDelta(double target, double position, double total) {
    double delta = target - position;
    if (total > 0.0) {
        double half = total * 0.5;
        if (delta > half) delta -= total;
        else if (delta < -half) delta += total;
    }
    return delta;
}

// Landing point for a seek, kept away from the seam.
//
// A zero-tolerance seek to exactly 0 (or exactly the duration) on a freshly
// enqueued loop copy can land on an item that never presents another frame: the
// picture parks on the first frame and the clock stops. Staying 100 ms inside
// the clip avoids that and nobody notices the difference.
static double MirageSyncLanding(double target, double total) {
    if (!(total > 0.4)) return target;
    if (target < 0.10) return 0.10;
    if (target > total - 0.10) return total - 0.10;
    return target;
}

// Aligns `eng` to the shared anchor.
//
// `trimAllowed` is NO on the renderer that owns audio. Changing its rate would
// shift pitch, so the muted screens do the chasing.
//
// `join` is YES on the first message a renderer gets after launching. It has
// been playing from zero since it started, so it has to land on the anchor
// right away; otherwise the next tick reads its stray playhead and re-anchors
// everyone onto it.
static void MirageSyncApply(VRVideoRendererEngine *eng, double anchor,
                            double hold, BOOL trimAllowed, BOOL join) {
    double total = [eng currentPlaybackDuration];
    if (!isfinite(total) || total <= 0.0) return;
    double elapsed = MirageSyncNow() - anchor;
    if (!(elapsed >= 0.0)) return;
    double target = fmod(elapsed, total);

    // Log the anchor itself every few ticks. `target` alone cannot distinguish
    // "the parent moved the anchor" from "this player fell behind", since both
    // look like target drifting away from the playhead. anchor = now - target,
    // so logging the pair shows which one moved.
    {
        static NSInteger sAnchorTick = 0;
        if (sAnchorTick++ % 4 == 0) {
            MirageSyncLog("anchor=%.3f now=%.3f target=%.3f role=%s",
                          anchor, MirageSyncNow(), target,
                          trimAllowed ? "follow" : "ref");
        }
    }

    // The join usually arrives while the wallpaper is still opening, and the
    // engine refuses a seek before the first frame is decoded. Dropping it would
    // leave this renderer playing from zero, half a second off the anchor, so
    // keep retrying until the engine takes one.
    if (sJoinPending) join = YES;

    // A paused screen is a stopped clock, not a late one. Nothing can be
    // corrected while it is stopped, and measuring against a frozen playhead
    // only spends the seek budget on seeks the engine refuses. Hold everything
    // and land on the anchor once it plays again.
    if ([eng paused]) {
        if (!sSyncPaused) {
            sSyncPaused = YES;
            MirageSyncLog("paused: holding sync (target %.3f)", target);
        }
        // The anchor keeps moving while this screen is stopped, so whatever the
        // loop-restart detector last saw means nothing by the time playback
        // resumes.
        sLastSyncTarget = target;
        sResumeJoin = YES;
        return;
    }
    if (sSyncPaused) {
        sSyncPaused = NO;
        MirageSyncLog("resumed: landing on target %.3f", target);
    }
    // Resuming is the one case where this screen was stopped while the
    // reference kept playing, so it is behind by the length of the pause.
    // Landing on the anchor beats asking a trim to close seconds at 4 %.
    if (sResumeJoin) {
        sResumeJoin = NO;
        join = YES;
    }

    if (join) {
        // No trim here. This is not "catch up", it is "land where the reference
        // is", and the player has only just started.
        double landing = MirageSyncLanding(target, total);
        BOOL issued = [eng seekApproximatelyTo:landing];
        sJoinPending = !issued;
        sLastSyncPosition = issued ? landing : -1.0;
        sLastSyncTarget = target;
        sSeamGraceUntil = -1.0e9;
        sDriftStreak = 0;
        sDriftStreakSign = 0;
        sTrimActive = NO;
        sTrimScale = 1.0;
        MirageSyncLog("join seek target=%.3f landing=%.3f %s total=%.3f",
                      target, landing, issued ? "ok" : "deferred", total);
        return;
    }

    // The screen that owns audio defines the reference, so it is never
    // corrected: a seek would break the soundtrack every other screen follows,
    // and it is already on time by definition.
    //
    // It still logs what it is playing every few ticks. Without a line from the
    // reference on the same clock, a follower's trace cannot tell "the follower
    // is late" from "the reference is early" or from "the anchor is wrong".
    if (!trimAllowed) {
        // This screen defines the timeline, so it has to run at exactly the
        // configured rate. A screen that was just promoted (the audio owner got
        // paused, this one kept playing) may still carry the catch-up trim it
        // had as a follower, and nothing corrects the reference, so that offset
        // would stick.
        [eng setSyncRateScale:1.0];
        static NSInteger sRefTick = 0;
        if (sRefTick++ % 4 == 0) {
            double refPosition = [eng currentPlaybackPosition];
            if (isfinite(refPosition) && refPosition >= 0.0) {
                MirageSyncLog("ref position=%.3f target=%.3f lag=%+.3f",
                              refPosition, target, refPosition - target);
            }
        }
        return;
    }

    // `target` stepping backwards means the reference screen just took a loop
    // seam. Its item swap costs it a slice of time and the parent only sees that
    // on its next measurement, so this follower is briefly ahead by that slice.
    // Nothing to do here: the seam window below keeps this screen from reacting
    // to its own priming, and the anchor catches up meanwhile.
    if (sLastSyncTarget >= 0.0 && target + total * 0.5 < sLastSyncTarget) {
        sSeamGraceUntil = MirageSyncNow() + kMirageSeamGrace;
        MirageSyncLog("reference crossed seam, target %.3f (was %.3f), quiet for %.1fs",
                      target, sLastSyncTarget, kMirageSeamGrace);
    }
    sLastSyncTarget = target;

    // Re-apply whatever trim is running. `rate` cannot be set while the player
    // is swapping items or waiting, and a trim set in that window is dropped
    // without a word and never reissued.
    [eng applySyncRateIfNeeded];

    double position = [eng currentPlaybackPosition];
    if (!isfinite(position) || position < 0.0) {
        // Read across a loop restart, where the playhead is momentarily
        // unreadable. Touching the player here is what parked the built-in
        // screen on the first frame of the new loop. Wait for the next tick.
        return;
    }

    // The playhead just wrapped, so AVPlayerLooper is swapping in a new copy
    // right now. Anything corrected here lands on an item that is still being
    // wired up.
    if (sLastSyncPosition >= 0.0 && position + 0.5 < sLastSyncPosition) {
        sLastSyncPosition = position;
        return;
    }
    sLastSyncPosition = position;

    // Quiet window after a seam. The playhead is still read above, so the
    // loop-restart detector stays honest, but nothing is corrected while the new
    // copy primes.
    double now = MirageSyncNow();
    if (now < sSeamGraceUntil) return;

    double delta = MirageSyncDelta(target, position, total);
    double drift = fabs(delta);

    // Below the stop band the trim has done its job: put the rate back.
    //
    // Checked before the tolerance, because a trim that started at the tolerance
    // has to run down past it instead of freezing halfway.
    if (drift <= kMirageTrimStop) {
        if (sTrimActive) {
            [eng setSyncRateScale:1.0];
            MirageSyncLog("trim done, drift %+.3f (scale %.3f->1.000)", delta, sTrimScale);
            sTrimActive = NO;
            sTrimScale = 1.0;
        } else if (sDriftStreak != 0) {
            MirageSyncLog("drift %+.3f back inside hold %.3f", delta, hold);
        }
        sDriftStreak = 0;
        sDriftStreakSign = 0;
        return;
    }

    // Inside tolerance and not already trimming: leave the player alone. No rate
    // assignment, no seek. The assignment on every tick is what used to drop a
    // frame here.
    if (!sTrimActive && drift <= hold) {
        sDriftStreak = 0;
        sDriftStreakSign = 0;
        return;
    }

    // One sample is not a decision: the playhead reads a frame low right after a
    // loop swap and right after a seek. Want two ticks in the same direction, or
    // an error too big to be sampling noise.
    BOOL hardSeek = drift > kMirageHardSeekDrift;
    if (!hardSeek && !sTrimActive) {
        NSInteger sign = delta > 0.0 ? 1 : -1;
        if (sign != sDriftStreakSign) {
            sDriftStreakSign = sign;
            sDriftStreak = 1;
            MirageSyncLog("drift %+.3f (single sample, %s) target=%.3f position=%.3f",
                          delta, sign > 0 ? "behind" : "ahead", target, position);
            return;
        }
        sDriftStreak += 1;
    }

    if (hardSeek) {
        // Only for an offset a trim should not be asked to close: a screen that
        // stopped while the reference kept playing. Budgeted, because a seek
        // costs about 450 ms, and if it keeps being needed the two screens are
        // not on the same timeline: chasing it only trades a wrong picture for
        // a stuttering one.
        if (now < sSyncSuspendUntil) return;

        if (now - sSyncCorrectionWindowStart > kMirageSyncBudgetWindow) {
            sSyncCorrectionWindowStart = now;
            sSyncCorrectionCount = 0;
        }
        if (sSyncCorrectionCount >= kMirageSyncBudgetCount) {
            sSyncSuspendUntil = now + kMirageSyncSuspendDuration;
            sSyncCorrectionCount = 0;
            MirageSyncLog("suspend hard seeks for %.0fs (drift %+.3f)",
                          kMirageSyncSuspendDuration, delta);
            return;
        }

        // Skip while a previous seek is still settling: the playhead measured
        // this tick may be the pre-seek one.
        if (now - sLastSyncLandingTime < kMirageSyncLandingCooldown) return;

        sLastSyncLandingTime = now;
        sSyncCorrectionCount += 1;
        sDriftStreak = 0;
        sDriftStreakSign = 0;
        sTrimActive = NO;
        sTrimScale = 1.0;

        double landing = MirageSyncLanding(target, total);
        MirageSyncLog("seek drift=%+.3f target=%.3f landing=%.3f", delta, target, landing);
        [eng seekApproximatelyTo:landing];
        // The playhead is about to move, so the loop-restart detector must
        // compare against where it is going, not where it was.
        sLastSyncPosition = landing;
        MirageEmitEvent(@{ @"event": @"sync-applied",
                           @"target": @(target),
                           @"drift": @(delta) });
        return;
    }

    // Steady state: hold a small rate offset until the drift is gone.
    //
    // Sized once from the offset (time to close is 1/gain either way) and only
    // re-checked every couple of seconds, because every assignment costs about a
    // frame and this path is meant to be invisible.
    if (!sTrimActive || now - sTrimStartedAt >= kMirageTrimReeval) {
        double scale = 1.0 + delta * kMirageTrimGain;
        if (scale > 1.0 + kMirageTrimMax) scale = 1.0 + kMirageTrimMax;
        if (scale < 1.0 - kMirageTrimMax) scale = 1.0 - kMirageTrimMax;
        // Quantised, so a slowly shrinking offset cannot produce an assignment
        // on every tick: anything under a quantum is the same trim.
        scale = round(scale / kMirageTrimQuantum) * kMirageTrimQuantum;
        if (!sTrimActive || fabs(scale - sTrimScale) >= kMirageTrimQuantum * 0.5) {
            double previous = sTrimScale;
            [eng setSyncRateScale:scale];
            sTrimScale = scale;
            sTrimStartedAt = now;
            sTrimActive = YES;
            sDriftStreak = 0;
            sDriftStreakSign = 0;
            MirageSyncLog("trim drift=%+.3f scale %.3f->%.3f target=%.3f position=%.3f",
                          delta, previous, scale, target, position);
        }
    }
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        WallpaperArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

        MirageSyncLogOpen(args.displayID, args.screen);
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
        config.positionX = args.positionX;
        config.positionY = args.positionY;
        config.initialVolume = args.volume;
        // A deferred candidate must decode while remaining inaudible. Mirage
        // replays the user's actual mute/volume policy only after activation.
        config.muted = args.deferredShow ? YES : args.muted;
        config.autoplay = YES;
        config.loadFromMemory = args.loadFromMemory;
        config.hdrEnabled = args.hdrEnabled;

        NSRect screenFrame = screen.frame;
        NSRect contentFrame = NSMakeRect(0, 0, NSWidth(screenFrame), NSHeight(screenFrame));
        VRVideoRendererEngine *engine = [[VRVideoRendererEngine alloc] initWithFrame:contentFrame
                                                                              config:config];
        // Queueing succeeds for undecodable files, so failures are reported
        // asynchronously; install the handlers before opening the wallpaper.
        engine.videoDidEndBlock = ^{
            MirageEmitEvent(@{ @"event": @"video-did-end" });
        };
        engine.positionAvailabilityBlock = ^(BOOL x, BOOL y) {
            MirageEmitEvent(@{ @"event": @"position-availability", @"x": @(x), @"y": @(y) });
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
        [engine updateDynamicRangeForScreen:screen];
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
                    } else if ([name isEqualToString:@"position"]) {
                        NSNumber *x = cmd[@"x"];
                        NSNumber *y = cmd[@"y"];
                        if ([x isKindOfClass:NSNumber.class] && [y isKindOfClass:NSNumber.class]) {
                            [eng setPositionX:x.doubleValue y:y.doubleValue];
                        }
                    } else if ([name isEqualToString:@"fillmode"]) {
                        if ([value isKindOfClass:[NSString class]]) {
                            VRVideoFillMode mode;
                            if (ParseFillMode([value UTF8String], mode)) [eng setFillMode:mode];
                        }
                    } else if ([name isEqualToString:@"seek"]) {
                        NSNumber *t = cmd[@"t"];
                        if ([t isKindOfClass:[NSNumber class]]) [eng seekToTime:t.doubleValue];
                    } else if ([name isEqualToString:@"playbackTime"]) {
                        // Report the playhead so the parent can build the anchor
                        // from real playback instead of from the moment the
                        // window appeared. The duration goes along because the
                        // parent needs it to fold both onto the same loop.
                        double position = CMTimeGetSeconds(eng.player.currentTime);
                        double duration = [eng currentPlaybackDuration];
                        if (isfinite(position) && position >= 0.0) {
                            MirageEmitEvent(@{ @"event": @"playbackTime",
                                               @"t": @(position),
                                               @"d": isfinite(duration) ? @(duration) : @0,
                                               // A paused screen reports a frozen
                                               // playhead. The parent must not take
                                               // that as the clock, so it hears
                                               // about the pause from here.
                                               @"paused": @([eng paused]) });
                        }
                    } else if ([name isEqualToString:@"sync"]) {
                        // `anchor` is the monotonic time playback started,
                        // `hold` the tolerance to keep, `trim` NO on the
                        // renderer that owns audio.
                        NSNumber *anchor = cmd[@"anchor"];
                        NSNumber *hold = cmd[@"hold"];
                        NSNumber *trim = cmd[@"trim"];
                        NSNumber *join = cmd[@"join"];
                        if ([anchor isKindOfClass:[NSNumber class]]) {
                            MirageSyncApply(eng,
                                            anchor.doubleValue,
                                            [hold isKindOfClass:[NSNumber class]]
                                                ? hold.doubleValue : kMirageSyncHold,
                                            [trim isKindOfClass:[NSNumber class]]
                                                ? trim.boolValue : YES,
                                            [join isKindOfClass:[NSNumber class]]
                                                ? join.boolValue : NO);
                        }
                    } else if ([name isEqualToString:@"hdr"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setHDREnabled:[value boolValue]];
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
