// ControlChannel — line-oriented JSON control protocol over stdin.
//
// Lets a parent process (Mirage.app) drive a running RmskinWallpaper without
// restarting it. Each line on stdin is one JSON object; the wire protocol used
// by RmskinRenderer:
//
//   {"cmd":"reload"}                             // re-parse + redraw all skins
//   {"cmd":"toggleConfig","name":"...","active":true}
//   {"cmd":"quit"}
//
// A background thread reads stdin; each parsed command is delivered to the
// handler block ON THE MAIN THREAD (AppKit requires main-thread access). EOF on
// stdin (parent closed the pipe / died) invokes onEOF so the host exits cleanly
// instead of outliving its owner.

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
