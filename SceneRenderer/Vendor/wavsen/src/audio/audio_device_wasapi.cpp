// audio_device_wasapi.cpp — WASAPI shared-mode output backend.
// Implements wavsen::audio::AudioDevice::Impl via IAudioRenderClient.
#ifdef WAVSEN_AUDIO_BACKEND_WASAPI

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
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

// Forward-declare wavsen types (resolved by module BMI at link time).
namespace wavsen::audio {
struct DeviceDesc { std::uint32_t channels; std::uint32_t sample_rate; };
class  IPullChannel {
public:
    virtual ~IPullChannel() = default;
    virtual auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t = 0;
    virtual void pass_desc(const DeviceDesc&) = 0;
};
class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    bool init();
    void uninit();
    bool is_inited() const;
    void start();
    void stop();
    void mount(std::unique_ptr<IPullChannel>);
    void unmount_all();
    float volume() const;
    bool  muted() const;
    void  set_volume(float v);
    void  set_muted(bool m);
    float volume_scale() const;
    void  set_volume_scale(float v);
    void  set_volume_scale(float v, std::uint32_t fade_ms);
    DeviceDesc desc() const;
    std::uint64_t stream_position_frames() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

// ---------------------------------------------------------------------------
// AudioDevice::Impl
// ---------------------------------------------------------------------------
class AudioDevice::Impl {
public:
    Impl()  = default;
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

        // Shared mode, event-driven.
        WAVEFORMATEX* pwfx = nullptr;
        hr = m_audio_client->GetMixFormat(&pwfx);
        if (FAILED(hr)) return false;

        m_desc.channels   = pwfx->nChannels;
        m_desc.sample_rate = pwfx->nSamplesPerSec;
        m_bytes_per_frame = pwfx->nBlockAlign;

        hr = m_audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            20 * 10000, // 20ms buffer in 100ns units
            0, pwfx, nullptr);
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) return false;

        UINT32 buffer_frames = 0;
        m_audio_client->GetBufferSize(&buffer_frames);
        m_buffer_frames = buffer_frames;

        m_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_audio_client->SetEventHandle(m_wake_event);

        m_inited = true;
        return true;
    }

    void uninit() {
        stop();
        if (m_audio_client) { m_audio_client->Release(); m_audio_client = nullptr; }
        if (m_wake_event)   { CloseHandle(m_wake_event); m_wake_event = nullptr; }
        m_inited = false;
    }

    bool is_inited() const { return m_inited; }

    void start() {
        if (m_running || !m_inited) return;
        m_running = true;
        m_position_frames = 0;
        m_audio_client->Start();
        m_pump_thread = std::thread(&Impl::pump_loop, this);
    }

    void stop() {
        m_running = false;
        if (m_pump_thread.joinable()) m_pump_thread.join();
        if (m_audio_client && m_inited) m_audio_client->Stop();
    }

    void mount(std::unique_ptr<wavsen::audio::IPullChannel> ch) {
        if (!ch) return;
        if (m_inited) ch->pass_desc(m_desc);
        std::lock_guard lock(m_ch_mutex);
        m_channels.push_back(std::move(ch));
    }

    void unmount_all() {
        std::lock_guard lock(m_ch_mutex);
        m_channels.clear();
    }

    float volume() const { return m_volume.load(); }
    bool  muted()  const { return m_muted.load(); }
    void  set_volume(float v) { m_volume.store(std::clamp(v, 0.0f, 1.0f)); }
    void  set_muted(bool m)   { m_muted.store(m); }

    float volume_scale() const { return m_volume_scale.load(); }
    void  set_volume_scale(float v) { m_volume_scale.store(std::clamp(v, 0.0f, 1.0f)); }
    void  set_volume_scale(float v, std::uint32_t fade_ms) {
        // Simplified: no fade for initial WASAPI impl.
        set_volume_scale(v);
    }

    wavsen::audio::DeviceDesc desc() const { return m_desc; }

    std::uint64_t stream_position_frames() const {
        return m_position_frames.load();
    }

private:
    void pump_loop() {
        std::vector<float> buffer(m_buffer_frames * m_desc.channels);
        while (m_running) {
            DWORD wait = WaitForSingleObject(m_wake_event, 200);
            if (wait != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            m_audio_client->GetCurrentPadding(&padding);
            UINT32 avail = m_buffer_frames - padding;
            if (avail == 0) continue;

            // Mix from all mounted channels.
            std::memset(buffer.data(), 0, avail * m_desc.channels * sizeof(float));
            {
                std::lock_guard lock(m_ch_mutex);
                for (auto it = m_channels.begin(); it != m_channels.end(); ) {
                    if (*it) {
                        (*it)->next_pcm(buffer.data(), avail);
                        ++it;
                    } else {
                        it = m_channels.erase(it);
                    }
                }
            }

            // Apply volume + mute.
            float vol = m_muted.load() ? 0.0f
                     : m_volume.load() * m_volume_scale.load();
            if (vol < 1.0f) {
                for (UINT32 i = 0; i < avail * m_desc.channels; ++i)
                    buffer[i] *= vol;
            }

            // Write to WASAPI buffer.
            BYTE* dst = nullptr;
            HRESULT hr = m_audio_client->GetBuffer(avail, &dst);
            if (SUCCEEDED(hr)) {
                std::memcpy(dst, buffer.data(),
                    avail * m_bytes_per_frame);
                m_audio_client->ReleaseBuffer(avail, 0);
                m_position_frames += avail;
            }
        }
    }

    IAudioClient* m_audio_client = nullptr;
    HANDLE        m_wake_event   = nullptr;
    UINT32        m_buffer_frames = 0;
    UINT32        m_bytes_per_frame = 0;
    wavsen::audio::DeviceDesc m_desc {};

    std::vector<std::unique_ptr<wavsen::audio::IPullChannel>> m_channels;
    std::mutex      m_ch_mutex;
    std::thread     m_pump_thread;
    std::atomic<bool> m_inited{false};
    std::atomic<bool> m_running{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_volume_scale{1.0f};
    std::atomic<bool>  m_muted{false};
    std::atomic<std::uint64_t> m_position_frames{0};
};

// --- Public class forwarding (identical to stub) ---
#include <memory>
AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() = default;
bool AudioDevice::init()    { return impl_->init(); }
void AudioDevice::uninit()  { impl_->uninit(); }
bool AudioDevice::is_inited() const { return impl_->is_inited(); }
void AudioDevice::start()   { impl_->start(); }
void AudioDevice::stop()    { impl_->stop(); }
void AudioDevice::mount(std::unique_ptr<wavsen::audio::IPullChannel> ch) { impl_->mount(std::move(ch)); }
void AudioDevice::unmount_all() { impl_->unmount_all(); }
float AudioDevice::volume() const { return impl_->volume(); }
bool  AudioDevice::muted()  const { return impl_->muted(); }
void  AudioDevice::set_volume(float v) { impl_->set_volume(v); }
void  AudioDevice::set_muted(bool m)   { impl_->set_muted(m); }
float AudioDevice::volume_scale() const { return impl_->volume_scale(); }
void  AudioDevice::set_volume_scale(float v) { impl_->set_volume_scale(v); }
void  AudioDevice::set_volume_scale(float v, std::uint32_t ms) { impl_->set_volume_scale(v, ms); }
wavsen::audio::DeviceDesc AudioDevice::desc() const { return impl_->desc(); }
std::uint64_t AudioDevice::stream_position_frames() const { return impl_->stream_position_frames(); }

#endif // WAVSEN_AUDIO_BACKEND_WASAPI
