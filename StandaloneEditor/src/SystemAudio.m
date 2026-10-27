//
//  SystemAudio.m
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stddef.h>

typedef int32_t (*AudioCall)(void *);
typedef struct {
    const uint16_t *name;
    const char *device;
    char *resultDevice;
    uint32_t capacity;
    int32_t result;
} LoopbackRequest;
_Static_assert(sizeof(LoopbackRequest) == 32 && offsetof(LoopbackRequest, result) == 28, "Wine 11.18 ABI");

@interface MirageAudioTap : NSObject
@property AudioObjectID tap;
@property AudioObjectID device;
@property(copy) NSString *uid;
@end

@implementation MirageAudioTap
- (void)dealloc {
    if (_device) AudioHardwareDestroyAggregateDevice(_device);
    if (_tap) {
        if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(_tap);
    }
}
@end

static NSMutableDictionary<NSString *, MirageAudioTap *> *taps;
static NSLock *tapLock;
static AudioCall functions[37];

static MirageAudioTap *createTap(NSString *output, OSStatus *status) {
    if (@available(macOS 14.2, *)) {
        CATapDescription *description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        description.deviceUID = output;
        description.name = @"Mirage Editor";
        description.privateTap = YES;
        description.muteBehavior = CATapUnmuted;
        MirageAudioTap *tap = [MirageAudioTap new];
        AudioObjectID identifier = kAudioObjectUnknown;
        *status = AudioHardwareCreateProcessTap(description, &identifier);
        if (*status != noErr) return nil;
        tap.tap = identifier;
        tap.uid = [@"cn.laobamac.Mirage.Editor.Audio." stringByAppendingString:NSUUID.UUID.UUIDString];
        NSDictionary *aggregate = @{
            @kAudioAggregateDeviceNameKey: @"Mirage Editor",
            @kAudioAggregateDeviceUIDKey: tap.uid,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceTapListKey: @[@{
                @kAudioSubTapUIDKey: description.UUID.UUIDString,
                @kAudioSubTapDriftCompensationKey: @YES
            }]
        };
        *status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &identifier);
        if (*status != noErr) return nil;
        tap.device = identifier;
        return tap;
    }
    *status = kAudioHardwareUnsupportedOperationError;
    return nil;
}

static int32_t loopback(void *argument) {
    LoopbackRequest *request = argument;
    @autoreleasepool {
        if (!request->device) { request->result = (int32_t)0x80070057; return 0; }
        NSString *output = [NSString stringWithUTF8String:request->device];
        if (!output.length) { request->result = (int32_t)0x80070057; return 0; }
        [tapLock lock];
        MirageAudioTap *tap = taps[output];
        if (!tap) {
            OSStatus status = noErr;
            tap = createTap(output, &status);
            if (tap) taps[output] = tap;
            else fprintf(stderr, "[Mirage audio] system audio tap unavailable: %d\n", (int)status);
        }
        if (!tap) request->result = (int32_t)0x88890004;
        else {
            const char *uid = tap.uid.UTF8String;
            uint32_t required = (uint32_t)strlen(uid) + 1;
            if (!request->resultDevice || request->capacity < required) {
                request->capacity = required;
                request->result = (int32_t)0xc0000023;
            } else {
                memcpy(request->resultDevice, uid, required);
                request->result = 0;
            }
        }
        [tapLock unlock];
    }
    return 0;
}

void *MirageAudioFunctions(void *original) {
    Dl_info info = {0};
    if (!original || !dladdr(original, &info) || !info.dli_fname || !strstr(info.dli_fname, "/winecoreaudio.so")) return original;
    const struct mach_header_64 *header = info.dli_fbase;
    if (header->magic != MH_MAGIC_64) return original;
    const uint8_t expected[16] = {0xf4,0xdc,0x30,0x8e,0xc7,0xf2,0x30,0xc7,0xb2,0xf1,0x1d,0x7d,0x1b,0xb6,0xdf,0x68};
    const struct load_command *command = (const void *)(header + 1);
    BOOL supported = NO;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if (command->cmd == LC_UUID && !memcmp(((const struct uuid_command *)command)->uuid, expected, 16)) supported = YES;
        command = (const void *)((const char *)command + command->cmdsize);
    }
    if (!supported) return original;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        taps = [NSMutableDictionary new];
        tapLock = [NSLock new];
        memcpy(functions, original, sizeof(functions));
        functions[15] = loopback;
        fprintf(stderr, "[Mirage audio] native system audio loopback enabled\n");
    });
    return functions;
}
