//
//  BakeSupport.mm
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import "BakeSupport.h"
#import <AudioToolbox/AudioToolbox.h>
#include <signal.h>
#include <unistd.h>
#include <cmath>
#include <algorithm>

static volatile sig_atomic_t stopped = 0;
static void MBSignal(int) { stopped = 1; }
BOOL MBCancelled(void) { return stopped || getppid() == 1; }
int MBShouldStop(void) { return MBCancelled(); }

void MBEvent(NSString *event, NSDictionary *values) {
    NSMutableDictionary *message = [values mutableCopy] ?: [NSMutableDictionary dictionary];
    message[@"event"] = event;
    NSData *data = [NSJSONSerialization dataWithJSONObject:message options:0 error:nil];
    if (data) { fwrite(data.bytes, 1, data.length, stdout); fputc('\n', stdout); fflush(stdout); }
}

NSDictionary *MBReadRequest(int argc, const char **argv) {
    signal(SIGINT, MBSignal); signal(SIGTERM, MBSignal);
    if (argc != 2) return nil;
    NSData *data = [NSData dataWithContentsOfFile:@(argv[1])];
    if (!data || data.length > 16 * 1024 * 1024) return nil;
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}

BOOL MBValidateRequest(NSDictionary *r) {
    if (![r[@"output"] isKindOfClass:NSString.class] || ![r[@"source"] isKindOfClass:NSString.class]) return NO;
    for (NSString *key in @[@"width", @"height", @"fps", @"duration", @"speed"])
        if (![r[key] isKindOfClass:NSNumber.class] || !std::isfinite([r[key] doubleValue])) return NO;
    NSInteger w = [r[@"width"] integerValue], h = [r[@"height"] integerValue], fps = [r[@"fps"] integerValue];
    double duration = [r[@"duration"] doubleValue], speed = [r[@"speed"] doubleValue];
    return w >= 128 && h >= 128 && w <= 4096 && h <= 4096 && w % 2 == 0 && h % 2 == 0 &&
        (fps == 24 || fps == 30 || fps == 60) && duration >= 1 && duration <= 600 &&
        speed >= 0.1 && speed <= 4 && ![NSFileManager.defaultManager fileExistsAtPath:r[@"output"]];
}

BOOL MBVerify(NSString *path, NSInteger width, NSInteger height, double duration) {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (!track || !track.isDecodable || !std::isfinite(CMTimeGetSeconds(asset.duration)) ||
        std::abs(CMTimeGetSeconds(asset.duration) - duration) > 0.12 ||
        std::abs(track.naturalSize.width - width) > 1 || std::abs(track.naturalSize.height - height) > 1) return NO;
    AVAssetImageGenerator *generator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
    for (NSNumber *fraction in @[@0.0, @0.5, @0.95]) {
        CGImageRef image = [generator copyCGImageAtTime:CMTimeMakeWithSeconds(duration * fraction.doubleValue, 60000)
                                           actualTime:nil error:nil];
        if (!image) return NO;
        CGImageRelease(image);
    }
    return YES;
}

