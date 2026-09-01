//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <pthread.h>

#include <cstdlib>
#include <cstring>

namespace {

id Send0(id target, const char* selector) {
    return reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(target, sel_registerName(selector));
}

id Send1(id target, const char* selector, id argument) {
    return reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(
        target, sel_registerName(selector), argument);
}

id Value(id object, NSString* key) {
    if (object == nil) return nil;
    @try {
        return [object valueForKey:key];
    } @catch (NSException*) {
        return nil;
    }
}

void Put(NSMutableDictionary* output, NSString* key, id value) {
    if (value != nil && value != [NSNull null]) output[key] = value;
}

id CurrentController() {
    static id controller = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSBundle* bundle = [NSBundle bundleWithPath:
            @"/System/Library/PrivateFrameworks/MediaRemote.framework"];
        [bundle loadAndReturnError:nil];
        Class destinationClass = NSClassFromString(@"MRDestination");
        Class configurationClass = NSClassFromString(@"MRNowPlayingControllerConfiguration");
        Class controllerClass = NSClassFromString(@"MRNowPlayingController");
        if (destinationClass == Nil || configurationClass == Nil || controllerClass == Nil) return;
        id destination = Send0((id)destinationClass, "userSelectedDestination");
        id configuration = Send0((id)configurationClass, "alloc");
        configuration = Send1(configuration, "initWithDestination:", destination);
        if (configuration == nil) return;
        @try {
            [configuration setValue:@NO forKey:@"singleShot"];
            [configuration setValue:@YES forKey:@"requestPlaybackState"];
            [configuration setValue:@YES forKey:@"requestPlaybackQueue"];
            Class requestClass = NSClassFromString(@"MRPlaybackQueueRequest");
            id request = Send0((id)requestClass, "new");
            [request setValue:@0 forKey:@"location"];
            [request setValue:@1 forKey:@"length"];
            [request setValue:@YES forKey:@"includeMetadata"];
            [request setValue:@512 forKey:@"artworkWidth"];
            [request setValue:@512 forKey:@"artworkHeight"];
            [configuration setValue:request forKey:@"playbackQueueRequest"];
        } @catch (NSException*) {
            return;
        }
        controller = Send0((id)controllerClass, "alloc");
        controller = Send1(controller, "initWithConfiguration:", configuration);
        if (controller != nil) Send0(controller, "beginLoadingUpdates");
    });
    return controller;
}

NSDictionary* NowPlayingPayload() {
    id response = Value(CurrentController(), @"response");
    if (response == nil) return nil;
    id queue = Value(response, @"playbackQueue");
    NSArray* items = Value(queue, @"contentItems");
    if (![items isKindOfClass:[NSArray class]] || items.count == 0) return nil;
    NSInteger location = [Value(queue, @"location") integerValue];
    id item = location >= 0 && location < (NSInteger)items.count ? items[(NSUInteger)location]
                                                                 : items.firstObject;
    id metadata = Value(item, @"metadata");
    if (metadata == nil) return nil;
    NSMutableDictionary* output = [NSMutableDictionary dictionary];
    Put(output, @"title", Value(metadata, @"title"));
    Put(output, @"artist", Value(metadata, @"trackArtistName"));
    Put(output, @"album", Value(metadata, @"albumName"));
    Put(output, @"albumArtist", Value(metadata, @"albumArtistName"));
    Put(output, @"duration", Value(metadata, @"duration"));
    id calculatedPosition = Value(metadata, @"calculatedPlaybackPosition");
    Put(output, @"position",
        calculatedPosition != nil ? calculatedPosition : Value(metadata, @"elapsedTime"));
    id artwork = Value(item, @"artwork");
    id itemArtworkData = Value(artwork, @"imageData");
    if (![itemArtworkData isKindOfClass:[NSData class]] &&
        [artwork respondsToSelector:sel_registerName("copyImageData")])
        itemArtworkData = Send0(artwork, "copyImageData");
    if ([itemArtworkData isKindOfClass:[NSData class]])
        output[@"artworkData"] =
            [itemArtworkData base64EncodedStringWithOptions:0];
    id artworkData = Value(metadata, @"artworkData");
    if ([artworkData isKindOfClass:[NSData class]])
        output[@"artworkData"] = [artworkData base64EncodedStringWithOptions:0];
    Put(output, @"artworkMimeType", Value(metadata, @"artworkMIMEType"));
    NSNumber* playbackRate = Value(response, @"playbackRate");
    NSNumber* playbackState = Value(response, @"playbackState");
    BOOL playing = playbackRate.doubleValue > 0 || playbackState.unsignedIntegerValue == 1;
    output[@"playing"] = @(playing);
    NSDictionary* extra = Value(metadata, @"nowPlayingInfo");
    if ([extra isKindOfClass:[NSDictionary class]]) {
        NSDictionary* keys = @{
            @"kMRMediaRemoteNowPlayingInfoTitle": @"title",
            @"kMRMediaRemoteNowPlayingInfoArtist": @"artist",
            @"kMRMediaRemoteNowPlayingInfoAlbum": @"album",
            @"kMRMediaRemoteNowPlayingInfoAlbumArtist": @"albumArtist",
            @"kMRMediaRemoteNowPlayingInfoDuration": @"duration",
            @"kMRMediaRemoteNowPlayingInfoElapsedTime": @"position"
        };
        for (NSString* source in keys) Put(output, keys[source], extra[source]);
        id artwork = extra[@"kMRMediaRemoteNowPlayingInfoArtworkData"];
        if ([artwork isKindOfClass:[NSData class]])
            output[@"artworkData"] = [artwork base64EncodedStringWithOptions:0];
        Put(output, @"artworkMimeType",
            extra[@"kMRMediaRemoteNowPlayingInfoArtworkMIMEType"]);
        NSDate* timestamp = extra[@"kMRMediaRemoteNowPlayingInfoTimestamp"];
        NSNumber* rate = extra[@"kMRMediaRemoteNowPlayingInfoPlaybackRate"];
        NSNumber* elapsed = extra[@"kMRMediaRemoteNowPlayingInfoElapsedTime"];
        if ([timestamp isKindOfClass:[NSDate class]] && [elapsed isKindOfClass:[NSNumber class]] &&
            rate.doubleValue > 0) {
            output[@"position"] = @(elapsed.doubleValue - timestamp.timeIntervalSinceNow *
                                     rate.doubleValue);
        }
    }
    if (![output[@"title"] isKindOfClass:[NSString class]] ||
        [output[@"title"] length] == 0) return nil;
    return output;
}

}

extern "C" char* MirageCopyNowPlayingJSON() {
    @autoreleasepool {
        __block NSDictionary* payload = nil;
        if (pthread_main_np() != 0)
            payload = NowPlayingPayload();
        else
            dispatch_sync(dispatch_get_main_queue(), ^{ payload = NowPlayingPayload(); });
        if (payload == nil) return nullptr;
        NSData* data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
        if (data == nil) return nullptr;
        NSString* json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        if (json == nil) return nullptr;
        return strdup(json.UTF8String);
    }
}

extern "C" void MirageFreeNowPlayingJSON(char* value) {
    free(value);
}

extern "C" void MiragePrintNowPlayingJSON() {
    char* value = nullptr;
    for (int index = 0; index < 25 && value == nullptr; ++index) {
        [[NSRunLoop currentRunLoop]
            runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        value = MirageCopyNowPlayingJSON();
    }
    if (value == nullptr) {
        fputs("null\n", stdout);
        return;
    }
    fputs(value, stdout);
    fputc('\n', stdout);
    MirageFreeNowPlayingJSON(value);
}
