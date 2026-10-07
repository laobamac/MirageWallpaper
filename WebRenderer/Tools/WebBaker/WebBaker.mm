//
//  WebBaker.mm
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import "BakeSupport.h"
#import "WebRendererEngine.h"
#import "WallpaperManifest.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <atomic>
#include <cmath>

@interface MBWebBakeWindow : NSWindow
@end

@implementation MBWebBakeWindow
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen *)screen { return frame; }
- (NSWindowOcclusionState)occlusionState { return NSWindowOcclusionStateVisible; }
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface MBWebBake : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, strong) NSDictionary *request;
@property(nonatomic, strong) WRManifest *manifest;
@property(nonatomic, strong) WebRendererEngine *engine;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic, strong) MBBakeWriter *writer;
@property(nonatomic, strong) dispatch_queue_t queue;
- (void)start;
@end

@implementation MBWebBake {
    std::atomic<bool> _finished;
    std::atomic<double> _lastFrame;
    std::atomic<bool> _ready;
    std::atomic<bool> _idle;
    double _startedAt;
    int64_t _reportedFrame;
    dispatch_source_t _timer;
    int64_t _frame;
    double _origin;
    CIImage *_previous;
}
- (void)shutdown {
    if (_timer) { dispatch_source_cancel(_timer); _timer = nil; }
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter removeObserver:self];
        self.engine.contentReadyHandler = nil;
        [self.engine setPaused:YES];
        [self.window orderOut:nil];
        [self.window close];
        self.window = nil;
        self.engine = nil;
        if (self.stream) {
            [self.stream stopCaptureWithCompletionHandler:^(NSError *error) {
                dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
            }];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        } else { [NSApp terminate:nil]; }
    });
}
- (void)fail:(NSString *)code {
    dispatch_async(self.queue, ^{
        if (self->_finished.exchange(true)) return;
        [self.writer cancel];
        MBEvent(@"error", @{@"code":code});
        [self shutdown];
    });
}
- (BOOL)positionOffscreen {
    NSArray<NSScreen *> *screens = NSScreen.screens;
    if (screens.count == 0) return NO;
    NSRect bounds = screens.firstObject.frame;
    for (NSScreen *screen in screens) bounds = NSUnionRect(bounds, screen.frame);
    [self.window setFrameOrigin:NSMakePoint(NSMaxX(bounds) + 128, NSMinY(bounds))];
    for (NSScreen *screen in screens) if (NSIntersectsRect(self.window.frame, screen.frame)) return NO;
    return YES;
}
- (void)screensChanged:(NSNotification *)notification {
    if (!_finished && ![self positionOffscreen]) [self fail:@"capture_failed"];
}
- (void)start {
    self.queue = dispatch_queue_create("cn.laobamac.Mirage.web-bake", DISPATCH_QUEUE_SERIAL);
    MBEvent(@"preparing", @{});
    _startedAt = NSProcessInfo.processInfo.systemUptime;
    _lastFrame = _startedAt;
    self.writer = [[MBBakeWriter alloc] initWithRequest:self.request];
    if (!self.writer) { [self fail:@"encoder_unavailable"]; return; }
    NSError *error;
    self.manifest = [WRManifest loadFromDirectory:self.request[@"directory"] error:&error];
    if (!self.manifest) { [self fail:@"invalid_source"]; return; }
    CGFloat width = [self.request[@"width"] doubleValue], height = [self.request[@"height"] doubleValue];
    WREngineConfig config = [WebRendererEngine defaultConfig];
    config.enableAudioPlayback = YES; config.enableAudioSpectrum = NO; config.initialVolume = 0;
    config.frameRate = [self.request[@"fps"] intValue];
    config.assetOverlayDirectories = self.request[@"overlays"];
    self.engine = [[WebRendererEngine alloc] initWithFrame:NSMakeRect(0, 0, width, height) config:config];
    self.window = [[MBWebBakeWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    self.window.releasedWhenClosed = NO; self.window.ignoresMouseEvents = YES;
    self.window.acceptsMouseMovedEvents = NO;
    self.window.level = NSNormalWindowLevel;
    self.window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    self.window.opaque = YES;
    self.window.backgroundColor = NSColor.blackColor;
    self.window.hasShadow = NO;
    self.window.canHide = NO;
    self.window.contentView = self.engine.webView;
    self.engine.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    if (![self positionOffscreen]) { [self fail:@"capture_failed"]; return; }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(screensChanged:)
        name:NSApplicationDidChangeScreenParametersNotification object:nil];
    [self.window orderFrontRegardless];
    __weak MBWebBake *weakSelf = self;
    self.engine.contentReadyHandler = ^{
        MBWebBake *owner = weakSelf;
        if (!owner || owner->_finished) return;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), owner.queue, ^{
            if (owner->_finished) return;
            owner->_ready = true;
            [owner beginWriting];
        });
    };
    [self beginCapture];
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        MBWebBake *owner = weakSelf;
        if (!owner || owner->_finished) { [timer invalidate]; return; }
        if (MBCancelled()) { [owner fail:@"cancelled"]; return; }
        double now = NSProcessInfo.processInfo.systemUptime;
        if ((!owner->_ready && now - owner->_startedAt > 60) ||
            (owner->_ready && !owner->_idle && now - owner->_lastFrame.load() > 60)) [owner fail:@"capture_timeout"];
    }];
}
- (void)beginCapture {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent *content, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_finished) return;
            SCWindow *target = nil;
            for (SCWindow *window in content.windows) if (window.windowID == self.window.windowNumber) { target = window; break; }
            if (!target || error) { [self fail:CGPreflightScreenCaptureAccess() ? @"capture_failed" : @"capture_permission"]; return; }
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
            SCStreamConfiguration *config = [SCStreamConfiguration new];
            config.width = [self.request[@"width"] integerValue]; config.height = [self.request[@"height"] integerValue];
            config.minimumFrameInterval = CMTimeMake(1, [self.request[@"fps"] intValue]);
            config.queueDepth = 3; config.showsCursor = NO; config.capturesAudio = NO;
            config.ignoreShadowsSingleWindow = YES; config.colorSpaceName = kCGColorSpaceSRGB;
            self.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
            if (![self.stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:self.queue error:nil]) {
                [self fail:@"capture_failed"]; return;
            }
            [self.stream startCaptureWithCompletionHandler:^(NSError *failure) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self->_finished) return;
                    if (failure) { [self fail:@"capture_failed"]; return; }
                    [self.engine openWallpaper:self.manifest];
                    [self.engine applyUserProperties:self.request[@"properties"] ?: @{} generation:@"bake"];
                });
            }];
        });
    }];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error { [self fail:@"capture_failed"]; }
