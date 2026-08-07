#import "VRTranscoder.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

// libavutil and AVFoundation both export the name AVMediaType — an enum in one,
// an NSString typedef in the other. Rename libav's for this translation unit;
// the AVMEDIA_TYPE_* enumerators it declares are unaffected.
#define AVMediaType FFAVMediaType
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#undef AVMediaType

NSString *const VRTranscoderErrorDomain = @"VideoRenderer.Transcoder";

enum {
    VRTranscoderErrorOpenInput = 1,
    VRTranscoderErrorNoVideoStream,
    VRTranscoderErrorNoDecoder,
    VRTranscoderErrorWriterSetup,
    VRTranscoderErrorEncodeFailed,
    VRTranscoderErrorVerifyFailed,
    VRTranscoderErrorInstallFailed,
};

// MPEG's classic timescale: divides 60, 30, 25, 24 and 29.97 exactly, so frame
// timestamps stay integral for every rate a wallpaper realistically uses.
static const int32_t kVRTimescale = 90000;

static NSError *VRTranscodeError(NSInteger code, NSString *description) {
    return [NSError errorWithDomain:VRTranscoderErrorDomain
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: description }];
}

static NSString *VRAVErrorString(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return @(buffer);
}

@implementation VRTranscoder

#pragma mark - Probing

+ (BOOL)fileIsDecodable:(NSURL *)url {
    if (url == nil) return NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path]) return NO;

    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSArray<AVAssetTrack *> *videoTracks = nil;
    [asset loadValuesAsynchronouslyForKeys:@[@"playable", @"tracks"] completionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    // Local metadata parsing; the bound only matters for a pathological file.
    if (dispatch_semaphore_wait(done,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20 * NSEC_PER_SEC))) != 0) {
        return NO;
    }
    if ([asset statusOfValueForKey:@"tracks" error:NULL] != AVKeyValueStatusLoaded) return NO;
    if (!asset.playable) return NO;

    videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    if (videoTracks.count == 0) return NO;

    for (AVAssetTrack *track in videoTracks) {
        dispatch_semaphore_t trackDone = dispatch_semaphore_create(0);
        [track loadValuesAsynchronouslyForKeys:@[@"decodable"] completionHandler:^{
            dispatch_semaphore_signal(trackDone);
        }];
        if (dispatch_semaphore_wait(trackDone,
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20 * NSEC_PER_SEC))) != 0) {
            return NO;
        }
        if ([track statusOfValueForKey:@"decodable" error:NULL] != AVKeyValueStatusLoaded) return NO;
        if (!track.isDecodable) return NO;
    }
    return YES;
}

+ (NSURL *)playableURLForSourceURL:(NSURL *)url {
    static NSSet<NSString *> *substitutable;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
      substitutable = [NSSet setWithArray:@[ @"mp4", @"m4v", @"mov", @"qt" ]];
    });
    NSString *extension = url.pathExtension.lowercaseString;
    if ([substitutable containsObject:extension]) return url;
    return [url.URLByDeletingPathExtension URLByAppendingPathExtension:@"mp4"];
}

#pragma mark - Colour

static NSDictionary *VRColorProperties(const AVCodecContext *decoder) {
    NSString *primaries = AVVideoColorPrimaries_ITU_R_709_2;
    NSString *transfer = AVVideoTransferFunction_ITU_R_709_2;
    NSString *matrix = AVVideoYCbCrMatrix_ITU_R_709_2;
    switch (decoder->color_primaries) {
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M:
        primaries = AVVideoColorPrimaries_SMPTE_C;
        break;
    case AVCOL_PRI_BT2020:
        primaries = AVVideoColorPrimaries_ITU_R_2020;
        break;
    default:
        break;
    }
    switch (decoder->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        matrix = AVVideoYCbCrMatrix_ITU_R_601_4;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        matrix = AVVideoYCbCrMatrix_ITU_R_2020;
        break;
    default:
        break;
    }
    if (decoder->color_trc == AVCOL_TRC_SMPTE170M || decoder->color_trc == AVCOL_TRC_BT709) {
        transfer = AVVideoTransferFunction_ITU_R_709_2;
    }
    return @{
        AVVideoColorPrimariesKey: primaries,
        AVVideoTransferFunctionKey: transfer,
        AVVideoYCbCrMatrixKey: matrix,
    };
}

