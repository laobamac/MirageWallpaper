#pragma once

#include <cstdint>

struct wavsen_coreaudio_capture;

using wavsen_coreaudio_capture_callback = void (*)(const float* samples, std::uint32_t frames,
                                                   std::uint32_t channels,
                                                   std::uint32_t sample_rate, void* user);

bool wavsen_coreaudio_capture_create(wavsen_coreaudio_capture_callback callback, void* user,
                                     wavsen_coreaudio_capture** out, int* out_status,
                                     std::uint32_t* out_source_channels,
                                     std::uint32_t* out_sample_rate);
void wavsen_coreaudio_capture_destroy(wavsen_coreaudio_capture* capture);
