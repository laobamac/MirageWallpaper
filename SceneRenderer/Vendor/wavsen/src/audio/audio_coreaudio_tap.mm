#include "audio_coreaudio_tap.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

extern "C" bool wavsen_coreaudio_create_global_tap(AudioObjectID* out_tap, char* out_uid,
                                                   std::size_t uid_capacity,
                                                   OSStatus* out_status) {
    if (out_tap) *out_tap = kAudioObjectUnknown;
    if (out_uid && uid_capacity > 0) out_uid[0] = '\0';
    if (out_status) *out_status = noErr;

    if (! out_tap || ! out_uid || uid_capacity == 0) {
        if (out_status) *out_status = paramErr;
        return false;
    }

    if (@available(macOS 14.2, *)) {
        @autoreleasepool {
            CATapDescription* desc =
                [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
            if (! desc) {
                if (out_status) *out_status = kAudioHardwareUnspecifiedError;
                return false;
            }

            desc.name = @"SceneRenderer Audio Capture";
            desc.privateTap = YES;
            desc.muteBehavior = CATapUnmuted;

            AudioObjectID tap = kAudioObjectUnknown;
            OSStatus st = AudioHardwareCreateProcessTap(desc, &tap);
            if (st != noErr || tap == kAudioObjectUnknown) {
                if (out_status) *out_status = st;
                return false;
            }

            CFStringRef uid_ref = nullptr;
            UInt32 size = sizeof(uid_ref);
            AudioObjectPropertyAddress addr {
                kAudioTapPropertyUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain,
            };
            st = AudioObjectGetPropertyData(tap, &addr, 0, nullptr, &size, &uid_ref);
            if (st != noErr || ! uid_ref) {
                AudioHardwareDestroyProcessTap(tap);
                if (out_status) *out_status = st;
                return false;
            }

            const bool copied =
                CFStringGetCString(uid_ref, out_uid, uid_capacity, kCFStringEncodingUTF8);
            CFRelease(uid_ref);
            if (! copied) {
                AudioHardwareDestroyProcessTap(tap);
                if (out_status) *out_status = kAudioHardwareUnspecifiedError;
                return false;
            }

            *out_tap = tap;
            if (out_status) *out_status = noErr;
            return true;
        }
    }

    if (out_status) *out_status = kAudioHardwareUnsupportedOperationError;
    return false;
}

extern "C" void wavsen_coreaudio_destroy_tap(AudioObjectID tap) {
    if (tap == kAudioObjectUnknown) return;
    if (@available(macOS 14.2, *)) {
        AudioHardwareDestroyProcessTap(tap);
    }
}
