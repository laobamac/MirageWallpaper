#import "WRAudioTap.h"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Accelerate/Accelerate.h>
#import <os/lock.h>

// System-audio spectrum capture via the macOS 14.2+ Core Audio process-tap API.
//
// A global process tap (CATapDescription + AudioHardwareCreateProcessTap)
// captures the full system output mix; we wrap that tap in a private aggregate
// device and read its input via an IO proc. Pure Core Audio — no Screen
// Recording permission (vs ScreenCaptureKit) and no microphone. This is the
// same mechanism wavsen uses for SceneRenderer's audio response.
//
// Pipeline:
//   aggregate IO proc → inInputData (tapped system mix) → downmix mono float
//     → ring buffer (most recent FFT_N samples)
//   copySpectrum: (polled ~30 Hz) → Hann window → vDSP real DFT
//     → |bin| → group → 64 bins → log-compress + smooth (0..1)
//
// FFT_N=1024 ⇒ ~21 ms window at 48 kHz; grouped 8:1 ⇒ 64 bins spanning
// ~0..24 kHz, matching WE's 64-bin audio-listener contract. The engine
// duplicates the mono spectrum into both halves of the 128-float (L+R) array
// the page expects.

static const vDSP_Length kFFT_N     = 1024;
static const NSUInteger  kBinCount  = 64;
static const NSUInteger  kRingCap   = 2048;

static OSStatus WRTapIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                            const AudioBufferList *inInputData,
                            const AudioTimeStamp *inInputTime,
                            AudioBufferList *outOutputData,
                            const AudioTimeStamp *inOutputTime, void *inClientData);

@interface WRAudioTap ()
@end

@implementation WRAudioTap {
    AudioObjectID              _tap;
    AudioObjectID              _aggregate;
    AudioDeviceIOProcID        _ioProcID;
    AudioStreamBasicDescription _asbd;

    float                      _ring[kRingCap];
    NSUInteger                 _ringWrite;
    NSUInteger                 _ringFilled;
    os_unfair_lock_s           _ringLock;

    vDSP_DFT_Setup             _dftSetup;
    float                      _hann[kFFT_N];

    float                      _smoothed[kBinCount];
    BOOL                       _haveData;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _tap = kAudioObjectUnknown;
        _aggregate = kAudioObjectUnknown;
        _ringLock = OS_UNFAIR_LOCK_INIT;
        memset(_smoothed, 0, sizeof(_smoothed));
    }
    return self;
}

- (void)dealloc {
    [self stop];
    if (_dftSetup != NULL) { vDSP_DFT_DestroySetup(_dftSetup); _dftSetup = NULL; }
}

- (NSUInteger)binCount { return kBinCount; }
- (BOOL)running        { return _ioProcID != NULL; }

#pragma mark - Lifecycle

// Create a global process tap (all system audio). Returns the tap UID string.
- (NSString *)createGlobalTap {
    if (@available(macOS 14.2, *)) {
        CATapDescription *desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        if (!desc) return nil;
        desc.name = @"WebRenderer Audio Capture";
        desc.privateTap = YES;
        desc.muteBehavior = CATapUnmuted;   // don't mute the tapped audio

        AudioObjectID tap = kAudioObjectUnknown;
        OSStatus st = AudioHardwareCreateProcessTap(desc, &tap);
        if (st != noErr || tap == kAudioObjectUnknown) return nil;
        _tap = tap;

        CFStringRef uidRef = NULL;
        UInt32 size = sizeof(uidRef);
        AudioObjectPropertyAddress addr = {
            kAudioTapPropertyUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(tap, &addr, 0, NULL, &size, &uidRef) != noErr || !uidRef) {
            AudioHardwareDestroyProcessTap(tap); _tap = kAudioObjectUnknown;
            return nil;
        }
        NSString *uid = (__bridge_transfer NSString *)uidRef;
        return uid;
    }
    return nil;
}

