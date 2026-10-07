// Copyright © 2026 王孝慈. All rights reserved.

#import <Foundation/Foundation.h>
#import "WRLive2DCompatibility.h"
#import "WRURLSchemeHandler.h"

static void Check(BOOL condition, NSString *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message.UTF8String);
        exit(1);
    }
}

@interface WRTestTask : NSObject <WKURLSchemeTask>
@property (nonatomic, copy) NSURLRequest *request;
@property (nonatomic, strong) NSHTTPURLResponse *response;
@property (nonatomic, strong) NSMutableData *body;
@property (nonatomic, strong) NSError *error;
@property (nonatomic) BOOL finished;
@end

@implementation WRTestTask
- (void)didReceiveResponse:(NSURLResponse *)response { self.response = (NSHTTPURLResponse *)response; }
- (void)didReceiveData:(NSData *)data { [self.body appendData:data]; }
- (void)didFinish { self.finished = YES; }
- (void)didFailWithError:(NSError *)error { self.error = error; self.finished = YES; }
@end

static WRTestTask *Request(WRURLSchemeHandler *handler, NSString *name, NSDictionary *headers) {
    WRTestTask *task = [WRTestTask new];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:
        [NSURL URLWithString:[@"we-wallpaper://wallpaper/" stringByAppendingString:name]]];
    request.allHTTPHeaderFields = headers;
    task.request = request;
    task.body = [NSMutableData data];
    WKWebView *view = nil;
    [handler webView:view startURLSchemeTask:task];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
    while (!task.finished && deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    Check(task.finished && task.error == nil, @"scheme request completed");
    return task;
}

