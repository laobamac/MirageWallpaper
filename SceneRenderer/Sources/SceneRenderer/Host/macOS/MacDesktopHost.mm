#import "MacDesktopHost.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CGWindowLevel.h>
#import <Metal/Metal.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cmath>

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
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    return frameRect;
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
    CGDirectDisplayID                display_id { 0 };
    NSTimer*                         input_timer { nil };
    SRHostRef*                       hostRef { nil };  // ObjC wrapper for safe weak reference
    CAMetalLayer*                    surface_layer { nil };
    NSUInteger                       last_buttons { 0 };
    bool                             mouse_inside { false };
    bool                             sent_enter { false };
    std::atomic<bool>                first_frame_presented { false };
    std::atomic<bool>                activation_requested { false };
    std::atomic<bool>                activation_confirmed { false };
    std::atomic<bool>                activation_frame_pending { false };
    std::atomic<bool>                activation_failure_pending { false };
    std::atomic<bool>                activation_failure_reported { false };
    std::atomic<bool>                deactivation_requested { false };
    std::atomic<bool>                deactivation_confirmed { false };
};

NSScreen* ResolveScreen(CGDirectDisplayID display_id) {
    if (display_id == 0) return nil;
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number != nil && number.unsignedIntValue == display_id) return screen;
    }
    return nil;
}

double Clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool NearlyEqual(CGFloat lhs, CGFloat rhs) {
    return std::abs(lhs - rhs) <= 0.5;
}

bool AppKitRectMatches(NSRect lhs, NSRect rhs) {
    return NearlyEqual(NSMinX(lhs), NSMinX(rhs)) &&
           NearlyEqual(NSMinY(lhs), NSMinY(rhs)) &&
           NearlyEqual(NSWidth(lhs), NSWidth(rhs)) &&
           NearlyEqual(NSHeight(lhs), NSHeight(rhs));
}

bool CoreGraphicsRectMatches(CGRect lhs, CGRect rhs) {
    return NearlyEqual(CGRectGetMinX(lhs), CGRectGetMinX(rhs)) &&
           NearlyEqual(CGRectGetMinY(lhs), CGRectGetMinY(rhs)) &&
           NearlyEqual(CGRectGetWidth(lhs), CGRectGetWidth(rhs)) &&
           NearlyEqual(CGRectGetHeight(lhs), CGRectGetHeight(rhs));
}

bool SizeMatches(CGSize lhs, CGSize rhs) {
    return NearlyEqual(lhs.width, rhs.width) && NearlyEqual(lhs.height, rhs.height);
}

CGDirectDisplayID ScreenDisplayID(NSScreen* screen) {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    return number != nil ? number.unsignedIntValue : 0;
}

bool WindowServerState(NSWindow* window, CGRect& bounds, bool& onscreen, double& alpha) {
    if (window == nil || window.windowNumber <= 0) return false;
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionIncludingWindow,
        static_cast<CGWindowID>(window.windowNumber));
    if (windows == nullptr || CFArrayGetCount(windows) != 1) {
        if (windows != nullptr) CFRelease(windows);
        return false;
    }
    auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
    auto bounds_value = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(info, kCGWindowBounds));
    const bool has_bounds = bounds_value != nullptr &&
                            CGRectMakeWithDictionaryRepresentation(bounds_value, &bounds);
    auto onscreen_value = static_cast<CFBooleanRef>(
        CFDictionaryGetValue(info, kCGWindowIsOnscreen));
    onscreen = onscreen_value != nullptr && CFBooleanGetValue(onscreen_value);
    auto alpha_value = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowAlpha));
    alpha = 0.0;
    if (alpha_value != nullptr) {
        CFNumberGetValue(alpha_value, kCFNumberDoubleType, &alpha);
    }
    CFRelease(windows);
    return has_bounds;
}

