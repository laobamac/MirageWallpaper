#pragma once

// RMSkin — runtime for a single Rainmeter skin config.
//
// Loads one skin .ini (via RMConfigParser), builds its measures and meters,
// ticks them on the [Rainmeter] Update interval, computes the content size
// (DynamicWindowSize), and draws the composited result. Also dispatches mouse
// actions and !Bangs.

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

@class RMMeasure;
@class RMMeter;
@class RMConfigParser;

NS_ASSUME_NONNULL_BEGIN

@interface RMSkin : NSObject

@property (nonatomic, strong, readonly) RMConfigParser *parser;
@property (nonatomic, strong, readonly) NSArray<RMMeasure *> *measures;
@property (nonatomic, strong, readonly) NSArray<RMMeter *> *meters;

@property (nonatomic, assign, readonly) NSTimeInterval updateInterval; // seconds
@property (nonatomic, assign, readonly) BOOL dynamicWindowSize;
@property (nonatomic, assign, readonly) NSSize contentSize;

// YES when the skin uses a FrostedGlass plugin measure, or declares Blur=1 /
// BlurRegion in [Rainmeter] (Rainmeter's built-in frosted-glass backdrop).
// When YES, the widget window samples the desktop behind it and renders a
// blurred tinted backdrop.
@property (nonatomic, assign, readonly) BOOL useBlur;

// Optional tint color for the blur backdrop, set via BlurTint=R,G,B,A in
// [Rainmeter] (light themes use a white tint, dark themes use black).
@property (nonatomic, strong, readonly, nullable) NSColor *blurTint;

@property (nonatomic, copy, nullable) NSString *mouseScrollUpAction;
@property (nonatomic, copy, nullable) NSString *mouseScrollDownAction;

// Skin-level actions (Rainmeter [Rainmeter] section equivalents).
@property (nonatomic, copy, nullable) NSString *onRefreshAction;
@property (nonatomic, copy, nullable) NSString *onUpdateAction;

// Window management properties (driven by !SetTransparency, !ClickThrough, !Fade).
// Setting these triggers the view to update its window accordingly.
@property (nonatomic, assign) CGFloat windowAlpha;       // 0..1, 1=opaque
@property (nonatomic, assign) BOOL    clickThrough;      // window ignores mouse events
@property (nonatomic, assign) NSTimeInterval fadeDuration; // seconds for fade animation
@property (nonatomic, assign) NSPoint  windowPosition;   // set via !Move

// Callbacks installed by RMSkinView / RMSkinWallpaper host.
@property (nonatomic, copy, nullable) void (^onWindowAlphaChanged)(CGFloat alpha, NSTimeInterval duration);
@property (nonatomic, copy, nullable) void (^onClickThroughChanged)(BOOL clickThrough);
@property (nonatomic, copy, nullable) void (^onWindowPositionChanged)(NSPoint pos);

// skinFile:   absolute path of the config's .ini
// resources:  #@# target (root config's @Resources)
// rootConfig: skin root folder (contains @Resources)
// skinsPath:  the Skins/ root
// config:     display config name, e.g. "# - TETRAKTYS\\SYSTEM INFO"
- (nullable instancetype)initWithSkinFile:(NSString *)skinFile
                                resources:(NSString *)resources
                               rootConfig:(NSString *)rootConfig
                                skinsPath:(NSString *)skinsPath
                                   config:(NSString *)config;

// Re-parse the skin from disk and rebuild measures/meters.
- (BOOL)reload;

// Advance one tick: update measures, prepare meters, recompute content size.
- (void)tick;

// Draw into the current AppKit graphics context (flipped, top-left origin),
// within a view of the given bounds.
- (void)drawInBounds:(NSRect)bounds;

// Hit-test a point (skin coordinates, top-left origin) and run its action.
- (void)handleMouseUpAt:(NSPoint)point rightButton:(BOOL)rightButton;
- (void)handleScrollUp:(BOOL)up;

// Track hover: fires MouseOverAction / MouseLeaveAction on enter/leave of each
// meter. Call handleMouseMoveAt: on move and handleMouseExit when the pointer
// leaves the widget entirely.
- (void)handleMouseMoveAt:(NSPoint)point;
- (void)handleMouseExit;

// Execute a bang-action string like "[!SetVariable X 1][!Refresh]".
- (void)executeActions:(NSString *)actions;

// Bang support hooks.
- (void)setOption:(NSString *)key value:(NSString *)value forSection:(NSString *)section;
- (void)requestRedraw;
@property (nonatomic, copy, nullable) void (^onNeedsRedraw)(void);

// Look-ups.
- (nullable RMMeter *)meterNamed:(NSString *)name;
- (nullable RMMeasure *)measureNamed:(NSString *)name;
- (NSArray<RMMeter *> *)metersInGroup:(NSString *)group;
- (NSArray<RMMeasure *> *)measuresInGroup:(NSString *)group;

@end

NS_ASSUME_NONNULL_END
