#import "RMSkinView.h"
#import "RMSkin.h"
#import "RMLog.h"
#import <CoreImage/CoreImage.h>
#import <QuartzCore/QuartzCore.h>

@implementation RMSkinView {
    NSTimer *_timer;
    NSPoint _dragStartInWindow;
    NSPoint _windowStartOrigin;
    BOOL    _dragged;
    NSSize  _lastContentSize;
    BOOL    _hasPerformedInitialLayout;
    NSTrackingArea *_trackingArea;
    NSVisualEffectView *_blurEffectView;
}

- (instancetype)initWithSkin:(RMSkin *)skin {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 100, 100)])) {
        _skin = skin;
        _draggable = YES;
        _blurRadius = 22.0;
        // Default tint is a 50% black overlay. Light themes (Quanto_Flx) can
        // override via `BlurTint=255,255,255,80` in [Rainmeter] for a white
        // Acrylic effect; dark themes (SystemFetch) work as-is.
        _blurTint = [NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:0.5];
        __weak RMSkinView *weakSelf = self;
        skin.onNeedsRedraw = ^{ weakSelf.needsDisplay = YES; };

        // Wire up window-management callbacks from bangs.
        skin.onWindowAlphaChanged = ^(CGFloat alpha, NSTimeInterval duration) {
            [weakSelf setWindowAlpha:alpha animated:duration];
        };
        skin.onClickThroughChanged = ^(BOOL clickThrough) {
            [weakSelf setWindowClickThrough:clickThrough];
        };
        skin.onWindowPositionChanged = ^(NSPoint pos) {
            [weakSelf setWindowPosition:pos];
        };
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }

// Respond to clicks even when the widget window is not the active/key window,
// so a single click on a background desktop widget fires its action instead of
// only bringing the window forward.
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (void)start {
    [self.skin tick];
    [self resizeToContent];
    self.needsDisplay = YES;

    NSTimeInterval interval = self.skin.updateInterval > 0 ? self.skin.updateInterval : 1.0;
    _timer = [NSTimer scheduledTimerWithTimeInterval:interval repeats:YES block:^(NSTimer *t) {
        [self.skin tick];
        [self resizeToContent];
        self.needsDisplay = YES;
    }];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
}

- (void)resizeToContent {
    NSSize cs = self.skin.contentSize;
    // Skip only if the size literally hasn't changed AND we've already
    // performed the initial layout. On the first pass, always position
    // the window, even when cs == {0,0}, so percentage/anchor placements
    // take effect and the window lands at its designated spot.
    BOOL wasInitial = !_hasPerformedInitialLayout;
    if (NSEqualSizes(cs, _lastContentSize) && _hasPerformedInitialLayout) return;
    _lastContentSize = cs;
    _hasPerformedInitialLayout = YES;

    NSWindow *win = self.window;
    if (win) {
        NSRect frame = win.frame;
        NSRect newFrame;

        if (self.hasDesiredPosition && !NSIsEmptyRect(self.targetScreenFrame)) {
            // Interpret desiredScreenTopLeft as: the widget's anchor point
            // (anchorFracX * w, anchorFracY * h, measured from widget's top-
            // left) should be placed exactly here on the screen. Coordinates
            // are AppKit (bottom-left origin); .y is the "top edge" line the
            // widget's top-left will sit at when anchor is (0,0).
            CGFloat topLeftScreenX = self.desiredScreenTopLeft.x - self.anchorFracX * cs.width;
            CGFloat topLeftScreenY = self.desiredScreenTopLeft.y + self.anchorFracY * cs.height;
            // AppKit window origin is the bottom-left corner.
            CGFloat originX = topLeftScreenX;
            CGFloat originY = topLeftScreenY - cs.height;
            newFrame = NSMakeRect(originX, originY, cs.width, cs.height);

            // Force fully-on-screen. Widget shall never sit off-screen; if
            // the requested position pushes it out, clamp into the target
            // screen with a small edge margin.
            NSRect sf = self.targetScreenFrame;
            CGFloat margin = 20;
            CGFloat minX = NSMinX(sf) + margin;
            CGFloat maxX = NSMaxX(sf) - cs.width - margin;
            CGFloat minY = NSMinY(sf) + margin;
            CGFloat maxY = NSMaxY(sf) - cs.height - margin;
            if (maxX < minX) maxX = minX;
            if (maxY < minY) maxY = minY;
            newFrame.origin.x = MIN(MAX(newFrame.origin.x, minX), maxX);
            newFrame.origin.y = MIN(MAX(newFrame.origin.y, minY), maxY);
        } else {
            // No layout position: keep the top edge fixed while the height
            // changes, so the widget grows downwards from its current spot.
            CGFloat top = NSMaxY(frame);
            newFrame = NSMakeRect(frame.origin.x, top - cs.height, cs.width, cs.height);
        }

        [win setFrame:newFrame display:YES];
    }
    self.frame = NSMakeRect(0, 0, cs.width, cs.height);
    // After the window has been positioned/sized, re-capture the blur backdrop
    // so it matches the actual on-screen area.
    if (self.useBlur && wasInitial) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [self refreshBlurBackground]; });
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor clearColor] set];
    NSRectFill(dirtyRect);
    [self.skin drawInBounds:self.bounds];
}

