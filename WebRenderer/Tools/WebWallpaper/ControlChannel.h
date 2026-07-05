// ControlChannel — line-oriented JSON control protocol over stdin.
//
// Lets a parent process (Mirage.app) drive a running WebWallpaper without
// restarting it. Each line on stdin is one JSON object; see the wire protocol
// shared across the three renderers:
//
//   {"cmd":"setProperty","key":"bgmvolume","value":30}
//   {"cmd":"pause"} / {"cmd":"resume"}
//   {"cmd":"volume","value":0.5}   // 0..1
//   {"cmd":"muted","value":true}
//   {"cmd":"fps","value":30}
//   {"cmd":"quit"}
//
// A background thread reads stdin; each parsed command is delivered to the
// handler block ON THE MAIN THREAD (WKWebView requires main-thread access).
// EOF on stdin (parent closed the pipe / died) invokes onEOF so the wallpaper
// exits cleanly instead of outliving its owner.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface MirageControlChannel : NSObject

// handler is called on the main thread with each parsed {"cmd":...} dictionary.
// onEOF is called on the main thread when stdin closes or {"cmd":"quit"} is seen.
- (instancetype)initWithHandler:(void (^)(NSDictionary *command))handler
                          onEOF:(void (^)(void))onEOF NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (void)start;

@end

NS_ASSUME_NONNULL_END
