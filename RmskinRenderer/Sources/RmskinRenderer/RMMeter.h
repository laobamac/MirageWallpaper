#pragma once

// RMMeter — visual elements (base class + factory).
//
// A meter occupies a rectangle (X/Y/W/H) inside the skin and draws using
// AppKit into the flipped RMSkinView (top-left origin, matching Rainmeter).
// Meters bind to measures by name; MeterString supports %1..%N substitution.
//
// Supported: Image, String, Bar, Line, Histogram, Rotator, RoundLine,
// Shape (rectangle/line subset), Bitmap. Relative positioning (Nr / nR) is
// resolved during layout by RMSkin.

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

@class RMConfigParser;
@class RMMeasure;

NS_ASSUME_NONNULL_BEGIN

@interface RMMeter : NSObject

@property (nonatomic, copy)   NSString *name;
@property (nonatomic, weak)   RMConfigParser *parser;

@property (nonatomic, assign) CGFloat x;
@property (nonatomic, assign) CGFloat y;
@property (nonatomic, assign) CGFloat w;
@property (nonatomic, assign) CGFloat h;
@property (nonatomic, assign) BOOL    hidden;
@property (nonatomic, assign) BOOL    antiAlias;
@property (nonatomic, copy, nullable) NSString *group;

// Raw X/Y strings for relative-position resolution ("5R", "r", "10r").
@property (nonatomic, copy, nullable) NSString *rawX;
@property (nonatomic, copy, nullable) NSString *rawY;
@property (nonatomic, assign) BOOL relativeX;   // X ends with 'r' or 'R'
@property (nonatomic, assign) BOOL relativeY;
// If relative, YES means 'R' (relative to prev's bottom-right = x+w / y+h);
// NO means 'r' (relative to prev's top-left = x / y). Rainmeter semantics.
@property (nonatomic, assign) BOOL relativeXBR;
@property (nonatomic, assign) BOOL relativeYBR;

// Authored (parsed) X/Y — the value written in the .ini. Used to reset the
// live x/y each tick so relative-positioning and alignment offsets never
// accumulate across frames.
@property (nonatomic, assign) CGFloat authoredX;
@property (nonatomic, assign) CGFloat authoredY;

// Bound measure names (MeasureName, MeasureName2, ...) and resolved objects.
@property (nonatomic, strong) NSArray<NSString *> *measureNames;
@property (nonatomic, strong) NSArray<RMMeasure *> *measures;

// Mouse actions (raw bang strings), executed by RMSkin on click.
@property (nonatomic, copy, nullable) NSString *leftMouseUpAction;
@property (nonatomic, copy, nullable) NSString *rightMouseUpAction;
@property (nonatomic, copy, nullable) NSString *middleMouseUpAction;
@property (nonatomic, copy, nullable) NSString *mouseOverAction;
@property (nonatomic, copy, nullable) NSString *mouseLeaveAction;
// Transient hover state used to fire MouseOver/MouseLeave on transitions.
@property (nonatomic, assign) BOOL hovered;

// Container (Rainmeter "Container=MeterX"): the meter is drawn with its
// origin shifted to MeterX's frame, then clipped to MeterX's frame. We
// resolve and apply the shift in RMSkin.drawInBounds; individual meter
// draw methods just use `self.x`/`self.y` as if the container's origin
// were the widget root.
@property (nonatomic, copy, nullable) NSString *container;

+ (nullable RMMeter *)meterWithType:(NSString *)type
                               name:(NSString *)name
                             parser:(RMConfigParser *)parser;

- (void)readOptions;

// Resolve relative X/Y ("5R" etc.) against the previously laid-out meter.
- (void)resolvePositionWithPrevious:(nullable RMMeter *)prev;

// Reset the live x/y to the authored (parsed) values. Called each tick before
// relative resolution so offsets never accumulate.
- (void)resetToAuthoredPosition;

// Recompute derived dimensions (e.g. String auto-size). Called each tick after
// measures update, before drawing.
- (void)prepare;

// Draw into the current AppKit graphics context (flipped, top-left origin).
- (void)draw;

// Read and return an affine transform ("a;b;c;d;tx;ty") or nil.
- (nullable NSAffineTransform *)transformationMatrix;

@property (nonatomic, assign, readonly) NSRect frame;   // {x,y,w,h}

@end

NS_ASSUME_NONNULL_END
