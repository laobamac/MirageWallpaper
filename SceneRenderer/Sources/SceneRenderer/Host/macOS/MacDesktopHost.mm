#import "MacDesktopHost.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CGWindowLevel.h>

#include <algorithm>
#include <atomic>
#include <cmath>

extern "C" void* SceneRendererMacMetalDisplayCreateForNSView(void* ns_view);
extern "C" void  SceneRendererMacMetalDisplayDestroy(void* handle);
extern "C" void  SceneRendererMacMetalDisplayDraw(void* handle, void* texture, std::uint32_t width,
                                                  std::uint32_t height);

// --- media-status routing --------------------------------------------------
//
// Runtime media-status support lives on `sr::SceneWallpaper::setMediaStatus`.
// It propagates the snapshot into scenescript as `mediaPlaybackChanged`,
// `mediaPropertiesChanged`, and `mediaThumbnailChanged` callbacks. This
// MacDesktopHost is a generic desktop-window/input host and intentionally does
// not own the wallpaper instance; app-level code that observes macOS
// now-playing state should construct `sr::MediaStatus` and call that runtime
// entry point directly.

@interface SceneRendererWallpaperWindow : NSWindow
@end

@implementation SceneRendererWallpaperWindow
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

// Weakly-held wrapper to safely pass a C++ host pointer into NSTimer blocks.
// NSTimer retains its block strongly; by wrapping the C++ pointer in an ObjC
// object that the host owns, we can nil out the pointer before the host is
// freed, preventing use-after-free in timer/dispatch_async callbacks.
@interface SRHostRef : NSObject
@property (nonatomic, assign) void* hostPtr;
@end
@implementation SRHostRef
@end

namespace
{

struct MacDesktopHost {
    SceneRendererMacDesktopCallbacks callbacks {};
    NSWindow*                        window { nil };
    // NOTE: never cache an NSScreen* long-term — the system may release and
    // recreate NSScreen objects when the display config changes (e.g. resizing
    // windows, sleep/wake, resolution changes). Cache the index and re-resolve
    // the NSScreen on demand instead.
    NSUInteger                       screen_index { 0 };
    NSTimer*                         input_timer { nil };
    SRHostRef*                       hostRef { nil };  // ObjC wrapper for safe weak reference
    void*                            metal_display { nullptr };
    NSUInteger                       last_buttons { 0 };
    bool                             mouse_inside { false };
    bool                             sent_enter { false };
    std::atomic<void*>               pending_texture { nullptr };
    std::atomic<std::uint32_t>       pending_width { 0 };
    std::atomic<std::uint32_t>       pending_height { 0 };
    std::atomic<bool>                present_scheduled { false };
};

// Resolve the target NSScreen fresh from the current screen list. Returns the
// window's own screen, the indexed screen, or the main screen as fallbacks.
// Never returns a stale/cached pointer.
NSScreen* ResolveScreen(MacDesktopHost* host) {
    if (host == nullptr) return nil;
    if (host->window != nil && host->window.screen != nil) {
        return host->window.screen;
    }
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (host->screen_index < screens.count) {
        return screens[host->screen_index];
    }
    return NSScreen.mainScreen;
}

double Clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

void EmitMouseEnter(MacDesktopHost* host, bool entered) {
    if (host == nullptr) return;
    if (host->sent_enter && host->mouse_inside == entered) return;
    host->sent_enter   = true;
    host->mouse_inside = entered;
    if (host->callbacks.mouse_enter != nullptr) {
        host->callbacks.mouse_enter(entered ? 1 : 0, host->callbacks.userdata);
    }
}

void PollInput(MacDesktopHost* host) {
    if (host == nullptr || host->window == nil) return;

    NSScreen* screen = ResolveScreen(host);
    if (screen == nil) return;

    const NSPoint mouse = NSEvent.mouseLocation;
    const NSRect  frame = screen.frame;
    const bool    inside =
        NSWidth(frame) > 0.0 && NSHeight(frame) > 0.0 && NSPointInRect(mouse, frame);
    EmitMouseEnter(host, inside);

    if (inside && host->callbacks.mouse_move != nullptr) {
        const double x = Clamp01((mouse.x - NSMinX(frame)) / NSWidth(frame));
        const double y = Clamp01(1.0 - ((mouse.y - NSMinY(frame)) / NSHeight(frame)));
        host->callbacks.mouse_move(x, y, host->callbacks.userdata);
    }

    const NSUInteger buttons = NSEvent.pressedMouseButtons;
    for (int button = 0; button < 3; ++button) {
        const NSUInteger mask    = static_cast<NSUInteger>(1u << button);
        const bool       was_down = (host->last_buttons & mask) != 0;
        const bool       is_down  = (buttons & mask) != 0;
        if (was_down == is_down) continue;
        if (host->callbacks.mouse_button != nullptr) {
            host->callbacks.mouse_button(button, is_down ? 1 : 0, host->callbacks.userdata);
        }
    }
    host->last_buttons = buttons;
}

void SchedulePresent(MacDesktopHost* host) {
    if (host == nullptr) return;
    if (host->present_scheduled.exchange(true)) return;

    SRHostRef* ref = host->hostRef;
    if (ref == nil) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      MacDesktopHost* h = static_cast<MacDesktopHost*>(ref.hostPtr);
      if (h == nullptr) return;
      if (h->metal_display != nullptr) {
          void* texture = h->pending_texture.load();
          const std::uint32_t width = h->pending_width.load();
          const std::uint32_t height = h->pending_height.load();
          if (texture != nullptr && width > 0 && height > 0) {
              SceneRendererMacMetalDisplayDraw(h->metal_display, texture, width, height);
          }
      }
      h->present_scheduled.store(false);
    });
}

