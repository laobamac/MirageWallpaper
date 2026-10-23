#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// System-audio spectrum capture for audio-reactive web wallpapers.
// Counterpart to OWE's wavsen capture + BrowserHost::PushAudioData: the page
// registers wallpaperRegisterAudioListener(cb); the engine polls this tap
// ~30 Hz and feeds a 128-float array (64 L + 64 R) via __wr_pushAudio.
//
// Uses AVAudioEngine (microphone input) + Accelerate/vDSP FFT. Microphone
// permission (TCC) is prompted on first start; deny it and the wallpaper
// still renders, only audio-listener callbacks go quiet.
@interface WRAudioTap : NSObject

@property (nonatomic, readonly) NSUInteger binCount; // always 64

- (void)startWithCompletion:(void (^)(BOOL ok, NSString *_Nullable message))completion;
- (void)stop;
@property (nonatomic, readonly) BOOL running;

// Fill outBins (length == binCount) with the current spectrum (0..1).
// Returns YES if audio has arrived; NO (and zeros the buffer) otherwise.
- (BOOL)copySpectrum:(float *)outBins count:(NSUInteger)count;

@end

NS_ASSUME_NONNULL_END