#pragma mark - Mouse

- (void)mouseDown:(NSEvent *)event {
    _dragged = NO;
    _dragStartInWindow = [event locationInWindow];
    _windowStartOrigin = self.window.frame.origin;
}

- (void)mouseDragged:(NSEvent *)event {
    if (!_draggable) return;
    NSPoint now = [event locationInWindow];
    // locationInWindow is relative to the (moving) window; use screen delta.
    NSPoint screenNow = [self.window convertPointToScreen:now];
    NSPoint screenStart = [self.window convertPointToScreen:_dragStartInWindow];
    CGFloat dx = screenNow.x - screenStart.x;
    CGFloat dy = screenNow.y - screenStart.y;
    if (fabs(dx) > 2 || fabs(dy) > 2) _dragged = YES;
    NSRect f = self.window.frame;
    f.origin = NSMakePoint(_windowStartOrigin.x + dx, _windowStartOrigin.y + dy);
    [self.window setFrameOrigin:f.origin];
}

- (void)mouseUp:(NSEvent *)event {
    BOOL wasDragged = _dragged;
    if (_dragged) {
        // Drag complete: re-capture wallpaper area for new window position.
        if (self.useBlur) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [self refreshBlurBackground]; });
        }
        return;
    }
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [self.skin handleMouseUpAt:p rightButton:NO];
    (void)wasDragged;
}

- (void)rightMouseUp:(NSEvent *)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [self.skin handleMouseUpAt:p rightButton:YES];
}

- (void)scrollWheel:(NSEvent *)event {
    if (event.deltaY > 0) [self.skin handleScrollUp:YES];
    else if (event.deltaY < 0) [self.skin handleScrollUp:NO];
}

#pragma mark - Hover tracking

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) { [self removeTrackingArea:_trackingArea]; _trackingArea = nil; }
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited |
                                 NSTrackingMouseMoved |
                                 NSTrackingActiveAlways |
                                 NSTrackingInVisibleRect;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                 options:opts
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [self.skin handleMouseMoveAt:p];
}

- (void)mouseEntered:(NSEvent *)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [self.skin handleMouseMoveAt:p];
}

- (void)mouseExited:(NSEvent *)event {
    [self.skin handleMouseExit];
}

#pragma mark - Frosted-glass background (NSVisualEffectView)

- (void)setUseBlur:(BOOL)useBlur {
    if (_useBlur == useBlur) return;
    _useBlur = useBlur;
    if (useBlur) {
        [self installBlurEffect];
    } else {
        [_blurEffectView removeFromSuperview];
        _blurEffectView = nil;
    }
}

