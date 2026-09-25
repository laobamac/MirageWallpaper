#pragma once

#include "audio_capture_dsp.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>

// This is the SceneRenderer capture cadence and calibration expressed for the
// WebRenderer host. The audio callback performs each 1024-frame FFT hop; the
// UI thread only snapshots the already-smoothed 64-bin stereo result.
class WRAudioSpectrum final {
public:
    // `leftScale` and `rightScale` undo the known sink-monitor attenuation
    // reported by the backend; ownership of `samples` remains with the audio
    // backend and the call is synchronous on its process thread.
    void append(const float* samples, std::uint32_t frames, std::uint32_t channels,
                float leftScale, float rightScale) {
        std::lock_guard<std::mutex> inputLock(m_inputMutex);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const std::uint32_t base = frame * channels;
            m_ringLeft[m_ringHead] = channels > 0 ? samples[base] * leftScale : 0.0f;
            m_ringRight[m_ringHead] = channels > 1 ? samples[base + 1] * rightScale : m_ringLeft[m_ringHead];
            m_ringHead = (m_ringHead + 1) % wavsen::audio::dsp::kFftSize;
            if (m_samplesFilled < wavsen::audio::dsp::kFftSize) ++m_samplesFilled;
            ++m_samplesSinceFft;
        }
        if (m_samplesFilled < wavsen::audio::dsp::kFftSize ||
            m_samplesSinceFft < wavsen::audio::dsp::kHopSize) return;
        m_samplesSinceFft = 0;

        std::array<std::complex<float>, wavsen::audio::dsp::kFftSize> transformLeft {};
        std::array<std::complex<float>, wavsen::audio::dsp::kFftSize> transformRight {};
        for (std::size_t index = 0; index < wavsen::audio::dsp::kFftSize; ++index) {
            const std::size_t source = (m_ringHead + index) % wavsen::audio::dsp::kFftSize;
            const float window = wavsen::audio::dsp::hann_window(index, wavsen::audio::dsp::kFftSize);
            transformLeft[index] = std::complex<float>(m_ringLeft[source] * window, 0.0f);
            transformRight[index] = std::complex<float>(m_ringRight[source] * window, 0.0f);
        }
        wavsen::audio::dsp::fft_inplace(transformLeft.data(), transformLeft.size());
        wavsen::audio::dsp::fft_inplace(transformRight.data(), transformRight.size());
        const auto raw = wavsen::audio::dsp::analyze_stereo_spectrum(
            transformLeft.data(), transformRight.data(), m_bandLayout,
            wavsen::audio::dsp::kFftAmplitudeNorm);
        const auto smoothed = wavsen::audio::dsp::smooth_spectrum(
            raw, m_smoothed,
            static_cast<float>(wavsen::audio::dsp::kHopSize) / kSampleRate);
        std::lock_guard<std::mutex> outputLock(m_outputMutex);
        m_left = smoothed.left;
        m_right = smoothed.right;
        m_hasSpectrum = true;
    }

    bool copy(std::array<float, 64>& left, std::array<float, 64>& right) const {
        std::lock_guard<std::mutex> outputLock(m_outputMutex);
        if (!m_hasSpectrum) return false;
        left = m_left;
        right = m_right;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> inputLock(m_inputMutex);
        std::lock_guard<std::mutex> outputLock(m_outputMutex);
        m_ringLeft.fill(0.0f);
        m_ringRight.fill(0.0f);
        m_ringHead = 0;
        m_samplesFilled = 0;
        m_samplesSinceFft = 0;
        m_smoothed = {};
        m_left.fill(0.0f);
        m_right.fill(0.0f);
        m_hasSpectrum = false;
    }

private:
    static constexpr float kSampleRate = 48000.0f;

    std::array<float, wavsen::audio::dsp::kFftSize> m_ringLeft {};
    std::array<float, wavsen::audio::dsp::kFftSize> m_ringRight {};
    std::size_t m_ringHead = 0;
    std::size_t m_samplesFilled = 0;
    std::size_t m_samplesSinceFft = 0;
    wavsen::audio::dsp::BandLayout m_bandLayout = wavsen::audio::dsp::make_we_layout(kSampleRate);
    wavsen::audio::dsp::SpectrumBands m_smoothed {};
    std::array<float, 64> m_left {};
    std::array<float, 64> m_right {};
    bool m_hasSpectrum = false;
    std::mutex m_inputMutex;
    mutable std::mutex m_outputMutex;
};
