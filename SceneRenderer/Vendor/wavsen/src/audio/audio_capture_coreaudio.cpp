module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "audio_capture_dsp.hpp"
#include "audio_coreaudio_capture.h"
#include "audio_loopback_analyzer.hpp"

module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :capture;

namespace wavsen::audio
{

namespace
{

constexpr std::uint32_t kDefaultRate = 48000;
constexpr std::uint32_t kDefaultChannels = 2;
constexpr std::int64_t  kMergeFreshMs = 250;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool fresh(std::int64_t publish_ms) {
    return publish_ms > 0 && (now_ms() - publish_ms) <= kMergeFreshMs;
}

bool audio_debug_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WAVSEN_AUDIO_DEBUG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

} // namespace

class AudioCapture::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (is_inited()) return true;

        int status = 0;
        std::uint32_t source_channels = kDefaultChannels;
        std::uint32_t sample_rate = kDefaultRate;
        if (! wavsen_coreaudio_capture_create(&Impl::on_samples, this, &capture_, &status,
                                              &source_channels, &sample_rate)) {
            capture_ = nullptr;
            inited_.store(true, std::memory_order_release);
            rstd::log::warn("wavsen::audio: coreaudio tap unavailable ({}); "
                            "using scene-output analyzer fallback",
                            status);
            return true;
        }

        sample_rate_.store(sample_rate != 0 ? sample_rate : kDefaultRate,
                           std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(dsp_mu_);
            band_layout_ = dsp::make_we_layout(static_cast<float>(sample_rate_.load()));
        }
        inited_.store(true, std::memory_order_release);
        rstd::log::info("wavsen::audio: coreaudio capture inited ({} ch @ {} Hz)",
                        source_channels, sample_rate_.load(std::memory_order_relaxed));
        return true;
    }

    void uninit() {
        wavsen_coreaudio_capture_destroy(capture_);
        capture_ = nullptr;
        inited_.store(false, std::memory_order_release);
    }

    bool is_inited() const { return inited_.load(std::memory_order_acquire); }

    bool snapshot(AudioSpectrum& out) const {
        AudioSpectrum tap_spec;
        const bool has_tap = snapshot_tap(tap_spec) && fresh(tap_spec.publish_ms);

        loopback::SpectrumSnapshot loop_spec;
        const bool has_loop = loopback::snapshot(loop_spec) && fresh(loop_spec.publish_ms);

        if (! has_tap && ! has_loop) {
            out.clear();
            return false;
        }

        out.clear();
        if (has_tap) out = tap_spec;
        if (has_loop) {
            for (std::size_t k = 0; k < loop_spec.average.size(); ++k) {
                out.left[k] = std::max(out.left[k], loop_spec.left[k]);
                out.right[k] = std::max(out.right[k], loop_spec.right[k]);
                out.average[k] = std::max(out.average[k], loop_spec.average[k]);
                out.bins[k] = out.average[k];
            }
            out.publish_ms = std::max(out.publish_ms, loop_spec.publish_ms);
        }
        if (audio_debug_enabled()) {
            const auto n = snapshot_debug_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || n % 120 == 0) {
                float max_avg = 0.0f;
                for (float v : out.average) max_avg = std::max(max_avg, v);
                std::fprintf(stderr,
                             "wavsen-audio-debug: capture snapshot=%llu tap=%d loop=%d "
                             "max_avg=%.4f publish=%lld\n",
                             static_cast<unsigned long long>(n),
                             has_tap ? 1 : 0,
                             has_loop ? 1 : 0,
                             max_avg,
                             static_cast<long long>(out.publish_ms));
            }
        }
        return true;
    }