void StopApplicationOnMainThread() {
    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApp stop:nil];
      NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                          location:NSZeroPoint
                                     modifierFlags:0
                                         timestamp:0
                                      windowNumber:0
                                           context:nil
                                           subtype:0
                                             data1:0
                                             data2:0];
      [NSApp postEvent:event atStart:NO];
    });
}

} // namespace

extern "C" void* SceneRendererMacDesktopCreate(const SceneRendererMacDesktopConfig* config,
                                               SceneRendererMacDesktopCallbacks callbacks) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app finishLaunching];

        NSScreen*  screen        = nil;
        NSUInteger resolvedIndex = 0;
        if (config != nullptr && config->screen_index > 0) {
            NSArray<NSScreen*>* screens = NSScreen.screens;
            if (config->screen_index < screens.count) {
                screen        = screens[config->screen_index];
                resolvedIndex = config->screen_index;
            }
        }
        if (screen == nil) screen = NSScreen.mainScreen;
        if (screen == nil) return nullptr;

        auto* host     = new MacDesktopHost();
        host->callbacks = callbacks;
        host->screen_index = resolvedIndex;

        NSString* title = @"SceneRenderer Wallpaper";
        if (config != nullptr && config->title != nullptr && config->title[0] != '\0') {
            title = [NSString stringWithUTF8String:config->title];
        }

        SceneRendererWallpaperWindow* window =
            [[SceneRendererWallpaperWindow alloc] initWithContentRect:screen.frame
                                                             styleMask:NSWindowStyleMaskBorderless
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO
                                                                screen:screen];
        window.title                 = title;
        window.level                 = CGWindowLevelForKey(kCGDesktopIconWindowLevelKey) - 1;
        window.collectionBehavior    = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorIgnoresCycle;
        window.opaque                = YES;
        window.backgroundColor       = NSColor.blackColor;
        window.hasShadow             = NO;
        window.ignoresMouseEvents    = YES;
        window.releasedWhenClosed    = NO;
        window.canHide               = NO;
        window.acceptsMouseMovedEvents = NO;
        window.restorable            = NO;

        host->window = window;
        [window orderFrontRegardless];

        host->metal_display =
            SceneRendererMacMetalDisplayCreateForNSView((__bridge void*)window.contentView);
        if (host->metal_display == nullptr) {
            [window orderOut:nil];
            delete host;
            return nullptr;
        }

        const std::uint32_t hz =
            config != nullptr && config->input_hz > 0 ? config->input_hz : 60u;
        const NSTimeInterval interval = 1.0 / static_cast<NSTimeInterval>(std::min(hz, 240u));

        // Use SRHostRef as a weak-like indirection: the block captures the ObjC
        // object (retained), but hostPtr is set to nullptr before the C++ host
        // is freed. This prevents use-after-free from NSTimer callbacks that
        // fire during or after teardown.
        host->hostRef = [[SRHostRef alloc] init];
        host->hostRef.hostPtr = host;
        SRHostRef* ref = host->hostRef;
        host->input_timer = [NSTimer timerWithTimeInterval:interval
                                                   repeats:YES
                                                     block:^(NSTimer*) {
                                                       MacDesktopHost* h = static_cast<MacDesktopHost*>(ref.hostPtr);
                                                       if (h != nullptr) PollInput(h);
                                                     }];
        [NSRunLoop.mainRunLoop addTimer:host->input_timer forMode:NSRunLoopCommonModes];
        PollInput(host);
        return host;
    }
}

