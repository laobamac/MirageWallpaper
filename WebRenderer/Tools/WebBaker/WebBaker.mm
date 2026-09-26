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

@interface MBWebBake : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, strong) NSDictionary *request;
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
    dispatch_source_t _timer;
    int64_t _frame;
    double _origin;
    CIImage *_previous;
}
- (void)shutdown {
    if (_timer) { dispatch_source_cancel(_timer); _timer = nil; }
    dispatch_async(dispatch_get_main_queue(), ^{
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
- (void)start {
    self.queue = dispatch_queue_create("cn.laobamac.Mirage.web-bake", DISPATCH_QUEUE_SERIAL);
    self.writer = [[MBBakeWriter alloc] initWithRequest:self.request];
    if (!self.writer) { [self fail:@"encoder_unavailable"]; return; }
    NSError *error;
    WRManifest *manifest = [WRManifest loadFromDirectory:self.request[@"directory"] error:&error];
    if (!manifest) { [self fail:@"invalid_source"]; return; }
    CGFloat width = [self.request[@"width"] doubleValue], height = [self.request[@"height"] doubleValue];
    WREngineConfig config = [WebRendererEngine defaultConfig];
    config.enableAudioPlayback = YES; config.enableAudioSpectrum = NO; config.initialVolume = 0;
    config.frameRate = [self.request[@"fps"] intValue];
    config.assetOverlayDirectories = self.request[@"overlays"];
    self.engine = [[WebRendererEngine alloc] initWithFrame:NSMakeRect(0, 0, width, height) config:config];
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    self.window.releasedWhenClosed = NO; self.window.ignoresMouseEvents = YES;
    self.window.level = CGWindowLevelForKey(kCGDesktopWindowLevelKey) + 1;
    self.window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary;
    self.window.contentView = self.engine.webView;
    [self.window setFrameOrigin:NSScreen.mainScreen.frame.origin];
    [self.window orderFrontRegardless];
    __weak MBWebBake *weakSelf = self;
    self.engine.contentReadyHandler = ^{
        MBWebBake *owner = weakSelf;
        if (!owner || owner->_finished) return;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [owner beginCapture]; });
    };
    [self.engine openWallpaper:manifest];
    [self.engine applyUserProperties:self.request[@"properties"] ?: @{} generation:@"bake"];
    MBEvent(@"preparing", @{});
    _lastFrame = NSProcessInfo.processInfo.systemUptime;
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        MBWebBake *owner = weakSelf;
        if (!owner || owner->_finished) { [timer invalidate]; return; }
        if (MBCancelled()) { [owner fail:@"cancelled"]; return; }
        if (NSProcessInfo.processInfo.systemUptime - owner->_lastFrame.load() > 60) [owner fail:@"capture_timeout"];
    }];
}
- (void)beginCapture {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent *content, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_finished) return;
            SCWindow *target = nil;
            for (SCWindow *window in content.windows) if (window.windowID == self.window.windowNumber) { target = window; break; }
            if (!target || error) { [self fail:@"capture_permission"]; return; }
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
            [self.stream startCaptureWithCompletionHandler:^(NSError *failure) { if (failure) [self fail:@"capture_failed"]; }];
        });
    }];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error { [self fail:@"capture_failed"]; }
- (void)writeFrame {
    @autoreleasepool {
        if (_finished || !_previous) return;
        if (MBCancelled()) { [self fail:@"cancelled"]; return; }
        double now = NSProcessInfo.processInfo.systemUptime;
        _lastFrame = now;
        NSInteger fps = [self.request[@"fps"] integerValue];
        int64_t total = llround([self.request[@"duration"] doubleValue] * fps);
        int64_t due = MIN(total - 1, (int64_t)floor((now - _origin) * fps));
        if (due - _frame > fps) { [self fail:@"capture_too_slow"]; return; }
        while (_frame <= due) {
            if (![self.writer appendImage:_previous frame:_frame]) { [self fail:@"encode_failed"]; return; }
            ++_frame;
        }
        if (_frame % fps == 0 || _frame == total) MBProgress((uint32_t)_frame, (uint32_t)total);
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
        if (status != SCFrameStatusComplete && status != SCFrameStatusIdle) return;
        CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
        CIImage *image = buffer && status == SCFrameStatusComplete ? [CIImage imageWithCVPixelBuffer:buffer] : _previous;
        if (!image) return;
        _previous = image;
        _lastFrame = NSProcessInfo.processInfo.systemUptime;
        if (!_timer) {
            _origin = NSProcessInfo.processInfo.systemUptime;
            _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
            uint64_t interval = NSEC_PER_SEC / [self.request[@"fps"] unsignedIntValue];
            dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, interval, interval / 10);
            __weak MBWebBake *weakSelf = self;
            dispatch_source_set_event_handler(_timer, ^{ [weakSelf writeFrame]; });
            dispatch_resume(_timer);
        }
    }
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
