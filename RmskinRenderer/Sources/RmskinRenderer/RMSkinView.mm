#import "RMSkinView.h"
#import "RMSkin.h"
#import "RMLog.h"
#import <CoreImage/CoreImage.h>

@implementation RMSkinView {
    NSTimer *_timer;
    NSPoint _dragStartInWindow;
    NSPoint _windowStartOrigin;
    BOOL    _dragged;
    NSSize  _lastContentSize;
    BOOL    _hasPerformedInitialLayout;
    NSTrackingArea *_trackingArea;
    NSImageView *_blurImageView;
    BOOL    _hasPerformedBlurCapture;
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

#pragma mark - Frosted-glass background

// Capture the desktop content behind this view's window. Uses
// CGWindowListCreateImage to grab on-screen pixels beneath the window level,
// which captures the wallpaper (and any other windows that happen to be
// behind us — these typically aren't there because we sit at desktop level).
- (NSImage *)captureBackgroundForBlur {
    NSWindow *win = self.window;
    if (win == nil) return nil;
    NSRect winFrame = win.frame;
    CGFloat sx = winFrame.origin.x;
    CGFloat sy = winFrame.origin.y;
    CGFloat sw = winFrame.size.width;
    CGFloat sh = winFrame.size.height;
    if (sw < 1 || sh < 1) return nil;

    // CGWindowListCreateImage uses bottom-left coordinates; the window's
    // `frame` is in screen coordinates (bottom-left origin), so it matches.
    CGImageRef cg = CGWindowListCreateImage(CGRectMake(sx, sy, sw, sh),
                                            kCGWindowListOptionOnScreenBelowWindow,
                                            kCGNullWindowID,
                                            kCGWindowImageDefault);
    if (cg == NULL) return nil;
    NSSize size = NSMakeSize(CGImageGetWidth(cg), CGImageGetHeight(cg));
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:size];
    CGImageRelease(cg);
    return img;
}

// Apply CIGaussianBlur to an NSImage and return a new NSImage at the same size.
- (NSImage *)applyBlur:(NSImage *)src radius:(CGFloat)radius {
    NSLog(@"[Rmskin] applyBlur NEW CODE (imageByClampingToExtent)");
    if (src == nil) return nil;
    CGImageRef cg = [src CGImageForProposedRect:NULL context:nil hints:nil];
    if (cg == NULL) return nil;
    CIImage *ci = [CIImage imageWithCGImage:cg];
    // Remember the original extent so we can crop the (expanded) blurred output
    // back to the source dimensions.
    CGRect srcExtent = ci.extent;
    CIFilter *blur = [CIFilter filterWithName:@"CIGaussianBlur"];
    // Clamp the input to its extent so the blur samples edge pixels instead of
    // transparent black, keeping the borders from darkening/fading.
    [blur setValue:[ci imageByClampingToExtent] forKey:kCIInputImageKey];
    [blur setValue:@(radius) forKey:kCIInputRadiusKey];
    CIImage *out = blur.outputImage;
    if (out == nil) return nil;
    CIContext *ctx = [CIContext context];
    CGImageRef outCg = [ctx createCGImage:out fromRect:srcExtent];
    if (outCg == NULL) return nil;
    NSSize outSize = NSMakeSize(CGImageGetWidth(outCg), CGImageGetHeight(outCg));
    NSImage *result = [[NSImage alloc] initWithCGImage:outCg size:outSize];
    CGImageRelease(outCg);
    return result;
}

// Composite the tint colour on top of the blurred image and return a single
// NSImage suitable for direct drawing as a backdrop.
- (NSImage *)compositeTint:(NSImage *)src color:(NSColor *)tint {
    if (src == nil) return nil;
    NSSize sz = src.size;
    NSImage *result = [[NSImage alloc] initWithSize:sz];
    [result lockFocus];
    [src drawInRect:NSMakeRect(0, 0, sz.width, sz.height)
           fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
    [tint setFill];
    NSRectFillUsingOperation(NSMakeRect(0, 0, sz.width, sz.height), NSCompositingOperationSourceOver);
    [result unlockFocus];
    return result;
}

- (void)setUseBlur:(BOOL)useBlur {
    if (_useBlur == useBlur) return;
    _useBlur = useBlur;
    if (useBlur) {
        [self refreshBlurBackground];
    } else {
        [_blurImageView removeFromSuperview];
        _blurImageView = nil;
    }
}

- (void)refreshBlurBackground {
    if (!_useBlur) return;
    NSWindow *win = self.window;
    if (win == nil) {
        // Defer until we have a window.
        _hasPerformedBlurCapture = NO;
        return;
    }
    NSImage *captured = [self captureBackgroundForBlur];
    if (captured == nil) return;
    NSImage *blurred = [self applyBlur:captured radius:self.blurRadius];
    if (blurred == nil) return;
    NSImage *backdrop = [self compositeTint:blurred color:self.blurTint ?: [NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:0.5]];
    if (backdrop == nil) return;
    if (_blurImageView == nil) {
        _blurImageView = [[NSImageView alloc] initWithFrame:self.bounds];
        _blurImageView.imageScaling = NSImageScaleProportionallyUpOrDown;
        _blurImageView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    _blurImageView.image = backdrop;
    _blurImageView.frame = self.bounds;
    if (_blurImageView.superview == nil) {
        [self addSubview:_blurImageView positioned:NSWindowBelow relativeTo:nil];
    }
    _hasPerformedBlurCapture = YES;
}

// Re-capture when the window's frame changes (e.g. after move/resize).
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    // The blur capture is handled in resizeToContent (after the window has
    // been sized and positioned). Doing it here would race with the
    // positioning logic in RmskinWallpaper and capture the wrong area.
}

- (void)setFrame:(NSRect)frameRect {
    [super setFrame:frameRect];
    if (self.useBlur && _blurImageView) {
        _blurImageView.frame = self.bounds;
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (self.useBlur && _blurImageView) {
        _blurImageView.frame = self.bounds;
    }
}

@end
