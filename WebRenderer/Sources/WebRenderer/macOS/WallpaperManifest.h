#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Wallpaper Engine "web" wallpaper manifest, parsed from project.json.
// Mirrors OWE's weweb::WebManifest: validates type == "web" (case-insensitive),
// exposes entry HTML, title, and general.properties (verbatim, for the page's
// wallpaperPropertyListener.applyUserProperties).
@interface WRManifest : NSObject

@property (nonatomic, copy, readonly) NSString *workshopDir;
@property (nonatomic, copy, readonly) NSString *entryHTML;   // default "index.html"
@property (nonatomic, strong, readonly) NSURL *entryURL;
@property (nonatomic, copy, readonly) NSString *title;       // default "Wallpaper"
@property (nonatomic, copy, readonly, nullable) NSString *preview;
@property (nonatomic, strong, readonly, nullable) NSDictionary *userProperties; // general.properties
@property (nonatomic, copy, readonly) NSString *userPropertiesJSON;             // for JS injection

+ (nullable instancetype)loadFromDirectory:(NSString *)dir error:(NSError **)error;

- (instancetype)init NS_UNAVAILABLE;

@end

NS_ASSUME_NONNULL_END