// Rewriting drops the container's rotation metadata, so the angle is folded into
// the writer transform instead — otherwise a portrait source comes back sideways.
static CGAffineTransform VRTransformForStream(AVStream *stream) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
    for (int i = 0; i < stream->codecpar->nb_coded_side_data; ++i) {
        const AVPacketSideData *side = &stream->codecpar->coded_side_data[i];
        if (side->type != AV_PKT_DATA_DISPLAYMATRIX) continue;
        double degrees = av_display_rotation_get((const int32_t *)side->data);
        if (isnan(degrees)) break;
        return CGAffineTransformMakeRotation((CGFloat)(-degrees * M_PI / 180.0));
    }
#else
    (void)stream;
#endif
    return CGAffineTransformIdentity;
}

#pragma mark - Encoding helpers

static BOOL VRWaitForInput(AVAssetWriterInput *input, AVAssetWriter *writer) {
    while (!input.isReadyForMoreMediaData) {
        if (writer.status == AVAssetWriterStatusFailed ||
            writer.status == AVAssetWriterStatusCancelled) {
            return NO;
        }
        usleep(2000);
    }
    return writer.status == AVAssetWriterStatusWriting;
}

static CVPixelBufferRef VRCreatePixelBuffer(AVAssetWriterInputPixelBufferAdaptor *adaptor,
                                            int width, int height) {
    CVPixelBufferRef buffer = NULL;
    if (adaptor.pixelBufferPool != NULL) {
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, adaptor.pixelBufferPool,
                                              &buffer) == kCVReturnSuccess) {
            return buffer;
        }
        buffer = NULL;
    }
    NSDictionary *attributes = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferCreate(kCFAllocatorDefault, (size_t)width, (size_t)height,
                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                        (__bridge CFDictionaryRef)attributes, &buffer);
    return buffer;
}

static BOOL VRFillPixelBuffer(CVPixelBufferRef pixelBuffer, SwsContext *scaler,
                              const AVFrame *frame, int width, int height) {
    if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess) return NO;
    uint8_t *destinationData[4] = {
        (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0),
        (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1),
        NULL, NULL,
    };
    int destinationStride[4] = {
        (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0),
        (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1),
        0, 0,
    };
    int scaled = sws_scale(scaler, frame->data, frame->linesize, 0, height,
                           destinationData, destinationStride);
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    (void)width;
    return scaled > 0;
}

static CMSampleBufferRef VRCreateAudioSampleBuffer(const uint8_t *data, int bytes,
                                                   int channels, int sampleRate,
                                                   int frames, int64_t presentedSamples) {
    AudioStreamBasicDescription description = {0};
    description.mSampleRate = sampleRate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    description.mBytesPerPacket = (UInt32)(2 * channels);
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = (UInt32)(2 * channels);
    description.mChannelsPerFrame = (UInt32)channels;
    description.mBitsPerChannel = 16;

    CMAudioFormatDescriptionRef format = NULL;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &description, 0, NULL, 0, NULL,
                                       NULL, &format) != noErr) {
        return NULL;
    }

    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, (size_t)bytes,
                                           kCFAllocatorDefault, NULL, 0, (size_t)bytes,
                                           0, &block) != noErr) {
        CFRelease(format);
        return NULL;
    }
    if (CMBlockBufferAssureBlockMemory(block) != noErr ||
        CMBlockBufferReplaceDataBytes(data, block, 0, (size_t)bytes) != noErr) {
        CFRelease(block);
        CFRelease(format);
        return NULL;
    }

    CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing = {
        .duration = CMTimeMake(1, sampleRate),
        .presentationTimeStamp = CMTimeMake(presentedSamples, sampleRate),
        .decodeTimeStamp = kCMTimeInvalid,
    };
    OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, frames, 1,
                                                &timing, 0, NULL, &sample);
    CFRelease(block);
    CFRelease(format);
    return status == noErr ? sample : NULL;
}

#pragma mark - Transcode

