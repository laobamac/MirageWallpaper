#include "audio_loopback_analyzer.hpp"

#include <atomic>
#include <chrono>
#include <complex>
#include <cstring>
#include <mutex>

#include "audio_capture_dsp.hpp"

namespace wavsen::audio::loopback
{

namespace
{

constexpr std::uint32_t kFallbackRate = 48000;

class Analyzer {
public:
    void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels,
                std::uint32_t sample_rate) {
        if (! src || n_frames == 0 || channels == 0) return;

        std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
        if (! lk.owns_lock()) return;

        const auto effective_rate = sample_rate != 0 ? sample_rate : kFallbackRate;
        if (effective_rate != sample_rate_) {
            reset_locked(effective_rate);
        }

        for (std::uint32_t f = 0; f < n_frames; ++f) {
            const std::uint32_t base = f * channels;
            const float left = src[base];
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
        const auto dt_sec =
            static_cast<float>(dsp::kHopSize) / static_cast<float>(sample_rate_);
        const auto bands = dsp::smooth_spectrum(raw, smoothed_, dt_sec);

        SpectrumSnapshot out {};
        for (std::size_t k = 0; k < dsp::kNumBins; ++k) {
            out.left[k] = bands.left[k];
            out.right[k] = bands.right[k];
            out.average[k] = bands.average[k];
        }
        out.publish_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

        seq_.fetch_add(1, std::memory_order_release);
        std::memcpy(&published_, &out, sizeof(SpectrumSnapshot));
        seq_.fetch_add(1, std::memory_order_release);
    }

    bool snapshot(SpectrumSnapshot& out) const {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0) {
                out.clear();
                return false;
            }
            if (s1 & 1u) continue;
            SpectrumSnapshot tmp;
            std::memcpy(&tmp, &published_, sizeof(SpectrumSnapshot));
            const std::uint32_t s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) {
                out = tmp;
                return true;
            }
        }
        out.clear();
        return false;
    }

private:
    void reset_locked(std::uint32_t sample_rate) {
        sample_rate_ = sample_rate;
        ring_left_.fill(0.0f);
        ring_right_.fill(0.0f);
        ring_head_ = 0;
        samples_filled_ = 0;
        samples_since_fft_ = 0;
        band_layout_ = dsp::make_we_layout(static_cast<float>(sample_rate_));
        smoothed_ = {};
    }

    mutable std::mutex mu_;
    std::uint32_t      sample_rate_ { kFallbackRate };

    std::array<float, dsp::kFftSize> ring_left_ {};
    std::array<float, dsp::kFftSize> ring_right_ {};
    std::size_t                      ring_head_ { 0 };
    std::size_t                      samples_filled_ { 0 };
    std::size_t                      samples_since_fft_ { 0 };
    dsp::BandLayout                  band_layout_ { dsp::make_we_layout(kFallbackRate) };
    dsp::SpectrumBands               smoothed_ {};

    mutable std::atomic<std::uint32_t> seq_ { 0 };
    SpectrumSnapshot                   published_ {};
};

Analyzer& analyzer() {
    static Analyzer instance;
    return instance;
}

} // namespace

void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels,
            std::uint32_t sample_rate) {
    analyzer().ingest(src, n_frames, channels, sample_rate);
}

bool snapshot(SpectrumSnapshot& out) {
    return analyzer().snapshot(out);
}

} // namespace wavsen::audio::loopback
