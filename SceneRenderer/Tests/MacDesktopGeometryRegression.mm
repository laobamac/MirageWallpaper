// Exercise the production host against AppKit without loading a scene or Vulkan.
#include "../Sources/SceneRenderer/Host/macOS/MacDesktopHost.mm"
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>

static unsigned flushes = 0;
static unsigned layouts = 0;

@interface CATransaction (GeometryRegression)
+ (void)geometryRegressionFlush;
@end
@implementation CATransaction (GeometryRegression)
+ (void)geometryRegressionFlush {
    ++flushes;
    [self geometryRegressionFlush];
}
@end

@interface GeometryRegressionView : NSView
@end
@implementation GeometryRegressionView
- (void)layoutSubtreeIfNeeded {
    ++layouts;
    [super layoutSubtreeIfNeeded];
}
@end

static void Check(bool condition, const char* message) {
    if (! condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        NSScreen* screen = NSScreen.screens.firstObject;
        if (screen == nil) return 77;
        method_exchangeImplementations(
            class_getClassMethod(CATransaction.class, @selector(flush)),
            class_getClassMethod(CATransaction.class, @selector(geometryRegressionFlush)));

        MacDesktopHost host;
        host.display_id = ScreenDisplayID(screen);
        host.window = [[SceneRendererWallpaperWindow alloc]
            initWithContentRect:screen.frame styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered defer:NO screen:screen];
        host.window.releasedWhenClosed = NO;
        auto* view = [[GeometryRegressionView alloc] initWithFrame:host.window.contentView.frame];
        host.window.contentView = view;
        [view release];
        view.wantsLayer = YES;
        host.surface_layer = [CAMetalLayer layer];
        view.layer = host.surface_layer;
        host.activation_confirmed.store(true);
        Check(NormalizeGeometry(&host), "initial geometry");
        const unsigned initial_flushes = flushes;
        Check(initial_flushes > 0, "initial geometry must flush");
        host.first_frame_presented.store(true);
        view.needsLayout = NO;
        view.needsDisplay = NO;
        host.window.viewsNeedDisplay = NO;
        const unsigned initial_layouts = layouts;
        SRHostRef* ref = [[SRHostRef alloc] init];
        ref.hostPtr = &host;
        for (int i = 0; i < 600; ++i) {
            FramePresented(ref);
            PollInput(&host);
        }
        Check(flushes == initial_flushes, "steady presentation/input must not flush");
        Check(layouts == initial_layouts, "steady presentation/input must not lay out");

        auto repair = [&](const char* message) {
            const unsigned before = flushes;
            Check(NormalizeGeometry(&host), message);
            Check(flushes > before, message);
        };
        host.surface_layer.contentsScale = screen.backingScaleFactor + 0.25;
        repair("repair fractional backing scale mismatch");
        Check(host.surface_layer.contentsScale == screen.backingScaleFactor, "backing scale restored");
        host.surface_layer.drawableSize = CGSizeMake(13, 17);
        repair("repair drawable size");
        Check(CGSizeEqualToSize(host.surface_layer.drawableSize,
              [view convertRectToBacking:view.bounds].size), "drawable size restored");
        host.surface_layer.frame = NSMakeRect(2, 3, 100, 100);
        repair("repair layer frame");
        Check(NSEqualRects(host.surface_layer.frame, view.bounds), "layer frame restored");
        [host.window setFrame:NSMakeRect(20, 30, 300, 200) display:NO];
        repair("repair moved/resized window");
        Check(AppKitRectMatches(host.window.frame, screen.frame), "window frame restored");
        view.needsLayout = YES;
        repair("service pending layout");
        view.needsDisplay = YES;
        repair("service pending display");
        host.window.viewsNeedDisplay = YES;
        repair("service window display request");

        host.activation_confirmed.store(false);
        repair("unconfirmed activation must flush");
        host.activation_confirmed.store(true);
        const unsigned before_force = flushes;
        Check(NormalizeGeometry(&host, true), "forced activation geometry");
        Check(flushes > before_force, "forced geometry must flush");
        host.first_frame_presented.store(false);
        repair("first frame must flush");
        const auto valid_display = host.display_id;
        host.display_id = 0;
        Check(! NormalizeGeometry(&host), "missing display rejected");
        host.display_id = valid_display;
        Check(! NormalizeGeometry(nullptr), "null host rejected");
        ref.hostPtr = nullptr;
        [ref release];
        [host.window close];
        [host.window release];
        std::puts("PASS: steady geometry, repair, dirty views, first frame and activation");
    }
}
