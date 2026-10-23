#pragma once

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Owns one immutable in-memory video and serves AVFoundation byte-range
// requests through a custom URL. No request reopens the source file.
@interface VRMemoryAssetLoader : NSObject <AVAssetResourceLoaderDelegate>

+ (nullable instancetype)loaderWithFileURL:(NSURL *)fileURL error:(NSError **)error;

@property (nonatomic, strong, readonly) NSURL *assetURL;
@property (nonatomic, assign, readonly) NSUInteger length;

- (void)attachToAsset:(AVURLAsset *)asset;

@end

NS_ASSUME_NONNULL_END
