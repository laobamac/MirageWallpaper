#import "VideoRendererEngine.h"
#import "VRMemoryAssetLoader.h"
#import "VRTranscoder.h"
#import "VRVideoLayout.h"

#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <mach/mach_time.h>

NSString *const VRVideoEngineErrorDomain = @"VideoRenderer.Engine";

enum {
    VRVideoEngineErrorInvalidManifest = 1,
    VRVideoEngineErrorCannotQueueItem,
};

// KVO contexts. -canInsertItem:afterItem: only validates queue constraints, so
// a truncated/garbage/unsupported file queues fine and then never produces a
// frame — the wallpaper reports success and the user gets a permanently black
// desktop with nothing on stderr or the control channel. These are the two
// signals that actually report decode failure: AVPlayerLooper fails when it
// cannot load the template asset (the common case for a bad file), and the
// current AVPlayerItem fails for anything that survives to enqueue. Note the
// looper plays COPIES of the template item, so observing the template's own
// status would never fire.
static void *kVRLooperStatusContext = &kVRLooperStatusContext;
static void *kVRCurrentItemStatusContext = &kVRCurrentItemStatusContext;
static void *kVRPresentationSizeContext = &kVRPresentationSizeContext;

// AVPlayerLooper restarts the media continuously, so end-of-item fires once per
// loop. The consumer writes a line to stdout for each one; rate-limit so a
// short clip cannot spin the control channel.
static const CFTimeInterval kVRMinItemEndInterval = 0.5;

// Neither status probe above catches a track with no decoder on this Mac: the
// container parses, the looper reports Ready and the play head advances, yet
// not one frame is ever produced. Undecodable files are normally rewritten
// before playback starts, so reaching this watchdog means something the probe
// passed still renders nothing — report it instead of showing black forever.
static const NSTimeInterval kVRFirstFrameTimeout = 10.0;

// A snapshot attempt fails benignly until the decoder starts vending into the
// output that the first attempt attaches. Bounded well inside the app-side
// 8 s snapshot timeout.
static const NSInteger kVRSnapshotAttempts = 6;
static const NSTimeInterval kVRSnapshotRetryDelay = 0.2;

static const NSInteger kVRMaxRecoveryAttempts = 5;
static const NSTimeInterval kVRRecoveryBaseDelay = 0.75;
static const NSTimeInterval kVRWakeVerifyDelay = 1.5;
static const NSInteger kVRErrorMediaServicesWereReset = -11819;
static const NSTimeInterval kVRPlaybackHealthInterval = 1.0;
static const NSInteger kVRPlaybackStallSamples = 3;
static const NSInteger kVRStablePlaybackSamples = 30;

static NSError *VRVideoEngineError(NSInteger code, NSString *description) {
    return [NSError errorWithDomain:VRVideoEngineErrorDomain
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: description }];
}

static AVLayerVideoGravity VRLayerGravityForFillMode(VRVideoFillMode mode) {
    switch (mode) {
    case VRVideoFillModeContain: return AVLayerVideoGravityResizeAspect;
    case VRVideoFillModeStretch: return AVLayerVideoGravityResize;
    case VRVideoFillModeCover:
    default: return AVLayerVideoGravityResizeAspectFill;
    }
}

static float VRClampVolume(float value) {
    if (!isfinite(value)) return 1.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float VRClampPlaybackRate(float value) {
    if (!isfinite(value)) return 1.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 2.0f) return 2.0f;
    return value;
}

// Bounds for the catch-up multiplier. 8 % closes a 200 ms gap in about 2.5 s and
// stays well below where the decoder starts dropping frames on a 4K clip.
static const double kVRSyncRateScaleMin = 0.92;
static const double kVRSyncRateScaleMax = 1.08;

// HEIC keeps a 5K still in the low hundreds of KB. Macs without an HEVC encoder
// return a null destination, so fall back to JPEG rather than writing nothing.
static BOOL VRWriteImage(CGImageRef image, NSString *path, CFStringRef type) {
    NSURL *url = [NSURL fileURLWithPath:path];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, NULL);
    if (dest == NULL) return NO;
    NSDictionary *options = @{ (id)kCGImageDestinationLossyCompressionQuality: @0.9 };
    CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)options);
    const BOOL ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok;
}

static BOOL VREncodeSnapshot(CGImageRef image, NSString *path) {
    if (image == NULL) return NO;
    if (VRWriteImage(image, path, (__bridge CFStringRef)UTTypeHEIC.identifier)) return YES;
    return VRWriteImage(image, path, (__bridge CFStringRef)UTTypeJPEG.identifier);
}

