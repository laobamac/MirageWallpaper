// audio_capture_wasapi.cpp — WASAPI loopback capture backend.
// Captures system audio output for spectrum analysis.
// Implements wavsen::audio::AudioCapture::Impl via IAudioCaptureClient (loopback).
#ifdef WAVSEN_AUDIO_BACKEND_WASAPI

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// Forward-declare
namespace wavsen::audio {
struct AudioSpectrum {
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float, 64> average{};
    std::array<float, 64> bins{};
    std::int64_t publish_ms{0};
    void clear() { left.fill(0); right.fill(0); average.fill(0); bins.fill(0); publish_ms = 0; }
};
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    bool init();
    void uninit();
    bool is_inited() const;
    bool snapshot(AudioSpectrum& out) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

// ---------------------------------------------------------------------------
// Simple FFT (64-bin, 256-point → decimate to 64).
// For production, consider using Accelerate-like library (e.g. FFTW).
// ---------------------------------------------------------------------------
namespace {
constexpr int FFT_N = 256;

// Simple DFT for 64 output bins from 256 input samples (not full FFT, adequate for spectrum).
void compute_spectrum(const float* samples, int count,
                       wavsen::audio::AudioSpectrum& out) {
    out.clear();
    int usable = (std::min)(count, FFT_N);
    for (int bin = 0; bin < 64; ++bin) {
        float real = 0, imag = 0;
        float freq = 2.0f * 3.14159265f * float(bin + 1) / float(FFT_N);
        for (int i = 0; i < usable; ++i) {
            float window = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * i / (usable - 1)));
            real += samples[i] * window * std::cos(freq * i);
            imag -= samples[i] * window * std::sin(freq * i);
        }
        float mag = std::sqrt(real * real + imag * imag) / float(usable);
        out.average[bin] = mag;
    }
    out.bins     = out.average;
    out.left     = out.average;
    out.right    = out.average;
    out.publish_ms = 1; // non-zero = ready
}
} // namespace

// ---------------------------------------------------------------------------
// AudioCapture::Impl
// ---------------------------------------------------------------------------
class AudioCapture::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (m_inited) return true;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr)) return false;

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) return false;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, (void**)&m_audio_client);
        device->Release();
        if (FAILED(hr)) return false;

        WAVEFORMATEX* pwfx = nullptr;
        hr = m_audio_client->GetMixFormat(&pwfx);
        if (FAILED(hr)) return false;

        m_channels    = pwfx->nChannels;
        m_sample_rate = pwfx->nSamplesPerSec;

        // Loopback mode: capture system audio output.
        hr = m_audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            0, 0, pwfx, nullptr);
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) return false;

        UINT32 buffer_frames = 0;
        m_audio_client->GetBufferSize(&buffer_frames);
        m_buffer_frames = buffer_frames;

        hr = m_audio_client->GetService(__uuidof(IAudioCaptureClient),
            (void**)&m_capture_client);
        if (FAILED(hr)) return false;

        m_inited = true;
        return true;
    }

    void uninit() {
        m_running = false;
        if (m_pump_thread.joinable()) m_pump_thread.join();
        if (m_capture_client) { m_capture_client->Release(); m_capture_client = nullptr; }
        if (m_audio_client)   { m_audio_client->Release();   m_audio_client   = nullptr; }
        m_inited = false;
    }

    bool is_inited() const { return m_inited; }

    bool snapshot(wavsen::audio::AudioSpectrum& out) const {
        std::lock_guard lock(m_spectrum_mutex);
        if (m_spectrum.publish_ms == 0) {
            out.clear();
            return false;
        }
        out = m_spectrum;
        return true;
    }

    // Start the capture thread (called after init).
    void start_capture() {
        if (m_running || !m_inited) return;
        m_running = true;
        m_audio_client->Start();
        m_pump_thread = std::thread(&Impl::capture_loop, this);
    }

private:
    void capture_loop() {
        std::vector<float> accum;
        while (m_running) {
            Sleep(16); // ~60 fps spectrum update
            UINT32 packet_length = 0;
            HRESULT hr = m_capture_client->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) continue;

            while (packet_length > 0) {
                BYTE*   data    = nullptr;
                UINT32  frames  = 0;
                DWORD   flags   = 0;
                hr = m_capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    float* fdata = reinterpret_cast<float*>(data);
                    // Downmix to mono for spectrum analysis.
                    for (UINT32 f = 0; f < frames; ++f) {
                        float mono = 0;
                        for (UINT32 c = 0; c < m_channels; ++c)
                            mono += fdata[f * m_channels + c];
                        mono /= float(m_channels);
                        accum.push_back(mono);
                    }
                }
                m_capture_client->ReleaseBuffer(frames);
                hr = m_capture_client->GetNextPacketSize(&packet_length);
            }

            // Compute spectrum when enough samples accumulated.
            if (accum.size() >= FFT_N) {
                wavsen::audio::AudioSpectrum spec;
                compute_spectrum(accum.data(), (int)accum.size(), spec);
                {
                    std::lock_guard lock(m_spectrum_mutex);
                    m_spectrum = spec;
                }
                accum.clear();
            }
        }
    }

    IAudioClient*        m_audio_client   = nullptr;
    IAudioCaptureClient* m_capture_client = nullptr;
    UINT32               m_buffer_frames  = 0;
    UINT32               m_channels       = 0;
    UINT32               m_sample_rate    = 0;

    mutable std::mutex         m_spectrum_mutex;
    wavsen::audio::AudioSpectrum m_spectrum;

    std::thread       m_pump_thread;
    std::atomic<bool> m_inited{false};
    std::atomic<bool> m_running{false};
};

// --- Public class forwarding ---
#include <memory>
AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;
bool AudioCapture::init()    { return impl_->init(); }
void AudioCapture::uninit()  { impl_->uninit(); }
bool AudioCapture::is_inited() const { return impl_->is_inited(); }
bool AudioCapture::snapshot(wavsen::audio::AudioSpectrum& out) const {
    return impl_->snapshot(out);
}

#endif // WAVSEN_AUDIO_BACKEND_WASAPI