bool NormalizeGeometry(MacDesktopHost* host) {
    if (host == nullptr || host->window == nil || host->surface_layer == nil) return false;
    NSScreen* screen = ResolveScreen(host->display_id);
    if (screen == nil || ScreenDisplayID(screen) != host->display_id) return false;
    NSRect frame = screen.frame;
    if (! std::isfinite(NSMinX(frame)) || ! std::isfinite(NSMinY(frame)) ||
        ! std::isfinite(NSWidth(frame)) || ! std::isfinite(NSHeight(frame)) ||
        NSWidth(frame) <= 0.0 || NSHeight(frame) <= 0.0) {
        return false;
    }
    if (! AppKitRectMatches(host->window.frame, frame)) {
        [host->window setFrame:frame display:YES];
    }
    NSView* content_view = host->window.contentView;
    if (content_view == nil) return false;
    [content_view layoutSubtreeIfNeeded];
    const NSRect bounds = content_view.bounds;
    host->surface_layer.frame = bounds;
    host->surface_layer.contentsScale = screen.backingScaleFactor;
    host->surface_layer.drawableSize = [content_view convertRectToBacking:bounds].size;
    [host->window displayIfNeeded];
    [content_view displayIfNeeded];
    [CATransaction flush];
    return true;
}

bool ValidateGeometry(MacDesktopHost* host, bool activated) {
    if (host == nullptr || host->window == nil || host->surface_layer == nil) return false;
    NSScreen* screen = ResolveScreen(host->display_id);
    if (screen == nil || ScreenDisplayID(screen) != host->display_id) return false;
    if (! AppKitRectMatches(host->window.frame, screen.frame)) return false;
    NSView* content_view = host->window.contentView;
    if (content_view == nil) return false;
    const NSRect expected_bounds = NSMakeRect(
        0.0, 0.0, NSWidth(screen.frame), NSHeight(screen.frame));
    if (! AppKitRectMatches(content_view.bounds, expected_bounds) ||
        ! AppKitRectMatches(host->surface_layer.frame, content_view.bounds) ||
        ! NearlyEqual(host->surface_layer.contentsScale, screen.backingScaleFactor)) {
        return false;
    }
    const CGSize expected_drawable =
        [content_view convertRectToBacking:content_view.bounds].size;
    if (! SizeMatches(host->surface_layer.drawableSize, expected_drawable)) return false;
    CGRect window_bounds = CGRectZero;
    bool onscreen = false;
    double alpha = 0.0;
    if (! WindowServerState(host->window, window_bounds, onscreen, alpha) ||
        ! CoreGraphicsRectMatches(window_bounds, CGDisplayBounds(host->display_id))) {
        return false;
    }
    if (activated && (! onscreen || alpha < 0.999 || host->window.alphaValue < 0.999 ||
                      ! host->window.opaque || ! host->surface_layer.opaque)) {
        return false;
    }
    return true;
}

bool ValidateHidden(MacDesktopHost* host) {
    if (host == nullptr || host->window == nil) return true;
    if (host->window.isVisible || host->window.alphaValue > 0.001) return false;
    CGRect window_bounds = CGRectZero;
    bool onscreen = false;
    double alpha = 0.0;
    // Once WindowServer drops the entry entirely, the window is necessarily no
    // longer composited. If it still has an entry, require both visibility and
    // alpha to have settled before acknowledging the handoff.
    if (! WindowServerState(host->window, window_bounds, onscreen, alpha)) return true;
    return ! onscreen && alpha <= 0.001;
}

void ConfirmDeactivated(SRHostRef* ref, int attempts_left) {
    auto* host = ref != nil ? static_cast<MacDesktopHost*>(ref.hostPtr) : nullptr;
    if (host == nullptr || ! host->deactivation_requested.load()) return;
    if (ValidateHidden(host)) {
        if (! host->deactivation_confirmed.exchange(true) &&
            host->callbacks.deactivated != nullptr) {
            host->callbacks.deactivated(host->callbacks.userdata);
        }
        return;
    }
    if (attempts_left <= 0) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
      ConfirmDeactivated(ref, attempts_left - 1);
    });
}

