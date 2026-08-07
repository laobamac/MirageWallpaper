// RmskinWallpaper — desktop host: one floating, draggable window per active
// Rainmeter skin config in the theme's layout.
//
// Usage:
//   RmskinWallpaper <theme-dir> [--load NAME] [--load-type TYPE]
//                   [--screen N] [--control-stdin] [--run-seconds N]

#import <AppKit/AppKit.h>
#import <CoreGraphics/CGWindowLevel.h>

#import "ControlChannel.h"
#import "RMLayout.h"
#import "RMSkin.h"
#import "RMSkinView.h"
#import "RMLog.h"

#include <string>
#include <vector>

struct HostArgs {
    const char *themeDir = nullptr;
    std::string load;
    std::string loadType;
    int  screen = 0;
    int  runSeconds = 0;
    BOOL controlStdin = NO;
};

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <theme-dir> [options]\n\n"
        "Options:\n"
        "  --load NAME            layout name / skin path (from RMSKIN.ini)\n"
        "  --load-type TYPE       Layout | Skin\n"
        "  --screen N             screen index (default 0 = main)\n"
        "  --control-stdin        accept live JSON control commands on stdin\n"
        "  --run-seconds N        exit after N seconds (test helper)\n"
        "  -h, --help             show this help\n",
        argv0);
}

static BOOL ParseArgs(int argc, char **argv, HostArgs &out) {
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        auto take = [&](int &i) -> const char * {
            if (i + 1 >= argc) return nullptr;
            return argv[++i];
        };
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) { PrintUsage(argv[0]); return NO; }
        else if (strcmp(arg, "--load") == 0)      { const char *v = take(i); if (!v) return NO; out.load = v; }
        else if (strcmp(arg, "--load-type") == 0) { const char *v = take(i); if (!v) return NO; out.loadType = v; }
        else if (strcmp(arg, "--screen") == 0)    { const char *v = take(i); if (!v) return NO; out.screen = atoi(v); }
        else if (strcmp(arg, "--run-seconds") == 0) { const char *v = take(i); if (!v) return NO; out.runSeconds = atoi(v); }
        else if (strcmp(arg, "--control-stdin") == 0) { out.controlStdin = YES; }
        else if (arg[0] == '-') { fprintf(stderr, "unknown option: %s\n", arg); return NO; }
        else { if (!out.themeDir) out.themeDir = arg; else { fprintf(stderr, "unexpected arg: %s\n", arg); return NO; } }
    }
    if (!out.themeDir) { PrintUsage(argv[0]); return NO; }
    return YES;
}

@interface RMWidgetWindow : NSWindow
@end
@implementation RMWidgetWindow
- (BOOL)canBecomeKeyWindow { return YES; }   // needed to receive drag/click
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface HostDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSMutableArray<NSWindow *> *windows;
@property (nonatomic, strong) NSMutableArray<RMSkinView *> *views;
@property (nonatomic, strong) MirageControlChannel *control;
@end
@implementation HostDelegate
@end

