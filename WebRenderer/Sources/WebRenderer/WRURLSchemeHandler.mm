#import "WRURLSchemeHandler.h"

static NSString *const kScheme = @"we-wallpaper";
static NSString *const kHost   = @"wallpaper";

static NSString *MIMEForExtension(NSString *ext) {
    static NSDictionary<NSString *, NSString *> *map = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        map = @{
            @"html": @"text/html", @"htm": @"text/html",
            @"js": @"application/javascript", @"mjs": @"application/javascript",
            @"css": @"text/css", @"json": @"application/json",
            @"txt": @"text/plain", @"xml": @"application/xml",
            @"svg": @"image/svg+xml",
            @"png": @"image/png", @"jpg": @"image/jpeg", @"jpeg": @"image/jpeg",
            @"gif": @"image/gif", @"webp": @"image/webp", @"bmp": @"image/bmp",
            @"ico": @"image/x-icon",
            @"webm": @"video/webm", @"mp4": @"video/mp4", @"ogv": @"video/ogg",
            @"mov": @"video/quicktime",
            @"mp3": @"audio/mpeg", @"wav": @"audio/wav", @"ogg": @"audio/ogg",
            @"oga": @"audio/ogg", @"opus": @"audio/opus", @"m4a": @"audio/mp4",
            @"aac": @"audio/aac", @"flac": @"audio/flac",
            // Spine / wallpaper assets.
            @"skel": @"application/octet-stream", @"atlas": @"text/plain",
            @"bin": @"application/octet-stream", @"wasm": @"application/wasm",
            @"woff": @"font/woff", @"woff2": @"font/woff2",
            @"ttf": @"font/ttf", @"otf": @"font/otf",
        };
    });
    return map[ext.lowercaseString] ?: @"application/octet-stream";
}

@interface WRURLSchemeHandler ()
@property (nonatomic, strong) dispatch_queue_t ioQueue;
@property (nonatomic, strong) NSMutableDictionary<NSValue *, NSNumber *> *taskStates;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSData *> *memoryCache;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSNumber *> *memoryModificationTimes;
@end

@implementation WRURLSchemeHandler

- (instancetype)init { return [self initWithBaseDirectory:@""]; }

- (instancetype)initWithBaseDirectory:(NSString *)baseDirectory {
    self = [super init];
    if (self) {
        _baseDirectory = [[baseDirectory stringByStandardizingPath] copy];
        _overlayDirectories = @[];
        _ioQueue = dispatch_queue_create("WebRenderer.schemeHandler", DISPATCH_QUEUE_CONCURRENT);
        _taskStates = [NSMutableDictionary dictionary];
        _memoryCache = [NSMutableDictionary dictionary];
        _memoryModificationTimes = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)clearMemoryCache {
    @synchronized (self) {
        [self.memoryCache removeAllObjects];
        [self.memoryModificationTimes removeAllObjects];
    }
}

- (nullable NSString *)safePathForRelative:(NSString *)relative inDirectory:(NSString *)directory {
    if (relative.length == 0) return nil;
    while (relative.length > 0 && [relative characterAtIndex:0] == '/') {
        relative = [relative substringFromIndex:1];
    }
    NSString *base = [[directory stringByStandardizingPath] stringByResolvingSymlinksInPath];
    NSString *combined = [base stringByAppendingPathComponent:relative];
    NSString *standardized = [[combined stringByStandardizingPath] stringByResolvingSymlinksInPath];
    if (![standardized isEqualToString:base] &&
        ![standardized hasPrefix:[base stringByAppendingString:@"/"]]) {
        return nil;
    }
    return standardized;
}

- (nullable NSString *)resolvedPathForRelative:(NSString *)relative {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *normalized = [relative stringByTrimmingCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@"/"]];
    if (![normalized.lowercaseString isEqualToString:@"project.json"]) {
        for (NSString *overlay in _overlayDirectories) {
            NSString *candidate = [self safePathForRelative:relative inDirectory:overlay];
            BOOL isDirectory = NO;
            BOOL cached = NO;
            if (self.loadFromMemory && candidate != nil) {
                @synchronized (self) { cached = self.memoryCache[candidate] != nil; }
            }
            if (candidate != nil && (cached ||
                ([fm fileExistsAtPath:candidate isDirectory:&isDirectory] && !isDirectory))) {
                return candidate;
            }
        }
    }
    return [self safePathForRelative:relative inDirectory:_baseDirectory];
}

