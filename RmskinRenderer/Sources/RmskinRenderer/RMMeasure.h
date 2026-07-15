#pragma once

// RMMeasure — data sources (base class + factory).
//
// A measure produces a numeric value and/or a string each update tick. Meters
// bind to measures by name (MeasureName, or %1..%N). Mirrors Rainmeter's
// Measure base + concrete subclasses; ported to macOS system APIs.
//
// Supported types: Time, Calc, CPU, Memory / PhysicalMemory / SwapMemory,
// FreeDiskSpace, Uptime, Net / NetIn / NetOut. WebParser and Plugin are
// accepted but degrade to empty output (network scraping / native plugins are
// out of scope for the first version).

#import <Foundation/Foundation.h>

@class RMConfigParser;

NS_ASSUME_NONNULL_BEGIN

@interface RMMeasure : NSObject

@property (nonatomic, copy)   NSString *name;          // section name
@property (nonatomic, weak)   RMConfigParser *parser;

@property (nonatomic, assign) double value;            // current numeric value
@property (nonatomic, assign) double minValue;
@property (nonatomic, assign) double maxValue;
@property (nonatomic, assign) BOOL   disabled;
@property (nonatomic, assign) BOOL   invert;
@property (nonatomic, assign) int    updateDivider;
@property (nonatomic, assign) int    defaultUpdateDivider;  // fallback from [Rainmeter] DefaultUpdateDivider
@property (nonatomic, assign) int    updateCounter;
@property (nonatomic, copy, nullable) NSString *group;

// True for measures whose primary output is text (Time, WebParser).
@property (nonatomic, assign, readonly) BOOL isStringMeasure;

+ (nullable RMMeasure *)measureWithType:(NSString *)type
                                   name:(NSString *)name
                                 parser:(RMConfigParser *)parser;

// Read common + subclass options from the measure's section.
- (void)readOptions;

// Recompute value / string. Honours UpdateDivider. Call once per skin tick.
- (void)update;

// Raw string produced by a string-measure (before number formatting).
- (nullable NSString *)rawString;

// Numeric value usable in Calc formulas. For string measures whose text is a
// number (e.g. a Time measure with Format="%d" → "15"), returns that number;
// otherwise returns `value`.
- (double)numericValue;

// Final display string for a bound meter. autoScale/decimals/percentual/scale
// come from the *meter*; string-measures ignore the numeric formatting.
- (NSString *)displayStringAutoScale:(BOOL)autoScale
                            decimals:(int)decimals
                          percentual:(BOOL)percentual
                               scale:(double)scale;

// Apply this measure's Substitute rules to a string.
- (NSString *)applySubstitute:(NSString *)input;

// Callback installed by RMSkin to execute bang strings (IfAboveAction, etc.).
@property (nonatomic, copy, nullable) void (^executeAction)(NSString *bang);

@end

NS_ASSUME_NONNULL_END
