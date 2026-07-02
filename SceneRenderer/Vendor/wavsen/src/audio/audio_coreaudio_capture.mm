#include "audio_coreaudio_capture.h"

#include "audio_coreaudio_tap.h"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/CoreAudioTypes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultChannels = 2;
constexpr OSStatus kParamErr = -50;
constexpr OSStatus kMemFullErr = -108;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

CFNumberRef cf_number(int value) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
}

float clamp_unit(float v) {
    if (! std::isfinite(v)) return 0.0f;
    return std::clamp(v, -1.0f, 1.0f);
}

} // namespace

struct wavsen_coreaudio_capture {
    wavsen_coreaudio_capture_callback callback = nullptr;
    void* user = nullptr;

    AudioObjectID tap = kAudioObjectUnknown;
    AudioObjectID aggregate = kAudioObjectUnknown;
    AudioDeviceIOProcID io_proc = nullptr;
    AudioStreamBasicDescription format {};
    std::uint32_t sample_rate = kDefaultRate;
    std::uint32_t source_channels = kDefaultChannels;
    std::vector<float> convert;
    std::atomic<bool> running { false };

    void set_default_format() {
        format = {};
        format.mSampleRate = kDefaultRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags =
            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        format.mBytesPerPacket = kDefaultChannels * static_cast<std::uint32_t>(sizeof(float));
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = kDefaultChannels * static_cast<std::uint32_t>(sizeof(float));
        format.mChannelsPerFrame = kDefaultChannels;
        format.mBitsPerChannel = 32;
        sample_rate = kDefaultRate;
        source_channels = kDefaultChannels;
    }

