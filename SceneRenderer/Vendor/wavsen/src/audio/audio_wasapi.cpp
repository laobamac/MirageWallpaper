#include "audio_coreaudio_output.h"
#include "audio_coreaudio_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Samples = std::function<void(float*, std::uint32_t, std::uint32_t, std::uint32_t)>;

class WasapiSession final : public IMMNotificationClient {
public:
    WasapiSession(bool capture, Samples samples) : capture_(capture), samples_(std::move(samples)) {
        command_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        audio_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~WasapiSession() {
        if (thread_.joinable()) {
            { std::lock_guard lock(mutex_); quit_ = true; }
            SetEvent(command_);
            thread_.join();
        }
        if (audio_) CloseHandle(audio_);
        if (command_) CloseHandle(command_);
    }
    HRESULT initialize() {
        if (!command_ || !audio_) return HRESULT_FROM_WIN32(GetLastError());
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return initialized_; });
        return status_;
    }
    HRESULT setRunning(bool running) {
        std::unique_lock lock(mutex_);
        desired_running_ = running;
        const auto revision = ++revision_;
        SetEvent(command_);
        ready_.wait(lock, [this, revision] { return applied_revision_ >= revision || exited_; });
        return status_;
    }
    std::uint32_t channels() const { return capture_ ? channels_.load() : 2U; }
    std::uint32_t rate() const { return capture_ ? rate_.load() : 48000U; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) { reopen_.store(true); SetEvent(command_); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT open() {
        close();
        ComPtr<IMMDevice> endpoint;
        HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
        if (FAILED(hr)) return hr;
        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr)) return hr;
        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr)) return hr;
        channels_ = mix->nChannels;
        rate_ = mix->nSamplesPerSec;
        bits_ = mix->wBitsPerSample;
        block_align_ = mix->nBlockAlign;
        floating_ = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool pcm = mix->wFormatTag == WAVE_FORMAT_PCM;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            floating_ = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            pcm = ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
        }
        if (channels_ == 0 || rate_ == 0 ||
            (capture_ && !((floating_ && bits_ == 32) ||
                          (pcm && (bits_ == 16 || bits_ == 24 || bits_ == 32))))) {
            CoTaskMemFree(mix);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        WAVEFORMATEX output {};
        output.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        output.nChannels = 2;
        output.nSamplesPerSec = 48000;
        output.wBitsPerSample = 32;
        output.nBlockAlign = 8;
        output.nAvgBytesPerSec = 48000 * 8;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (capture_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        else flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0,
                                  capture_ ? mix : &output, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = client_->SetEventHandle(audio_))) return hr;
        if (FAILED(hr = client_->GetBufferSize(&buffer_frames_))) return hr;
        if (capture_) {
            hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(capture_client_.GetAddressOf()));
        } else {
            hr = client_->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(render_client_.GetAddressOf()));
        }
        scratch_.resize(static_cast<std::size_t>(buffer_frames_) * channels());
        return hr;
    }
    void close() {
        if (client_ && running_) client_->Stop();
        running_ = false;
        capture_client_.Reset();
        render_client_.Reset();
        client_.Reset();
    }
    HRESULT output() {
        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) return hr;
        if (padding >= buffer_frames_) return S_OK;
        const UINT32 frames = buffer_frames_ - padding;
        BYTE* destination = nullptr;
        if (FAILED(hr = render_client_->GetBuffer(frames, &destination))) return hr;
        std::memset(destination, 0, static_cast<std::size_t>(frames) * 8U);
        samples_(reinterpret_cast<float*>(destination), frames, 2U, 48000U);
        return render_client_->ReleaseBuffer(frames, 0);
    }
    HRESULT capture() {
        UINT32 packet = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const auto count = static_cast<std::size_t>(frames) * channels_;
            scratch_.resize(count);
            std::fill(scratch_.begin(), scratch_.end(), 0.0f);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
                        const auto* sample = data + frame * block_align_ + channel * (bits_ / 8U);
                        float value = 0.0f;
                        if (floating_) std::memcpy(&value, sample, sizeof(float));
                        else if (bits_ == 16) {
                            std::int16_t pcm; std::memcpy(&pcm, sample, sizeof(pcm));
                            value = static_cast<float>(pcm) / 32768.0f;
                        } else if (bits_ == 24) {
                            std::int32_t pcm = sample[0] | (sample[1] << 8) | (sample[2] << 16);
                            if (pcm & 0x800000) pcm |= static_cast<std::int32_t>(0xff000000U);
                            value = static_cast<float>(pcm) / 8388608.0f;
                        } else {
                            std::int32_t pcm; std::memcpy(&pcm, sample, sizeof(pcm));
                            value = static_cast<float>(pcm) / 2147483648.0f;
                        }
                        scratch_[frame * channels_ + channel] = value;
                    }
                }
            }
            samples_(scratch_.data(), frames, channels_, rate_);
            hr = capture_client_->ReleaseBuffer(frames);
            if (FAILED(hr)) break;
            hr = capture_client_->GetNextPacketSize(&packet);
        }
        return hr;
    }
    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = com;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator_.GetAddressOf()));
        bool subscribed = false;
        if (SUCCEEDED(hr)) {
            subscribed = SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(this));
            hr = open();
        }
        {
            std::lock_guard lock(mutex_);
            status_ = hr;
            initialized_ = true;
        }
        ready_.notify_all();
        HANDLE events[] { command_, audio_ };
        for (;;) {
            const DWORD wake = WaitForMultipleObjects(2, events, FALSE, 1000);
            std::unique_lock lock(mutex_);
            if (quit_) break;
            if (wake == WAIT_FAILED) { status_ = HRESULT_FROM_WIN32(GetLastError()); break; }
            const bool reopen = reopen_.exchange(false);
            const bool requested = desired_running_;
            const auto revision = revision_;
            lock.unlock();
            if ((reopen || !client_) && enumerator_) hr = open();
            if (SUCCEEDED(hr) && client_ && requested != running_) {
                if (requested) {
                    if (!capture_) hr = output();
                    if (SUCCEEDED(hr)) hr = client_->Start();
                } else {
                    hr = client_->Stop();
                    if (SUCCEEDED(hr)) hr = client_->Reset();
                }
                running_ = requested && SUCCEEDED(hr);
            }
            if (SUCCEEDED(hr) && running_ && wake == WAIT_OBJECT_0 + 1)
                hr = capture_ ? capture() : output();
            if (FAILED(hr)) close();
            lock.lock();
            status_ = hr;
            applied_revision_ = revision;
            lock.unlock();
            ready_.notify_all();
        }
        close();
        if (subscribed) enumerator_->UnregisterEndpointNotificationCallback(this);
        enumerator_.Reset();
        if (SUCCEEDED(com)) CoUninitialize();
        { std::lock_guard lock(mutex_); exited_ = true; }
        ready_.notify_all();
    }

    bool capture_;
    Samples samples_;
    HANDLE command_ { nullptr }, audio_ { nullptr };
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool initialized_ { false }, exited_ { false }, quit_ { false }, desired_running_ { false };
    bool running_ { false };
    HRESULT status_ { E_FAIL };
    std::uint64_t revision_ { 0 }, applied_revision_ { 0 };
    std::atomic<bool> reopen_ { false };
    std::atomic<ULONG> references_ { 1 };
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_client_;
    ComPtr<IAudioCaptureClient> capture_client_;
    UINT32 buffer_frames_ { 0 }, block_align_ { 0 }, bits_ { 0 };
    std::atomic<UINT32> channels_ { 0 }, rate_ { 0 };
    bool floating_ { false };
    std::vector<float> scratch_;
};
} // namespace

