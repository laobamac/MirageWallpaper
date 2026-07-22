#import "ControlChannel.h"

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
        char chunk[4096];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n <= 0) break; // EOF or error
            [buffer appendBytes:chunk length:(NSUInteger)n];

            // Split on newlines; process each complete line, keep the remainder.
            for (;;) {
                const char *bytes = (const char *)buffer.bytes;
                NSUInteger len = buffer.length;
                NSUInteger nl = NSNotFound;
                for (NSUInteger i = 0; i < len; i++) {
                    if (bytes[i] == '\n') { nl = i; break; }
                }
                if (nl == NSNotFound) break;
                @autoreleasepool {
                    NSData *lineData = [buffer subdataWithRange:NSMakeRange(0, nl)];
                    [buffer replaceBytesInRange:NSMakeRange(0, nl + 1) withBytes:NULL length:0];
                    [self handleLineData:lineData];
                }
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