@interface VRVideoRendererEngine ()
@property (nonatomic, strong) AVQueuePlayer *player;
@property (nonatomic, strong) AVPlayerLayer *playerLayer;
@property (nonatomic, strong) AVPlayerLooper *looper;
@property (nonatomic, assign) BOOL loaded;
@property (nonatomic, assign) float playbackRate;
/// Multiplier on top of `playbackRate` while a sync is catching up. Kept
/// separate so -applyPlaybackState and -setPlaybackRate still own the
/// wallpaper's configured rate. 1.0 means no catch-up is running.
@property (nonatomic, assign) double syncRateScale;
@property (nonatomic, assign) float volume;
@property (nonatomic, assign) BOOL muted;
@property (nonatomic, assign) VRVideoFillMode fillMode;
@property (nonatomic, assign) double positionX;
@property (nonatomic, assign) double positionY;
@property (nonatomic, assign) CGSize videoPresentationSize;
@property (nonatomic, assign) BOOL positionAvailabilityReported;
@property (nonatomic, assign) BOOL canMoveX;
@property (nonatomic, assign) BOOL canMoveY;
@property (nonatomic, assign) BOOL autoplay;
@property (nonatomic, assign) BOOL loadFromMemory;
@property (nonatomic, assign) BOOL hdrEnabled;
@property (nonatomic, strong) VRMemoryAssetLoader *memoryAssetLoader;
@property (nonatomic, strong) NSMutableArray<id> *itemEndObservers;
@property (nonatomic, strong) id itemFailedObserver;
@property (nonatomic, assign) BOOL looperObserved;
@property (nonatomic, assign) BOOL failureReported;
@property (nonatomic, assign) BOOL firstFrameReported;
// Monotonic time the player entered "waiting to play at the specified rate". It
// can sit there forever if whatever it waits for never arrives, which looks like
// a freeze; the health check watches this so it can kick the player out.
@property (nonatomic, assign) double waitingSinceClock;
@property (nonatomic, assign) BOOL hostPaused;
@property (nonatomic, assign) CFTimeInterval lastItemEndReport;
@property (nonatomic, strong) AVPlayerItemVideoOutput *frameProbe;
/// Kept alive across snapshots: attaching an output makes the decoder start
/// vending pixel buffers, which does not take effect until a frame or two later.
/// A per-call output would therefore fail the first time every time.
@property (nonatomic, strong) AVPlayerItemVideoOutput *snapshotOutput;
@property (nonatomic, assign) uint64_t openGeneration;
@property (nonatomic, strong) dispatch_queue_t transcodeQueue;
@property (nonatomic, strong, nullable) NSURL *playingURL;
@property (nonatomic, assign) BOOL everProducedFrame;
@property (nonatomic, assign) NSInteger recoveryAttempts;
@property (nonatomic, assign) BOOL recoveryScheduled;
@property (nonatomic, strong) NSMutableArray<id> *systemPowerObservers;
@property (nonatomic, strong) dispatch_source_t playbackHealthTimer;
@property (nonatomic, assign) double lastPlaybackTime;
@property (nonatomic, assign) NSInteger lastLoopCount;
@property (nonatomic, assign) NSInteger stalledPlaybackSamples;
/// Remedies already tried for the current stall, before the item is rebuilt. A
/// wallpaper parked on the first frame of a new loop recovers from a kick;
/// rebuilding the item costs a multi-second black flash.
@property (nonatomic, assign) NSInteger stallNudges;
@property (nonatomic, assign) NSInteger stablePlaybackSamples;
- (void)detachLooperObserver;
- (void)removeItemEndObservers;
- (void)installItemEndObserversForLooper:(AVPlayerLooper *)looper;
- (void)handleItemDidPlayToEnd;
- (void)reportPlaybackFailure:(NSError *)error fallback:(NSString *)fallback;
- (BOOL)startPlaybackOfURL:(NSURL *)url error:(NSError **)error;
- (void)armFirstFrameWatchdogForGeneration:(uint64_t)generation deadline:(NSDate *)deadline;
- (void)applyPlaybackState;
- (void)releaseStuckLandingIfNeeded;
- (void)nudgeStalledPlayback;
- (void)scheduleRecovery;
- (void)teardownPlaybackForRestart;
- (BOOL)canRecoverFromError:(nullable NSError *)error;
- (void)observeSystemPowerTransitions;
- (void)handleSystemDidWake;
- (void)verifyPlaybackAfterWakeForGeneration:(uint64_t)generation
                                attemptsLeft:(NSInteger)attemptsLeft;
- (void)checkPlaybackHealth;
- (void)clearSnapshotOutput;
- (void)updateVideoLayout;
@end

@implementation VRVideoRendererEngine

+ (VRVideoEngineConfig)defaultConfig {
    VRVideoEngineConfig config;
    config.fillMode = VRVideoFillModeCover;
    config.positionX = 0.5;
    config.positionY = 0.5;
    config.initialVolume = 1.0f;
    config.muted = NO;
    config.autoplay = YES;
    config.loadFromMemory = NO;
    config.hdrEnabled = NO;
    return config;
}

