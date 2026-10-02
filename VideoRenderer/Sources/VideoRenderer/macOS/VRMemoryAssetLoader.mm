#import "VRMemoryAssetLoader.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

@interface VRMemoryAssetLoader ()
- (void)serveLoadingRequest:(AVAssetResourceLoadingRequest *)loadingRequest;
@end

@implementation VRMemoryAssetLoader {
    NSData *_data;
    NSURL *_assetURL;
    NSString *_contentType;
    dispatch_queue_t _loaderQueue;
}

+ (instancetype)loaderWithFileURL:(NSURL *)fileURL error:(NSError **)error {
    int descriptor = open(fileURL.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        }
        return nil;
    }

    struct stat status = {};
    if (fstat(descriptor, &status) != 0 || status.st_size <= 0 ||
        (uint64_t)status.st_size > (uint64_t)NSUIntegerMax) {
        int code = errno != 0 ? errno : EINVAL;
        close(descriptor);
        if (error != NULL) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
        }
        return nil;
    }

    NSUInteger length = (NSUInteger)status.st_size;
    void *bytes = malloc(length);
    if (bytes == NULL) {
        close(descriptor);
        if (error != NULL) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOMEM userInfo:nil];
        }
        return nil;
    }

    NSUInteger offset = 0;
    while (offset < length) {
        ssize_t count = read(descriptor, (uint8_t *)bytes + offset, length - offset);
        if (count > 0) {
            offset += (NSUInteger)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        int code = count == 0 ? EIO : errno;
        free(bytes);
        close(descriptor);
        if (error != NULL) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
        }
        return nil;
    }
    close(descriptor);

    NSData *data = [NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES];
    if (data == nil) {
        free(bytes);
        if (error != NULL) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOMEM userInfo:nil];
        }
        return nil;
    }

    NSURLComponents *components = [NSURLComponents componentsWithURL:fileURL
                                                resolvingAgainstBaseURL:NO];
    components.scheme = @"mirage-memory-video";
    NSURL *assetURL = components.URL;
    if (assetURL == nil) return nil;

    VRMemoryAssetLoader *loader = [VRMemoryAssetLoader new];
    loader->_data = data;
    loader->_assetURL = assetURL;
    UTType *type = [UTType typeWithFilenameExtension:fileURL.pathExtension];
    loader->_contentType = type.identifier ?: UTTypeMovie.identifier;
    loader->_loaderQueue = dispatch_queue_create("VideoRenderer.memoryAsset", DISPATCH_QUEUE_SERIAL);
    return loader;
}

- (NSURL *)assetURL { return _assetURL; }
- (NSUInteger)length { return _data.length; }

- (void)attachToAsset:(AVURLAsset *)asset {
    [asset.resourceLoader setDelegate:self queue:_loaderQueue];
}

- (BOOL)resourceLoader:(AVAssetResourceLoader *)resourceLoader
    shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest *)loadingRequest {
    (void)resourceLoader;
    AVAssetResourceLoadingContentInformationRequest *content =
        loadingRequest.contentInformationRequest;
    if (content != nil) {
        NSArray<NSString *> *allowed = content.allowedContentTypes;
        content.contentType = allowed.count == 0 || [allowed containsObject:_contentType]
            ? _contentType : allowed.firstObject;
        content.contentLength = (long long)_data.length;
        content.byteRangeAccessSupported = YES;
        content.entireLengthAvailableOnDemand = YES;
    }

    if (loadingRequest.dataRequest == nil) {
        [loadingRequest finishLoading];
        return YES;
    }

    [self serveLoadingRequest:loadingRequest];
    return YES;
}

- (void)serveLoadingRequest:(AVAssetResourceLoadingRequest *)loadingRequest {
    if (loadingRequest.cancelled || loadingRequest.finished) return;

    AVAssetResourceLoadingDataRequest *request = loadingRequest.dataRequest;
    if (request == nil) {
        [loadingRequest finishLoading];
        return;
    }

    long long currentOffset = request.currentOffset;
    if (currentOffset < request.requestedOffset) currentOffset = request.requestedOffset;
    if (currentOffset < 0 || (unsigned long long)currentOffset > _data.length) {
        [loadingRequest finishLoadingWithError:[NSError
            errorWithDomain:NSURLErrorDomain code:NSURLErrorBadServerResponse userInfo:nil]];
        return;
    }

    NSUInteger offset = (NSUInteger)currentOffset;
    NSUInteger end = _data.length;
    if (!request.requestsAllDataToEndOfResource) {
        unsigned long long start = (unsigned long long)MAX(request.requestedOffset, 0);
        unsigned long long length = (unsigned long long)MAX(request.requestedLength, 0);
        unsigned long long requestedEnd = length > ULLONG_MAX - start
            ? ULLONG_MAX : start + length;
        end = (NSUInteger)MIN(requestedEnd, (unsigned long long)_data.length);
    }

    if (offset >= end) {
        [loadingRequest finishLoading];
        return;
    }

    NSUInteger length = end - offset;
    NSData *backing = _data;
    NSData *slice = [[NSData alloc]
        initWithBytesNoCopy:(uint8_t *)backing.bytes + offset
                     length:length
                deallocator:^(void * _Nonnull bytes, NSUInteger length) {
        (void)bytes;
        (void)length;
        (void)backing;
    }];
    if (slice == nil) {
        [loadingRequest finishLoadingWithError:[NSError
            errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotDecodeContentData userInfo:nil]];
        return;
    }

    [request respondWithData:slice];
    if (loadingRequest.cancelled || loadingRequest.finished) return;
    [loadingRequest finishLoading];
}

@end
