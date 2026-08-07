#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *const VRTranscoderErrorDomain;

// Reported from the transcode queue, in [0, 1].
typedef void (^VRTranscodeProgressBlock)(double fraction);

@interface VRTranscoder : NSObject

// Whether AVFoundation can actually turn this file into frames. A successfully
// parsed container is not enough: a VP9 or AV1 track in an MP4 loads, reports
// ready and even advances the play head on a Mac with no decoder for it, while
// never producing a single frame. Only the per-track decodable flag catches it.
+ (BOOL)fileIsDecodable:(NSURL *)url;

// Where the H.264 rewrite of `url` belongs. MP4-family containers keep their
// exact path — the rewrite is byte-for-byte substitutable there, so project.json
// and every other reference stay valid. Other containers get an .mp4 sibling,
// since MP4 content behind a .webm name would not be probed correctly.
+ (NSURL *)playableURLForSourceURL:(NSURL *)url;

// Decodes with libav* and re-encodes to H.264 (plus AAC when the source has
// audio) through AVAssetWriter, keeping the output on Apple's own encoder and
// muxer. Writes a temporary file first and verifies it is decodable before
// moving it onto `destinationURL`, so a crash or a broken encode can never
// leave the wallpaper without a playable file. Returns NO and leaves everything
// untouched on failure.
+ (BOOL)transcodeFileAtURL:(NSURL *)sourceURL
                     toURL:(NSURL *)destinationURL
                  progress:(nullable VRTranscodeProgressBlock)progress
                     error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