- (instancetype)initWithFrame:(NSRect)frameRect config:(VRVideoEngineConfig)config {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layer = [CALayer layer];
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        self.layer.masksToBounds = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;

        _player = [AVQueuePlayer queuePlayerWithItems:@[]];
        _player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
        _player.automaticallyWaitsToMinimizeStalling = YES;
        _player.volume = VRClampVolume(config.initialVolume);
        _player.muted = config.muted;

        _playerLayer = [AVPlayerLayer playerLayerWithPlayer:_player];
        _playerLayer.frame = self.bounds;
        _playerLayer.autoresizingMask = 0;
        _playerLayer.backgroundColor = NSColor.blackColor.CGColor;
        _playerLayer.needsDisplayOnBoundsChange = NO;
        [self.layer addSublayer:_playerLayer];

        _volume = _player.volume;
        _muted = config.muted;
        _playbackRate = 1.0f;
        _syncRateScale = 1.0;
        _autoplay = config.autoplay;
        _hostPaused = !config.autoplay;
        _loadFromMemory = config.loadFromMemory;
        _hdrEnabled = config.hdrEnabled;
        _itemEndObservers = [NSMutableArray array];
        _systemPowerObservers = [NSMutableArray array];
        _lastPlaybackTime = NAN;
        _lastLoopCount = NSNotFound;
        _transcodeQueue = dispatch_queue_create("VideoRenderer.transcode",
                                                DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_transcodeQueue,
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        _positionX = VRClampPosition(config.positionX);
        _positionY = VRClampPosition(config.positionY);
        [self setFillMode:config.fillMode];
        [_player addObserver:self forKeyPath:@"currentItem.presentationSize"
                     options:NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionNew
                     context:kVRPresentationSizeContext];
        [self updateDynamicRangeForScreen:NSScreen.mainScreen];

        // The player outlives every wallpaper this engine opens, so these two
        // registrations are made once here and torn down once in -dealloc.
        [_player addObserver:self
                  forKeyPath:@"currentItem.status"
                     options:NSKeyValueObservingOptionNew
                     context:kVRCurrentItemStatusContext];
        __weak __typeof__(self) weakSelf = self;
        _itemFailedObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification * _Nonnull note) {
            __strong __typeof__(weakSelf) strongSelf = weakSelf;
            if (strongSelf == nil || note.object == nil) return;
            if (![strongSelf.player.items containsObject:note.object]) return;
            [strongSelf reportPlaybackFailure:note.userInfo[AVPlayerItemFailedToPlayToEndTimeErrorKey]
                                     fallback:@"video stopped: failed to play to end"];
        }];
        _playbackHealthTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                       dispatch_get_main_queue());
        dispatch_source_set_timer(_playbackHealthTimer,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kVRPlaybackHealthInterval * NSEC_PER_SEC)),
            (uint64_t)(kVRPlaybackHealthInterval * NSEC_PER_SEC),
            (uint64_t)(0.1 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(_playbackHealthTimer, ^{
            [weakSelf checkPlaybackHealth];
        });
        dispatch_resume(_playbackHealthTimer);
        [self observeSystemPowerTransitions];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (void)layout {
    [super layout];
    [self updateVideoLayout];
}

- (void)updateVideoLayout {
    CGSize source = self.player.currentItem.presentationSize;
    if (source.width > 0 && source.height > 0) self.videoPresentationSize = source;
    source = self.videoPresentationSize;
    VRVideoLayout layout = VRCalculateVideoLayout(source, self.bounds, (int)self.fillMode,
                                                   self.positionX, self.positionY,
                                                   self.layer.geometryFlipped);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.playerLayer.videoGravity = self.fillMode == VRVideoFillModeStretch
        ? AVLayerVideoGravityResize : AVLayerVideoGravityResizeAspect;
    self.playerLayer.frame = layout.frame;
    [CATransaction commit];
    if (source.width <= 0 || source.height <= 0) return;
    if (!self.positionAvailabilityReported || self.canMoveX != layout.canMoveX ||
        self.canMoveY != layout.canMoveY) {
        self.canMoveX = layout.canMoveX;
        self.canMoveY = layout.canMoveY;
        self.positionAvailabilityReported = self.positionAvailabilityBlock != nil;
        if (self.positionAvailabilityBlock) self.positionAvailabilityBlock(layout.canMoveX, layout.canMoveY);
    }
}

// KVO and NSNotification registrations that survive -dealloc crash the process,
// so every one of them is undone here. Ivars only, no property accessors.
- (void)dealloc {
    [_player removeObserver:self forKeyPath:@"currentItem.presentationSize"
                   context:kVRPresentationSizeContext];
    [_player removeObserver:self
                 forKeyPath:@"currentItem.status"
                    context:kVRCurrentItemStatusContext];
    if (_looperObserved) {
        [_looper removeObserver:self forKeyPath:@"status" context:kVRLooperStatusContext];
        _looperObserved = NO;
    }
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    if (_itemFailedObserver != nil) {
        [center removeObserver:_itemFailedObserver];
        _itemFailedObserver = nil;
    }
    if (_playbackHealthTimer != nil) {
        dispatch_source_cancel(_playbackHealthTimer);
        _playbackHealthTimer = nil;
    }
    for (id observer in _itemEndObservers) {
        [center removeObserver:observer];
    }
    [_itemEndObservers removeAllObjects];
    NSNotificationCenter *workspaceCenter = NSWorkspace.sharedWorkspace.notificationCenter;
    for (id observer in _systemPowerObservers) {
        [workspaceCenter removeObserver:observer];
    }
    [_systemPowerObservers removeAllObjects];
    [_player pause];
    [_player removeAllItems];
}

#pragma mark - Failure reporting

- (void)detachLooperObserver {
    if (!self.looperObserved) return;
    [self.looper removeObserver:self forKeyPath:@"status" context:kVRLooperStatusContext];
    self.looperObserved = NO;
}

- (BOOL)canRecoverFromError:(NSError *)error {
    if (self.playingURL == nil) return NO;
    if (self.recoveryAttempts >= kVRMaxRecoveryAttempts) return NO;
    if (self.everProducedFrame) return YES;
    return [error.domain isEqualToString:AVFoundationErrorDomain] &&
           error.code == kVRErrorMediaServicesWereReset;
}

- (void)reportPlaybackFailure:(NSError *)error fallback:(NSString *)fallback {
    if (self.failureReported || self.recoveryScheduled) return;
    if ([self canRecoverFromError:error]) {
        NSString *reason = error.localizedDescription.length > 0
            ? error.localizedDescription : fallback;
        fprintf(stderr, "VideoRenderer: playback interrupted (%s); rebuilding\n",
                reason.UTF8String ?: "unknown");
        [self scheduleRecovery];
        return;
    }
    self.failureReported = YES;
    NSString *message = error.localizedDescription.length > 0 ? error.localizedDescription : fallback;
    fprintf(stderr, "VideoRenderer: playback failed: %s\n", message.UTF8String ?: "unknown error");
    if (self.videoDidFailBlock) self.videoDidFailBlock(message);
}

- (void)teardownPlaybackForRestart {
    self.openGeneration += 1;
    [self.player pause];
    [self clearSnapshotOutput];
    [self.player removeAllItems];
    [self removeItemEndObservers];
    [self detachLooperObserver];
    self.looper = nil;
    self.memoryAssetLoader = nil;
    self.frameProbe = nil;
    self.snapshotOutput = nil;
    self.loaded = NO;
    self.failureReported = NO;
    self.firstFrameReported = NO;
    self.lastItemEndReport = 0;
    self.lastPlaybackTime = NAN;
    self.lastLoopCount = NSNotFound;
    self.stalledPlaybackSamples = 0;
    self.stallNudges = 0;
    self.stablePlaybackSamples = 0;
}

- (void)scheduleRecovery {
    if (self.recoveryScheduled) return;
    self.recoveryScheduled = YES;
    self.recoveryAttempts += 1;
    NSURL *url = self.playingURL;
    const uint64_t generation = self.openGeneration;
    const NSTimeInterval delay = kVRRecoveryBaseDelay * (NSTimeInterval)self.recoveryAttempts;
    __weak __typeof__(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        __strong __typeof__(weakSelf) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf.recoveryScheduled = NO;
        if (strongSelf.openGeneration != generation) return;
        [strongSelf teardownPlaybackForRestart];
        NSError *playError = nil;
        if (![strongSelf startPlaybackOfURL:url error:&playError]) {
            [strongSelf reportPlaybackFailure:playError
                                     fallback:@"video could not be restarted"];
        }
    });
}

- (void)observeSystemPowerTransitions {
    NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
    __weak __typeof__(self) weakSelf = self;
    void (^wake)(NSNotification *) = ^(NSNotification *note) {
        (void)note;
        [weakSelf handleSystemDidWake];
    };
    for (NSNotificationName name in @[ NSWorkspaceDidWakeNotification,
                                       NSWorkspaceScreensDidWakeNotification ]) {
        [self.systemPowerObservers addObject:
            [center addObserverForName:name
                                object:nil
                                 queue:NSOperationQueue.mainQueue
                            usingBlock:wake]];
    }
}