- (void)writeFrame {
    @autoreleasepool {
        if (_finished || !_previous) return;
        if (MBCancelled()) { [self fail:@"cancelled"]; return; }
        double now = NSProcessInfo.processInfo.systemUptime;
        NSInteger fps = [self.request[@"fps"] integerValue];
        int64_t total = llround([self.request[@"duration"] doubleValue] * fps);
        int64_t due = MIN(total - 1, (int64_t)floor((now - _origin) * fps));
        if (due - _frame > fps) { [self fail:@"capture_too_slow"]; return; }
        while (_frame <= due) {
            if (![self.writer appendImage:_previous frame:_frame]) { [self fail:@"encode_failed"]; return; }
            ++_frame;
        }
        if (_frame - _reportedFrame >= std::max<NSInteger>(1, fps / 4) || _frame == total) {
            MBProgress((uint32_t)_frame, (uint32_t)total);
            _reportedFrame = _frame;
        }
        if (_frame == total && !_finished.exchange(true)) {
            if ([self.writer finish]) MBEvent(@"complete", @{});
            else MBEvent(@"error", @{@"code":@"encode_failed"});
            [self shutdown];
        }
    }
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    @autoreleasepool {
        if (_finished || type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
        NSArray *attachments = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, NO);
        NSNumber *frameStatus = attachments.firstObject[SCStreamFrameInfoStatus];
        if (!frameStatus) return;
        NSInteger status = frameStatus.integerValue;
        if (status != SCFrameStatusComplete && status != SCFrameStatusIdle) {
            if (_ready && (status == SCFrameStatusBlank || status == SCFrameStatusSuspended)) [self fail:@"capture_failed"];
            return;
        }
        _idle = status == SCFrameStatusIdle;
        CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
        CIImage *image = buffer && status == SCFrameStatusComplete ? [CIImage imageWithCVPixelBuffer:buffer] : _previous;
        if (!image) return;
        _previous = image;
        _lastFrame = NSProcessInfo.processInfo.systemUptime;
        [self beginWriting];
    }
}
- (void)beginWriting {
    if (_finished || !_ready || !_previous || _timer) return;
    _origin = NSProcessInfo.processInfo.systemUptime;
    NSInteger fps = [self.request[@"fps"] integerValue];
    MBProgress(0, (uint32_t)llround([self.request[@"duration"] doubleValue] * fps));
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
    uint64_t interval = NSEC_PER_SEC / fps;
    dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, interval, interval / 10);
    __weak MBWebBake *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{ [weakSelf writeFrame]; });
    dispatch_resume(_timer);
}

@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSDictionary *request = MBReadRequest(argc, argv);
        if (!MBValidateRequest(request) || [request[@"audio"] boolValue] ||
            ![request[@"directory"] isKindOfClass:NSString.class] || [request[@"speed"] doubleValue] != 1.0) {
            MBEvent(@"error", @{@"code":@"invalid_request"}); return 1;
        }
        if (!CGPreflightScreenCaptureAccess()) { MBEvent(@"error", @{@"code":@"capture_permission"}); return 1; }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        MBWebBake *bake = [MBWebBake new]; bake.request = request;
        dispatch_async(dispatch_get_main_queue(), ^{ [bake start]; });
        [NSApp run];
        return 0;
    }
}
