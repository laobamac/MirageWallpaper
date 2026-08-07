#pragma once

// RMLayout — parses an installed .rmskin theme's RMSKIN.ini metadata and, for
// LoadType=Layout, the Layout's Rainmeter.ini to discover which skin configs
// are Active and where (WindowX/WindowY). Produces one entry per widget window.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface RMLayoutEntry : NSObject
@property (nonatomic, copy) NSString *skinFile;        // absolute .ini path
@property (nonatomic, copy) NSString *resourcesPath;   // rootConfig/@Resources
@property (nonatomic, copy) NSString *rootConfigPath;  // Skins/<root>
@property (nonatomic, copy) NSString *configName;      // "root\\config"

// WindowX/WindowY: the requested position. Interpretation depends on the
// *Percent flags: absolute pixels, or a fraction (0..1) of the target
// screen's frame. Rainmeter accepts either "300" or "50%".
@property (nonatomic, assign) CGFloat windowX;
@property (nonatomic, assign) CGFloat windowY;
@property (nonatomic, assign) BOOL    windowXPercent;
@property (nonatomic, assign) BOOL    windowYPercent;

// True when the position value contains Rainmeter formulas/variables that
// our parser cannot resolve (e.g. "(#WORKAREAWIDTH#/2-416)" → 0). The host
// should provide a sensible default position for such entries.
@property (nonatomic, assign) BOOL    windowXUnresolved;
@property (nonatomic, assign) BOOL    windowYUnresolved;

// AnchorX/AnchorY: the point inside the widget content that lines up with
// WindowX/WindowY. Same percent semantics but applied to the widget size.
@property (nonatomic, assign) CGFloat anchorX;
@property (nonatomic, assign) CGFloat anchorY;
@property (nonatomic, assign) BOOL    anchorXPercent;
@property (nonatomic, assign) BOOL    anchorYPercent;

@property (nonatomic, assign) BOOL draggable;
@end

@interface RMLayout : NSObject

@property (nonatomic, copy, readonly) NSString *themeDirectory;
@property (nonatomic, copy, readonly) NSString *skinsPath;      // themeDir/Skins
@property (nonatomic, copy, readonly, nullable) NSString *name;
@property (nonatomic, copy, readonly, nullable) NSString *author;
@property (nonatomic, copy, readonly, nullable) NSString *version;
@property (nonatomic, copy, readonly, nullable) NSString *loadType;   // Layout | Skin
@property (nonatomic, copy, readonly, nullable) NSString *load;       // layout name / skin path

- (nullable instancetype)initWithThemeDirectory:(NSString *)dir;

// Active widget entries to render (folders that actually exist on disk).
- (NSArray<RMLayoutEntry *> *)activeEntries;

@end

NS_ASSUME_NONNULL_END