- (void)handleSystemDidWake {
    if (!self.loaded) return;
    [self updateDynamicRangeForScreen:self.window.screen ?: NSScreen.mainScreen];
    AVPlayerItem *current = self.player.currentItem;
    if (current != nil && current.status == AVPlayerItemStatusFailed) {
        [self reportPlaybackFailure:current.error
                           fallback:@"video failed after the system woke"];
        return;
    }
    [self applyPlaybackState];
    const uint64_t generation = self.openGeneration;
    __weak __typeof__(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kVRWakeVerifyDelay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        __strong __typeof__(weakSelf) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf.openGeneration != generation) return;
        [strongSelf verifyPlaybackAfterWakeForGeneration:generation attemptsLeft:3];
    });
}

- (void)verifyPlaybackAfterWakeForGeneration:(uint64_t)generation
                                attemptsLeft:(NSInteger)attemptsLeft {
    if (self.openGeneration != generation) return;
    if (!self.loaded || self.failureReported || self.recoveryScheduled) return;
    if (self.hostPaused || self.playbackRate <= 0.0f) return;
    AVPlayerItem *current = self.player.currentItem;
    if (current == nil) {
        [self reportPlaybackFailure:nil fallback:@"video lost its item after the system woke"];
        return;
    }
    if (current.status == AVPlayerItemStatusFailed) {
        [self reportPlaybackFailure:current.error
                           fallback:@"video failed after the system woke"];
        return;
    }
    if (self.player.timeControlStatus == AVPlayerTimeControlStatusPlaying) return;
    if (attemptsLeft <= 0) {
        [self reportPlaybackFailure:nil fallback:@"video stalled after the system woke"];
        return;
    }
    [self.player playImmediatelyAtRate:self.playbackRate];
    __weak __typeof__(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kVRWakeVerifyDelay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf verifyPlaybackAfterWakeForGeneration:generation
                                          attemptsLeft:attemptsLeft - 1];
    });
}

- (void)nudgeStalledPlayback {
    AVPlayerItem *item = self.player.currentItem;
    if (item == nil) return;

    // 1. Re-assert the rate. A player stuck at a loop seam can sit at rate 0
    //    with its item ready, so nothing ever advances the clock.
    double scale = self.syncRateScale;
    if (!isfinite(scale) || scale <= 0.0) scale = 1.0;
    self.player.rate = (float)(self.playbackRate * scale);

    // 2. A playhead parked on the seam is the other half of it: the
    //    zero-tolerance seek that put it there can land on a copy whose first
    //    frame is the last thing it will ever present. Move a little past the
    //    seam so the decoder has somewhere to go.
    double position = CMTimeGetSeconds(item.currentTime);
    if (!isfinite(position) || position < 0.25) {
        [self.player seekToTime:CMTimeMakeWithSeconds(0.10, 600)
                toleranceBefore:CMTimeMake(1, 600)
                 toleranceAfter:CMTimeMake(1, 600)];
    }

    // 3. An AVPlayerLayer that stopped getting frames only starts again once its
    //    player is reattached.
    self.playerLayer.player = nil;
    self.playerLayer.player = self.player;

    fprintf(stderr, "VideoRenderer: nudged stalled playback at %.3fs\n",
            isfinite(position) ? position : -1.0);
}

- (void)checkPlaybackHealth {
    if (!self.loaded || !self.firstFrameReported || self.failureReported ||
        self.recoveryScheduled || self.hostPaused || self.playbackRate <= 0.0f) {
        self.lastPlaybackTime = NAN;
        self.lastLoopCount = NSNotFound;
        self.stalledPlaybackSamples = 0;
        self.stallNudges = 0;
        self.stablePlaybackSamples = 0;
        return;
    }

    // A player waiting out a scheduled landing has not failed: it is holding,
    // and no stall heuristic below will ever see progress. Release it first so
    // the rest of this check runs against a player that is actually playing.
    [self releaseStuckLandingIfNeeded];

    AVPlayerItem *item = self.player.currentItem;
    if (item == nil || item.status == AVPlayerItemStatusFailed) return;
    double currentTime = CMTimeGetSeconds(item.currentTime);
    if (!isfinite(currentTime)) return;

    NSInteger loopCount = self.looper.loopCount;
    BOOL progressed = !isfinite(self.lastPlaybackTime) ||
        loopCount != self.lastLoopCount ||
        fabs(currentTime - self.lastPlaybackTime) >= 0.01;
    self.lastPlaybackTime = currentTime;
    self.lastLoopCount = loopCount;

    if (progressed) {
        self.stalledPlaybackSamples = 0;
        self.stallNudges = 0;
        self.stablePlaybackSamples += 1;
        if (self.stablePlaybackSamples >= kVRStablePlaybackSamples) {
            self.recoveryAttempts = 0;
            self.stablePlaybackSamples = kVRStablePlaybackSamples;
        }
        return;
    }

    self.stablePlaybackSamples = 0;
    self.stalledPlaybackSamples += 1;
    if (self.stalledPlaybackSamples < kVRPlaybackStallSamples) return;

    if (self.stallNudges < 2) {
        self.stallNudges += 1;
        // Retry on the next tick instead of waiting out another full window.
        self.stalledPlaybackSamples = kVRPlaybackStallSamples - 1;
        [self nudgeStalledPlayback];
        return;
    }

    NSString *reason = self.player.reasonForWaitingToPlay ?: @"none";
    NSString *message = [NSString stringWithFormat:
        @"video playback stalled at %.3fs; status=%ld reason=%@ empty=%d likely=%d",
        currentTime, (long)self.player.timeControlStatus, reason,
        item.playbackBufferEmpty, item.playbackLikelyToKeepUp];
    [self reportPlaybackFailure:nil fallback:message];
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    if (context == kVRPresentationSizeContext) {
        __weak __typeof__(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf updateVideoLayout]; });
        return;
    }
    if (context == kVRCurrentItemStatusContext) {
        AVPlayerItem *current = self.player.currentItem;
        if (current != nil && current.status == AVPlayerItemStatusFailed) {
            [self reportPlaybackFailure:current.error fallback:@"video item failed to load"];
        }
        return;
    }
    if (context == kVRLooperStatusContext) {
        AVPlayerLooper *looper = (AVPlayerLooper *)object;
        if (looper.status == AVPlayerLooperStatusFailed) {
            [self reportPlaybackFailure:looper.error
                               fallback:@"video could not be decoded or looped"];
            return;
        }
        // Re-scope the end-of-item observers: the items the looper actually
        // plays only exist once it leaves the unknown state.
        [self installItemEndObserversForLooper:looper];
        return;
    }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

