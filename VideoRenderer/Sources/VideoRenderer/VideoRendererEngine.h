#pragma once

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

#import "VideoManifest.h"

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *const VRVideoEngineErrorDomain;

typedef NS_ENUM(NSInteger, VRVideoFillMode) {
    VRVideoFillModeCover = 0,
    VRVideoFillModeContain = 1,
    VRVideoFillModeStretch = 2,
};

typedef struct {
    VRVideoFillMode fillMode;
    float initialVolume;
    BOOL muted;
    BOOL autoplay;
    BOOL loadFromMemory;
} VRVideoEngineConfig;

@interface VRVideoRendererEngine : NSView

+ (VRVideoEngineConfig)defaultConfig;

- (instancetype)initWithFrame:(NSRect)frameRect config:(VRVideoEngineConfig)config;
- (nullable instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

- (BOOL)openWallpaper:(VRVideoManifest *)manifest error:(NSError **)error;

- (void)play;
- (void)pause;
- (void)setPlaybackRate:(float)playbackRate;
- (void)setVolume:(float)volume;
- (void)setMuted:(BOOL)muted;
- (void)setFillMode:(VRVideoFillMode)fillMode;

// Writes a still of the frame currently on screen to `path` (HEIC, falling back
// to JPEG where no HEVC encoder exists). Mirage.app installs it as the system
// desktop picture so the menu bar and Dock tint match the wallpaper.
//
// AVPlayerLayer is a protected compositing path and cannot be read back, so the
// frame comes from an AVPlayerItemVideoOutput attached to the current item —
// which also means it reflects whatever transcode or in-memory loading the
// played asset went through, unlike re-decoding the source file.
//
// Asynchronous because attaching that output only makes the decoder start
// vending pixel buffers a frame or two later; the first attempt after attach
// legitimately has nothing yet, so this retries briefly before giving up.
// `completion` runs on the main thread exactly once; NO means no frame was
// available (still starting up, or paused before the first frame).
- (void)takeSnapshotToPath:(NSString *)path
                completion:(void (^)(BOOL ok))completion;

@property (nonatomic, copy, nullable) void (^videoDidEndBlock)(void);

// Called once when the current AVPlayer item has yielded a decoded pixel buffer.
// A hidden desktop candidate is not eligible for activation before this fires.
@property (nonatomic, copy, nullable) void (^firstFrameReadyBlock)(void);

// Called on the main thread the first time playback of the current wallpaper
// fails. -openWallpaper:error: cannot detect an undecodable file — queueing
// only validates queue constraints — so without this a malformed video is a
// permanently black desktop with no diagnostic anywhere.
@property (nonatomic, copy, nullable) void (^videoDidFailBlock)(NSString *message);

// Called on the main thread while a wallpaper AVFoundation cannot decode is
// being rewritten to H.264. `fraction` runs 0...1; `done` marks the last call,
// after which playback either starts or videoDidFailBlock fires.
@property (nonatomic, copy, nullable) void (^videoTranscodeProgressBlock)(double fraction, BOOL done);

@property (nonatomic, strong, readonly) AVQueuePlayer *player;
@property (nonatomic, assign, readonly) BOOL loaded;
@property (nonatomic, assign, readonly) float playbackRate;
@property (nonatomic, assign, readonly) float volume;
@property (nonatomic, assign, readonly) BOOL muted;
@property (nonatomic, assign, readonly) VRVideoFillMode fillMode;

@end

NS_ASSUME_NONNULL_END
