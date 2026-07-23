#pragma once

// RMSkinView — flipped NSView that hosts one RMSkin.
//
// Runs an update timer at the skin's Update interval, redraws on demand, moves
// its (borderless) window on drag, and forwards click / scroll actions to the
// skin. Coordinates are top-left origin (isFlipped=YES) to match Rainmeter.

#import <AppKit/AppKit.h>

@class RMSkin;

NS_ASSUME_NONNULL_BEGIN

@interface RMSkinView : NSView

- (instancetype)initWithSkin:(RMSkin *)skin;

@property (nonatomic, strong, readonly) RMSkin *skin;
@property (nonatomic, assign) BOOL draggable;      // move window on drag

// Frosted-glass background: when YES, reads the macOS desktop wallpaper image
// via NSWorkspace, crops it to the widget's screen region, applies a Gaussian
// blur, and composites a tint overlay behind the meters. No screen capture
// (CGWindowListCreateImage) — uses the static wallpaper file directly.
@property (nonatomic, assign) BOOL useBlur;
@property (nonatomic, assign) CGFloat blurRadius;     // CIGaussianBlur radius, default 22
@property (nonatomic, strong, nullable) NSColor *blurTint; // dark overlay color, default rgba(0,0,0,0.5)

// Force re-render of the blur backdrop. Call after moving the window or when
// the wallpaper has changed.
- (void)refreshBlurBackground;

// Desired top-left position of the widget on the target screen, in AppKit
// screen coordinates (bottom-left origin). Applied whenever the content size
// changes so the widget stays anchored regardless of its dynamic dimensions.
@property (nonatomic, assign) NSPoint desiredScreenTopLeft;
@property (nonatomic, assign) BOOL    hasDesiredPosition;
@property (nonatomic, assign) NSRect  targetScreenFrame;

// Fractional anchor inside the widget content (0..1). Widget's (ax*w, ay*h)
// point is placed at desiredScreenTopLeft. Rainmeter AnchorX/AnchorY.
@property (nonatomic, assign) CGFloat anchorFracX;
@property (nonatomic, assign) CGFloat anchorFracY;

- (void)start;   // begin ticking + first layout
- (void)stop;

@end

NS_ASSUME_NONNULL_END
