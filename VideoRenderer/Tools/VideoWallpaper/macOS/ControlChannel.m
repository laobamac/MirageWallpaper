#import "ControlChannel.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

// A control line is a single JSON command; anything approaching this is either
// a bug or an attempt to make the renderer allocate until it dies. Without a
// cap a newline-free stream grows the buffer without bound.
static const NSUInteger kMirageMaxLineLength = 1024 * 1024; // 1 MiB

@implementation MirageControlChannel {
    void (^_handler)(NSDictionary *);
    void (^_onEOF)(void);
    BOOL _eofSent;
}

- (instancetype)initWithHandler:(void (^)(NSDictionary *))handler
                          onEOF:(void (^)(void))onEOF {
    self = [super init];
    if (self) {
        _handler = [handler copy];
        _onEOF = [onEOF copy];
        _eofSent = NO;
    }
    return self;
}

- (void)start {
    // Read stdin on a background thread; dispatch parsed commands to the main
    // thread. NSFileHandle readInBackground would couple to the run loop; a
    // plain thread with blocking reads is simpler and robust to EOF.
    [NSThread detachNewThreadSelector:@selector(readLoop) toTarget:self withObject:nil];
}

- (void)readLoop {
    @autoreleasepool {
        NSMutableData *buffer = [NSMutableData data];
        NSUInteger searchOffset = 0;  // bytes of `buffer` already scanned for '\n'
        BOOL discarding = NO;         // dropping an over-long line until the next '\n'
        BOOL warnedOverflow = NO;
        char chunk[4096];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break; // EOF or error

            if (discarding) {
                // Resync: throw away everything up to and including the next
                // newline, then resume parsing from the remainder.
                const char *nl = (const char *)memchr(chunk, '\n', (size_t)n);
                if (nl == NULL) continue;
                NSUInteger consumed = (NSUInteger)(nl - chunk) + 1;
                discarding = NO;
                searchOffset = 0;
                [buffer setLength:0];
                if (consumed < (NSUInteger)n) {
                    [buffer appendBytes:chunk + consumed length:(NSUInteger)n - consumed];
                }
            } else {
                [buffer appendBytes:chunk length:(NSUInteger)n];
            }

            // Split on newlines; process each complete line, keep the remainder.
            // searchOffset keeps this linear: without it every 4 KiB chunk
            // rescanned the whole buffer from the start.
            for (;;) {
                const char *bytes = (const char *)buffer.bytes;
                NSUInteger len = buffer.length;
                if (searchOffset >= len) break;
                const char *nl = (const char *)memchr(bytes + searchOffset, '\n', len - searchOffset);
                if (nl == NULL) { searchOffset = len; break; }
                NSUInteger index = (NSUInteger)(nl - bytes);
                @autoreleasepool {
                    NSData *lineData = [buffer subdataWithRange:NSMakeRange(0, index)];
                    [buffer replaceBytesInRange:NSMakeRange(0, index + 1) withBytes:NULL length:0];
                    searchOffset = 0;
                    [self handleLineData:lineData];
                }
            }

            if (buffer.length > kMirageMaxLineLength) {
                if (!warnedOverflow) {
                    warnedOverflow = YES;
                    fprintf(stderr,
                            "MirageControlChannel: control line exceeds %lu bytes; "
                            "discarding until the next newline\n",
                            (unsigned long)kMirageMaxLineLength);
                }
                [buffer setLength:0];
                searchOffset = 0;
                discarding = YES;
            }
        }
        [self signalEOF];
    }
}

- (void)handleLineData:(NSData *)lineData {
    if (lineData.length == 0) return;
    NSError *err = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:lineData options:0 error:&err];
    if (![obj isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *cmd = (NSDictionary *)obj;
    NSString *name = cmd[@"cmd"];
    if (![name isKindOfClass:[NSString class]]) return;

    if ([name isEqualToString:@"quit"]) {
        [self signalEOF];
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_handler) self->_handler(cmd);
    });
}

- (void)signalEOF {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_eofSent) return;
        self->_eofSent = YES;
        if (self->_onEOF) self->_onEOF();
    });
}

@end
