//
//  VideoBaker.mm
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import "BakeSupport.h"
#import "VRTranscoder.h"
#include <algorithm>
#include <cmath>

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSDictionary *r = MBReadRequest(argc, argv);
        if (!MBValidateRequest(r)) { MBEvent(@"error", @{@"code":@"invalid_request"}); return 1; }
        MBEvent(@"preparing", @{});
        NSURL *source = [NSURL fileURLWithPath:r[@"source"]];
        if (![VRTranscoder fileIsDecodable:source]) {
            NSURL *converted = [[NSURL fileURLWithPath:r[@"output"]] URLByDeletingLastPathComponent];
            converted = [converted URLByAppendingPathComponent:@"decoded.mp4"];
            if (![VRTranscoder transcodeFileAtURL:source toURL:converted progress:nil error:nil] || MBCancelled()) {
                MBEvent(@"error", @{@"code":@"decode_failed"}); return 1;
            }
            source = converted;
        }
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:source options:nil];
        AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        double sourceDuration = CMTimeGetSeconds(asset.duration), duration = [r[@"duration"] doubleValue];
        double speed = [r[@"speed"] doubleValue];
        if (!track || !std::isfinite(sourceDuration) || sourceDuration < 0.01) {
            MBEvent(@"error", @{@"code":@"decode_failed"}); return 1;
        }
        for (id description in track.formatDescriptions) {
            CFPropertyListRef transfer = CMFormatDescriptionGetExtension((__bridge CMFormatDescriptionRef)description, kCMFormatDescriptionExtension_TransferFunction);
            if (transfer && (CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                             CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG))) {
                MBEvent(@"error", @{@"code":@"hdr_unsupported"}); return 1;
            }
        }
        AVMutableComposition *composition = [AVMutableComposition composition];
        AVMutableCompositionTrack *video = [composition addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
        AVAssetTrack *sourceAudio = [r[@"audio"] boolValue] ? [asset tracksWithMediaType:AVMediaTypeAudio].firstObject : nil;
        AVMutableCompositionTrack *audio = sourceAudio ? [composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid] : nil;
        CMTime target = CMTimeMakeWithSeconds(duration * speed, 60000), offset = kCMTimeZero;
        NSUInteger segments = 0;
        while (CMTimeCompare(offset, target) < 0) {
            if (MBCancelled() || ++segments > 100000) return 1;
            CMTime remaining = CMTimeSubtract(target, offset);
            CMTime length = CMTimeMinimum(asset.duration, remaining);
            CMTimeRange range = CMTimeRangeMake(kCMTimeZero, length);
            if (![video insertTimeRange:range ofTrack:track atTime:offset error:nil]) {
                MBEvent(@"error", @{@"code":@"decode_failed"}); return 1;
            }
            if (audio) {
                CMTimeRange available = CMTimeRangeGetIntersection(range, sourceAudio.timeRange);
                if (CMTimeCompare(available.duration, kCMTimeZero) > 0 &&
                    ![audio insertTimeRange:available ofTrack:sourceAudio atTime:CMTimeAdd(offset, available.start) error:nil]) {
                    MBEvent(@"error", @{@"code":@"audio_failed"}); return 1;
                }
            }
            offset = CMTimeAdd(offset, length);
        }
        [composition scaleTimeRange:CMTimeRangeMake(kCMTimeZero, target) toDuration:CMTimeMakeWithSeconds(duration, 60000)];
        CGFloat width = [r[@"width"] doubleValue], height = [r[@"height"] doubleValue];
        CGAffineTransform transform = track.preferredTransform;
        CGRect extent = CGRectApplyAffineTransform((CGRect){CGPointZero, track.naturalSize}, transform);
        transform = CGAffineTransformConcat(transform, CGAffineTransformMakeTranslation(-extent.origin.x, -extent.origin.y));
        CGFloat sx = width / extent.size.width, sy = height / extent.size.height;
        if (![r[@"fillMode"] isEqual:@"stretch"]) {
            CGFloat scale = [r[@"fillMode"] isEqual:@"contain"] ? MIN(sx, sy) : MAX(sx, sy);
            sx = sy = scale;
        }
        transform = CGAffineTransformConcat(transform, CGAffineTransformMakeScale(sx, sy));
        CGFloat px = [r[@"fillMode"] isEqual:@"cover"] ? std::clamp([r[@"positionX"] doubleValue], 0.0, 1.0) : 0.5;
        CGFloat py = [r[@"fillMode"] isEqual:@"cover"] ? std::clamp([r[@"positionY"] doubleValue], 0.0, 1.0) : 0.5;
        transform = CGAffineTransformConcat(transform, CGAffineTransformMakeTranslation((width - extent.size.width * sx) * px,
            (height - extent.size.height * sy) * (1.0 - py)));
        AVMutableVideoCompositionLayerInstruction *layer = [AVMutableVideoCompositionLayerInstruction videoCompositionLayerInstructionWithAssetTrack:video];
        [layer setTransform:transform atTime:kCMTimeZero];
        AVMutableVideoCompositionInstruction *instruction = [AVMutableVideoCompositionInstruction videoCompositionInstruction];
        instruction.timeRange = CMTimeRangeMake(kCMTimeZero, composition.duration);
        instruction.layerInstructions = @[layer];
        CGColorRef black = CGColorCreateGenericRGB(0, 0, 0, 1);
        instruction.backgroundColor = black; CGColorRelease(black);
        AVMutableVideoComposition *videoComposition = [AVMutableVideoComposition videoComposition];
        videoComposition.renderSize = CGSizeMake(width, height);
        videoComposition.frameDuration = CMTimeMake(1, [r[@"fps"] intValue]);
        videoComposition.instructions = @[instruction];
        videoComposition.colorPrimaries = AVVideoColorPrimaries_ITU_R_709_2;
        videoComposition.colorTransferFunction = AVVideoTransferFunction_ITU_R_709_2;
        videoComposition.colorYCbCrMatrix = AVVideoYCbCrMatrix_ITU_R_709_2;
        AVAssetExportSession *exporter = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
        exporter.videoComposition = videoComposition;
        exporter.outputURL = [NSURL fileURLWithPath:r[@"output"]]; exporter.outputFileType = AVFileTypeMPEG4;
        exporter.timeRange = CMTimeRangeMake(kCMTimeZero, CMTimeMake(llround(duration * [r[@"fps"] intValue]), [r[@"fps"] intValue]));
        exporter.audioTimePitchAlgorithm = AVAudioTimePitchAlgorithmVarispeed;
        if (audio) {
            AVMutableAudioMixInputParameters *parameters = [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:audio];
            [parameters setVolume:[r[@"volume"] floatValue] atTime:kCMTimeZero];
            AVMutableAudioMix *mix = [AVMutableAudioMix audioMix]; mix.inputParameters = @[parameters]; exporter.audioMix = mix;
        }
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [exporter exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
        double lastProgress = -1, lastChange = NSProcessInfo.processInfo.systemUptime;
        while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC))) {
            double progress = exporter.progress;
            if (progress > lastProgress) { lastProgress = progress; lastChange = NSProcessInfo.processInfo.systemUptime; }
            MBEvent(@"progress", @{@"completed":@(progress * 1000), @"total":@1000});
            if (MBCancelled() || NSProcessInfo.processInfo.systemUptime - lastChange > 90) { [exporter cancelExport]; break; }
        }
        MBEvent(@"verifying", @{});
        if (exporter.status != AVAssetExportSessionStatusCompleted || MBCancelled() || !MBVerify(r[@"output"], width, height, duration)) {
            MBEvent(@"error", @{@"code":@"encode_failed"}); return 1;
        }
        MBEvent(@"complete", @{}); return 0;
    }
}