#pragma mark - End of item

- (void)removeItemEndObservers {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    for (id observer in self.itemEndObservers) {
        [center removeObserver:observer];
    }
    [self.itemEndObservers removeAllObjects];
}

- (void)installItemEndObserversForLooper:(AVPlayerLooper *)looper {
    [self removeItemEndObservers];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    __weak __typeof__(self) weakSelf = self;
    // AVPlayerLooper plays COPIES of the template item, so these are the only
    // items that ever post the notification for us. Scoping the observer to
    // each of them keeps it off every other AVPlayerItem in the process.
    for (AVPlayerItem *looped in looper.loopingPlayerItems) {
        [self.itemEndObservers addObject:
            [center addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                object:looped
                                 queue:NSOperationQueue.mainQueue
                            usingBlock:^(NSNotification * _Nonnull note) {
                (void)note;
                [weakSelf handleItemDidPlayToEnd];
            }]];
    }
    if (self.itemEndObservers.count > 0) return;
    // The looper is not ready yet (or exposes no items): keep a filtered
    // process-wide observer so the event is never silently lost.
    [self.itemEndObservers addObject:
        [center addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                            object:nil
                             queue:NSOperationQueue.mainQueue
                        usingBlock:^(NSNotification * _Nonnull note) {
            __strong __typeof__(weakSelf) strongSelf = weakSelf;
            if (strongSelf == nil || note.object == nil) return;
            if (![strongSelf.player.items containsObject:note.object]) return;
            [strongSelf handleItemDidPlayToEnd];
        }]];
}

- (void)handleItemDidPlayToEnd {
    CFTimeInterval now = CACurrentMediaTime();
    if (self.lastItemEndReport > 0 && (now - self.lastItemEndReport) < kVRMinItemEndInterval) return;
    self.lastItemEndReport = now;
    if (self.videoDidEndBlock) self.videoDidEndBlock();
}

- (BOOL)openWallpaper:(VRVideoManifest *)manifest error:(NSError **)error {
    if (manifest == nil || manifest.videoURL == nil) {
        if (error != NULL) *error = VRVideoEngineError(VRVideoEngineErrorInvalidManifest,
                                                       @"invalid video wallpaper manifest");
        return NO;
    }

    [self.player pause];
    [self.player removeAllItems];
    [self removeItemEndObservers];
    [self detachLooperObserver];
    self.looper = nil;
    self.memoryAssetLoader = nil;
    self.frameProbe = nil;
    self.loaded = NO;
    self.failureReported = NO;
    self.firstFrameReported = NO;
    self.hostPaused = !self.autoplay;
    self.lastItemEndReport = 0;
    self.playingURL = nil;
    self.everProducedFrame = NO;
    self.recoveryAttempts = 0;
    self.recoveryScheduled = NO;
    self.lastPlaybackTime = NAN;
    self.lastLoopCount = NSNotFound;
    self.stalledPlaybackSamples = 0;
    self.stallNudges = 0;
    self.stablePlaybackSamples = 0;
    self.openGeneration += 1;

    NSURL *source = manifest.videoURL;
    NSURL *rewritten = [VRTranscoder playableURLForSourceURL:source];
    NSDictionary *sourceAttributes = [NSFileManager.defaultManager attributesOfItemAtPath:source.path error:nil];
    NSDictionary *cacheAttributes = [NSFileManager.defaultManager attributesOfItemAtPath:rewritten.path error:nil];
    NSDate *sourceDate = sourceAttributes[NSFileModificationDate];
    NSDate *cacheDate = cacheAttributes[NSFileModificationDate];
    BOOL cacheIsCurrent = cacheDate != nil && sourceDate != nil &&
        [cacheDate compare:sourceDate] != NSOrderedAscending;

    if ([VRTranscoder fileIsDecodable:source]) {
        if (![rewritten isEqual:source] && !cacheIsCurrent) {
            [NSFileManager.defaultManager removeItemAtURL:rewritten error:nil];
        }
        return [self startPlaybackOfURL:source error:error];
    }

    if (![rewritten isEqual:source] &&
        [NSFileManager.defaultManager fileExistsAtPath:rewritten.path] &&
        cacheIsCurrent &&
        [VRTranscoder fileIsDecodable:rewritten]) {
        return [self startPlaybackOfURL:rewritten error:error];
    }

    // Undecodable: rewrite to H.264 off the main thread. -openWallpaper: runs
    // before [app run], so blocking here would stall the whole launch.
    fprintf(stderr, "VideoRenderer: %s cannot be decoded; converting to H.264\n",
            source.lastPathComponent.UTF8String ?: "video");
    uint64_t generation = self.openGeneration;
    __weak __typeof__(self) weakSelf = self;
    // Announce the rewrite before any of it happens: the first real progress
    // callback can be seconds away on a large file, and the UI needs something
    // to show for that gap other than a black wallpaper.
    if (self.videoTranscodeProgressBlock) self.videoTranscodeProgressBlock(0.0, NO);
    dispatch_async(self.transcodeQueue, ^{
        NSError *transcodeError = nil;
        BOOL ok = [VRTranscoder transcodeFileAtURL:source
                                             toURL:rewritten
                                          progress:^(double fraction) {
            dispatch_async(dispatch_get_main_queue(), ^{
                __strong __typeof__(weakSelf) strongSelf = weakSelf;
                if (strongSelf == nil || strongSelf.openGeneration != generation) return;
                if (strongSelf.videoTranscodeProgressBlock) {
                    strongSelf.videoTranscodeProgressBlock(fraction, NO);
                }
            });
        }
                                             error:&transcodeError];
        dispatch_async(dispatch_get_main_queue(), ^{
            __strong __typeof__(weakSelf) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf.openGeneration != generation) return;
            if (strongSelf.videoTranscodeProgressBlock) {
                strongSelf.videoTranscodeProgressBlock(ok ? 1.0 : 0.0, YES);
            }
            if (!ok) {
                [strongSelf reportPlaybackFailure:transcodeError
                                        fallback:@"video format is not supported"];
                return;
            }
            NSError *playError = nil;
            if (![strongSelf startPlaybackOfURL:rewritten error:&playError]) {
                [strongSelf reportPlaybackFailure:playError
                                        fallback:@"converted video could not be played"];
            }
        });
    });
    return YES;
}

