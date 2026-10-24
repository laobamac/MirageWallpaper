// ControlChannel — line-oriented JSON control protocol over stdin.
//
// Lets a parent process (Mirage.app) drive a running VideoWallpaper without
// restarting it. Each line on stdin is one JSON object; shared wire protocol
// across the three renderers:
//
//   {"cmd":"pause"} / {"cmd":"resume"}
//   {"cmd":"volume","value":0.5}   // 0..1
//   {"cmd":"muted","value":true}
//   {"cmd":"fillmode","value":"cover"|"contain"|"stretch"}
//   {"cmd":"seek","t":12.5}        // jump to an absolute offset (seconds)
//   {"cmd":"playbackTime"}         // replies {"event":"playbackTime","t":..}
//   {"cmd":"sync","anchor":<CLOCK_MONOTONIC_RAW>}
//         Cross-display playback sync. `anchor` is the monotonic time at which
//         this wallpaper's playback started, shared by every renderer showing
//         it; each one maps it to fmod(now - anchor, duration) and trims its
//         rate to stay on it. Optional keys: "hold" (tolerance), "trim" (false
//         on the renderer that owns audio), "join" (land now, do not chase).
//   {"cmd":"setProperty","key":"...","value":...}   // (no live video props; ignored)
//   {"cmd":"quit"}
//
// A background thread reads stdin; each parsed command is delivered to the
// handler block ON THE MAIN THREAD (AVPlayer/CALayer require main-thread
// access). EOF on stdin (parent closed the pipe / died) invokes onEOF so the
// wallpaper exits cleanly instead of outliving its owner.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface MirageControlChannel : NSObject

- (instancetype)initWithHandler:(void (^)(NSDictionary *command))handler
                          onEOF:(void (^)(void))onEOF NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (void)start;

@end

NS_ASSUME_NONNULL_END
