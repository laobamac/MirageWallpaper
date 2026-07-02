module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "audio_coreaudio_output.h"
#include "audio_loopback_analyzer.hpp"

module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :core;

namespace wavsen::audio
{

namespace
{

constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultChannels = 2;

float clamp_volume_scale(float v) {
    if (! std::isfinite(v) || v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

bool audio_debug_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WAVSEN_AUDIO_DEBUG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

class AudioDevice::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (is_inited()) return true;

        std::uint32_t channels = kDefaultChannels;
        std::uint32_t sample_rate = kDefaultRate;
        int status = 0;
        if (! wavsen_coreaudio_output_create(&Impl::on_output, this, &output_, &status, &channels,
                                             &sample_rate)) {
            rstd::log::error("wavsen::audio: coreaudio output init failed ({})", status);
            output_ = nullptr;
            return false;
        }

        desc_ = { channels, sample_rate };
        {
            std::lock_guard<std::mutex> lk(channels_mu_);
            for (auto& c : channels_) {
                if (c) c->pass_desc(desc_);
            }
        }

        if (audio_debug_enabled()) {
            std::fprintf(stderr, "wavsen-audio-debug: coreaudio output init %u ch @ %u Hz\n",
                         desc_.channels, desc_.sample_rate);
        }
        inited_.store(true, std::memory_order_release);
        rstd::log::info("wavsen::audio: coreaudio output inited ({} ch @ {} Hz)",
                        desc_.channels, desc_.sample_rate);
        return true;
    }

    void uninit() {
        if (! is_inited()) return;
        stop();
        unmount_all();
        wavsen_coreaudio_output_destroy(output_);
        output_ = nullptr;
        inited_.store(false, std::memory_order_release);
    }

    bool is_inited() const {
        return inited_.load(std::memory_order_acquire) && output_ != nullptr;
    }

    void start() {
        if (! output_) return;
        bool expected = false;
        if (! started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

        const auto base = position_frames_.load(std::memory_order_relaxed);
        rendered_frames_.store(base, std::memory_order_relaxed);
        position_at_start_frames_.store(base, std::memory_order_relaxed);
        start_time_ns_.store(now_ns(), std::memory_order_release);

        if (audio_debug_enabled()) {
            std::fprintf(stderr, "wavsen-audio-debug: AudioDevice start base=%llu\n",
                         static_cast<unsigned long long>(base));
        }
        int status = 0;
        if (! wavsen_coreaudio_output_start(output_, &status)) {
            started_.store(false, std::memory_order_release);
            position_frames_.store(base, std::memory_order_relaxed);
            rendered_frames_.store(base, std::memory_order_relaxed);
            rstd::log::error("wavsen::audio: coreaudio output start failed ({})", status);
        }
    }

    void stop() {
        if (! output_) return;
        if (! started_.exchange(false, std::memory_order_acq_rel)) return;
        const auto pos = current_position_frames();
        wavsen_coreaudio_output_stop(output_);
        position_frames_.store(pos, std::memory_order_relaxed);
        rendered_frames_.store(pos, std::memory_order_relaxed);
    }

    void mount(std::unique_ptr<IPullChannel> ch) {
        if (! ch) return;
        if (is_inited()) ch->pass_desc(desc_);
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.push_back(std::move(ch));
        if (audio_debug_enabled()) {
            std::fprintf(stderr, "wavsen-audio-debug: mounted channel, total=%zu\n",
                         channels_.size());
        }
    }

    void unmount_all() {
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.clear();
    }

    float volume() const { return volume_.load(std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }
    void set_volume(float v) {
        if (! std::isfinite(v)) v = 0.0f;
        volume_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }

    float volume_scale() const { return volume_scale_.load(std::memory_order_relaxed); }
    void set_volume_scale(float v) {
        v = clamp_volume_scale(v);
        volume_scale_epoch_.fetch_add(1, std::memory_order_acq_rel);
        volume_scale_target_.store(v, std::memory_order_relaxed);
        volume_scale_step_.store(0.0f, std::memory_order_relaxed);
        volume_scale_frames_left_.store(0, std::memory_order_relaxed);
        volume_scale_.store(v, std::memory_order_relaxed);
        volume_scale_epoch_.fetch_add(1, std::memory_order_release);
    }
    void set_volume_scale(float v, std::uint32_t fade_ms) {
        v = clamp_volume_scale(v);
        if (fade_ms == 0) {
            set_volume_scale(v);
            return;
        }
        auto fade_frames64 = static_cast<std::uint64_t>(desc_.sample_rate) * fade_ms / 1000ULL;
        if (fade_frames64 == 0) {
            set_volume_scale(v);
            return;
        }
        if (fade_frames64 > 0xffffffffULL) fade_frames64 = 0xffffffffULL;
        const auto fade_frames = static_cast<std::uint32_t>(fade_frames64);
        const auto current = volume_scale_.load(std::memory_order_relaxed);
        if (current == v) {
            set_volume_scale(v);
            return;
        }

        volume_scale_epoch_.fetch_add(1, std::memory_order_acq_rel);
        volume_scale_target_.store(v, std::memory_order_relaxed);
        volume_scale_step_.store((v - current) / static_cast<float>(fade_frames),
                                 std::memory_order_relaxed);
        volume_scale_frames_left_.store(fade_frames, std::memory_order_relaxed);
        volume_scale_epoch_.fetch_add(1, std::memory_order_release);
    }

    DeviceDesc desc() const { return desc_; }

    std::uint64_t stream_position_frames() const { return current_position_frames(); }

private:
    std::uint64_t current_position_frames() const {
        if (! started_.load(std::memory_order_acquire)) {
            return position_frames_.load(std::memory_order_relaxed);
        }
        const auto base = position_at_start_frames_.load(std::memory_order_relaxed);
        const auto start_ns = start_time_ns_.load(std::memory_order_acquire);
        if (start_ns <= 0) return base;
        const auto elapsed_ns = std::max<std::int64_t>(0, now_ns() - start_ns);
        const auto elapsed_frames =
            static_cast<std::uint64_t>((static_cast<long double>(elapsed_ns) *
                                        static_cast<long double>(desc_.sample_rate)) /
                                       1'000'000'000.0L);
        const auto played = base + elapsed_frames;
        const auto rendered = rendered_frames_.load(std::memory_order_relaxed);
        return std::min(played, rendered);
    }

    void apply_output_gain(float* out_f, std::uint32_t n_frames, std::uint32_t channels) {
        const float volume = volume_.load(std::memory_order_relaxed);
        auto left = volume_scale_frames_left_.load(std::memory_order_relaxed);
        if (left == 0) {
            const float gain = volume * volume_scale_.load(std::memory_order_relaxed);
            const auto total_samples = static_cast<std::size_t>(n_frames) * channels;
            for (std::size_t i = 0; i < total_samples; ++i) {
                out_f[i] *= gain;
            }
            return;
        }

        const auto epoch = volume_scale_epoch_.load(std::memory_order_acquire);
        const float step = volume_scale_step_.load(std::memory_order_relaxed);
        const float target = volume_scale_target_.load(std::memory_order_relaxed);
        float scale = volume_scale_.load(std::memory_order_relaxed);
        for (std::uint32_t frame = 0; frame < n_frames; ++frame) {
            const float gain = volume * scale;
            const auto base = static_cast<std::size_t>(frame) * channels;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                out_f[base + ch] *= gain;
            }
            if (left > 0) {
                --left;
                scale = left == 0 ? target : scale + step;
            }
        }
        if ((epoch & 1u) == 0 &&
            volume_scale_epoch_.load(std::memory_order_acquire) == epoch) {
            volume_scale_.store(scale, std::memory_order_relaxed);
            volume_scale_frames_left_.store(left, std::memory_order_relaxed);
        }
    }

    void fill_output(float* out_f, std::uint32_t n_frames, std::uint32_t channels,
                     std::uint32_t sample_rate) {
        if (! out_f || n_frames == 0 || channels == 0) return;
        const auto total_samples = static_cast<std::size_t>(n_frames) * channels;
        std::memset(out_f, 0, total_samples * sizeof(float));

        std::uint64_t produced_total = 0;
        if (! muted_.load(std::memory_order_relaxed)) {
            std::vector<float> scratch(total_samples);
            {
                std::lock_guard<std::mutex> lk(channels_mu_);
                for (auto& ch : channels_) {
                    if (! ch) continue;
                    std::memset(scratch.data(), 0, total_samples * sizeof(float));
                    const auto produced = ch->next_pcm(scratch.data(), n_frames);
                    produced_total += produced;
                    const auto produced_samples =
                        std::min<std::size_t>(static_cast<std::size_t>(produced) * channels,
                                              total_samples);
                    for (std::size_t i = 0; i < produced_samples; ++i) {
                        out_f[i] += scratch[i];
                    }
                }
            }
            apply_output_gain(out_f, n_frames, channels);
            loopback::ingest(out_f, n_frames, channels, sample_rate);
        }

        rendered_frames_.fetch_add(n_frames, std::memory_order_relaxed);
        if (audio_debug_enabled()) {
            const auto cb = callbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cb <= 12 || cb % 120 == 0) {
                std::fprintf(stderr,
                             "wavsen-audio-debug: output cb=%llu frames=%u produced=%llu "
                             "channels=%zu muted=%d\n",
                             static_cast<unsigned long long>(cb),
                             n_frames,
                             static_cast<unsigned long long>(produced_total),
                             channel_count_debug(),
                             muted_.load(std::memory_order_relaxed) ? 1 : 0);
            }
        }
    }

    static void on_output(float* out, std::uint32_t frames, std::uint32_t channels,
                          std::uint32_t sample_rate, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (! self || ! self->started_.load(std::memory_order_acquire)) return;
        if (audio_debug_enabled()) {
            const auto cb = self->raw_callbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cb <= 6) {
                std::fprintf(stderr,
                             "wavsen-audio-debug: raw output callback cb=%llu frames=%u\n",
                             static_cast<unsigned long long>(cb),
                             frames);
            }
        }
        self->fill_output(out, frames, channels, sample_rate);
    }

    std::size_t channel_count_debug() {
        std::lock_guard<std::mutex> lk(channels_mu_);
        return channels_.size();
    }

    std::atomic<bool> inited_ { false };
    std::atomic<bool> started_ { false };

    wavsen_coreaudio_output* output_ { nullptr };
    DeviceDesc               desc_ { kDefaultChannels, kDefaultRate };

    std::mutex                                 channels_mu_;
    std::vector<std::unique_ptr<IPullChannel>> channels_;
    std::atomic<std::uint64_t> callbacks_ { 0 };
    std::atomic<std::uint64_t> raw_callbacks_ { 0 };

    std::atomic<float> volume_ { 1.0f };
    std::atomic<float> volume_scale_ { 1.0f };
    std::atomic<float> volume_scale_target_ { 1.0f };
    std::atomic<float> volume_scale_step_ { 0.0f };
    std::atomic<std::uint32_t> volume_scale_frames_left_ { 0 };
    std::atomic<std::uint32_t> volume_scale_epoch_ { 0 };
    std::atomic<bool> muted_ { false };

    std::atomic<std::uint64_t> position_frames_ { 0 };
    std::atomic<std::uint64_t> rendered_frames_ { 0 };
    std::atomic<std::uint64_t> position_at_start_frames_ { 0 };
    std::atomic<std::int64_t>  start_time_ns_ { 0 };
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() = default;

bool AudioDevice::init() { return impl_->init(); }
void AudioDevice::uninit() { impl_->uninit(); }
bool AudioDevice::is_inited() const { return impl_->is_inited(); }
void AudioDevice::start() { impl_->start(); }
void AudioDevice::stop() { impl_->stop(); }
void AudioDevice::mount(std::unique_ptr<IPullChannel> ch) { impl_->mount(std::move(ch)); }
void AudioDevice::unmount_all() { impl_->unmount_all(); }
float AudioDevice::volume() const { return impl_->volume(); }
bool AudioDevice::muted() const { return impl_->muted(); }
void AudioDevice::set_volume(float v) { impl_->set_volume(v); }
void AudioDevice::set_muted(bool m) { impl_->set_muted(m); }
float AudioDevice::volume_scale() const { return impl_->volume_scale(); }
void AudioDevice::set_volume_scale(float v) { impl_->set_volume_scale(v); }
void AudioDevice::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->set_volume_scale(v, fade_ms);
}
DeviceDesc AudioDevice::desc() const { return impl_->desc(); }
std::uint64_t AudioDevice::stream_position_frames() const {
    return impl_->stream_position_frames();
}

} // namespace wavsen::audio
