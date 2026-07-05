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

// --- mpris / media-status routing (port of upstream WW_EVT_IN_MPRIS) -------
//
// Upstream's `waywallen/scene_main.cpp` receives WW_EVT_IN_MPRIS events from
// the wayland compositor (Linux mpris) and forwards them to
// `owe::SceneWallpaper::setMediaStatus(...)`. The runtime-side support is
// fully ported: `sr::SceneWallpaper::setMediaStatus(sr::MediaStatus)` is the
// entry point and propagates the snapshot into the script runtime as
// `mediaPlaybackChanged` / `mediaPropertiesChanged` / `mediaThumbnailChanged`
// JS callbacks (see ScriptRuntime.cpp + WallpaperEngineRuntime.cpp).
//
// macOS has no mpris equivalent. The natural media-event sources here are
// `MPNowPlayingInfoCenter` / `MPMusicPlayerController` (MediaPlayer.framework)
// or `NSDistributedNotificationCenter` for the "now playing" feed. This
// MacDesktopHost is a generic desktop-window/input host and intentionally
// does NOT own the `sr::SceneWallpaper` (the app layer — e.g.
// Tools/SceneWallpaper/WallpaperApp.cpp — owns it and wires these callbacks
// to it). So media-status routing belongs there, not here.
//
// TODO(macos-media): when a macOS now-playing observer is added, have it
// construct an `sr::MediaStatus` from the now-playing dictionary and call
// `wallpaper.setMediaStatus(...)` on the app's `sr::SceneWallpaper` instance.
// The `state` field maps to the WE `MediaPlaybackEvent` JS enum
// (0=PLAYBACK_STOPPED, 1=PLAYBACK_PLAYING, 2=PLAYBACK_PAUSED); `art_url`
// should be a `file://...` or absolute path so TextureDecoder's
// ResolveExternalImagePath can percent-decode + load it.

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

namespace
{

struct MacDesktopHost {
    SceneRendererMacDesktopCallbacks callbacks {};
    NSWindow*                        window { nil };
    NSScreen*                        screen { nil };
    NSTimer*                         input_timer { nil };
    void*                            metal_display { nullptr };
    NSUInteger                       last_buttons { 0 };
    bool                             mouse_inside { false };
    bool                             sent_enter { false };
    std::atomic<void*>               pending_texture { nullptr };
    std::atomic<std::uint32_t>       pending_width { 0 };
    std::atomic<std::uint32_t>       pending_height { 0 };
    std::atomic<bool>                present_scheduled { false };
};

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

    NSScreen* screen = host->screen != nil ? host->screen : host->window.screen;
    if (screen == nil) screen = NSScreen.mainScreen;
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

    dispatch_async(dispatch_get_main_queue(), ^{
      if (host->metal_display != nullptr) {
          void* texture = host->pending_texture.load();
          const std::uint32_t width = host->pending_width.load();
          const std::uint32_t height = host->pending_height.load();
          if (texture != nullptr && width > 0 && height > 0) {
              SceneRendererMacMetalDisplayDraw(host->metal_display, texture, width, height);
          }
      }
      host->present_scheduled.store(false);
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

        NSScreen* screen = nil;
        if (config != nullptr && config->screen_index > 0) {
            NSArray<NSScreen*>* screens = NSScreen.screens;
            if (config->screen_index < screens.count) {
                screen = screens[config->screen_index];
            }
        }
        if (screen == nil) screen = NSScreen.mainScreen;
        if (screen == nil) return nullptr;

        auto* host     = new MacDesktopHost();
        host->callbacks = callbacks;
        host->screen    = screen;

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
        host->input_timer = [NSTimer timerWithTimeInterval:interval
                                                   repeats:YES
                                                     block:^(NSTimer*) {
                                                       PollInput(host);
                                                     }];
        [NSRunLoop.mainRunLoop addTimer:host->input_timer forMode:NSRunLoopCommonModes];
        PollInput(host);
        return host;
    }
}

extern "C" void SceneRendererMacDesktopDestroy(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr) return;

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
    const CGFloat scale = host->window.backingScaleFactor > 0.0 ? host->window.backingScaleFactor
                                                                : host->screen.backingScaleFactor;
    return static_cast<std::uint32_t>(std::lround(NSWidth(host->window.contentView.bounds) * scale));
}

extern "C" std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle) {
    auto* host = static_cast<MacDesktopHost*>(handle);
    if (host == nullptr || host->window == nil) return 0;
    const CGFloat scale = host->window.backingScaleFactor > 0.0 ? host->window.backingScaleFactor
                                                                : host->screen.backingScaleFactor;
    return static_cast<std::uint32_t>(
        std::lround(NSHeight(host->window.contentView.bounds) * scale));
}
