#pragma once

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN

// WRDesktopInputForwarder — feeds real desktop mouse events to a wallpaper
// that sits BELOW Finder's desktop window.
//
// Problem: on macOS a borderless wallpaper window placed just under the
// desktop-icon layer is also below Finder's full-screen desktop window, which
// absorbs every desktop click (icons + empty desktop). The wallpaper renders
// fine but never sees the mouse, so click-interactive web wallpapers
// (e.g. gallery-style click-to-drag) are dead.
//
// Fix: keep the wallpaper window below Finder (icons stay fully clickable) and
// observe the mouse GLOBALLY instead. Monitors LeftMouseDown, LeftMouseDragged,
// LeftMouseUp, and MouseMoved. On desktop interactions we synthesize the
// complete mousedown -> mousemove -> mouseup sequence with proper button/buttons
// fields so pages that rely on the full gesture (drag-interactive wallpapers)
// work correctly.
@interface WRDesktopInputForwarder : NSObject

- (instancetype)initWithWebView:(WKWebView *)webView screen:(NSScreen *)screen NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (void)start;
- (void)stop;
- (void)setPaused:(BOOL)paused;

@end

NS_ASSUME_NONNULL_END
