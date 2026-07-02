#include "audio_coreaudio_output.h"

#import <AudioToolbox/AudioToolbox.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultChannels = 2;
constexpr std::uint32_t kQuantum = 1024;
constexpr std::uint32_t kBufferCount = 3;
constexpr OSStatus kParamErr = -50;

bool audio_debug_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WAVSEN_AUDIO_DEBUG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

} // namespace

struct wavsen_coreaudio_output {
    wavsen_coreaudio_output_callback callback = nullptr;
    void* user = nullptr;
    AudioQueueRef queue = nullptr;
    std::array<AudioQueueBufferRef, kBufferCount> buffers {};
    AudioStreamBasicDescription format {};
    std::atomic<bool> started { false };

    void fill_and_enqueue(AudioQueueBufferRef buffer) {
        if (! queue || ! buffer) return;
        const auto frames =
            static_cast<std::uint32_t>(buffer->mAudioDataBytesCapacity / format.mBytesPerFrame);
        auto* out = static_cast<float*>(buffer->mAudioData);
        std::memset(out, 0,
                    static_cast<std::size_t>(frames) * kDefaultChannels * sizeof(float));
        if (callback) callback(out, frames, kDefaultChannels, kDefaultRate, user);
        buffer->mAudioDataByteSize = frames * format.mBytesPerFrame;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
};

static void wavsen_coreaudio_output_cb(void* user, AudioQueueRef /*queue*/,
                                       AudioQueueBufferRef buffer) {
    auto* output = static_cast<wavsen_coreaudio_output*>(user);
    if (! output || ! output->started.load(std::memory_order_acquire)) return;
    output->fill_and_enqueue(buffer);
}

bool wavsen_coreaudio_output_create(wavsen_coreaudio_output_callback callback, void* user,
                                    wavsen_coreaudio_output** out, int* out_status,
                                    std::uint32_t* out_channels, std::uint32_t* out_sample_rate) {
    if (out) *out = nullptr;
    if (out_status) *out_status = noErr;
    if (out_channels) *out_channels = kDefaultChannels;
    if (out_sample_rate) *out_sample_rate = kDefaultRate;
    if (! out || ! callback) {
        if (out_status) *out_status = kParamErr;
        return false;
    }

    auto* output = new wavsen_coreaudio_output();
    output->callback = callback;
    output->user = user;
    output->format.mSampleRate = static_cast<Float64>(kDefaultRate);
    output->format.mFormatID = kAudioFormatLinearPCM;
    output->format.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    output->format.mBytesPerPacket = kDefaultChannels * static_cast<std::uint32_t>(sizeof(float));
    output->format.mFramesPerPacket = 1;
    output->format.mBytesPerFrame = kDefaultChannels * static_cast<std::uint32_t>(sizeof(float));
    output->format.mChannelsPerFrame = kDefaultChannels;
    output->format.mBitsPerChannel = 32;

    OSStatus st = AudioQueueNewOutput(&output->format, wavsen_coreaudio_output_cb, output, nullptr,
                                      nullptr, 0, &output->queue);
    if (st != noErr || ! output->queue) {
        if (out_status) *out_status = st;
        delete output;
        return false;
    }

    const auto buffer_bytes = kQuantum * output->format.mBytesPerFrame;
    for (auto& buffer : output->buffers) {
        st = AudioQueueAllocateBuffer(output->queue, buffer_bytes, &buffer);
        if (st != noErr || ! buffer) {
            if (out_status) *out_status = st;
            wavsen_coreaudio_output_destroy(output);
            return false;
        }
    }

    *out = output;
    if (audio_debug_enabled()) {
        std::fprintf(stderr, "wavsen-audio-debug: AudioQueue created, bufferBytes=%u\n",
                     buffer_bytes);
    }
    return true;
}

void wavsen_coreaudio_output_destroy(wavsen_coreaudio_output* output) {
    if (! output) return;
    wavsen_coreaudio_output_stop(output);
    if (output->queue) {
        AudioQueueDispose(output->queue, true);
        output->queue = nullptr;
    }
    delete output;
}

bool wavsen_coreaudio_output_start(wavsen_coreaudio_output* output, int* out_status) {
    if (out_status) *out_status = noErr;
    if (! output || ! output->queue) {
        if (out_status) *out_status = kParamErr;
        return false;
    }
    bool expected = false;
    if (! output->started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }
    if (audio_debug_enabled()) {
        std::fprintf(stderr, "wavsen-audio-debug: AudioQueue start: priming %zu buffers\n",
                     output->buffers.size());
    }
    for (auto* buffer : output->buffers) {
        output->fill_and_enqueue(buffer);
    }
    const auto st = AudioQueueStart(output->queue, nullptr);
    if (audio_debug_enabled()) {
        std::fprintf(stderr, "wavsen-audio-debug: AudioQueueStart status=%d\n",
                     static_cast<int>(st));
    }
    if (st != noErr) {
        output->started.store(false, std::memory_order_release);
        if (out_status) *out_status = st;
        return false;
    }
    return true;
}

void wavsen_coreaudio_output_stop(wavsen_coreaudio_output* output) {
    if (! output || ! output->queue) return;
    if (! output->started.exchange(false, std::memory_order_acq_rel)) return;
    AudioQueueStop(output->queue, true);
}