// Build the aggregate's description as a CFDictionary (the key constants are
// #define'd to C string literals, so CFSTR() wraps them). Mirrors wavsen's
// proven construction: tap-list only, private, no auto-start.
- (OSStatus)createAggregateWithTapUID:(NSString *)tapUID {
    CFStringRef aggUID = CFStringCreateWithCString(NULL,
        [[NSString stringWithFormat:@"WebRenderer.Tap.%u", (unsigned)arc4random()] UTF8String],
        kCFStringEncodingUTF8);
    CFStringRef tapUIDRef = (__bridge CFStringRef)tapUID;

    CFMutableDictionaryRef subtap = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int one = 1, zero = 0;
    CFNumberRef oneNum = CFNumberCreate(NULL, kCFNumberIntType, &one);
    CFNumberRef zeroNum = CFNumberCreate(NULL, kCFNumberIntType, &zero);
    CFDictionarySetValue(subtap, CFSTR(kAudioSubTapUIDKey), tapUIDRef);
    CFDictionarySetValue(subtap, CFSTR(kAudioSubTapDriftCompensationKey), oneNum);

    const void *subtaps[] = { subtap };
    CFArrayRef tapList = CFArrayCreate(NULL, subtaps, 1, &kCFTypeArrayCallBacks);

    CFMutableDictionaryRef aggDesc = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(aggDesc, CFSTR(kAudioAggregateDeviceNameKey), CFSTR("WebRenderer Tap"));
    CFDictionarySetValue(aggDesc, CFSTR(kAudioAggregateDeviceUIDKey), aggUID);
    CFDictionarySetValue(aggDesc, CFSTR(kAudioAggregateDeviceIsPrivateKey), oneNum);
    CFDictionarySetValue(aggDesc, CFSTR(kAudioAggregateDeviceTapAutoStartKey), zeroNum);
    CFDictionarySetValue(aggDesc, CFSTR(kAudioAggregateDeviceTapListKey), tapList);

    AudioObjectID aggID = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice(aggDesc, &aggID);
    if (st == noErr) _aggregate = aggID;

    CFRelease(aggDesc); CFRelease(tapList); CFRelease(subtap);
    CFRelease(oneNum); CFRelease(zeroNum); CFRelease(aggUID);
    return st;
}

- (BOOL)readTapFormat {
    if (_tap == kAudioObjectUnknown) return NO;
    AudioStreamBasicDescription asbd = {0};
    UInt32 size = sizeof(asbd);
    AudioObjectPropertyAddress addr = {
        kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(_tap, &addr, 0, NULL, &size, &asbd) != noErr) return NO;
    if (asbd.mSampleRate <= 0.0 || asbd.mChannelsPerFrame == 0) return NO;
    _asbd = asbd;
    return YES;
}

- (void)startWithCompletion:(void (^)(BOOL, NSString *_Nullable))completion {
    if (_ioProcID != NULL) { if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(YES, nil); }); return; }

    NSString *tapUID = [self createGlobalTap];
    if (tapUID.length == 0) {
        [self fail:completion msg:@"global process tap unavailable (needs macOS 14.2+)"];
        return;
    }

    OSStatus st = [self createAggregateWithTapUID:tapUID];
    if (st != noErr || _aggregate == kAudioObjectUnknown) {
        [self destroyTap];
        [self fail:completion msg:[NSString stringWithFormat:@"create aggregate failed: %d", (int)st]];
        return;
    }

    [self readTapFormat];  // best-effort; IO proc reads inInputData regardless

    st = AudioDeviceCreateIOProcID(_aggregate, WRTapIOProc, (__bridge void *)self, &_ioProcID);
    if (st != noErr) {
        [self destroyAggregate]; [self destroyTap];
        [self fail:completion msg:[NSString stringWithFormat:@"create IO proc failed: %d", (int)st]];
        return;
    }

    st = AudioDeviceStart(_aggregate, _ioProcID);
    if (st != noErr) {
        AudioDeviceDestroyIOProcID(_aggregate, _ioProcID); _ioProcID = NULL;
        [self destroyAggregate]; [self destroyTap];
        [self fail:completion msg:[NSString stringWithFormat:@"AudioDeviceStart failed: %d", (int)st]];
        return;
    }

    if (getenv("WR_DEBUG")) {
        fprintf(stderr, "WebRenderer: audio tap running (%u ch @ %.0f Hz)\n",
                _asbd.mChannelsPerFrame, _asbd.mSampleRate);
    }
    if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(YES, nil); });
}