    bool read_tap_format() {
        if (tap == kAudioObjectUnknown) return false;
        AudioObjectPropertyAddress addr {
            kAudioTapPropertyFormat,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        UInt32 size = sizeof(format);
        const auto st = AudioObjectGetPropertyData(tap, &addr, 0, nullptr, &size, &format);
        if (st != noErr || format.mSampleRate <= 0.0 || format.mChannelsPerFrame == 0) {
            return false;
        }
        sample_rate = static_cast<std::uint32_t>(std::llround(format.mSampleRate));
        if (sample_rate == 0) sample_rate = kDefaultRate;
        source_channels = format.mChannelsPerFrame;
        return true;
    }

    OSStatus create_aggregate_device(const char* tap_uid) {
        if (! tap_uid || tap_uid[0] == '\0') return kParamErr;

        const auto uid_string =
            "org.scenerenderer.audio-capture." + std::to_string(now_ms());
        CFStringRef aggregate_uid =
            CFStringCreateWithCString(kCFAllocatorDefault, uid_string.c_str(), kCFStringEncodingUTF8);
        CFStringRef aggregate_name = CFSTR("SceneRenderer Audio Capture");
        CFStringRef tap_uid_ref =
            CFStringCreateWithCString(kCFAllocatorDefault, tap_uid, kCFStringEncodingUTF8);

        CFMutableDictionaryRef subtap =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef aggregate_dict =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        OSStatus status = noErr;

        if (! aggregate_uid || ! tap_uid_ref || ! subtap || ! aggregate_dict) {
            status = kMemFullErr;
        } else {
            int one_value = 1;
            int zero_value = 0;
            CFNumberRef one = cf_number(one_value);
            CFNumberRef zero = cf_number(zero_value);
            const void* subtaps[] = { subtap };
            CFArrayRef subtap_list =
                CFArrayCreate(kCFAllocatorDefault, subtaps, 1, &kCFTypeArrayCallBacks);

            if (! one || ! zero || ! subtap_list) {
                status = kMemFullErr;
            } else {
                CFDictionarySetValue(subtap, CFSTR(kAudioSubTapUIDKey), tap_uid_ref);
                CFDictionarySetValue(subtap, CFSTR(kAudioSubTapDriftCompensationKey), one);

                CFDictionarySetValue(aggregate_dict, CFSTR(kAudioAggregateDeviceNameKey),
                                     aggregate_name);
                CFDictionarySetValue(aggregate_dict, CFSTR(kAudioAggregateDeviceUIDKey),
                                     aggregate_uid);
                CFDictionarySetValue(aggregate_dict, CFSTR(kAudioAggregateDeviceIsPrivateKey),
                                     one);
                CFDictionarySetValue(aggregate_dict,
                                     CFSTR(kAudioAggregateDeviceTapAutoStartKey), zero);
                CFDictionarySetValue(aggregate_dict, CFSTR(kAudioAggregateDeviceTapListKey),
                                     subtap_list);

                status = AudioHardwareCreateAggregateDevice(aggregate_dict, &aggregate);
            }

            if (subtap_list) CFRelease(subtap_list);
            if (one) CFRelease(one);
            if (zero) CFRelease(zero);
        }

        if (aggregate_dict) CFRelease(aggregate_dict);
        if (subtap) CFRelease(subtap);
        if (tap_uid_ref) CFRelease(tap_uid_ref);
        if (aggregate_uid) CFRelease(aggregate_uid);
        return status;
    }

    void destroy_io_proc() {
        if (aggregate == kAudioObjectUnknown || ! io_proc) return;
        AudioDeviceStop(aggregate, io_proc);
        AudioDeviceDestroyIOProcID(aggregate, io_proc);
        io_proc = nullptr;
    }

    void destroy_aggregate() {
        if (aggregate == kAudioObjectUnknown) return;
        AudioHardwareDestroyAggregateDevice(aggregate);
        aggregate = kAudioObjectUnknown;
    }

    void destroy_tap() {
        if (tap == kAudioObjectUnknown) return;
        wavsen_coreaudio_destroy_tap(tap);
        tap = kAudioObjectUnknown;
    }

    void process_input(const AudioBufferList* input_data) {
        if (! input_data || input_data->mNumberBuffers == 0) return;
        if (format.mFormatID != kAudioFormatLinearPCM || format.mBitsPerChannel == 0) return;

        const bool non_interleaved =
            (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        const std::uint32_t bytes_per_sample =
            std::max<std::uint32_t>(1, (format.mBitsPerChannel + 7) / 8);
        const std::uint32_t channels = std::max<std::uint32_t>(1, source_channels);

        std::uint32_t frames = 0;
        if (non_interleaved) {
            const auto& b = input_data->mBuffers[0];
            const auto channels_in_buffer = std::max<std::uint32_t>(1, b.mNumberChannels);
            frames = b.mDataByteSize / (bytes_per_sample * channels_in_buffer);
        } else {
            const auto bytes_per_frame =
                format.mBytesPerFrame != 0 ? format.mBytesPerFrame : bytes_per_sample * channels;
            frames = input_data->mBuffers[0].mDataByteSize / bytes_per_frame;
        }
        if (frames == 0) return;

        convert.resize(static_cast<std::size_t>(frames) * kDefaultChannels);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float left = read_channel(input_data, frame, 0, non_interleaved,
                                            bytes_per_sample, channels);
            const float right = channels > 1
                                    ? read_channel(input_data, frame, 1, non_interleaved,
                                                   bytes_per_sample, channels)
                                    : left;
            const auto base = static_cast<std::size_t>(frame) * kDefaultChannels;
            convert[base] = left;
            convert[base + 1] = right;
        }

        if (callback) callback(convert.data(), frames, kDefaultChannels, sample_rate, user);
    }

    float read_channel(const AudioBufferList* input_data, std::uint32_t frame,
                       std::uint32_t channel, bool non_interleaved,
                       std::uint32_t bytes_per_sample, std::uint32_t channels) const {
        if (non_interleaved) {
            const auto buffer_index =
                std::min<std::uint32_t>(channel, input_data->mNumberBuffers - 1);
            const auto& buffer = input_data->mBuffers[buffer_index];
            const auto channels_in_buffer = std::max<std::uint32_t>(1, buffer.mNumberChannels);
            const auto channel_in_buffer = input_data->mNumberBuffers == 1
                                               ? std::min(channel, channels_in_buffer - 1)
                                               : 0u;
            const auto sample_index =
                static_cast<std::size_t>(frame) * channels_in_buffer + channel_in_buffer;
            return read_sample(buffer.mData, sample_index, bytes_per_sample);
        }

        const auto& buffer = input_data->mBuffers[0];
        const auto sample_index =
            static_cast<std::size_t>(frame) * channels + std::min(channel, channels - 1);
        return read_sample(buffer.mData, sample_index, bytes_per_sample);
    }

    float read_sample(const void* data, std::size_t sample_index,
                      std::uint32_t bytes_per_sample) const {
        if (! data) return 0.0f;
        const bool is_float = (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
        const bool is_signed = (format.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
        const bool is_big_endian = (format.mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;

        if (is_float && format.mBitsPerChannel == 32) {
            const auto* f = static_cast<const float*>(data);
            return clamp_unit(f[sample_index]);
        }
        if (is_float && format.mBitsPerChannel == 64) {
            const auto* f = static_cast<const double*>(data);
            return clamp_unit(static_cast<float>(f[sample_index]));
        }
        if (! is_signed) return 0.0f;

        if (format.mBitsPerChannel == 16) {
            const auto* s = static_cast<const std::int16_t*>(data);
            return clamp_unit(static_cast<float>(s[sample_index]) / 32768.0f);
        }
        if (format.mBitsPerChannel == 32) {
            const auto* s = static_cast<const std::int32_t*>(data);
            return clamp_unit(static_cast<float>(s[sample_index]) / 2147483648.0f);
        }
        if (format.mBitsPerChannel == 24) {
            const auto* bytes = static_cast<const std::uint8_t*>(data) +
                                sample_index * bytes_per_sample;
            std::int32_t v = 0;
            if (is_big_endian) {
                v = (static_cast<std::int32_t>(bytes[0]) << 16) |
                    (static_cast<std::int32_t>(bytes[1]) << 8) |
                    static_cast<std::int32_t>(bytes[2]);
            } else {
                v = static_cast<std::int32_t>(bytes[0]) |
                    (static_cast<std::int32_t>(bytes[1]) << 8) |
                    (static_cast<std::int32_t>(bytes[2]) << 16);
            }
            if (v & 0x00800000) v |= static_cast<std::int32_t>(0xff000000);
            return clamp_unit(static_cast<float>(v) / 8388608.0f);
        }
        return 0.0f;
    }
};

static OSStatus wavsen_coreaudio_capture_io_proc(AudioObjectID /*device*/,
                                                 const AudioTimeStamp* /*now*/,
                                                 const AudioBufferList* input_data,
                                                 const AudioTimeStamp* /*input_time*/,
                                                 AudioBufferList* /*output_data*/,
                                                 const AudioTimeStamp* /*output_time*/,
                                                 void* user) {
    auto* capture = static_cast<wavsen_coreaudio_capture*>(user);
    if (! capture || ! capture->running.load(std::memory_order_acquire)) return noErr;
    capture->process_input(input_data);
    return noErr;
}

bool wavsen_coreaudio_capture_create(wavsen_coreaudio_capture_callback callback, void* user,
                                     wavsen_coreaudio_capture** out, int* out_status,
                                     std::uint32_t* out_source_channels,
                                     std::uint32_t* out_sample_rate) {
    if (out) *out = nullptr;
    if (out_status) *out_status = noErr;
    if (out_source_channels) *out_source_channels = kDefaultChannels;
    if (out_sample_rate) *out_sample_rate = kDefaultRate;
    if (! out || ! callback) {
        if (out_status) *out_status = kParamErr;
        return false;
    }

    auto* capture = new wavsen_coreaudio_capture();
    capture->callback = callback;
    capture->user = user;
    capture->set_default_format();

    char tap_uid[256] {};
    OSStatus st = noErr;
    if (! wavsen_coreaudio_create_global_tap(&capture->tap, tap_uid, sizeof(tap_uid), &st)) {
        if (out_status) *out_status = st;
        delete capture;
        return false;
    }

    (void)capture->read_tap_format();

    st = capture->create_aggregate_device(tap_uid);
    if (st != noErr || capture->aggregate == kAudioObjectUnknown) {
        if (out_status) *out_status = st;
        capture->destroy_tap();
        delete capture;
        return false;
    }

    st = AudioDeviceCreateIOProcID(capture->aggregate, wavsen_coreaudio_capture_io_proc,
                                  capture, &capture->io_proc);
    if (st != noErr || ! capture->io_proc) {
        if (out_status) *out_status = st;
        capture->destroy_aggregate();
        capture->destroy_tap();
        delete capture;
        return false;
    }

    st = AudioDeviceStart(capture->aggregate, capture->io_proc);
    if (st != noErr) {
        if (out_status) *out_status = st;
        capture->destroy_io_proc();
        capture->destroy_aggregate();
        capture->destroy_tap();
        delete capture;
        return false;
    }

    capture->running.store(true, std::memory_order_release);
    if (out_source_channels) *out_source_channels = capture->source_channels;
    if (out_sample_rate) *out_sample_rate = capture->sample_rate;
    *out = capture;
    return true;
}

void wavsen_coreaudio_capture_destroy(wavsen_coreaudio_capture* capture) {
    if (! capture) return;
    capture->running.store(false, std::memory_order_release);
    capture->destroy_io_proc();
    capture->destroy_aggregate();
    capture->destroy_tap();
    delete capture;
}