+ (BOOL)transcodeFileAtURL:(NSURL *)sourceURL
                     toURL:(NSURL *)destinationURL
                  progress:(VRTranscodeProgressBlock)progress
                     error:(NSError **)error {
    if (sourceURL == nil || destinationURL == nil) {
        if (error) *error = VRTranscodeError(VRTranscoderErrorOpenInput, @"missing transcode path");
        return NO;
    }

    AVFormatContext *input = NULL;
    int status = avformat_open_input(&input, sourceURL.path.UTF8String, NULL, NULL);
    if (status < 0) {
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorOpenInput,
                [NSString stringWithFormat:@"cannot open video: %@", VRAVErrorString(status)]);
        }
        return NO;
    }
    status = avformat_find_stream_info(input, NULL);
    if (status < 0) {
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorOpenInput,
                [NSString stringWithFormat:@"cannot read video streams: %@", VRAVErrorString(status)]);
        }
        return NO;
    }

    const AVCodec *videoCodec = NULL;
    int videoIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0);
    if (videoIndex < 0 || videoCodec == NULL) {
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorNoVideoStream, @"file has no video stream");
        }
        return NO;
    }
    AVStream *videoStream = input->streams[videoIndex];

    AVCodecContext *videoDecoder = avcodec_alloc_context3(videoCodec);
    if (videoDecoder == NULL) {
        avformat_close_input(&input);
        if (error) *error = VRTranscodeError(VRTranscoderErrorNoDecoder, @"out of memory");
        return NO;
    }
    avcodec_parameters_to_context(videoDecoder, videoStream->codecpar);
    videoDecoder->pkt_timebase = videoStream->time_base;
    videoDecoder->thread_count = 0;
    status = avcodec_open2(videoDecoder, videoCodec, NULL);
    if (status < 0) {
        avcodec_free_context(&videoDecoder);
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorNoDecoder,
                [NSString stringWithFormat:@"no decoder for %s: %@", videoCodec->name,
                                           VRAVErrorString(status)]);
        }
        return NO;
    }

    const int width = videoDecoder->width;
    const int height = videoDecoder->height;
    if (width <= 0 || height <= 0) {
        avcodec_free_context(&videoDecoder);
        avformat_close_input(&input);
        if (error) *error = VRTranscodeError(VRTranscoderErrorNoVideoStream, @"video has no size");
        return NO;
    }

    AVRational guessed = av_guess_frame_rate(input, videoStream, NULL);
    double fps = (guessed.num > 0 && guessed.den > 0) ? av_q2d(guessed) : 30.0;
    if (fps < 1.0 || fps > 240.0) fps = 30.0;

    double duration = 0;
    if (input->duration != AV_NOPTS_VALUE && input->duration > 0) {
        duration = (double)input->duration / AV_TIME_BASE;
    } else if (videoStream->duration != AV_NOPTS_VALUE && videoStream->duration > 0) {
        duration = (double)videoStream->duration * av_q2d(videoStream->time_base);
    }

    // H.264 needs materially more bits than VP9/AV1 for the same picture, so the
    // source rate is scaled up rather than matched; the floor keeps small inputs
    // from banding and the ceiling keeps a 4K60 rewrite off the whole disk.
    int64_t sourceBitrate = input->bit_rate > 0 ? input->bit_rate : 0;
    if (sourceBitrate == 0 && videoStream->codecpar->bit_rate > 0) {
        sourceBitrate = videoStream->codecpar->bit_rate;
    }
    int64_t bitrate = sourceBitrate > 0
        ? (int64_t)(sourceBitrate * 1.25)
        : (int64_t)((double)width * height * fps * 0.05);
    int64_t ceiling = 60000000;
    int64_t floorRate = 4000000;
    if (bitrate > ceiling) bitrate = ceiling;
    if (bitrate < floorRate) bitrate = floorRate;

    // Audio is optional: a source whose audio cannot be decoded still yields a
    // silent but visible wallpaper, which beats refusing the whole file.
    const AVCodec *audioCodec = NULL;
    int audioIndex = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
    AVCodecContext *audioDecoder = NULL;
    SwrContext *resampler = NULL;
    int audioChannels = 0;
    int audioRate = 0;
    if (audioIndex >= 0 && audioCodec != NULL) {
        audioDecoder = avcodec_alloc_context3(audioCodec);
        if (audioDecoder != NULL) {
            avcodec_parameters_to_context(audioDecoder, input->streams[audioIndex]->codecpar);
            audioDecoder->pkt_timebase = input->streams[audioIndex]->time_base;
            if (avcodec_open2(audioDecoder, audioCodec, NULL) < 0) {
                avcodec_free_context(&audioDecoder);
            }
        }
    }
    if (audioDecoder != NULL) {
        audioChannels = audioDecoder->ch_layout.nb_channels > 1 ? 2 : 1;
        audioRate = audioDecoder->sample_rate > 0 ? audioDecoder->sample_rate : 48000;
        if (audioRate < 8000 || audioRate > 192000) audioRate = 48000;
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, audioChannels);
        if (swr_alloc_set_opts2(&resampler, &outLayout, AV_SAMPLE_FMT_S16, audioRate,
                                &audioDecoder->ch_layout, audioDecoder->sample_fmt,
                                audioDecoder->sample_rate, 0, NULL) < 0 ||
            swr_init(resampler) < 0) {
            if (resampler) swr_free(&resampler);
            resampler = NULL;
            avcodec_free_context(&audioDecoder);
        }
        av_channel_layout_uninit(&outLayout);
    }

    // Encode beside the destination so the final move stays on one volume, and
    // under a dot-name so a crashed run leaves nothing a directory scan picks up.
    NSString *temporaryName = [NSString stringWithFormat:@".mirage-transcode-%@.mp4",
                                                          NSUUID.UUID.UUIDString];
    NSURL *temporaryURL = [destinationURL.URLByDeletingLastPathComponent
        URLByAppendingPathComponent:temporaryName];
    [NSFileManager.defaultManager removeItemAtURL:temporaryURL error:nil];

    NSError *writerError = nil;
    AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:temporaryURL
                                                     fileType:AVFileTypeMPEG4
                                                        error:&writerError];
    if (writer == nil) {
        if (resampler) swr_free(&resampler);
        if (audioDecoder) avcodec_free_context(&audioDecoder);
        avcodec_free_context(&videoDecoder);
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorWriterSetup,
                writerError.localizedDescription ?: @"cannot create video writer");
        }
        return NO;
    }

    NSDictionary *compression = @{
        AVVideoAverageBitRateKey: @(bitrate),
        AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
        AVVideoAllowFrameReorderingKey: @YES,
        AVVideoExpectedSourceFrameRateKey: @((int)llround(fps)),
        // Two-second GOP: looping seeks back to zero on every pass, and a sparse
        // keyframe grid makes that seek visibly slow.
        AVVideoMaxKeyFrameIntervalKey: @((int)llround(fps * 2)),
    };
    AVAssetWriterInput *videoInput = [AVAssetWriterInput
        assetWriterInputWithMediaType:AVMediaTypeVideo
                       outputSettings:@{
                           AVVideoCodecKey: AVVideoCodecTypeH264,
                           AVVideoWidthKey: @(width),
                           AVVideoHeightKey: @(height),
                           AVVideoCompressionPropertiesKey: compression,
                           AVVideoColorPropertiesKey: VRColorProperties(videoDecoder),
                       }];
    videoInput.expectsMediaDataInRealTime = NO;
    videoInput.transform = VRTransformForStream(videoStream);
    AVAssetWriterInputPixelBufferAdaptor *adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                   sourcePixelBufferAttributes:@{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (id)kCVPixelBufferWidthKey: @(width),
            (id)kCVPixelBufferHeightKey: @(height),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        }];
    if (![writer canAddInput:videoInput]) {
        if (resampler) swr_free(&resampler);
        if (audioDecoder) avcodec_free_context(&audioDecoder);
        avcodec_free_context(&videoDecoder);
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorWriterSetup, @"H.264 encoder unavailable");
        }
        return NO;
    }
    [writer addInput:videoInput];

    AVAssetWriterInput *audioInput = nil;
    if (audioDecoder != NULL) {
        audioInput = [AVAssetWriterInput
            assetWriterInputWithMediaType:AVMediaTypeAudio
                           outputSettings:@{
                               AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                               AVNumberOfChannelsKey: @(audioChannels),
                               AVSampleRateKey: @(audioRate),
                               AVEncoderBitRateKey: @(audioChannels > 1 ? 192000 : 96000),
                           }];
        audioInput.expectsMediaDataInRealTime = NO;
        if ([writer canAddInput:audioInput]) {
            [writer addInput:audioInput];
        } else {
            audioInput = nil;
        }
    }

    if (![writer startWriting]) {
        if (resampler) swr_free(&resampler);
        if (audioDecoder) avcodec_free_context(&audioDecoder);
        avcodec_free_context(&videoDecoder);
        avformat_close_input(&input);
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorWriterSetup,
                writer.error.localizedDescription ?: @"cannot start video writer");
        }
        return NO;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    SwsContext *scaler = sws_getContext(width, height, videoDecoder->pix_fmt,
                                        width, height, AV_PIX_FMT_NV12,
                                        SWS_BILINEAR, NULL, NULL, NULL);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    AVFrame *resampled = av_frame_alloc();
    BOOL ok = (scaler != NULL && frame != NULL && packet != NULL && resampled != NULL);
    NSString *failureReason = ok ? nil : @"out of memory";

    int64_t firstPts = AV_NOPTS_VALUE;
    int64_t frameIndex = 0;
    int64_t audioSamplesWritten = 0;
    double lastReported = -1;

    // Local closure state kept explicit: the decode loop is shared by the packet
    // pass and the end-of-stream flush.
    auto drainVideo = [&]() -> BOOL {
        for (;;) {
            int received = avcodec_receive_frame(videoDecoder, frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return YES;
            if (received < 0) {
                failureReason = [NSString stringWithFormat:@"decode failed: %@",
                                                           VRAVErrorString(received)];
                return NO;
            }
            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                pts = av_rescale_q((int64_t)llround(frameIndex * kVRTimescale / fps),
                                   AVRational{1, kVRTimescale}, videoStream->time_base);
            }
            if (firstPts == AV_NOPTS_VALUE) firstPts = pts;
            int64_t scaledPts = av_rescale_q(pts - firstPts, videoStream->time_base,
                                             AVRational{1, kVRTimescale});
            if (scaledPts < 0) scaledPts = 0;

            if (!VRWaitForInput(videoInput, writer)) {
                failureReason = writer.error.localizedDescription ?: @"encoder stopped";
                av_frame_unref(frame);
                return NO;
            }
            CVPixelBufferRef pixelBuffer = VRCreatePixelBuffer(adaptor, width, height);
            if (pixelBuffer == NULL) {
                failureReason = @"cannot allocate frame buffer";
                av_frame_unref(frame);
                return NO;
            }
            BOOL filled = VRFillPixelBuffer(pixelBuffer, scaler, frame, width, height);
            BOOL appended = filled && [adaptor appendPixelBuffer:pixelBuffer
                                            withPresentationTime:CMTimeMake(scaledPts, kVRTimescale)];
            CVPixelBufferRelease(pixelBuffer);
            av_frame_unref(frame);
            if (!appended) {
                failureReason = writer.error.localizedDescription ?: @"cannot encode frame";
                return NO;
            }
            frameIndex += 1;

            if (progress != nil && duration > 0) {
                double fraction = ((double)scaledPts / kVRTimescale) / duration;
                if (fraction > 1.0) fraction = 1.0;
                if (fraction - lastReported >= 0.01) {
                    lastReported = fraction;
                    progress(fraction);
                }
            }
        }
        return YES;
    };

    auto drainAudio = [&]() -> BOOL {
        if (audioInput == nil) return YES;
        for (;;) {
            int received = avcodec_receive_frame(audioDecoder, resampled);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return YES;
            if (received < 0) return YES; // audio is best-effort

            int maxOut = (int)swr_get_out_samples(resampler, resampled->nb_samples);
            if (maxOut <= 0) {
                av_frame_unref(resampled);
                continue;
            }
            int bytes = maxOut * audioChannels * 2;
            uint8_t *buffer = (uint8_t *)av_malloc((size_t)bytes);
            if (buffer == NULL) {
                av_frame_unref(resampled);
                return YES;
            }
            uint8_t *outputPlanes[1] = { buffer };
            int converted = swr_convert(resampler, outputPlanes, maxOut,
                                        (const uint8_t **)resampled->extended_data,
                                        resampled->nb_samples);
            av_frame_unref(resampled);
            if (converted <= 0) {
                av_free(buffer);
                continue;
            }
            if (!VRWaitForInput(audioInput, writer)) {
                av_free(buffer);
                return YES;
            }
            CMSampleBufferRef sample = VRCreateAudioSampleBuffer(
                buffer, converted * audioChannels * 2, audioChannels, audioRate,
                converted, audioSamplesWritten);
            av_free(buffer);
            if (sample == NULL) continue;
            if ([audioInput appendSampleBuffer:sample]) {
                audioSamplesWritten += converted;
            }
            CFRelease(sample);
        }
    };

    while (ok) {
        int read = av_read_frame(input, packet);
        if (read == AVERROR_EOF) break;
        if (read < 0) {
            failureReason = [NSString stringWithFormat:@"read failed: %@", VRAVErrorString(read)];
            ok = NO;
            break;
        }
        if (packet->stream_index == videoIndex) {
            if (avcodec_send_packet(videoDecoder, packet) >= 0) {
                ok = drainVideo();
            }
        } else if (audioInput != nil && packet->stream_index == audioIndex) {
            if (avcodec_send_packet(audioDecoder, packet) >= 0) {
                (void)drainAudio();
            }
        }
        av_packet_unref(packet);
    }

    if (ok) {
        avcodec_send_packet(videoDecoder, NULL);
        ok = drainVideo();
    }
    if (ok && audioInput != nil) {
        avcodec_send_packet(audioDecoder, NULL);
        (void)drainAudio();
    }
    if (ok && frameIndex == 0) {
        ok = NO;
        failureReason = @"source produced no frames";
    }

    if (ok) {
        [videoInput markAsFinished];
        if (audioInput != nil) [audioInput markAsFinished];
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(finished);
        }];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        if (writer.status != AVAssetWriterStatusCompleted) {
            ok = NO;
            failureReason = writer.error.localizedDescription ?: @"encoder did not finish";
        }
    } else {
        [writer cancelWriting];
    }

    if (scaler) sws_freeContext(scaler);
    if (frame) av_frame_free(&frame);
    if (resampled) av_frame_free(&resampled);
    if (packet) av_packet_free(&packet);
    if (resampler) swr_free(&resampler);
    if (audioDecoder) avcodec_free_context(&audioDecoder);
    avcodec_free_context(&videoDecoder);
    avformat_close_input(&input);

    if (!ok) {
        [NSFileManager.defaultManager removeItemAtURL:temporaryURL error:nil];
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorEncodeFailed,
                                      failureReason ?: @"video conversion failed");
        }
        return NO;
    }

    // Nothing is moved into place until the rewrite proves decodable, so a bad
    // encode cannot replace a source that at least still holds the pixels.
    if (![self fileIsDecodable:temporaryURL]) {
        [NSFileManager.defaultManager removeItemAtURL:temporaryURL error:nil];
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorVerifyFailed,
                                      @"converted video is still not playable");
        }
        return NO;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *installError = nil;
    BOOL installed = NO;
    if ([fm fileExistsAtPath:destinationURL.path]) {
        installed = [fm replaceItemAtURL:destinationURL
                           withItemAtURL:temporaryURL
                          backupItemName:nil
                                 options:0
                        resultingItemURL:NULL
                                   error:&installError];
    } else {
        installed = [fm moveItemAtURL:temporaryURL toURL:destinationURL error:&installError];
    }
    if (!installed) {
        [fm removeItemAtURL:temporaryURL error:nil];
        if (error) {
            *error = VRTranscodeError(VRTranscoderErrorInstallFailed,
                installError.localizedDescription ?: @"cannot install converted video");
        }
        return NO;
    }
    if (progress != nil) progress(1.0);
    return YES;
}

@end