- (void)fail:(void (^)(BOOL, NSString *))completion msg:(NSString *)msg {
    if (getenv("WR_DEBUG")) fprintf(stderr, "WebRenderer: audio tap disabled (%s)\n", msg.UTF8String ?: "?");
    if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(NO, msg); });
}

- (void)stop {
    if (_ioProcID != NULL && _aggregate != kAudioObjectUnknown) {
        AudioDeviceStop(_aggregate, _ioProcID);
        AudioDeviceDestroyIOProcID(_aggregate, _ioProcID);
        _ioProcID = NULL;
    }
    [self destroyAggregate];
    [self destroyTap];
}

- (void)destroyAggregate {
    if (_aggregate != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(_aggregate);
        _aggregate = kAudioObjectUnknown;
    }
}

- (void)destroyTap {
    if (_tap != kAudioObjectUnknown) {
        if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(_tap);
        _tap = kAudioObjectUnknown;
    }
}

#pragma mark - IO proc (downmix to mono float, append to ring)

- (void)ingestInput:(const AudioBufferList *)abl {
    if (abl == NULL || abl->mNumberBuffers == 0) return;

    UInt32 channels = _asbd.mChannelsPerFrame > 0 ? _asbd.mChannelsPerFrame : 2;
    BOOL isFloat = (_asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    BOOL isNonInterleaved = (_asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;

    // Default to non-interleaved float32 if the format wasn't read.
    if (_asbd.mFormatID == 0) { isFloat = YES; isNonInterleaved = YES; }
    if (!isFloat) return;  // tap delivers float in practice

    const NSUInteger kBlock = 2048;
    float tmp[kBlock];

    if (isNonInterleaved) {
        UInt32 nBuffers = abl->mNumberBuffers; if (nBuffers == 0) return;
        UInt32 nFrames = abl->mBuffers[0].mDataByteSize / sizeof(float);
        for (UInt32 f = 0; f < nFrames; ) {
            UInt32 block = (UInt32)MIN(kBlock, (NSUInteger)(nFrames - f));
            for (UInt32 i = 0; i < block; ++i) {
                float sum = 0;
                for (UInt32 b = 0; b < nBuffers; ++b)
                    sum += ((const float *)abl->mBuffers[b].mData)[f + i];
                tmp[i] = sum / (float)nBuffers;
            }
            [self appendMonoFloat:tmp count:block];
            f += block;
        }
    } else {
        UInt32 bytesPerFrame = _asbd.mBytesPerFrame ?: (channels * sizeof(float));
        const float *src = (const float *)abl->mBuffers[0].mData;
        UInt32 nFrames = abl->mBuffers[0].mDataByteSize / bytesPerFrame;
        for (UInt32 f = 0; f < nFrames; ) {
            UInt32 block = (UInt32)MIN(kBlock, (NSUInteger)(nFrames - f));
            for (UInt32 i = 0; i < block; ++i) {
                const float *p = src + (f + i) * channels;
                float sum = 0;
                for (UInt32 c = 0; c < channels; ++c) sum += p[c];
                tmp[i] = sum / (float)channels;
            }
            [self appendMonoFloat:tmp count:block];
            f += block;
        }
    }
}

- (void)appendMonoFloat:(const float *)samples count:(NSUInteger)count {
    os_unfair_lock_lock(&_ringLock);
    for (NSUInteger i = 0; i < count; ++i) {
        _ring[_ringWrite & (kRingCap - 1)] = samples[i];
        _ringWrite++;
        if (_ringFilled < kRingCap) _ringFilled++;
    }
    _haveData = YES;
    os_unfair_lock_unlock(&_ringLock);
}

#pragma mark - Spectrum

- (void)ensureDFTSetup {
    if (_dftSetup != NULL) return;
    _dftSetup = vDSP_DFT_zrop_CreateSetup(NULL, kFFT_N, vDSP_DFT_FORWARD);
    vDSP_hann_window(_hann, kFFT_N, vDSP_HANN_NORM);
}

- (BOOL)copySpectrum:(float *)outBins count:(NSUInteger)count {
    if (count != kBinCount || !_haveData) {
        memset(outBins, 0, count * sizeof(float));
        return NO;
    }
    [self ensureDFTSetup];
    if (_dftSetup == NULL) { memset(outBins, 0, count * sizeof(float)); return NO; }

    float window[kFFT_N];
    os_unfair_lock_lock(&_ringLock);
    NSUInteger filled = _ringFilled;
    if (filled >= kFFT_N) {
        NSUInteger start = _ringWrite - kFFT_N;
        for (vDSP_Length i = 0; i < kFFT_N; ++i)
            window[i] = _ring[(start + i) & (kRingCap - 1)];
    } else if (filled > 0) {
        memset(window, 0, (kFFT_N - filled) * sizeof(float));
        for (NSUInteger i = 0; i < filled; ++i)
            window[(kFFT_N - filled) + i] = _ring[i & (kRingCap - 1)];
    } else {
        os_unfair_lock_unlock(&_ringLock);
        memset(outBins, 0, count * sizeof(float));
        return NO;
    }
    os_unfair_lock_unlock(&_ringLock);

    vDSP_vmul(window, 1, _hann, 1, window, 1, kFFT_N);

    // vDSP real DFT expects even/odd split, not contiguous input.
    // Passing it directly uses only half the samples and halves the bin.
    float splitEven[kFFT_N / 2];
    float splitOdd[kFFT_N / 2];
    float realOut[kFFT_N / 2];
    float imagOut[kFFT_N / 2];
    DSPSplitComplex packed = { .realp = splitEven, .imagp = splitOdd };
    vDSP_ctoz((const DSPComplex *)window, 2, &packed, 1, kFFT_N / 2);
    vDSP_DFT_Execute(_dftSetup, splitEven, splitOdd, realOut, imagOut);

    const NSUInteger half = kFFT_N / 2;
    const NSUInteger usableBins = half - 1;
    const NSUInteger perGroup = usableBins / kBinCount;

    float grouped[kBinCount];
    for (NSUInteger b = 0; b < kBinCount; ++b) {
        float sumMag = 0;
        NSUInteger startBin = 1 + b * perGroup;
        for (NSUInteger k = 0; k < perGroup; ++k) {
            float re = realOut[startBin + k];
            float im = imagOut[startBin + k];
            sumMag += sqrtf(re * re + im * im);
        }
        grouped[b] = sumMag / (float)perGroup;
    }

    // vDSP real DFT is 2x the mathematical DFT, so 2/N becomes 1/half here.
    const float norm = 1.0f / (float)half;
    const float logBase = 4.0f;
    for (NSUInteger b = 0; b < kBinCount; ++b) {
        float m = grouped[b] * norm;
        float v = log10f(1.0f + m * 1000.0f) / logBase;
        if (v < 0) v = 0; if (v > 1) v = 1;
        _smoothed[b] = 0.4f * v + 0.6f * _smoothed[b];
        outBins[b] = _smoothed[b];
    }
    return YES;
}

@end

static OSStatus WRTapIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                            const AudioBufferList *inInputData,
                            const AudioTimeStamp *inInputTime,
                            AudioBufferList *outOutputData,
                            const AudioTimeStamp *inOutputTime, void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;
    WRAudioTap *tap = (__bridge WRAudioTap *)inClientData;
    [tap ingestInput:inInputData];
    return noErr;
}
