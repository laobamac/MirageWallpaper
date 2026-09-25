// VideoViewer — standalone debug window for Wallpaper Engine video wallpapers.
//
// Usage:
//   VideoViewer <wallpaper-dir> [--width N] [--height N] [--volume 0..1]
//               [--muted] [--fill cover|contain|stretch] [--run-seconds N]

#import <AppKit/AppKit.h>

#import "VideoManifest.h"
#import "VideoRendererEngine.h"

#include <cstdlib>
#include <cstring>

struct ViewerArgs {
    const char *workshop = nullptr;
    int width = 1280;
    int height = 720;
    float volume = 1.0f;
    BOOL muted = NO;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
};

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <wallpaper-dir> [options]\n\n"
        "Options:\n"
        "  --width N              window width  (default 1280)\n"
        "  --height N             window height (default 720)\n"
        "  --volume 0..1          audio volume (default 1.0)\n"
        "  --muted                start muted\n"
        "  --fill MODE            cover | contain | stretch (default cover)\n"
        "  --run-seconds N        exit after N seconds (test helper)\n"
        "  -h, --help             show this help\n",
        argv0);
}

static BOOL ParseArgs(int argc, char **argv, ViewerArgs &out) {
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
        } else if (strcmp(arg, "--width") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.width = atoi(v);
        } else if (strcmp(arg, "--height") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.height = atoi(v);
        } else if (strcmp(arg, "--volume") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.volume = strtof(v, nullptr);
        } else if (strcmp(arg, "--muted") == 0) {
            out.muted = YES;
        } else if (strcmp(arg, "--fill") == 0) {
            const char *v = take(i, arg); if (!v || !VRParseVideoFillMode(v, out.fillMode)) return NO;
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char *v = take(i, arg); if (!v) return NO; out.runSeconds = atoi(v);
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
    if (out.width < 64) out.width = 64;
    if (out.height < 64) out.height = 64;
    if (out.volume < 0.0f) out.volume = 0.0f;
    if (out.volume > 1.0f) out.volume = 1.0f;
    if (out.runSeconds < 0) out.runSeconds = 0;
    return YES;
}

@interface VideoViewerAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) VRVideoRendererEngine *engine;
@end

@implementation VideoViewerAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self.engine pause];
}
@end

static void InstallMainMenu(NSString *appName) {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:appName action:NULL keyEquivalent:@""];
    [bar addItem:appItem];

    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %@", appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %@", appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Hide Others"
                       action:@selector(hideOtherApplications:)
                keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [NSApp setMainMenu:bar];
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        ViewerArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

        NSError *manifestError = nil;
        VRVideoManifest *manifest = [VRVideoManifest loadFromDirectory:@(args.workshop)
                                                                  error:&manifestError];
        if (manifest == nil) {
            fprintf(stderr, "VideoViewer: %s\n",
                    manifestError.localizedDescription.UTF8String ?: "failed to load project.json");
            return 2;
        }

        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        VideoViewerAppDelegate *delegate = [VideoViewerAppDelegate new];
        app.delegate = delegate;
        [app finishLaunching];
        InstallMainMenu(manifest.title.length ? manifest.title : @"VideoViewer");

        VRVideoEngineConfig config = [VRVideoRendererEngine defaultConfig];
        config.fillMode = args.fillMode;
        config.initialVolume = args.volume;
        config.muted = args.muted;
        config.autoplay = YES;

        NSRect frame = NSMakeRect(0, 0, args.width, args.height);
        VRVideoRendererEngine *engine = [[VRVideoRendererEngine alloc] initWithFrame:frame
                                                                              config:config];
        NSError *openError = nil;
        if (![engine openWallpaper:manifest error:&openError]) {
            fprintf(stderr, "VideoViewer: %s\n",
                    openError.localizedDescription.UTF8String ?: "failed to open video");
            return 3;
        }
        delegate.engine = engine;

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:style
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = manifest.title.length ? manifest.title : @"VideoViewer";
        window.backgroundColor = NSColor.blackColor;
        window.contentView = engine;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        delegate.window = window;

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