- (BOOL)startPlaybackOfURL:(NSURL *)url error:(NSError **)error {
    NSDictionary *assetOptions = @{
        AVURLAssetPreferPreciseDurationAndTimingKey: @NO,
    };
    NSURL *assetURL = url;
    if (self.loadFromMemory) {
        VRMemoryAssetLoader *loader = [VRMemoryAssetLoader loaderWithFileURL:url error:error];
        if (loader == nil) return NO;
        self.memoryAssetLoader = loader;
        assetURL = loader.assetURL;
    }
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:assetURL options:assetOptions];
    [self.memoryAssetLoader attachToAsset:asset];
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];

    if (![self.player canInsertItem:item afterItem:nil]) {
        if (error != NULL) {
            *error = VRVideoEngineError(VRVideoEngineErrorCannotQueueItem,
                                        [NSString stringWithFormat:@"cannot queue video: %@",
                                                                   url.path]);
        }
        return NO;
    }

    self.looper = [AVPlayerLooper playerLooperWithPlayer:self.player templateItem:item];
    self.player.volume = self.volume;
    self.player.muted = self.muted;
    self.playingURL = url;
    self.loaded = YES;

    // Initial delivery arms the fallback end-of-item observer immediately and
    // reports a looper that has already failed; the Ready transition then
    // re-scopes the observers to the items the looper actually plays.
    // NSKeyValueObservingOptionInitial delivers SYNCHRONOUSLY from inside
    // -addObserver:, so the flag has to be set first: an already-failed looper
    // reports the failure from that call, and a handler that re-enters
    // -openWallpaper: would otherwise reach -detachLooperObserver with the flag
    // still NO, skip the removal, and leave the looper deallocating with a live
    // KVO registration.
    self.looperObserved = YES;
    [self.looper addObserver:self
                  forKeyPath:@"status"
                     options:NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionNew
                     context:kVRLooperStatusContext];

    [self armFirstFrameWatchdogForGeneration:self.openGeneration
                                    deadline:[NSDate dateWithTimeIntervalSinceNow:
                                                 kVRFirstFrameTimeout]];

    [self applyPlaybackState];
    return YES;
}

// Polls rather than sampling once, and attaches the output to the item the looper
// is actually playing — a copy of the template, which is also why the probe
// cannot be installed up front. hasNewPixelBufferForItemTime: reads false for a
// moment after each loop restart too, so only an entire window without a single
// frame counts as failure.
- (void)armFirstFrameWatchdogForGeneration:(uint64_t)generation
                                  deadline:(NSDate *)deadline {
    __weak __typeof__(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        __strong __typeof__(weakSelf) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf.openGeneration != generation) return;
        if (strongSelf.failureReported || !strongSelf.loaded) return;

        AVPlayerItem *current = strongSelf.player.currentItem;
        if (current == nil) {
            if (deadline.timeIntervalSinceNow <= 0) {
                [strongSelf reportPlaybackFailure:nil
                                        fallback:@"video produced no playable item"];
                return;
            }
            [strongSelf armFirstFrameWatchdogForGeneration:generation deadline:deadline];
            return;
        }
        AVPlayerItemVideoOutput *probe = strongSelf.frameProbe;
        if (probe == nil || ![current.outputs containsObject:probe]) {
            probe = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            }];
            strongSelf.frameProbe = probe;
            [current addOutput:probe];
            [strongSelf armFirstFrameWatchdogForGeneration:generation deadline:deadline];
            return;
        }
        if ([probe hasNewPixelBufferForItemTime:current.currentTime]) {
            [current removeOutput:probe];
            strongSelf.frameProbe = nil;
            strongSelf.everProducedFrame = YES;
            if (!strongSelf.firstFrameReported) {
                strongSelf.firstFrameReported = YES;
                [strongSelf applyPlaybackState];
                if (strongSelf.firstFrameReadyBlock) strongSelf.firstFrameReadyBlock();
            }
            return;
        }
        if (deadline.timeIntervalSinceNow > 0) {
            [strongSelf armFirstFrameWatchdogForGeneration:generation deadline:deadline];
            return;
        }
        [current removeOutput:probe];
        strongSelf.frameProbe = nil;
        [strongSelf reportPlaybackFailure:nil
                                fallback:@"video produced no frames; format is unsupported"];
    });
}

// No NSProcessInfo activity assertion: this is background desktop decoration.
// Holding one for the whole playback lifetime raised the process to
// UserInitiated QoS and disabled App Nap, timer coalescing and automatic
// termination. AVPlayer already takes the assertions it actually needs while
// it has frames to present, and a wallpaper that gets throttled while it is
// fully occluded is the desired behaviour, not a bug.
- (void)play {
    self.hostPaused = NO;
    [self applyPlaybackState];
}

- (void)pause {
    self.hostPaused = YES;
    // A catch-up trim latched before the pause would resume the wallpaper at a
    // trimmed rate with nothing to walk it back: the sync loop holds every
    // correction while paused and a resume lands on the anchor instead. Written
    // straight to the ivar for the same reason as -setSyncRateScale:, because a
    // property assignment here re-enters that method and then tries to apply a
    // rate to a player that is being paused.
    _syncRateScale = 1.0;
    [self applyPlaybackState];
}

- (BOOL)paused {
    return self.hostPaused || self.playbackRate <= 0.0f;
}

- (void)seekToTime:(double)seconds {
    if (!self.loaded) return;
    // currentItem is briefly nil across a loop boundary. Skipping the seek there
    // is fine, the parent re-issues sync.
    AVPlayerItem *item = self.player.currentItem;
    CMTime duration = item != nil ? item.duration : kCMTimeInvalid;
    double total = CMTimeGetSeconds(duration);
    if (!isfinite(total) || total <= 0.0) return;
    if (!isfinite(seconds)) return;
    seconds = fmin(fmax(seconds, 0.0), total);
    // Zero tolerance, so every display lands on the same offset.
    [self.player seekToTime:CMTimeMakeWithSeconds(seconds, 600)
            toleranceBefore:kCMTimeZero
             toleranceAfter:kCMTimeZero];
}