void ConfirmActivationFailed(SRHostRef* ref, int attempts_left) {
    auto* host = ref != nil ? static_cast<MacDesktopHost*>(ref.hostPtr) : nullptr;
    if (host == nullptr || ! host->activation_failure_pending.load()) return;
    if (ValidateHidden(host)) {
        host->activation_failure_pending.store(false);
        if (! host->activation_failure_reported.exchange(true) &&
            host->callbacks.activation_failed != nullptr) {
            host->callbacks.activation_failed(host->callbacks.userdata);
        }
        return;
    }
    if (attempts_left <= 0) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
      ConfirmActivationFailed(ref, attempts_left - 1);
    });
}

void BeginActivationFailure(SRHostRef* ref) {
    auto* host = ref != nil ? static_cast<MacDesktopHost*>(ref.hostPtr) : nullptr;
    if (host == nullptr || host->window == nil ||
        host->activation_failure_pending.exchange(true)) return;
    host->activation_requested.store(false);
    host->activation_confirmed.store(false);
    host->window.alphaValue = 0.0;
    [host->window orderOut:nil];
    [CATransaction flush];
    ConfirmActivationFailed(ref, 200);
}

void ConfirmActivated(SRHostRef* ref, int attempts_left) {
    auto* host = ref != nil ? static_cast<MacDesktopHost*>(ref.hostPtr) : nullptr;
    if (host == nullptr || ! host->activation_requested.load() ||
        host->activation_confirmed.load()) return;
    if (ValidateGeometry(host, true)) {
        if (! host->activation_confirmed.exchange(true) &&
            host->callbacks.activated != nullptr) {
            host->callbacks.activated(host->callbacks.userdata);
        }
        return;
    }
    if (attempts_left <= 0) {
        BeginActivationFailure(ref);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
      ConfirmActivated(ref, attempts_left - 1);
    });
}

void StopApplicationOnMainThread();

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

    NSScreen* screen = ResolveScreen(host->display_id);
    if (screen == nil) {
        [host->window orderOut:nil];
        StopApplicationOnMainThread();
        return;
    }

    if (! host->activation_confirmed.load()) {
        EmitMouseEnter(host, false);
        host->last_buttons = 0;
        return;
    }

    const NSRect frame = screen.frame;
    NormalizeGeometry(host);

    const NSPoint mouse = NSEvent.mouseLocation;
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

        NSScreen* screen = nil;
        CGDirectDisplayID display_id = config != nullptr ? config->display_id : 0;
        if (display_id != 0) {
            screen = ResolveScreen(display_id);
        } else {
            NSArray<NSScreen*>* screens = NSScreen.screens;
            const std::uint32_t screen_index = config != nullptr ? config->screen_index : 0;
            if (screen_index < screens.count) screen = screens[screen_index];
            NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
            if (number != nil) display_id = number.unsignedIntValue;
        }
        if (screen == nil || display_id == 0) return nullptr;

        auto* host     = new MacDesktopHost();
        host->callbacks = callbacks;
        host->display_id = display_id;

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
        const bool deferred_show     = config != nullptr && config->deferred_show;
        host->activation_confirmed.store(! deferred_show);
        window.opaque                = deferred_show ? NO : YES;
        window.backgroundColor       = deferred_show ? NSColor.clearColor : NSColor.blackColor;
        window.alphaValue            = deferred_show ? 0.0 : 1.0;
        window.hasShadow             = NO;
        window.ignoresMouseEvents    = YES;
        window.releasedWhenClosed    = NO;
        window.canHide               = NO;
        window.acceptsMouseMovedEvents = NO;
        window.restorable            = NO;

        host->window = window;
        [window orderFrontRegardless];

        NSView* content_view = window.contentView;
        content_view.wantsLayer = YES;
        CAMetalLayer* surface_layer = [CAMetalLayer layer];
        surface_layer.device = MTLCreateSystemDefaultDevice();
        surface_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        surface_layer.framebufferOnly = YES;
        surface_layer.opaque = deferred_show ? NO : YES;
        surface_layer.contentsScale = screen.backingScaleFactor;
        surface_layer.frame = content_view.bounds;
        surface_layer.drawableSize = [content_view convertRectToBacking:content_view.bounds].size;
        content_view.layer = surface_layer;
        host->surface_layer = surface_layer;
        if (surface_layer.device == nil) {
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
      host->surface_layer = nil;
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

extern "C" void SceneRendererMacDesktopWake(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->hostRef == nil) return;
    SRHostRef* ref = host->hostRef;
    dispatch_async(dispatch_get_main_queue(), ^{
      auto* current = static_cast<MacDesktopHost*>(ref.hostPtr);
      if (current == nullptr) return;
      if (! NormalizeGeometry(current)) return;
      if (! current->first_frame_presented.exchange(true) &&
          current->callbacks.first_frame_presented != nullptr) {
          current->callbacks.first_frame_presented(current->callbacks.userdata);
      }
      if (current->activation_requested.load() &&
          current->activation_frame_pending.load() &&
          ValidateGeometry(current, false)) {
          current->activation_frame_pending.store(false);
          current->window.alphaValue = 1.0;
          [CATransaction flush];
          ConfirmActivated(ref, 200);
          return;
      }
      if (current->activation_requested.load() &&
          ! current->activation_confirmed.load() && ValidateGeometry(current, true)) {
          if (! current->activation_confirmed.exchange(true) &&
              current->callbacks.activated != nullptr) {
              current->callbacks.activated(current->callbacks.userdata);
          }
      }
    });
}