private:
    static void on_samples(const float* samples, std::uint32_t frames, std::uint32_t channels,
                           std::uint32_t sample_rate, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (! self || ! samples) return;
        self->ingest(samples, frames, channels, sample_rate);
    }

    void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels,
                std::uint32_t sample_rate) {
        if (! src || n_frames == 0 || channels == 0) return;

        std::lock_guard<std::mutex> lk(dsp_mu_);
        if (sample_rate != 0 && sample_rate != sample_rate_.load(std::memory_order_relaxed)) {
            sample_rate_.store(sample_rate, std::memory_order_relaxed);
            ring_left_.fill(0.0f);
            ring_right_.fill(0.0f);
            ring_head_ = 0;
            samples_filled_ = 0;
            samples_since_fft_ = 0;
            smoothed_ = {};
            band_layout_ = dsp::make_we_layout(static_cast<float>(sample_rate));
        }

        for (std::uint32_t f = 0; f < n_frames; ++f) {
            const std::uint32_t base = f * channels;
            const float left = channels > 0 ? src[base] : 0.0f;
            const float right = channels > 1 ? src[base + 1] : left;
            ring_left_[ring_head_] = left;
            ring_right_[ring_head_] = right;
            ring_head_ = (ring_head_ + 1) % dsp::kFftSize;
            if (samples_filled_ < dsp::kFftSize) ++samples_filled_;
            ++samples_since_fft_;
        }

        if (samples_filled_ < dsp::kFftSize || samples_since_fft_ < dsp::kHopSize) return;
        samples_since_fft_ = 0;

        std::array<std::complex<float>, dsp::kFftSize> buf_left;
        std::array<std::complex<float>, dsp::kFftSize> buf_right;
        for (std::size_t i = 0; i < dsp::kFftSize; ++i) {
            const std::size_t idx = (ring_head_ + i) % dsp::kFftSize;
            const float w = dsp::hann_window(i, dsp::kFftSize);
            buf_left[i] = std::complex<float>(ring_left_[idx] * w, 0.0f);
            buf_right[i] = std::complex<float>(ring_right_[idx] * w, 0.0f);
        }

        dsp::fft_inplace(buf_left.data(), dsp::kFftSize);
        dsp::fft_inplace(buf_right.data(), dsp::kFftSize);

        const float norm = 2.0f / static_cast<float>(dsp::kFftSize);
        const auto raw =
            dsp::analyze_stereo_spectrum(buf_left.data(), buf_right.data(), band_layout_, norm);
        const auto dt_sec = static_cast<float>(dsp::kHopSize) /
                            static_cast<float>(sample_rate_.load(std::memory_order_relaxed));
        const auto bands = dsp::smooth_spectrum(raw, smoothed_, dt_sec);

        AudioSpectrum out {};
        for (std::size_t k = 0; k < dsp::kNumBins; ++k) {
            out.left[k] = bands.left[k];
            out.right[k] = bands.right[k];
            out.average[k] = bands.average[k];
            out.bins[k] = bands.average[k];
        }
        out.publish_ms = now_ms();

        seq_.fetch_add(1, std::memory_order_release);
        std::memcpy(&published_, &out, sizeof(AudioSpectrum));
        seq_.fetch_add(1, std::memory_order_release);
    }

    bool snapshot_tap(AudioSpectrum& out) const {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0) {
                out.clear();
                return false;
            }
            if (s1 & 1u) continue;
            AudioSpectrum tmp;
            std::memcpy(&tmp, &published_, sizeof(AudioSpectrum));
            const std::uint32_t s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) {
                out = tmp;
                return true;
            }
        }
        out.clear();
        return false;
    }

    std::atomic<bool> inited_ { false };
    wavsen_coreaudio_capture* capture_ { nullptr };

    std::mutex                         dsp_mu_;
    std::atomic<std::uint32_t>         sample_rate_ { kDefaultRate };
    std::array<float, dsp::kFftSize>   ring_left_ {};
    std::array<float, dsp::kFftSize>   ring_right_ {};
    std::size_t                        ring_head_ { 0 };
    std::size_t                        samples_filled_ { 0 };
    std::size_t                        samples_since_fft_ { 0 };
    dsp::BandLayout                    band_layout_ { dsp::make_we_layout(kDefaultRate) };
    dsp::SpectrumBands                 smoothed_ {};

    mutable std::atomic<std::uint32_t> seq_ { 0 };
    AudioSpectrum                       published_ {};
    mutable std::atomic<std::uint64_t>  snapshot_debug_count_ { 0 };
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;

bool AudioCapture::init() { return impl_->init(); }
void AudioCapture::uninit() { impl_->uninit(); }
bool AudioCapture::is_inited() const { return impl_->is_inited(); }
bool AudioCapture::snapshot(AudioSpectrum& out) const { return impl_->snapshot(out); }

} // namespace wavsen::audio
