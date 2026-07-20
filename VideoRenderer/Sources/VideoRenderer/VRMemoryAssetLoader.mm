#import "VRMemoryAssetLoader.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@implementation VRMemoryAssetLoader {
    NSData *_data;
    NSURL *_assetURL;
    NSString *_contentType;
    dispatch_queue_t _loaderQueue;
}

+ (instancetype)loaderWithFileURL:(NSURL *)fileURL error:(NSError **)error {
    NSData *data = [NSData dataWithContentsOfURL:fileURL
                                         options:NSDataReadingUncached
                                           error:error];
    if (data == nil) return nil;

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
            [request respondWithData:[_data subdataWithRange:NSMakeRange(offset, length)]];
        }
    }
    [loadingRequest finishLoading];
    return YES;
}

@end