@implementation MBBakeWriter {
    AVAssetWriter *_writer;
    AVAssetWriter *_audioWriter;
    AVAssetWriterInput *_video;
    AVAssetWriterInput *_audio;
    AVAssetWriterInputPixelBufferAdaptor *_adaptor;
    CIContext *_context;
    NSInteger _width, _height, _fps;
    int64_t _nextFrame, _nextAudio;
    double _duration;
    NSString *_path;
    NSString *_failure;
    NSString *_audioPath;
    NSString *_muxPath;
}
- (NSString *)failure { return _failure ?: _writer.error.localizedDescription ?: _audioWriter.error.localizedDescription ?: @"encode_failed"; }
- (instancetype)initWithRequest:(NSDictionary *)r {
    if (!(self = [super init])) return nil;
    _width = [r[@"width"] integerValue]; _height = [r[@"height"] integerValue];
    _fps = [r[@"fps"] integerValue]; _duration = [r[@"duration"] doubleValue]; _path = r[@"output"];
    _writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:_path] fileType:AVFileTypeMPEG4 error:nil];
    double bpp = [r[@"quality"] isEqual:@"high"] ? 0.16 : 0.09;
    _video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey:@(_width), AVVideoHeightKey:@(_height),
        AVVideoColorPropertiesKey:@{AVVideoColorPrimariesKey:AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey:AVVideoYCbCrMatrix_ITU_R_709_2},
        AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(std::clamp(_width * _height * _fps * bpp, 2000000.0, 80000000.0)),
            AVVideoExpectedSourceFrameRateKey:@(_fps), AVVideoMaxKeyFrameIntervalKey:@(_fps),
            AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel}}];
    _video.expectsMediaDataInRealTime = NO;
    if (![_writer canAddInput:_video]) return nil;
    [_writer addInput:_video];
    _adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_video
        sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey:@(_width), (id)kCVPixelBufferHeightKey:@(_height),
        (id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
    if ([r[@"audio"] boolValue]) {
        _audioPath = [_path stringByAppendingString:@".audio.m4a"];
        _muxPath = [_path stringByAppendingString:@".mux.mp4"];
        _audioWriter = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:_audioPath] fileType:AVFileTypeAppleM4A error:nil];
        _audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
            AVFormatIDKey:@(kAudioFormatMPEG4AAC), AVSampleRateKey:@48000, AVNumberOfChannelsKey:@2, AVEncoderBitRateKey:@192000}];
        _audio.expectsMediaDataInRealTime = NO;
        if (![_audioWriter canAddInput:_audio]) return nil;
        [_audioWriter addInput:_audio];
        if (![_audioWriter startWriting]) return nil;
        [_audioWriter startSessionAtSourceTime:kCMTimeZero];
    }
    _context = [CIContext contextWithOptions:@{kCIContextCacheIntermediates:@NO}];
    if (![_writer startWriting]) return nil;
    [_writer startSessionAtSourceTime:kCMTimeZero];
    return self;
}
- (BOOL)waitForInput:(AVAssetWriterInput *)input {
    AVAssetWriter *writer = input == _audio ? _audioWriter : _writer;
    double deadline = NSProcessInfo.processInfo.systemUptime + 30;
    while (!input.readyForMoreMediaData) {
        if (MBCancelled() || writer.status != AVAssetWriterStatusWriting || NSProcessInfo.processInfo.systemUptime > deadline) return NO;
        usleep(2000);
    }
    return !MBCancelled();
}
- (BOOL)appendRGBA:(const uint8_t *)bytes width:(uint32_t)width height:(uint32_t)height frame:(int64_t)frame {
    if (!bytes || !width || !height) return NO;
    NSData *data = [NSData dataWithBytesNoCopy:(void *)bytes length:(NSUInteger)width * height * 4 freeWhenDone:NO];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CIImage *image = [CIImage imageWithBitmapData:data bytesPerRow:width * 4 size:CGSizeMake(width, height)
                                          format:kCIFormatRGBA8 colorSpace:space];
    CGColorSpaceRelease(space);
    return [self appendImage:image frame:frame];
}
- (BOOL)appendImage:(CIImage *)image frame:(int64_t)frame {
    if (!image || frame != _nextFrame || ![self waitForInput:_video]) return NO;
    CVPixelBufferRef buffer = NULL;
    if (CVPixelBufferPoolCreatePixelBuffer(NULL, _adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess) return NO;
    CGRect bounds = image.extent;
    image = [image imageByApplyingTransform:CGAffineTransformMakeTranslation(-bounds.origin.x, -bounds.origin.y)];
    image = [image imageByApplyingTransform:CGAffineTransformMakeScale(_width / bounds.size.width, _height / bounds.size.height)];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    [_context render:image toCVPixelBuffer:buffer bounds:CGRectMake(0, 0, _width, _height) colorSpace:space];
    CGColorSpaceRelease(space);
    BOOL ok = [_adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame, (int32_t)_fps)];
    CVPixelBufferRelease(buffer);
    if (ok) ++_nextFrame;
    return ok;
}
- (BOOL)appendAudio:(const float *)samples frames:(uint32_t)frames at:(int64_t)offset {
    if (!_audio) return YES;
    if (!frames || !samples || offset != _nextAudio || ![self waitForInput:_audio]) return NO;
    AudioStreamBasicDescription desc = {48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    CMAudioFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    OSStatus status = CMAudioFormatDescriptionCreate(NULL, &desc, 0, NULL, 0, NULL, NULL, &format);
    if (status == noErr) status = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, frames * 8, NULL, NULL, 0, frames * 8, 0, &block);
    if (status == noErr) status = CMBlockBufferReplaceDataBytes(samples, block, 0, frames * 8);
    if (status == noErr) status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, format, frames,
        CMTimeMake(offset, 48000), NULL, &sample);
    BOOL ok = status == noErr && [_audio appendSampleBuffer:sample];
    if (!ok && !_failure) _failure = status != noErr ? [NSString stringWithFormat:@"audio_sample_%d", (int)status] : @"audio_append_failed";
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    if (ok) _nextAudio += frames;
    return ok;
}
- (BOOL)finishWriter:(AVAssetWriter *)writer {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC))) return NO;
    return writer.status == AVAssetWriterStatusCompleted && !MBCancelled();
}
- (BOOL)muxAudio {
    AVMutableComposition *composition = [AVMutableComposition composition];
    CMTimeRange range = CMTimeRangeMake(kCMTimeZero, CMTimeMake(_nextFrame, (int32_t)_fps));
    for (NSArray<NSString *> *entry in @[@[_path, AVMediaTypeVideo], @[_audioPath, AVMediaTypeAudio]]) {
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:entry[0]] options:nil];
        AVAssetTrack *source = [asset tracksWithMediaType:entry[1]].firstObject;
        AVMutableCompositionTrack *track = [composition addMutableTrackWithMediaType:entry[1] preferredTrackID:kCMPersistentTrackID_Invalid];
        if (!source || ![track insertTimeRange:range ofTrack:source atTime:kCMTimeZero error:nil]) return NO;
    }
    AVAssetExportSession *exporter = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetPassthrough];
    if (!exporter) return NO;
    exporter.outputURL = [NSURL fileURLWithPath:_muxPath];
    exporter.outputFileType = AVFileTypeMPEG4;
    exporter.timeRange = range;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [exporter exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    double deadline = NSProcessInfo.processInfo.systemUptime + 90;
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC))) {
        if (MBCancelled() || NSProcessInfo.processInfo.systemUptime > deadline) { [exporter cancelExport]; return NO; }
    }
    if (exporter.status != AVAssetExportSessionStatusCompleted) return NO;
    if (![NSFileManager.defaultManager replaceItemAtURL:[NSURL fileURLWithPath:_path]
        withItemAtURL:[NSURL fileURLWithPath:_muxPath] backupItemName:nil options:0 resultingItemURL:nil error:nil]) return NO;
    [NSFileManager.defaultManager removeItemAtPath:_audioPath error:nil];
    return YES;
}
- (BOOL)finish {
    if (_nextFrame != llround(_duration * _fps) || MBCancelled()) { [self cancel]; return NO; }
    [_writer endSessionAtSourceTime:CMTimeMake(_nextFrame, (int32_t)_fps)];
    [_video markAsFinished];
    if (_audio) {
        [_audioWriter endSessionAtSourceTime:CMTimeMake(_nextAudio, 48000)];
        [_audio markAsFinished];
        if (_nextAudio != llround(_duration * 48000) || ![self finishWriter:_audioWriter]) { [self cancel]; return NO; }
    }
    if (![self finishWriter:_writer] || (_audio && ![self muxAudio])) { [self cancel]; return NO; }
    MBEvent(@"verifying", @{});
    return _writer.status == AVAssetWriterStatusCompleted && MBVerify(_path, _width, _height, _duration);
}
- (void)cancel { [_writer cancelWriting]; [_audioWriter cancelWriting]; }
@end

int MBWriteFrame(void *writer, const uint8_t *rgba, uint32_t width, uint32_t height, int64_t frame) {
    @autoreleasepool { return [(__bridge MBBakeWriter *)writer appendRGBA:rgba width:width height:height frame:frame]; }
}
int MBWriteAudio(void *writer, const float *samples, uint32_t count, int64_t offset) {
    @autoreleasepool { return [(__bridge MBBakeWriter *)writer appendAudio:samples frames:count at:offset]; }
}
void MBProgress(uint32_t frame, uint32_t count) {
    @autoreleasepool { MBEvent(@"progress", @{@"completed":@(frame), @"total":@(count)}); }
}
