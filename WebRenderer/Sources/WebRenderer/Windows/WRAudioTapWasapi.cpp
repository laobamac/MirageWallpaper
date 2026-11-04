#include "WRAudioTap.h"
#include "WRAudioSpectrum.h"

#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

class WRAudioTap::Impl {
public:
    WRAudioSpectrum spectrum;
    std::atomic<bool> running{false};
    HANDLE stopEvent = nullptr;
    std::thread worker;

    HRESULT capture(std::promise<HRESULT>& ready) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> captureClient;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint);
        if (SUCCEEDED(result)) result = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                                          nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec = 48000 * 8;
        if (SUCCEEDED(result)) result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &format, nullptr);
        if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&captureClient));
        if (SUCCEEDED(result)) result = client->Start();
        running.store(SUCCEEDED(result));
        ready.set_value(result);
        if (FAILED(result)) return result;
        while (WaitForSingleObject(stopEvent, 10) == WAIT_TIMEOUT) {
            UINT32 available = 0;
            result = captureClient->GetNextPacketSize(&available);
            while (SUCCEEDED(result) && available > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                result = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(result)) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::vector<float> silence(static_cast<std::size_t>(frames) * 2, 0.0f);
                    spectrum.append(silence.data(), frames, 2, 1.0f, 1.0f);
                } else {
                    spectrum.append(reinterpret_cast<const float*>(data), frames, 2, 1.0f, 1.0f);
                }
                result = captureClient->ReleaseBuffer(frames);
                if (SUCCEEDED(result)) result = captureClient->GetNextPacketSize(&available);
            }
            if (FAILED(result)) break;
        }
        client->Stop();
        running.store(false);
        return result;
    }
};

WRAudioTap::WRAudioTap() : m_impl(std::make_unique<Impl>()) {}
WRAudioTap::~WRAudioTap() { stop(); }

bool WRAudioTap::start(QString* error) {
    stop();
    m_impl->spectrum.clear();
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->stopEvent) {
        if (error) *error = QStringLiteral("WASAPI cannot create capture cancellation event: %1").arg(GetLastError());
        return false;
    }
    std::promise<HRESULT> ready;
    auto started = ready.get_future();
    m_impl->worker = std::thread([this, ready = std::move(ready)]() mutable {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) { ready.set_value(initialized); return; }
        m_impl->capture(ready);
        CoUninitialize();
    });
    const HRESULT result = started.get();
    if (FAILED(result)) {
        if (error) *error = QStringLiteral("WASAPI loopback initialization failed: 0x%1")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        stop();
        return false;
    }
    return true;
}

void WRAudioTap::stop() {
    if (m_impl->stopEvent) SetEvent(m_impl->stopEvent);
    if (m_impl->worker.joinable()) m_impl->worker.join();
    if (m_impl->stopEvent) CloseHandle(m_impl->stopEvent);
    m_impl->stopEvent = nullptr;
    m_impl->running.store(false);
}

bool WRAudioTap::isRunning() const { return m_impl->running.load(); }

bool WRAudioTap::copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const {
    return isRunning() && m_impl->spectrum.copy(left, right);
}