static void BuildWindows(HostDelegate *delegate, RMLayout *layout, int screenIndex) {
    NSArray<NSScreen *> *screens = NSScreen.screens;
    NSScreen *screen = (screenIndex < (int)screens.count) ? screens[screenIndex] : NSScreen.mainScreen;
    if (screen == nil) screen = NSScreen.mainScreen;
    NSRect sf = screen.frame;

    // Cascading offset for widgets whose layout positions could not be
    // resolved (Rainmeter formulas with #WORKAREAWIDTH# etc.). Place them
    // in the top-right area of the screen, stacking downward with spacing.
    int cascadeIdx = 0;

    for (RMLayoutEntry *entry in [layout activeEntries]) {
        RMSkin *skin = [[RMSkin alloc] initWithSkinFile:entry.skinFile
                                              resources:entry.resourcesPath
                                             rootConfig:entry.rootConfigPath
                                              skinsPath:layout.skinsPath
                                                 config:entry.configName];
        if (skin == nil) { RMLogWarn(@"skip skin: %@", entry.skinFile); continue; }

        RMSkinView *view = [[RMSkinView alloc] initWithSkin:skin];
        view.draggable = entry.draggable;
        view.useBlur = skin.useBlur;
        if (skin.blurTint) view.blurTint = skin.blurTint;

        // Determine whether the layout position is trustworthy.
        // Rainmeter formulas such as "(#WORKAREAWIDTH#/2-416)" are not
        // evaluated by our simple parser — they resolve to 0 and must
        // be replaced with a sensible default.
        BOOL needFallback = entry.windowXUnresolved || entry.windowYUnresolved;

        view.targetScreenFrame = sf;
        view.hasDesiredPosition = YES;

        if (needFallback) {
            // Top-right cascading layout: start 32px from the top edge,
            // offset each subsequent widget downward by a fixed step.
            static const CGFloat kFallbackWidth  = 320;  // estimated widget width
            static const CGFloat kFallbackStep   = 72;   // vertical spacing per widget
            CGFloat wx = NSMaxX(sf) - kFallbackWidth;
            CGFloat wy = NSMaxY(sf) - 32 - cascadeIdx * kFallbackStep;
            view.desiredScreenTopLeft = NSMakePoint(wx, wy);
            view.anchorFracX = 0;
            view.anchorFracY = 0;
            cascadeIdx++;
        } else {
            // Resolve WindowX/WindowY against the screen. Rainmeter puts the
            // origin at the top-left; convert to AppKit's bottom-left screen
            // coordinates for NSWindow.
            CGFloat wxScreenTopLeftX, wyFromTop;
            if (entry.windowXPercent) wxScreenTopLeftX = NSMinX(sf) + entry.windowX * NSWidth(sf);
            else                       wxScreenTopLeftX = NSMinX(sf) + entry.windowX;
            if (entry.windowYPercent) wyFromTop = entry.windowY * NSHeight(sf);
            else                       wyFromTop = entry.windowY;
            CGFloat desiredScreenY = NSMaxY(sf) - wyFromTop;

            view.desiredScreenTopLeft = NSMakePoint(wxScreenTopLeftX, desiredScreenY);
            view.anchorFracX = entry.anchorXPercent ? entry.anchorX : 0;
            view.anchorFracY = entry.anchorYPercent ? entry.anchorY : 0;
            // Non-percent AnchorX/Y are pixels; store as ratio if we know
            // size — but we don't yet, so approximate: treat as fraction of
            // the current (unknown) size — resizeToContent will re-clamp.
        }

        // Initial frame at the desired position with a reasonable minimum
        // size so the widget is visible even before the first tick populates
        // the content size. resizeToContent will adjust once real dimensions
        // are known.
        CGFloat initX = view.desiredScreenTopLeft.x;
        CGFloat initY = needFallback ? view.desiredScreenTopLeft.y - 64
                     : view.desiredScreenTopLeft.y - 100;
        if (initY < NSMinY(sf)) initY = NSMinY(sf) + 20;
        NSRect initial = NSMakeRect(initX, initY, 100, 100);

        RMWidgetWindow *window = [[RMWidgetWindow alloc]
            initWithContentRect:initial
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.level = kCGNormalWindowLevel - 1;   // desktop widget: behind apps
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorIgnoresCycle;
        window.opaque = NO;
        window.backgroundColor = [NSColor clearColor];
        window.hasShadow = NO;
        window.movableByWindowBackground = NO;   // dragging handled by the view
        window.releasedWhenClosed = NO;
        // Frosted-glass blur is handled by NSVisualEffectView inside the skin
        // view (with NSVisualEffectStateActive + BehindWindow blending). The
        // window itself stays transparent so non-blurred areas show through to
        // the desktop wallpaper.
        window.contentView = view;

        [window orderFrontRegardless];
        [view start];

        [delegate.windows addObject:window];
        [delegate.views addObject:view];
    }
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        HostArgs args;
        if (!ParseArgs(argc, argv, args)) return 1;

        RMLayout *layout = [[RMLayout alloc] initWithThemeDirectory:@(args.themeDir)];
        if (layout == nil) { fprintf(stderr, "RmskinWallpaper: cannot load theme\n"); return 2; }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        HostDelegate *delegate = [HostDelegate new];
        delegate.windows = [NSMutableArray array];
        delegate.views = [NSMutableArray array];
        [app setDelegate:delegate];
        [app finishLaunching];

        BuildWindows(delegate, layout, args.screen);
        if (delegate.windows.count == 0) {
            fprintf(stderr, "RmskinWallpaper: no active widgets to render\n");
            return 3;
        }

        if (args.controlStdin) {
            delegate.control = [[MirageControlChannel alloc]
                initWithHandler:^(NSDictionary *cmd) {
                    NSString *name = cmd[@"cmd"];
                    if ([name isEqualToString:@"reload"]) {
                        for (RMSkinView *v in delegate.views) { [v.skin reload]; [v.skin tick]; v.needsDisplay = YES; }
                    }
                }
                onEOF:^{ [NSApp terminate:nil]; }];
            [delegate.control start];
        }

        if (args.runSeconds > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)args.runSeconds * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        }

        [app run];

        for (RMSkinView *v in delegate.views) [v stop];
    }
    return 0;
}
