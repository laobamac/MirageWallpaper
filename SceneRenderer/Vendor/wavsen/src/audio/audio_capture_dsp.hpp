#pragma once

// Shared DSP bits between the PulseAudio and PipeWire backends of
// wavsen::audio::AudioCapture. Pure CPU code; no backend headers.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace wavsen::audio::dsp
{

inline constexpr std::size_t kFftSize = 4096;
inline constexpr std::size_t kNumBins = 64;
inline constexpr std::size_t kHalfFft = kFftSize / 2;
inline constexpr std::size_t kHopSize = 1024;
inline constexpr float kMinFrequencyHz = 10.0f;
inline constexpr float kMaxFrequencyHz = 16000.0f;
inline constexpr float kTiltPivotHz = 1000.0f;
inline constexpr float kTiltExp = 1.15f;
inline constexpr float kDbFloor = -100.0f;
inline constexpr float kDbCeil = -8.0f;
inline constexpr float kResponseContrast = 1.6f;
inline constexpr float kResponseScale = 1.0f;
inline constexpr float kResponseCeil = 1.0f;
inline constexpr float kAttackTimeSec = 0.030f;
inline constexpr float kReleaseTimeSec = 0.140f;

struct BandLayout {
    std::array<std::size_t, kNumBins + 1> edges{};
    std::array<float, kNumBins> gain{};
};

struct SpectrumBands {
    std::array<float, kNumBins> left{};
    std::array<float, kNumBins> right{};
    std::array<float, kNumBins> average{};
};

struct BandAnchor {
    float hz;
    float band;
};

inline constexpr std::array<BandAnchor, 11> kBandAnchors{{
    {kMinFrequencyHz, 0.0f},
    {60.0f, 2.0f},
    {125.0f, 5.0f},
    {250.0f, 10.0f},
    {500.0f, 21.0f},
    {1000.0f, 32.0f},
    {2000.0f, 38.0f},
    {3000.0f, 46.0f},
    {8000.0f, 54.0f},
    {12000.0f, 60.0f},
    {16000.0f, 64.0f},
}};

inline std::size_t hz_to_upper_bin(float hz, float sample_rate) {
    const auto bin =
        static_cast<std::size_t>(std::ceil(static_cast<double>(hz) * static_cast<double>(kFftSize) /
                                           static_cast<double>(sample_rate)));
    return std::clamp<std::size_t>(bin, 1, kHalfFft);
}

inline float anchor_frequency_for_band(float band) {
    if (band <= kBandAnchors.front().band)
        return kBandAnchors.front().hz;
    if (band >= kBandAnchors.back().band)
        return kBandAnchors.back().hz;

    for (std::size_t i = 1; i < kBandAnchors.size(); ++i) {
        const auto& hi = kBandAnchors[i];
        if (band > hi.band)
            continue;
        const auto& lo = kBandAnchors[i - 1];
        const float t = (band - lo.band) / (hi.band - lo.band);
        const float log_lo = std::log(lo.hz);
        const float log_hi = std::log(hi.hz);
        return std::exp(log_lo + (log_hi - log_lo) * t);
    }
    return kBandAnchors.back().hz;
}

inline BandLayout make_we_layout(float sample_rate) {
    BandLayout layout{};
    const float nyquist = sample_rate * 0.5f;
    const float max_hz = std::min(kMaxFrequencyHz, nyquist);
    const auto max_bin = hz_to_upper_bin(max_hz, sample_rate);

    for (std::size_t k = 0; k < kNumBins; ++k) {
        const float hz = std::min(anchor_frequency_for_band(static_cast<float>(k) - 0.5f), max_hz);
        std::size_t next = hz_to_upper_bin(hz, sample_rate);
        if (k > 0 && next <= layout.edges[k - 1])
            next = layout.edges[k - 1] + 1;
        const std::size_t remaining = kNumBins - k;
        if (next + remaining > max_bin)
            next = max_bin - remaining;
        layout.edges[k] = next;
    }
    layout.edges[kNumBins] = max_bin;
    for (std::size_t k = 0; k < kNumBins; ++k) {
        const float upper_hz =
            static_cast<float>(layout.edges[k + 1]) * sample_rate / static_cast<float>(kFftSize);
        layout.gain[k] = std::pow(upper_hz / kTiltPivotHz, kTiltExp);
    }
    return layout;
}

inline float band_magnitude(const std::complex<float>* left, const BandLayout& layout,
                            std::size_t band, float norm) {
    const std::size_t lo = layout.edges[band];
    const std::size_t hi = layout.edges[band + 1];
    float sum = 0.f;
    for (std::size_t i = lo; i < hi; ++i) {
        const float v = std::abs(left[i]);
        sum += v;
    }
    const float width = static_cast<float>(std::max<std::size_t>(hi - lo, 1));
    return (sum / width) * norm;
}

inline float shape_response(float unit) {
    const float x = std::clamp(unit, 0.0f, 1.0f);
    if (x <= 0.5f)
        return 0.5f * std::pow(x * 2.0f, kResponseContrast);
    return 1.0f - 0.5f * std::pow((1.0f - x) * 2.0f, kResponseContrast);
}

inline float visual_response(float mag, const BandLayout& layout, std::size_t band) {
    const float compensated = std::max(mag * layout.gain[band], 1.0e-12f);
    const float db = 20.0f * std::log10(compensated);
    const float unit = std::clamp((db - kDbFloor) / (kDbCeil - kDbFloor), 0.0f, 1.0f);
    const float shaped = shape_response(unit);
    return std::min(shaped * kResponseScale, kResponseCeil);
}

inline SpectrumBands analyze_stereo_spectrum(const std::complex<float>* left,
                                             const std::complex<float>* right,
                                             const BandLayout& layout, float norm) {
    SpectrumBands raw{};
    for (std::size_t k = 0; k < kNumBins; ++k) {
        raw.left[k] = visual_response(band_magnitude(left, layout, k, norm), layout, k);
        raw.right[k] = visual_response(band_magnitude(right, layout, k, norm), layout, k);
        raw.average[k] = 0.5f * (raw.left[k] + raw.right[k]);
    }
    return raw;
}

inline float smooth_value(float prev, float cur, float dt_sec) {
    const float tau = cur > prev ? kAttackTimeSec : kReleaseTimeSec;
    const float a = 1.0f - std::exp(-dt_sec / tau);
    return prev + a * (cur - prev);
}

inline SpectrumBands smooth_spectrum(const SpectrumBands& raw, SpectrumBands& state, float dt_sec) {
    SpectrumBands out{};
    for (std::size_t k = 0; k < kNumBins; ++k) {
        state.left[k] = smooth_value(state.left[k], raw.left[k], dt_sec);
        state.right[k] = smooth_value(state.right[k], raw.right[k], dt_sec);
        out.left[k] = state.left[k];
        out.right[k] = state.right[k];
        out.average[k] = 0.5f * (out.left[k] + out.right[k]);
    }
    return out;
}

// In-place radix-2 Cooley-Tukey FFT (forward).
inline void fft_inplace(std::complex<float>* data, std::size_t n) {
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            const std::size_t half = len >> 1;
            for (std::size_t k = 0; k < half; ++k) {
                const auto u = data[i + k];
                const auto v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }
}

inline float hann_window(std::size_t i, std::size_t n) {
    return 0.5f * (1.0f - std::cos(2.f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                   static_cast<float>(n - 1)));
}

} // namespace wavsen::audio::dsp