struct wavsen_native_output {
    WasapiSession session;
    wavsen_native_output(wavsen_native_output_callback callback, void* user)
        : session(false, [callback, user](float* samples, std::uint32_t frames,
            std::uint32_t channels, std::uint32_t rate) { callback(samples, frames, channels, rate, user); }) {}
};
struct wavsen_native_capture {
    WasapiSession session;
    wavsen_native_capture(wavsen_native_capture_callback callback, void* user)
        : session(true, [callback, user](float* samples, std::uint32_t frames,
            std::uint32_t channels, std::uint32_t rate) { callback(samples, frames, channels, rate, user); }) {}
};

bool wavsen_native_output_create(wavsen_native_output_callback callback, void* user,
    wavsen_native_output** out, int* status, std::uint32_t* channels, std::uint32_t* rate) {
    if (!out || !callback) return false;
    *out = nullptr;
    auto* output = new wavsen_native_output(callback, user);
    const HRESULT hr = output->session.initialize();
    if (status) *status = static_cast<int>(hr);
    if (FAILED(hr)) { delete output; return false; }
    if (channels) *channels = output->session.channels();
    if (rate) *rate = output->session.rate();
    *out = output;
    return true;
}
void wavsen_native_output_destroy(wavsen_native_output* output) { delete output; }
bool wavsen_native_output_start(wavsen_native_output* output, int* status) {
    const HRESULT hr = output ? output->session.setRunning(true) : E_POINTER;
    if (status) *status = static_cast<int>(hr);
    return SUCCEEDED(hr);
}
void wavsen_native_output_stop(wavsen_native_output* output) {
    if (output) output->session.setRunning(false);
}
bool wavsen_native_capture_create(wavsen_native_capture_callback callback, void* user,
    wavsen_native_capture** out, int* status, std::uint32_t* channels, std::uint32_t* rate) {
    if (!out || !callback) return false;
    *out = nullptr;
    auto* capture = new wavsen_native_capture(callback, user);
    HRESULT hr = capture->session.initialize();
    if (SUCCEEDED(hr)) hr = capture->session.setRunning(true);
    if (status) *status = static_cast<int>(hr);
    if (FAILED(hr)) { delete capture; return false; }
    if (channels) *channels = capture->session.channels();
    if (rate) *rate = capture->session.rate();
    *out = capture;
    return true;
}
void wavsen_native_capture_destroy(wavsen_native_capture* capture) { delete capture; }
