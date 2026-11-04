#pragma once

#include <cstdint>

struct wavsen_native_output;

using wavsen_native_output_callback = void (*)(float* out, std::uint32_t frames,
                                                  std::uint32_t channels,
                                                  std::uint32_t sample_rate, void* user);

bool wavsen_native_output_create(wavsen_native_output_callback callback, void* user,
                                    wavsen_native_output** out, int* out_status,
                                    std::uint32_t* out_channels, std::uint32_t* out_sample_rate);
void wavsen_native_output_destroy(wavsen_native_output* output);
bool wavsen_native_output_start(wavsen_native_output* output, int* out_status);
void wavsen_native_output_stop(wavsen_native_output* output);
