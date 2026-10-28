// Mirage Wallpaper
// Created on 2026/10/29. Copyright © 2026 王孝慈. All rights reserved.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <sys/sysctl.h>

int main(void) {
    @autoreleasepool {
        int translated = 0;
        size_t length = sizeof(translated);
        sysctlbyname("sysctl.proc_translated", &translated, &length, NULL, 0);
        NSMutableArray *devices = [NSMutableArray new];
        for (id<MTLDevice> device in MTLCopyAllDevices()) [devices addObject:device.name];
        NSDictionary *result = @{
#if defined(__arm64__)
            @"architecture": @"arm64",
#else
            @"architecture": @"x86_64",
#endif
            @"translated": @(translated != 0),
            @"macOS": NSProcessInfo.processInfo.operatingSystemVersionString,
            @"metalDevices": devices
        };
        NSData *data = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:NULL];
        fwrite(data.bytes, 1, data.length, stdout);
        fputc('\n', stdout);
        return translated ? 1 : 0;
    }
}
