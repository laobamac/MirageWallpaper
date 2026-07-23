// RmskinViewer — standalone debug window(s) for a .rmskin theme.
//
// Renders the same floating widgets as RmskinWallpaper but as ordinary
// interactive windows with the app activated, for quick visual iteration.
//
// Usage: RmskinViewer <theme-dir> [--screen N]

#import <AppKit/AppKit.h>

#import "RMLayout.h"
#import "RMSkin.h"
#import "RMSkinView.h"

@interface ViewerDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSMutableArray *windows;
@property (nonatomic, strong) NSMutableArray<RMSkinView *> *views;
@end
@implementation ViewerDelegate
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "Usage: %s <theme-dir> [--screen N]\n", argv[0]); return 1; }
        const char *themeDir = argv[1];
        int screenIndex = 0;
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--screen") == 0) screenIndex = atoi(argv[i + 1]);
        }

        RMLayout *layout = [[RMLayout alloc] initWithThemeDirectory:@(themeDir)];
        if (layout == nil) { fprintf(stderr, "RmskinViewer: cannot load theme\n"); return 2; }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        ViewerDelegate *delegate = [ViewerDelegate new];
        delegate.windows = [NSMutableArray array];
        delegate.views = [NSMutableArray array];
        [app setDelegate:delegate];
        [app finishLaunching];

        NSArray<NSScreen *> *screens = NSScreen.screens;
        (void)screenIndex;
        CGFloat primaryTop = NSMaxY(screens.firstObject.frame);

        for (RMLayoutEntry *entry in [layout activeEntries]) {
            RMSkin *skin = [[RMSkin alloc] initWithSkinFile:entry.skinFile
                                                  resources:entry.resourcesPath
                                                 rootConfig:entry.rootConfigPath
                                                  skinsPath:layout.skinsPath
                                                     config:entry.configName];
            if (skin == nil) continue;
            RMSkinView *view = [[RMSkinView alloc] initWithSkin:skin];

            CGFloat topY = primaryTop - entry.windowY;
            NSRect initial = NSMakeRect(entry.windowX, topY - 100, 100, 100);
            NSWindow *window = [[NSWindow alloc]
                initWithContentRect:initial
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered defer:NO];
            window.opaque = NO;
            window.backgroundColor = [NSColor clearColor];
            window.hasShadow = NO;
            window.level = NSFloatingWindowLevel;
            window.contentView = view;
            [window makeKeyAndOrderFront:nil];
            [view start];

            [delegate.windows addObject:window];
            [delegate.views addObject:view];
        }

        [app activateIgnoringOtherApps:YES];
        [app run];
        for (RMSkinView *v in delegate.views) [v stop];
    }
    return 0;
}
