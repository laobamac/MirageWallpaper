#pragma once

// RMBangs — parse and execute Rainmeter !bang actions against a skin.
//
// Actions look like "[!SetVariable Scale 1][!WriteKeyValue Variables Scale 1][!Refresh]".
// Supported bangs: SetVariable, WriteKeyValue, Refresh, Redraw, Update,
// Show/Hide/ToggleMeter, ShowMeter/HideMeter, SetOption. Unknown bangs are
// ignored (logged at debug level).

#import <Foundation/Foundation.h>

@class RMSkin;

NS_ASSUME_NONNULL_BEGIN

@interface RMBangs : NSObject
+ (void)execute:(NSString *)actions onSkin:(RMSkin *)skin;
@end

NS_ASSUME_NONNULL_END
