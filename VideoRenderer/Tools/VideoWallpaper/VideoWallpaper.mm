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

#include <cstdlib>
#include <cstring>

struct WallpaperArgs {
    const char *workshop = nullptr;
    int screen = 0;
    float volume = 1.0f;
    BOOL muted = NO;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
    BOOL controlStdin = NO;
    BOOL loadFromMemory = NO;
};

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <wallpaper-dir> [options]\n\n"
        "Options:\n"
        "  --screen N             screen index to cover (default 0 = main)\n"
        "  --volume 0..1          audio volume (default 1.0)\n"
        "  --muted                start muted\n"
        "  --fill MODE            cover | contain | stretch (default cover)\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
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
@end

@interface VideoWallpaperAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) VRVideoRendererEngine *engine;
@property (nonatomic, strong) MirageControlChannel *control;
@end

@implementation VideoWallpaperAppDelegate
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self.engine pause];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        WallpaperArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

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
        NSScreen *screen = (args.screen < (int)screens.count) ? screens[args.screen] : NSScreen.mainScreen;
        if (screen == nil) screen = NSScreen.mainScreen;
        if (screen == nil) {
            fprintf(stderr, "VideoWallpaper: no screen available\n");
            return 1;
        }

        VRVideoEngineConfig config = [VRVideoRendererEngine defaultConfig];
        config.fillMode = args.fillMode;
        config.initialVolume = args.volume;
        config.muted = args.muted;
        config.autoplay = YES;
        config.loadFromMemory = args.loadFromMemory;

        NSRect screenFrame = screen.frame;
        VRVideoRendererEngine *engine = [[VRVideoRendererEngine alloc] initWithFrame:screenFrame
                                                                              config:config];
        NSError *openError = nil;
        if (![engine openWallpaper:manifest error:&openError]) {
            fprintf(stderr, "VideoWallpaper: %s\n",
                    openError.localizedDescription.UTF8String ?: "failed to open video");
            return 3;
        }
        delegate.engine = engine;

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
        window.hasShadow = NO;
        window.ignoresMouseEvents = YES;
        window.acceptsMouseMovedEvents = NO;
        window.releasedWhenClosed = NO;
        window.canHide = NO;
        window.contentView = engine;
        [window orderFrontRegardless];
        delegate.window = window;

        // Live control channel: Mirage.app pipes JSON commands on stdin.
        if (args.controlStdin) {
            VRVideoRendererEngine *eng = engine;
            delegate.control = [[MirageControlChannel alloc]
                initWithHandler:^(NSDictionary *cmd) {
                    NSString *name = cmd[@"cmd"];
                    id value = cmd[@"value"];
                    if ([name isEqualToString:@"pause"]) {
                        [eng pause];
                    } else if ([name isEqualToString:@"resume"] || [name isEqualToString:@"play"]) {
                        [eng play];
                    } else if ([name isEqualToString:@"volume"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setVolume:[value floatValue]];
                    } else if ([name isEqualToString:@"muted"]) {
                        if ([value isKindOfClass:[NSNumber class]]) [eng setMuted:[value boolValue]];
                    } else if ([name isEqualToString:@"fillmode"]) {
                        if ([value isKindOfClass:[NSString class]]) {
                            VRVideoFillMode mode;
                            if (ParseFillMode([value UTF8String], mode)) [eng setFillMode:mode];
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
