#pragma once

#include <CoreAudio/AudioHardware.h>

#include <cstddef>

extern "C" bool wavsen_coreaudio_create_global_tap(AudioObjectID* out_tap, char* out_uid,
                                                   std::size_t uid_capacity,
                                                   OSStatus* out_status);
extern "C" void wavsen_coreaudio_destroy_tap(AudioObjectID tap);