- (void)setPlaybackRate:(float)playbackRate {
    _playbackRate = VRClampPlaybackRate(playbackRate);
    [self applyPlaybackState];
}

- (void)applyPlaybackState {
    if (!self.loaded) return;
    if (!self.firstFrameReported) {
        float startupRate = self.playbackRate > 0.0f ? self.playbackRate : 1.0f;
        [self.player playImmediatelyAtRate:startupRate];
        return;
    }
    if (self.hostPaused || self.playbackRate <= 0.0f) {
        [self.player pause];
        return;
    }
    // Fold in any catch-up trim. Re-applying the plain rate here would cancel a
    // trim on the next volume, mute or property replay.
    double scale = self.syncRateScale;
    if (!isfinite(scale) || scale <= 0.0) scale = 1.0;
    [self.player playImmediatelyAtRate:(float)(self.playbackRate * scale)];
}

- (void)setSyncRateScale:(double)scale {
    if (!isfinite(scale) || scale <= 0.0) scale = 1.0;
    scale = fmin(fmax(scale, kVRSyncRateScaleMin), kVRSyncRateScaleMax);
    if (fabs(scale - _syncRateScale) < 1e-4) return;
    // The ivar, not the property. This class overrides the synthesised setter,
    // so `self.syncRateScale = scale` here calls this method again with an
    // unchanged ivar, the guard above lets it through, and it recurses until the
    // stack runs out. Writing the ivar is the whole point of the override.
    _syncRateScale = scale;
    [self applySyncRateIfNeeded];
}

// Applies a catch-up trim to a player that is already running.
//
// -playImmediatelyAtRate: is the wrong tool: that is the "start now, ignore
// readiness" kick, and calling it every tick re-primes the render pipeline. On a
// 4K clip that shows as a hitch, and near a loop seam it can leave the new item
// parked on its first frame. Assigning `rate` only retimes a pipeline that is
// already playing.
//
// Assigning is not free either: AVFoundation recomputes playback timing, which
// costs about a frame. So this compares against the player's actual rate first
// and does nothing when they already agree, which makes it safe to call every
// tick and keeps a trim alive across the windows where it cannot be set (a loop
// swap, a seek that has not landed, a player briefly out of `Playing`).
- (void)applySyncRateIfNeeded {
    if (!self.loaded || !self.firstFrameReported) return;
    if (self.hostPaused || self.playbackRate <= 0.0f) return;
    if (self.player.timeControlStatus != AVPlayerTimeControlStatusPlaying) return;
    double scale = self.syncRateScale;
    if (!isfinite(scale) || scale <= 0.0) scale = 1.0;
    float want = (float)(self.playbackRate * scale);
    // AVPlayer quantises `rate`, so compare loosely: within a thousandth is the
    // value it would be set to anyway.
    if (fabs((double)self.player.rate - (double)want) < 1e-3) return;
    self.player.rate = want;
}

// Re-locates the playhead with a tolerant seek.
//
// Deliberately not -setRate:time:atHostTime:. That API pauses first and resumes
// at the given host time, which is right for starting several players together
// and wrong for nudging one that is already running: under AVPlayerLooper a
// pause near an item swap can leave the player holding with no frames until the
// swap finishes, which looks like a frozen picture. A tolerant seek moves the
// playhead without stopping playback.
//
// Returns NO when the seek cannot be issued yet. The first correction usually
// arrives while the wallpaper is still opening: `loaded` is set but the first
// frame is not decoded, and the guard below would drop the seek silently,
// leaving this renderer playing from zero. The caller needs to know so it can
// retry.
- (BOOL)seekApproximatelyTo:(double)seconds {
    if (!self.loaded || self.hostPaused || self.playbackRate <= 0.0f) return NO;
    if (!self.firstFrameReported) return NO;
    AVPlayerItem *item = self.player.currentItem;
    if (item == nil) return NO;
    double duration = CMTimeGetSeconds(item.duration);
    if (!isfinite(duration) || duration <= 0.0) return NO;
    if (!isfinite(seconds) || seconds < 0.0) return NO;
    if (seconds > duration) seconds = duration;

    // Drop any catch-up trim. The seek is about to put the playhead exactly
    // where the sync wants it, so there is nothing left to catch up with. The
    // ivar is written directly so the reset cannot be skipped, and the player's
    // rate then goes back to plain playback: a stale 1.03 left behind would keep
    // this screen fast for the rest of the loop.
    _syncRateScale = 1.0;
    [self applySyncRateIfNeeded];

    // One frame of slop. An exact seek has to decode up to the frame and
    // re-primes the pipeline, which is slower and is what used to park a looping
    // item on its first frame. A frame is nothing next to the tolerance the sync
    // keeps anyway.
    CMTime tolerance = CMTimeMake(1, 30);
    [self.player seekToTime:CMTimeMakeWithSeconds(seconds, 600)
            toleranceBefore:tolerance
             toleranceAfter:tolerance];
    return YES;
}