extern "C" void SceneRendererMacDesktopActivate(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr) return;
    auto activate = ^{
      if (host->window == nil) return;
      if (! NormalizeGeometry(host)) return;
      host->deactivation_requested.store(false);
      host->deactivation_confirmed.store(false);
      host->activation_confirmed.store(false);
      host->activation_failure_pending.store(false);
      host->activation_failure_reported.store(false);
      host->activation_requested.store(true);
      host->activation_frame_pending.store(true);
      host->window.backgroundColor = NSColor.blackColor;
      host->window.opaque          = YES;
      if (host->surface_layer != nil) host->surface_layer.opaque = YES;

      // A standby has been orderOut'ed, so geometry cannot be checked against
      // WindowServer until it is registered again. Re-register it at alpha 0,
      // validate the exact target-display bounds, and reveal only afterwards.
      // This keeps a stale or misplaced window from ever becoming visible.
      host->window.alphaValue = 0.0;
      [host->window orderFrontRegardless];
      [CATransaction flush];
    };
    if (NSThread.isMainThread) {
        activate();
    } else {
        dispatch_sync(dispatch_get_main_queue(), activate);
    }
}

extern "C" void SceneRendererMacDesktopDeactivate(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr) return;
    auto deactivate = ^{
      if (host->window == nil) return;
      host->activation_requested.store(false);
      host->activation_confirmed.store(false);
      host->activation_failure_pending.store(false);
      host->deactivation_confirmed.store(false);
      host->deactivation_requested.store(true);
      host->window.alphaValue = 0.0;
      [host->window orderOut:nil];
      [CATransaction flush];
      ConfirmDeactivated(host->hostRef, 200);
    };
    if (NSThread.isMainThread) {
        deactivate();
    } else {
        dispatch_sync(dispatch_get_main_queue(), deactivate);
    }
}

extern "C" void* SceneRendererMacDesktopMetalLayer(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    return host != nullptr && host->surface_layer != nil
               ? (__bridge void*)host->surface_layer
               : nullptr;
}

extern "C" std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->window == nil) return 0;
    NSView* view = host->window.contentView;
    const NSSize backing = [view convertRectToBacking:view.bounds].size;
    return static_cast<std::uint32_t>(std::lround(backing.width));
}

extern "C" std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->window == nil) return 0;
    NSView* view = host->window.contentView;
    const NSSize backing = [view convertRectToBacking:view.bounds].size;
    return static_cast<std::uint32_t>(std::lround(backing.height));
}