static void CheckResponses(WRURLSchemeHandler *handler, NSString *name, NSData *expected, BOOL adapted) {
    WRTestTask *full = Request(handler, name, @{});
    Check(full.response.statusCode == 200, @"full response status");
    Check([full.body isEqualToData:expected], @"full response bytes");
    Check([[full.response valueForHTTPHeaderField:@"Content-Length"] integerValue] == (NSInteger)expected.length,
          @"full response length");
    NSString *etag = [full.response valueForHTTPHeaderField:@"ETag"];
    Check(etag.length > 0, @"response ETag");
    if (adapted) Check([etag isEqualToString:WRLive2DScriptETag(expected)], @"adapted ETag");
    WRTestTask *cached = Request(handler, name, @{@"If-None-Match": etag});
    Check(cached.response.statusCode == 304 && cached.body.length == 0, @"conditional response");
    NSUInteger start = MIN((NSUInteger)7, expected.length - 1);
    NSUInteger length = MIN((NSUInteger)29, expected.length - start);
    NSString *range = [NSString stringWithFormat:@"bytes=%lu-%lu", start, start + length - 1];
    WRTestTask *partial = Request(handler, name, @{@"Range": range});
    Check(partial.response.statusCode == 206, @"range response status");
    Check([partial.body isEqualToData:[expected subdataWithRange:NSMakeRange(start, length)]], @"range response bytes");
    Check([[partial.response valueForHTTPHeaderField:@"Content-Length"] integerValue] == (NSInteger)length,
          @"range response length");
    Check([[partial.response valueForHTTPHeaderField:@"Content-Range"] isEqualToString:
        [NSString stringWithFormat:@"bytes %lu-%lu/%lu", start, start + length - 1, expected.length]], @"range total");
    WRTestTask *invalid = Request(handler, name,
        @{@"Range": [NSString stringWithFormat:@"bytes=%lu-", expected.length]});
    Check(invalid.response.statusCode == 416, @"out-of-bounds range rejected");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSData *plain = [@"window.example = 'ordinary wallpaper';" dataUsingEncoding:NSUTF8StringEncoding];
        Check(WRAdaptLive2DScript(plain) == nil, @"ordinary script unchanged");
        Check(WRAdaptLive2DScript([NSData data]) == nil, @"empty script unchanged");
        unsigned char invalid[] = {0xff, 0xfe, 0xff};
        Check(WRAdaptLive2DScript([NSData dataWithBytes:invalid length:sizeof(invalid)]) == nil, @"invalid UTF-8 unchanged");
        Check(WRAdaptLive2DScript([NSMutableData dataWithLength:4 * 1024 * 1024 + 1]) == nil, @"oversized script unchanged");
        NSData *unknown = [@"class CubismRenderer_WebGL {}\n  const WL = window.WallpaperLayout;" dataUsingEncoding:NSUTF8StringEncoding];
        Check(WRAdaptLive2DScript(unknown) == nil, @"unknown runtime unchanged");
        Check(![WRLive2DScriptETag(plain) isEqualToString:WRLive2DScriptETag(unknown)], @"ETags distinguish content");

        NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"mirage-live2d-tests-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSFileManager *fm = NSFileManager.defaultManager;
        Check([fm createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil], @"temporary directory");
        NSString *path = [directory stringByAppendingPathComponent:@"ordinary.js"];
        Check([plain writeToFile:path atomically:YES], @"ordinary fixture");
        WRURLSchemeHandler *handler = [[WRURLSchemeHandler alloc] initWithBaseDirectory:directory];
        for (NSNumber *memory in @[@NO, @YES]) {
            handler.loadFromMemory = memory.boolValue;
            [handler clearMemoryCache];
            CheckResponses(handler, @"ordinary.js", plain, NO);
        }

        if (argc > 1) {
            NSData *original = [NSData dataWithContentsOfFile:@(argv[1])];
            Check(original != nil, @"integration fixture readable");
            NSData *adapted = WRAdaptLive2DScript(original);
            Check(adapted != nil && ![adapted isEqualToData:original], @"supported runtime adapted");
            Check(WRAdaptLive2DScript(adapted) == nil, @"adaptation is not applied twice");
            NSString *text = [[NSString alloc] initWithData:original encoding:NSUTF8StringEncoding];
            NSString *result = [[NSString alloc] initWithData:adapted encoding:NSUTF8StringEncoding];
            Check([result componentsSeparatedByString:@"if (!bufferData.__wrUploaded) this.gl.bufferData"].count == 6,
                  @"all five upload sites adapted");
            Check([result componentsSeparatedByString:@"__wrLive2DCompatibility.attachApplication(app);"].count == 2,
                  @"one application hook");
            Check([result containsString:@"resolution: window.devicePixelRatio || 1"], @"Retina resolution enabled");
            NSString *changed = [text stringByReplacingOccurrencesOfString:@"6.5.10" withString:@"6.5.11"];
            Check(WRAdaptLive2DScript([changed dataUsingEncoding:NSUTF8StringEncoding]) == nil, @"different runtime rejected");
            changed = [text stringByReplacingOccurrencesOfString:@"      resizeTo: window,"
                                                     withString:@"      resolution: 2,\n      resizeTo: window,"];
            Check(WRAdaptLive2DScript([changed dataUsingEncoding:NSUTF8StringEncoding]) == nil, @"authored resolution not overridden");
            changed = [text stringByAppendingString:@"\n  const WL = window.WallpaperLayout;"];
            Check(WRAdaptLive2DScript([changed dataUsingEncoding:NSUTF8StringEncoding]) == nil, @"ambiguous boundary rejected");
            Check(![WRLive2DScriptETag(adapted) isEqualToString:WRLive2DScriptETag(original)], @"original and adapted ETags differ");

            for (NSString *name in @[@"renamed.js", @"module.mjs", @"untouched.json"]) {
                Check([original writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES], @"integration fixture copy");
            }
            for (NSNumber *memory in @[@NO, @YES]) {
                handler.loadFromMemory = memory.boolValue;
                [handler clearMemoryCache];
                CheckResponses(handler, @"renamed.js", adapted, YES);
                CheckResponses(handler, @"module.mjs", adapted, YES);
                CheckResponses(handler, @"untouched.json", original, NO);
                WRTestTask *end = Request(handler, @"renamed.js", @{@"Range":
                    [NSString stringWithFormat:@"bytes=%lu-", adapted.length - 17]});
                Check([end.body isEqualToData:[adapted subdataWithRange:NSMakeRange(adapted.length - 17, 17)]], @"adapted tail range");
                WRTestTask *oldETag = Request(handler, @"renamed.js", @{@"If-None-Match": WRLive2DScriptETag(original)});
                Check(oldETag.response.statusCode == 200, @"old script ETag cannot validate adapted content");
            }
            NSString *overlay = [directory stringByAppendingPathComponent:@"overlay"];
            Check([fm createDirectoryAtPath:overlay withIntermediateDirectories:YES attributes:nil error:nil], @"overlay directory");
            Check([plain writeToFile:[overlay stringByAppendingPathComponent:@"renamed.js"] atomically:YES], @"overlay fixture");
            handler.overlayDirectories = @[overlay];
            [handler clearMemoryCache];
            CheckResponses(handler, @"renamed.js", plain, NO);
            handler.overlayDirectories = @[];
            [handler clearMemoryCache];
            CheckResponses(handler, @"renamed.js", adapted, YES);
            Check([[NSData dataWithContentsOfFile:@(argv[1])] isEqualToData:original], @"original wallpaper file unchanged");
            fprintf(stdout, "PASS: supported Live2D adaptation, isolation, disk/memory, Range, ETag and overlays\n");
        } else {
            fprintf(stdout, "SKIP: external Live2D fixture (pass a supported script path to enable integration checks)\n");
        }
        [handler clearMemoryCache];
        Check([fm removeItemAtPath:directory error:nil], @"temporary fixture cleanup");
        fprintf(stdout, "PASS: compatibility rejection and ordinary resource responses\n");
    }
    return 0;
}