extern "C" void SceneRendererMacDesktopDestroy(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr) return;

    // Nil out the ObjC wrapper BEFORE freeing the host so any in-flight async
    // blocks (PollInput timer, SchedulePresent dispatch) see nullptr and
    // short-circuit, preventing use-after-free.
    if (host->hostRef != nil) {
        host->hostRef.hostPtr = nullptr;
    }

    auto cleanup = ^{
      if (host->input_timer != nil) {
          [host->input_timer invalidate];
          host->input_timer = nil;
      }
      if (host->metal_display != nullptr) {
          SceneRendererMacMetalDisplayDestroy(host->metal_display);
          host->metal_display = nullptr;
      }
      if (host->window != nil) {
          [host->window orderOut:nil];
          host->window = nil;
      }
    };
    if (NSThread.isMainThread) {
        cleanup();
    } else {
        dispatch_sync(dispatch_get_main_queue(), cleanup);
    }
    delete host;
}

extern "C" int SceneRendererMacDesktopRun(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr) return 0;
    @autoreleasepool {
        [NSApp run];
        if (host->callbacks.closed != nullptr) {
            host->callbacks.closed(host->callbacks.userdata);
        }
        return 1;
    }
}

extern "C" void SceneRendererMacDesktopStop(void*) { StopApplicationOnMainThread(); }

extern "C" void SceneRendererMacDesktopWake(void*) {
    dispatch_async(dispatch_get_main_queue(), ^{
    });
}

extern "C" void SceneRendererMacDesktopPresent(void* handle, void* texture, std::uint32_t width,
                                               std::uint32_t height) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || texture == nullptr || width == 0 || height == 0) return;
    host->pending_texture.store(texture);
    host->pending_width.store(width);
    host->pending_height.store(height);
    SchedulePresent(host);
}

extern "C" std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->window == nil) return 0;
    NSScreen*     screen = ResolveScreen(host);
    const CGFloat fallbackScale = screen != nil ? screen.backingScaleFactor : 1.0;
    const CGFloat scale = host->window.backingScaleFactor > 0.0 ? host->window.backingScaleFactor
                                                                : fallbackScale;
    return static_cast<std::uint32_t>(std::lround(NSWidth(host->window.contentView.bounds) * scale));
}

extern "C" std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->window == nil) return 0;
    NSScreen*     screen = ResolveScreen(host);
    const CGFloat fallbackScale = screen != nil ? screen.backingScaleFactor : 1.0;
    const CGFloat scale = host->window.backingScaleFactor > 0.0 ? host->window.backingScaleFactor
                                                                : fallbackScale;
    return static_cast<std::uint32_t>(
        std::lround(NSHeight(host->window.contentView.bounds) * scale));
}
