#import "VRMemoryAssetLoader.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@implementation VRMemoryAssetLoader {
    NSData *_data;
    dispatch_data_t _backing;
    NSURL *_assetURL;
    NSString *_contentType;
    dispatch_queue_t _loaderQueue;
}

+ (instancetype)loaderWithFileURL:(NSURL *)fileURL error:(NSError **)error {
    // Map, don't read. A 2 GB video used to be pulled into anonymous memory in
    // full and synchronously — on the main thread, since this runs before
    // [app run] — and NSDataReadingUncached explicitly told the kernel it may
    // not reclaim any of it, so the whole file stayed resident for the lifetime
    // of the wallpaper. Mapped pages fault in on demand and evict under
    // pressure, which is exactly the behaviour wanted for linear playback.
    NSData *data = [NSData dataWithContentsOfURL:fileURL
                                         options:NSDataReadingMappedIfSafe
                                           error:error];
    if (data == nil) return nil;

    NSURLComponents *components = [NSURLComponents componentsWithURL:fileURL
                                                resolvingAgainstBaseURL:NO];
    components.scheme = @"mirage-memory-video";
    NSURL *assetURL = components.URL;
    if (assetURL == nil) return nil;

    VRMemoryAssetLoader *loader = [VRMemoryAssetLoader new];
    loader->_data = data;
    // Wrapped once so range requests can be answered with zero-copy subranges.
    // AVFoundation asks for the whole resource at offset 0, and the previous
    // subdataWithRange: answered that by allocating and copying a second full
    // copy of the file — peak memory was twice the file size. The destructor
    // block captures `data`, so every outstanding subrange keeps the mapping
    // alive even if this loader is replaced.
    loader->_backing = dispatch_data_create(
        data.bytes, data.length,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
        ^{ (void)data; });
    if (loader->_backing == nil) return nil;
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
        content.contentType = _contentType;
        content.contentLength = (long long)_data.length;
        content.byteRangeAccessSupported = YES;
    }

    AVAssetResourceLoadingDataRequest *request = loadingRequest.dataRequest;
    if (request != nil) {
        long long requestedOffset = request.currentOffset != 0
                                        ? request.currentOffset
                                        : request.requestedOffset;
        if (requestedOffset < 0 || (unsigned long long)requestedOffset > _data.length) {
            [loadingRequest finishLoadingWithError:[NSError
                errorWithDomain:NSURLErrorDomain code:NSURLErrorBadServerResponse userInfo:nil]];
            return YES;
        }

        NSUInteger offset = (NSUInteger)requestedOffset;
        NSUInteger available = _data.length - offset;
        NSUInteger length = request.requestsAllDataToEndOfResource
                                ? available
                                : MIN((NSUInteger)request.requestedLength, available);
        if (length > 0) {
            dispatch_data_t slice = dispatch_data_create_subrange(_backing, offset, length);
            if (slice != nil) [request respondWithData:(NSData *)(id)slice];
        }
    }
    [loadingRequest finishLoading];
    return YES;
}

@end
