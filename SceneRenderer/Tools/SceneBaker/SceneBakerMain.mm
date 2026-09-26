//
//  SceneBakerMain.mm
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import "BakeSupport.h"
#include <cmath>
#include <algorithm>
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSDictionary *r = MBReadRequest(argc, argv);
        if (!MBValidateRequest(r) || ![r[@"assets"] isKindOfClass:NSString.class] ||
            ![r[@"cache"] isKindOfClass:NSString.class]) { MBEvent(@"error", @{@"code":@"invalid_request"}); return 1; }
        NSString *(^json)(id) = ^NSString *(id value) {
            NSData *data = [NSJSONSerialization dataWithJSONObject:value ?: @{} options:NSJSONWritingSortedKeys error:nil];
            return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        };
        NSString *properties = json(r[@"properties"]), *storage = json(r[@"storage"]);
        MBBakeWriter *writer = [[MBBakeWriter alloc] initWithRequest:r];
        if (!writer) { MBEvent(@"error", @{@"code":@"encoder_unavailable"}); return 1; }
        MBBakeSceneOptions o = {};
        o.source = [r[@"source"] UTF8String]; o.assets = [r[@"assets"] UTF8String]; o.cache = [r[@"cache"] UTF8String];
        o.properties = properties.UTF8String; o.storage = storage.UTF8String;
        o.fill = [r[@"fillMode"] UTF8String] ?: "cover";
        o.width = [r[@"width"] unsignedIntValue]; o.height = [r[@"height"] unsignedIntValue];
        o.fps = [r[@"fps"] unsignedIntValue]; o.seed = [r[@"seed"] unsignedIntValue];
        o.frames = llround([r[@"duration"] doubleValue] * o.fps);
        o.warmup = std::clamp([r[@"warmup"] intValue], 0, 10) * o.fps;
        o.speed = [r[@"speed"] doubleValue]; o.position_x = [r[@"positionX"] doubleValue];
        o.position_y = [r[@"positionY"] doubleValue]; o.volume = [r[@"volume"] doubleValue];
        MBEvent(@"preparing", @{});
        int status = MBRunScene(&o, (__bridge void *)writer);
        if (status == 0 && [writer finish]) { MBEvent(@"complete", @{}); return 0; }
        [writer cancel];
        MBEvent(@"error", @{@"code":status == 5 ? @"render_timeout" : MBCancelled() ? @"cancelled" : @"render_failed",
            @"detail":writer.failure});
        return 1;
    }
}