- (void)installBlurEffect {
    if (_blurEffectView != nil) return;

    // NSVisualEffectView must be placed as the window's contentView with
    // RMSkinView on top of it — not as a subview of RMSkinView (which would
    // cover drawRect output). We insert a new layer: window.contentView =
    // effectView, effectView contains self (the skin view).
    NSWindow *win = self.window;
    if (win == nil) return;

    _blurEffectView = [[NSVisualEffectView alloc] initWithFrame:self.bounds];
    _blurEffectView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _blurEffectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    _blurEffectView.state = NSVisualEffectStateActive;
    _blurEffectView.wantsLayer = YES;

    // Choose material based on tint.
    CGFloat r = 0, g = 0, b = 0, a = 0;
    NSColor *tint = self.blurTint;
    if (tint) {
        [[tint colorUsingColorSpace:NSColorSpace.sRGBColorSpace] getRed:&r green:&g blue:&b alpha:&a];
    }
    _blurEffectView.material = ((r + g + b) > 1.5 && a > 0.3)
        ? NSVisualEffectMaterialLight
        : NSVisualEffectMaterialHUDWindow;

    // Reparent: remove self from window, set effectView as contentView,
    // then add self back on top of the effectView.
    [self removeFromSuperview];
    _blurEffectView.frame = NSMakeRect(0, 0, win.frame.size.width, win.frame.size.height);
    win.contentView = _blurEffectView;
    self.frame = _blurEffectView.bounds;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [_blurEffectView addSubview:self];

    // Near-transparent background so the compositor can sample behind.
    if ([win.backgroundColor isEqual:[NSColor clearColor]]) {
        win.backgroundColor = [NSColor colorWithCalibratedWhite:0 alpha:0.001];
    }
}

- (void)refreshBlurBackground {
    if (!_useBlur) return;
    if (_blurEffectView == nil) [self installBlurEffect];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    // Install the blur effect now that we have a window.
    if (_useBlur && _blurEffectView == nil) {
        [self installBlurEffect];
    }
}

- (void)setFrame:(NSRect)frameRect {
    [super setFrame:frameRect];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
}

- (void)setBlurTint:(NSColor *)blurTint {
    if (_blurTint == blurTint || [_blurTint isEqual:blurTint]) return;
    _blurTint = blurTint;
    if (_useBlur && _blurEffectView != nil) {
        CGFloat r = 0, g = 0, b = 0, a = 0;
        if (blurTint) {
            [[blurTint colorUsingColorSpace:NSColorSpace.sRGBColorSpace] getRed:&r green:&g blue:&b alpha:&a];
        }
        _blurEffectView.material = ((r + g + b) > 1.5 && a > 0.3)
            ? NSVisualEffectMaterialLight
            : NSVisualEffectMaterialHUDWindow;
    }
}

#pragma mark - Window management (bang-driven)

- (void)setWindowAlpha:(CGFloat)alpha animated:(NSTimeInterval)duration {
    NSWindow *win = self.window;
    if (!win) return;

    if (duration > 0.01) {
        // Animated fade.
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = duration;
            ctx.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
            win.animator.alphaValue = alpha;
        } completionHandler:nil];
    } else {
        win.alphaValue = alpha;
    }
}

- (void)setWindowClickThrough:(BOOL)clickThrough {
    NSWindow *win = self.window;
    if (!win) return;

    if (clickThrough) {
        win.ignoresMouseEvents = YES;
        win.level = NSPopUpMenuWindowLevel;
    } else {
        win.ignoresMouseEvents = NO;
        win.level = NSNormalWindowLevel;
    }
}

- (void)setWindowPosition:(NSPoint)pos {
    NSWindow *win = self.window;
    if (!win) return;

    self.desiredScreenTopLeft = NSMakePoint(pos.x, pos.y + self.skin.contentSize.height);
    self.anchorFracX = 0;
    self.anchorFracY = 0;
    self.hasDesiredPosition = YES;
    self.targetScreenFrame = win.screen ? win.screen.frame : NSScreen.mainScreen.frame;

    NSRect newFrame = win.frame;
    newFrame.origin = pos;
    [win setFrame:newFrame display:YES];
}

@end
