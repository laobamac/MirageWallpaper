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
@end

@implementation WRURLSchemeHandler

- (instancetype)init { return [self initWithBaseDirectory:@""]; }

- (instancetype)initWithBaseDirectory:(NSString *)baseDirectory {
    self = [super init];
    if (self) {
        _baseDirectory = [[baseDirectory stringByStandardizingPath] copy];
        _overlayDirectories = @[];
        _ioQueue = dispatch_queue_create("WebRenderer.schemeHandler", DISPATCH_QUEUE_SERIAL);
    }
    return self;
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
            if (candidate != nil && [fm fileExistsAtPath:candidate isDirectory:&isDirectory] && !isDirectory) {
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

- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    NSString *rawPath = task.request.URL.path ?: @"";
    NSString *rel = [rawPath stringByRemovingPercentEncoding] ?: rawPath;
    NSString *filePath = [rel isEqualToString:@"/__mirage_local"]
        ? [self resolvedLocalPathForURL:task.request.URL]
        : [self resolvedPathForRelative:rel];

    if (filePath == nil) { [self respondNotFound:task]; return; }

    dispatch_async(self.ioQueue, ^{
        NSFileManager *fm = NSFileManager.defaultManager;
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:filePath isDirectory:&isDir] || isDir) {
            dispatch_async(dispatch_get_main_queue(), ^{ [self respondNotFound:task]; });
            return;
        }
        NSData *data = [NSData dataWithContentsOfFile:filePath
                                              options:NSDataReadingMappedIfSafe error:nil];
        if (data == nil) {
            dispatch_async(dispatch_get_main_queue(), ^{ [self respondNotFound:task]; });
            return;
        }

        NSString *mime = MIMEForExtension(filePath.pathExtension);
        NSUInteger total = data.length;

        // Parse Range: bytes=start-end (end optional).
        NSString *rangeHeader = [task.request valueForHTTPHeaderField:@"Range"];
        NSUInteger start = 0, end = total - 1;
        BOOL hasRange = NO;
        if (rangeHeader.length > 0) {
            NSRange eq = [rangeHeader rangeOfString:@"bytes="];
            if (eq.location != NSNotFound) {
                NSString *spec = [rangeHeader substringFromIndex:eq.location + eq.length];
                NSArray<NSString *> *parts = [spec componentsSeparatedByString:@"-"];
                if (parts.count >= 1 && parts[0].length > 0) {
                    start = (NSUInteger)[parts[0] longLongValue];
                    hasRange = YES;
                    if (parts.count >= 2 && parts[1].length > 0)
                        end = (NSUInteger)[parts[1] longLongValue];
                    if (end >= total) end = total - 1;
                    if (start > end || start >= total) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [self respondRangeNotSatisfiable:task total:total];
                        });
                        return;
                    }
                }
            }
        }

        NSUInteger length = end - start + 1;
        NSData *body = (start == 0 && length == total)
                           ? data : [data subdataWithRange:NSMakeRange(start, length)];

        NSDictionary *headers;
        NSInteger status;
        if (hasRange) {
            status = 206;
            headers = @{
                @"Content-Type": mime,
                @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)length],
                @"Content-Range": [NSString stringWithFormat:@"bytes %lu-%lu/%lu",
                                   (unsigned long)start, (unsigned long)end, (unsigned long)total],
                @"Accept-Ranges": @"bytes", @"Cache-Control": @"no-store",
            };
        } else {
            status = 200;
            headers = @{
                @"Content-Type": mime,
                @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)length],
                @"Accept-Ranges": @"bytes", @"Cache-Control": @"no-store",
            };
        }

        NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
            initWithURL:task.request.URL statusCode:status
             HTTPVersion:@"HTTP/1.1" headerFields:headers];

        dispatch_async(dispatch_get_main_queue(), ^{
            @try {
                [task didReceiveResponse:response];
                [task didReceiveData:body];
                [task didFinish];
            } @catch (NSException *ex) { /* task stopped */ }
        });
    });
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView; (void)task;
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
