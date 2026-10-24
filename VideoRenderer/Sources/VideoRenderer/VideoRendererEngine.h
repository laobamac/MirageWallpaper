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
    double positionX;
    double positionY;
    float initialVolume;
    BOOL muted;
    BOOL autoplay;
    BOOL loadFromMemory;
    BOOL hdrEnabled;
} VRVideoEngineConfig;

@interface VRVideoRendererEngine : NSView

+ (VRVideoEngineConfig)defaultConfig;

- (instancetype)initWithFrame:(NSRect)frameRect config:(VRVideoEngineConfig)config;
- (nullable instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

- (BOOL)openWallpaper:(VRVideoManifest *)manifest error:(NSError **)error;

- (void)play;
- (void)pause;
// True while playback is halted, either because the host asked for a pause or
// because the configured rate is zero. The sync code reads this as "stopped
// clock": nothing may be corrected against a frozen playhead, and the screen
// lands on the anchor once it plays again.
@property (nonatomic, assign, readonly) BOOL paused;
// Jumps the playhead to `seconds` (clamped to the item duration), zero tolerance.
- (void)seekToTime:(double)seconds;
- (void)setPlaybackRate:(float)playbackRate;
// Cross-display sync. `scale` multiplies the configured rate while this renderer
// catches up with the anchor; 1.0 is normal speed. Out-of-range values are
// clamped, and it never resumes a paused player.
//
// Only for a deliberate, sustained change. Assigning `rate` makes AVFoundation
// recompute its playback timing, so driving this from a periodic tick costs a
// dropped frame every tick.
- (void)setSyncRateScale:(double)scale;
// Makes the player's rate match the last `-setSyncRateScale:` value.
//
// Does nothing when the rates already agree. That matters twice: a scale has to
// survive the windows where `rate` cannot be set (a loop swap, a seek still
// landing, a player briefly out of `Playing`), and it has to come back without
// paying for an assignment on every tick.
- (void)applySyncRateIfNeeded;

// Re-locates the playhead for cross-display sync.
//
// Tolerant seek, a frame either way, instead of an exact one: an exact seek has
// to decode up to the frame and re-primes the pipeline, which is slower and is
// what used to park a looping item on its first frame. A frame of slop is far
// below the tolerance the sync keeps anyway. Does not touch `rate`.
- (BOOL)seekApproximatelyTo:(double)seconds;
// Position and duration of the item playing right now, in the timeline the sync
// target uses. NAN when unknown, i.e. nothing loaded or read across a loop
// restart, in which case the caller should re-anchor instead of correcting.
- (double)currentPlaybackPosition;
- (double)currentPlaybackDuration;
- (void)setVolume:(float)volume;
- (void)setMuted:(BOOL)muted;
- (void)setFillMode:(VRVideoFillMode)fillMode;
- (void)setPositionX:(double)x y:(double)y;
- (void)setHDREnabled:(BOOL)enabled;
- (void)updateDynamicRangeForScreen:(nullable NSScreen *)screen;

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
@property (nonatomic, copy, nullable) void (^positionAvailabilityBlock)(BOOL x, BOOL y);

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
@property (nonatomic, assign, readonly) BOOL hdrEnabled;

@end

NS_ASSUME_NONNULL_END
