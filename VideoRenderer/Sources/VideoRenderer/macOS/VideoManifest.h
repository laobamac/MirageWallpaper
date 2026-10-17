#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *const VRManifestErrorDomain;

@interface VRVideoManifest : NSObject

+ (nullable instancetype)loadFromDirectory:(NSString *)dir error:(NSError **)error;

@property (nonatomic, copy, readonly) NSString *wallpaperDirectory;
@property (nonatomic, copy, readonly) NSString *title;
@property (nonatomic, copy, readonly, nullable) NSString *preview;
@property (nonatomic, copy, readonly) NSString *videoFile;
@property (nonatomic, strong, readonly) NSURL *videoURL;
@property (nonatomic, copy, readonly) NSDictionary<NSString *, id> *userProperties;

@end

NS_ASSUME_NONNULL_END