- (nullable NSString *)resolvedLocalPathForURL:(NSURL *)url {
    NSURLComponents *components = [NSURLComponents componentsWithURL:url
                                               resolvingAgainstBaseURL:NO];
    NSString *requestedPath = nil;
    for (NSURLQueryItem *item in components.queryItems ?: @[]) {
        if ([item.name isEqualToString:@"path"] && item.value.length > 0) {
            requestedPath = item.value;
            break;
        }
    }
    if (requestedPath.length == 0 || ![requestedPath isAbsolutePath]) return nil;

    NSString *candidate = [[requestedPath stringByStandardizingPath] stringByResolvingSymlinksInPath];
    NSArray<NSString *> *roots = [(_overlayDirectories ?: @[]) arrayByAddingObject:_baseDirectory ?: @""];
    for (NSString *directory in roots) {
        NSString *root = [[directory stringByStandardizingPath] stringByResolvingSymlinksInPath];
        if (root.length == 0) continue;
        if ([candidate isEqualToString:root] ||
            [candidate hasPrefix:[root stringByAppendingString:@"/"]]) {
            return candidate;
        }
    }
    return nil;
}

#pragma mark - WKURLSchemeHandler

- (NSValue *)keyForTask:(id<WKURLSchemeTask>)task {
    return [NSValue valueWithPointer:(__bridge const void *)task];
}

- (BOOL)isTaskActive:(id<WKURLSchemeTask>)task {
    @synchronized (self) {
        return self.taskStates[[self keyForTask:task]] != nil;
    }
}

- (void)finishTrackingTask:(id<WKURLSchemeTask>)task {
    @synchronized (self) {
        [self.taskStates removeObjectForKey:[self keyForTask:task]];
    }
}

- (void)reportReadFailure:(id<WKURLSchemeTask>)task {
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([self isTaskActive:task]) {
            NSError *error = [NSError errorWithDomain:NSCocoaErrorDomain
                                                 code:NSFileReadUnknownError
                                             userInfo:nil];
            @try { [task didFailWithError:error]; } @catch (NSException *exception) {}
        }
        [self finishTrackingTask:task];
    });
}

- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    NSString *rawPath = task.request.URL.path ?: @"";
    NSString *rel = [rawPath stringByRemovingPercentEncoding] ?: rawPath;
    NSString *filePath = [rel isEqualToString:@"/__mirage_local"]
        ? [self resolvedLocalPathForURL:task.request.URL]
        : [self resolvedPathForRelative:rel];

    if (filePath == nil) { [self respondNotFound:task]; return; }
    @synchronized (self) {
        self.taskStates[[self keyForTask:task]] = @YES;
    }

    dispatch_async(self.ioQueue, ^{
        @autoreleasepool {
        NSData *memoryData = nil;
        NSNumber *memoryModificationTime = nil;
        if (self.loadFromMemory) {
            @synchronized (self) {
                memoryData = self.memoryCache[filePath];
                memoryModificationTime = self.memoryModificationTimes[filePath];
            }
        }

        NSFileManager *fm = NSFileManager.defaultManager;
        NSDictionary<NSFileAttributeKey, id> *attributes = nil;
        unsigned long long fileSize = memoryData.length;
        if (memoryData == nil) {
            BOOL isDir = NO;
            if (![fm fileExistsAtPath:filePath isDirectory:&isDir] || isDir) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([self isTaskActive:task]) [self respondNotFound:task];
                    [self finishTrackingTask:task];
                });
                return;
            }
            attributes = [fm attributesOfItemAtPath:filePath error:nil];
            fileSize = [attributes[NSFileSize] unsignedLongLongValue];
            if (fileSize > NSUIntegerMax) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([self isTaskActive:task]) [self respondNotFound:task];
                    [self finishTrackingTask:task];
                });
                return;
            }

            if (self.loadFromMemory) {
                // Serialize the first read for a path so concurrent Range
                // requests cannot load the same large file more than once.
                @synchronized (self) {
                    memoryData = self.memoryCache[filePath];
                    memoryModificationTime = self.memoryModificationTimes[filePath];
                    if (memoryData == nil) {
                        memoryData = [NSData dataWithContentsOfFile:filePath
                                                           options:NSDataReadingUncached
                                                             error:nil];
                        if (memoryData != nil) {
                            memoryModificationTime = @([attributes[NSFileModificationDate]
                                timeIntervalSince1970]);
                            self.memoryCache[filePath] = memoryData;
                            self.memoryModificationTimes[filePath] = memoryModificationTime;
                        }
                    }
                }
                if (memoryData == nil) {
                    [self reportReadFailure:task];
                    return;
                }
                fileSize = memoryData.length;
            }
        }

        NSString *mime = MIMEForExtension(filePath.pathExtension);
        NSUInteger total = (NSUInteger)fileSize;

        // Parse Range: bytes=start-end (end optional).
        NSString *rangeHeader = [task.request valueForHTTPHeaderField:@"Range"];
        NSUInteger start = 0, end = total > 0 ? total - 1 : 0;
        BOOL hasRange = NO;
        if (rangeHeader.length > 0 && total > 0) {
            NSRange prefix = [rangeHeader rangeOfString:@"bytes=" options:NSCaseInsensitiveSearch];
            if (prefix.location != NSNotFound) {
                NSString *spec = [rangeHeader substringFromIndex:prefix.location + prefix.length];
                NSArray<NSString *> *parts = [spec componentsSeparatedByString:@"-"];
                if (parts.count >= 1 && parts[0].length > 0) {
                    unsigned long long requestedStart = strtoull(parts[0].UTF8String, NULL, 10);
                    start = requestedStart > NSUIntegerMax ? NSUIntegerMax : (NSUInteger)requestedStart;
                    hasRange = YES;
                    if (parts.count >= 2 && parts[1].length > 0) {
                        unsigned long long requestedEnd = strtoull(parts[1].UTF8String, NULL, 10);
                        end = requestedEnd > NSUIntegerMax ? NSUIntegerMax : (NSUInteger)requestedEnd;
                    }
                    if (end >= total) end = total - 1;
                    if (start > end || start >= total) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if ([self isTaskActive:task])
                                [self respondRangeNotSatisfiable:task total:total];
                            [self finishTrackingTask:task];
                        });
                        return;
                    }
                }
            }
        }

        NSUInteger length = total == 0 ? 0 : end - start + 1;
        NSTimeInterval modified = memoryModificationTime != nil
                                      ? memoryModificationTime.doubleValue
                                      : [attributes[NSFileModificationDate] timeIntervalSince1970];
        NSString *etag = [NSString stringWithFormat:@"\"%llx-%llx\"",
                          fileSize, (unsigned long long)modified];
        NSString *ifNoneMatch = [task.request valueForHTTPHeaderField:@"If-None-Match"];
        if (!hasRange && [ifNoneMatch isEqualToString:etag]) {
            NSHTTPURLResponse *notModified = [[NSHTTPURLResponse alloc]
                initWithURL:task.request.URL statusCode:304 HTTPVersion:@"HTTP/1.1"
                headerFields:@{@"ETag": etag, @"Cache-Control": @"private, max-age=60"}];
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([self isTaskActive:task]) {
                    @try { [task didReceiveResponse:notModified]; [task didFinish]; }
                    @catch (NSException *exception) {}
                }
                [self finishTrackingTask:task];
            });
            return;
        }

        NSDictionary *headers;
        NSInteger status;
        if (hasRange) {
            status = 206;
            headers = @{
                @"Content-Type": mime,
                @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)length],
                @"Content-Range": [NSString stringWithFormat:@"bytes %lu-%lu/%lu",
                                   (unsigned long)start, (unsigned long)end, (unsigned long)total],
                @"Accept-Ranges": @"bytes", @"Cache-Control": @"private, max-age=60",
                @"ETag": etag,
            };
        } else {
            status = 200;
            headers = @{
                @"Content-Type": mime,
                @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)length],
                @"Accept-Ranges": @"bytes", @"Cache-Control": @"private, max-age=60",
                @"ETag": etag,
            };
        }

        NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
            initWithURL:task.request.URL statusCode:status
             HTTPVersion:@"HTTP/1.1" headerFields:headers];

        __block BOOL delivered = NO;
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (![self isTaskActive:task]) return;
            @try { [task didReceiveResponse:response]; delivered = YES; }
            @catch (NSException *exception) {}
        });
        if (!delivered) {
            [self finishTrackingTask:task];
            return;
        }

        if (memoryData != nil) {
            NSUInteger offset = start;
            NSUInteger remaining = length;
            static const NSUInteger kMemoryChunkSize = 256 * 1024;
            while (remaining > 0 && [self isTaskActive:task]) {
                @autoreleasepool {
                    NSUInteger count = MIN(remaining, kMemoryChunkSize);
                    NSData *chunk = [memoryData subdataWithRange:NSMakeRange(offset, count)];
                    offset += count;
                    remaining -= count;
                    dispatch_sync(dispatch_get_main_queue(), ^{
                        if (![self isTaskActive:task]) return;
                        @try { [task didReceiveData:chunk]; }
                        @catch (NSException *exception) {}
                    });
                }
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([self isTaskActive:task]) {
                    @try { [task didFinish]; } @catch (NSException *exception) {}
                }
                [self finishTrackingTask:task];
            });
            return;
        }

        NSFileHandle *handle = [NSFileHandle fileHandleForReadingAtPath:filePath];
        if (handle == nil) {
            [self reportReadFailure:task];
            return;
        }
        @try {
            [handle seekToFileOffset:start];
            NSUInteger remaining = length;
            static const NSUInteger kChunkSize = 256 * 1024;
            while (remaining > 0 && [self isTaskActive:task]) {
                @autoreleasepool {
                    NSData *chunk = [handle readDataOfLength:MIN(remaining, kChunkSize)];
                    if (chunk.length == 0) break;
                    remaining -= chunk.length;
                    dispatch_sync(dispatch_get_main_queue(), ^{
                        if (![self isTaskActive:task]) return;
                        @try { [task didReceiveData:chunk]; }
                        @catch (NSException *exception) {}
                    });
                }
            }
            [handle closeFile];
            if (remaining > 0 && [self isTaskActive:task]) {
                [self reportReadFailure:task];
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([self isTaskActive:task]) {
                    @try { [task didFinish]; } @catch (NSException *exception) {}
                }
                [self finishTrackingTask:task];
            });
        } @catch (NSException *exception) {
            [handle closeFile];
            [self reportReadFailure:task];
        }
        }
    });
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    [self finishTrackingTask:task];
}

#pragma mark - Error responses

- (void)respondNotFound:(id<WKURLSchemeTask>)task {
    NSData *body = [@"404 Not Found" dataUsingEncoding:NSUTF8StringEncoding];
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
        initWithURL:task.request.URL statusCode:404 HTTPVersion:@"HTTP/1.1"
        headerFields:@{@"Content-Type": @"text/plain; charset=utf-8",
                       @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)body.length]}];
    @try {
        [task didReceiveResponse:response];
        [task didReceiveData:body];
        [task didFinish];
    } @catch (NSException *ex) {}
}

- (void)respondRangeNotSatisfiable:(id<WKURLSchemeTask>)task total:(NSUInteger)total {
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
        initWithURL:task.request.URL statusCode:416 HTTPVersion:@"HTTP/1.1"
        headerFields:@{@"Content-Range": [NSString stringWithFormat:@"bytes */%lu", (unsigned long)total]}];
    @try { [task didReceiveResponse:response]; [task didFinish]; }
    @catch (NSException *ex) {}
}

@end