// Kicks a player out of an indefinite "waiting to play at the specified rate".
//
// A player can sit in that state if what it waits for never arrives. The stall
// heuristics below only look at whether the playhead moves, so a player parked
// there looks exactly like a frozen one; catch it early and restart it instead
// of waiting out the longer escalation.
- (void)releaseStuckLandingIfNeeded {
    if (self.player.timeControlStatus != AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate) {
        self.waitingSinceClock = 0.0;
        return;
    }
    double now = (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9;
    if (self.waitingSinceClock <= 0.0) {
        self.waitingSinceClock = now;
        return;
    }
    // Local files do wait briefly while a seek settles. Anything past this is a
    // wait that is not going to end on its own.
    if (now - self.waitingSinceClock < 2.0) return;

    self.waitingSinceClock = 0.0;
    NSString *reason = self.player.reasonForWaitingToPlay ?: @"none";
    fprintf(stderr, "VideoRenderer: released stalled wait (reason=%s)\n",
            reason.UTF8String);
    [self.player playImmediatelyAtRate:self.playbackRate];
}

- (double)currentPlaybackPosition {
    if (!self.loaded) return NAN;
    // With AVPlayerLooper the current item is one of several copies, so this is
    // the position inside the copy that is playing now, which is the timeline the
    // sync target is expressed in.
    AVPlayerItem *item = self.player.currentItem;
    if (item == nil) return NAN;
    double position = CMTimeGetSeconds(item.currentTime);
    return isfinite(position) ? position : NAN;
}

- (double)currentPlaybackDuration {
    if (!self.loaded) return NAN;
    AVPlayerItem *item = self.player.currentItem;
    if (item == nil) return NAN;
    double duration = CMTimeGetSeconds(item.duration);
    return (isfinite(duration) && duration > 0.0) ? duration : NAN;
}

- (void)setVolume:(float)volume {
    _volume = VRClampVolume(volume);
    self.player.volume = _volume;
}

- (void)setMuted:(BOOL)muted {
    _muted = muted;
    self.player.muted = muted;
}

- (void)takeSnapshotToPath:(NSString *)path
                completion:(void (^)(BOOL ok))completion {
    if (path.length == 0) {
        if (completion) completion(NO);
        return;
    }
    [self attemptSnapshotToPath:path attemptsLeft:kVRSnapshotAttempts
                     completion:completion];
}

- (void)attemptSnapshotToPath:(NSString *)path
                 attemptsLeft:(NSInteger)attemptsLeft
                   completion:(void (^)(BOOL ok))completion {
    if ([self copySnapshotToPath:path]) {
        [self clearSnapshotOutput];
        if (completion) completion(YES);
        return;
    }
    if (attemptsLeft <= 1) {
        [self clearSnapshotOutput];
        if (completion) completion(NO);
        return;
    }
    __weak __typeof__(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(kVRSnapshotRetryDelay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        __strong __typeof__(weakSelf) strongSelf = weakSelf;
        if (strongSelf == nil) {
            if (completion) completion(NO);
            return;
        }
        [strongSelf attemptSnapshotToPath:path
                            attemptsLeft:attemptsLeft - 1
                              completion:completion];
    });
}

/// One attempt. Attaching the output is itself an attempt that cannot succeed:
/// the decoder needs a frame or two before it vends into a new output.
- (BOOL)copySnapshotToPath:(NSString *)path {
    AVPlayerItem *item = self.player.currentItem;
    if (item == nil || item.status != AVPlayerItemStatusReadyToPlay) return NO;

    AVPlayerItemVideoOutput *output = self.snapshotOutput;
    if (output == nil || ![item.outputs containsObject:output]) {
        [self clearSnapshotOutput];
        output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        }];
        self.snapshotOutput = output;
        [item addOutput:output];
        return NO;
    }

    const CMTime now = item.currentTime;
    if (!CMTIME_IS_VALID(now)) return NO;
    CVPixelBufferRef buffer =
        [output copyPixelBufferForItemTime:now itemTimeForDisplay:NULL];
    if (buffer == NULL) return NO;

    CIImage *ciImage = [CIImage imageWithCVPixelBuffer:buffer];
    AVAssetTrack *track = [item.asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (track != nil) {
        const CGAffineTransform transform = track.preferredTransform;
        const CGAffineTransform imageTransform = CGAffineTransformMake(
            transform.a, -transform.b, -transform.c, transform.d, transform.tx, -transform.ty);
        ciImage = [ciImage imageByApplyingTransform:imageTransform];
    }
    CGRect extent = ciImage.extent;
    CGRect target = [self convertRectToBacking:self.bounds];
    target.origin = CGPointZero;
    target.size.width = round(target.size.width);
    target.size.height = round(target.size.height);
    CGSize source = item.presentationSize;
    if (extent.size.width <= 0 || extent.size.height <= 0 || source.width <= 0 ||
        source.height <= 0 || target.size.width <= 0 || target.size.height <= 0) {
        CVPixelBufferRelease(buffer);
        return NO;
    }
    VRVideoLayout layout = VRCalculateVideoLayout(source, target, (int)self.fillMode,
                                                   self.positionX, self.positionY, false);
    ciImage = [ciImage imageByApplyingTransform:CGAffineTransformMakeTranslation(-extent.origin.x, -extent.origin.y)];
    ciImage = [ciImage imageByApplyingTransform:CGAffineTransformMakeScale(
        layout.frame.size.width / extent.size.width, layout.frame.size.height / extent.size.height)];
    ciImage = [ciImage imageByApplyingTransform:CGAffineTransformMakeTranslation(
        layout.frame.origin.x, layout.frame.origin.y)];
    CIImage *background = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:0 alpha:1]]
        imageByCroppingToRect:target];
    ciImage = [[ciImage imageByCompositingOverImage:background] imageByCroppingToRect:target];
    CIContext *context = [CIContext contextWithOptions:nil];
    CGImageRef cgImage = [context createCGImage:ciImage fromRect:ciImage.extent];
    CVPixelBufferRelease(buffer);
    if (cgImage == NULL) return NO;

    const BOOL ok = VREncodeSnapshot(cgImage, path);
    CGImageRelease(cgImage);
    return ok;
}

- (void)clearSnapshotOutput {
    AVPlayerItemVideoOutput *output = self.snapshotOutput;
    if (output == nil) return;
    for (AVPlayerItem *item in self.player.items) {
        if ([item.outputs containsObject:output]) [item removeOutput:output];
    }
    self.snapshotOutput = nil;
}

- (void)setFillMode:(VRVideoFillMode)fillMode {
    _fillMode = fillMode;
    [self updateVideoLayout];
}

- (void)setPositionX:(double)x y:(double)y {
    _positionX = VRClampPosition(x);
    _positionY = VRClampPosition(y);
    [self updateVideoLayout];
}

- (void)setHDREnabled:(BOOL)enabled {
    _hdrEnabled = enabled;
    [self updateDynamicRangeForScreen:self.window.screen ?: NSScreen.mainScreen];
}

- (void)updateDynamicRangeForScreen:(NSScreen *)screen {
    BOOL enabled = self.hdrEnabled && screen != nil &&
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
    if (@available(macOS 26.0, *)) {
        CADynamicRange range = enabled ? CADynamicRangeConstrainedHigh : CADynamicRangeStandard;
        self.layer.preferredDynamicRange = range;
        self.playerLayer.preferredDynamicRange = range;
    } else if (@available(macOS 14.0, *)) {
        self.layer.wantsExtendedDynamicRangeContent = enabled;
        self.playerLayer.wantsExtendedDynamicRangeContent = enabled;
    }
}

@end
