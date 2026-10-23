#include "WRAudioTap.h"

#include "WRAudioSpectrum.h"

#include <pulse/pulseaudio.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kQuantumFrames = 1024;

QString ErrorString(const char* action, const pa_context* context) {
    return QStringLiteral("%1: %2")
        .arg(QString::fromLatin1(action), QString::fromLocal8Bit(pa_strerror(pa_context_errno(context))));
}

} // namespace

class WRAudioTap::Impl final {
public:
    ~Impl() { stop(); }

    bool start(QString* error) {
        if (isRunning()) return true;

        m_loop = pa_threaded_mainloop_new();
        if (m_loop == nullptr) {
            *error = QStringLiteral("PulseAudio main loop creation failed");
            return false;
        }
        if (pa_threaded_mainloop_start(m_loop) < 0) {
            *error = QStringLiteral("PulseAudio main loop start failed");
            destroyLoop();
            return false;
        }

        pa_threaded_mainloop_lock(m_loop);
        m_context = pa_context_new(pa_threaded_mainloop_get_api(m_loop), "Mirage WebRenderer");
        if (m_context == nullptr) {
            *error = QStringLiteral("PulseAudio context creation failed");
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_context_set_state_callback(m_context, &Impl::onContextState, this);
        if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            *error = ErrorString("PulseAudio context connection failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        while (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
            if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context))) {
                *error = ErrorString("PulseAudio context connection failed", m_context);
                pa_threaded_mainloop_unlock(m_loop);
                stop();
                return false;
            }
            pa_threaded_mainloop_wait(m_loop);
        }

        m_serverInfoReady = false;
        pa_operation* const serverInfoOperation =
            pa_context_get_server_info(m_context, &Impl::onServerInfo, this);
        if (serverInfoOperation == nullptr) {
            *error = ErrorString("PulseAudio server-info request failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_operation_unref(serverInfoOperation);
        while (!m_serverInfoReady) pa_threaded_mainloop_wait(m_loop);
        if (m_defaultSink.empty()) {
            *error = QStringLiteral("PulseAudio did not provide a default output sink");
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }

        pa_sample_spec sampleSpec {};
        sampleSpec.format = PA_SAMPLE_FLOAT32LE;
        sampleSpec.rate = kSampleRate;
        sampleSpec.channels = static_cast<std::uint8_t>(kChannels);
        pa_channel_map channelMap {};
        pa_channel_map_init_stereo(&channelMap);
        m_stream = pa_stream_new(m_context, "Mirage WebRenderer spectrum", &sampleSpec, &channelMap);
        if (m_stream == nullptr) {
            *error = ErrorString("PulseAudio stream creation failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_stream_set_state_callback(m_stream, &Impl::onStreamState, this);
        pa_stream_set_read_callback(m_stream, &Impl::onRead, this);

        constexpr std::uint32_t kFrameBytes = kChannels * static_cast<std::uint32_t>(sizeof(float));
        pa_buffer_attr attributes {};
        attributes.maxlength = static_cast<std::uint32_t>(-1);
        attributes.tlength = static_cast<std::uint32_t>(-1);
        attributes.prebuf = static_cast<std::uint32_t>(-1);
        attributes.minreq = static_cast<std::uint32_t>(-1);
        attributes.fragsize = kQuantumFrames * kFrameBytes;
        // Pulse applies the monitor source's independent recording volume to
        // captured samples. Read that explicit linear gain so the visual
        // response represents the system mix rather than a persisted source
        // slider; the actual sink playback volume is never modified.
        m_monitorSource = m_defaultSink + ".monitor";
        m_sourceInfoReady = false;
        pa_operation* const sourceInfoOperation = pa_context_get_source_info_by_name(
            m_context, m_monitorSource.c_str(), &Impl::onSourceInfo, this);
        if (sourceInfoOperation == nullptr) {
            *error = ErrorString("PulseAudio monitor-volume request failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_operation_unref(sourceInfoOperation);
        while (!m_sourceInfoReady) pa_threaded_mainloop_wait(m_loop);
        pa_context_set_subscribe_callback(m_context, &Impl::onSubscribe, this);
        pa_operation* const subscribeOperation = pa_context_subscribe(
            m_context, PA_SUBSCRIPTION_MASK_SOURCE, nullptr, nullptr);
        if (subscribeOperation == nullptr) {
            *error = ErrorString("PulseAudio source subscription failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_operation_unref(subscribeOperation);
        if (pa_stream_connect_record(m_stream, m_monitorSource.c_str(), &attributes,
                                     PA_STREAM_ADJUST_LATENCY) < 0) {
            *error = ErrorString("PulseAudio monitor connection failed", m_context);
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        while (pa_stream_get_state(m_stream) != PA_STREAM_READY) {
            if (!PA_STREAM_IS_GOOD(pa_stream_get_state(m_stream))) {
                *error = ErrorString("PulseAudio monitor connection failed", m_context);
                pa_threaded_mainloop_unlock(m_loop);
                stop();
                return false;
            }
            pa_threaded_mainloop_wait(m_loop);
        }
        pa_threaded_mainloop_unlock(m_loop);
        return true;
    }

    void stop() {
        if (m_loop == nullptr) return;
        pa_threaded_mainloop_lock(m_loop);
        if (m_stream != nullptr) {
            pa_stream_set_state_callback(m_stream, nullptr, nullptr);
            pa_stream_set_read_callback(m_stream, nullptr, nullptr);
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        if (m_context != nullptr) {
            pa_context_set_state_callback(m_context, nullptr, nullptr);
            pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        pa_threaded_mainloop_unlock(m_loop);
        destroyLoop();
        m_spectrum.clear();
        m_gainReady.store(false, std::memory_order_release);
    }

    bool isRunning() const { return m_loop != nullptr && m_stream != nullptr; }

    bool copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const {
        return m_spectrum.copy(left, right);
    }

private:
    void destroyLoop() {
        pa_threaded_mainloop_stop(m_loop);
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
    }

    static void onContextState(pa_context*, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        pa_threaded_mainloop_signal(impl->m_loop, 0);
    }

    static void onStreamState(pa_stream*, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        pa_threaded_mainloop_signal(impl->m_loop, 0);
    }

    static void onServerInfo(pa_context*, const pa_server_info* info, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        if (info->default_sink_name != nullptr) impl->m_defaultSink = info->default_sink_name;
        impl->m_serverInfoReady = true;
        pa_threaded_mainloop_signal(impl->m_loop, 0);
    }

    static void onSourceInfo(pa_context*, const pa_source_info* info, int eol, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        if (info != nullptr && info->name != nullptr &&
            impl->m_monitorSource == info->name && info->volume.channels >= kChannels) {
            const double leftVolume = pa_sw_volume_to_linear(info->volume.values[0]);
            const double rightVolume = pa_sw_volume_to_linear(info->volume.values[1]);
            if (info->mute == 0 && leftVolume > 0.0 && rightVolume > 0.0) {
                impl->m_leftScale.store(static_cast<float>(1.0 / leftVolume),
                                        std::memory_order_release);
                impl->m_rightScale.store(static_cast<float>(1.0 / rightVolume),
                                         std::memory_order_release);
                impl->m_gainReady.store(true, std::memory_order_release);
            } else {
                impl->m_gainReady.store(false, std::memory_order_release);
                impl->m_spectrum.clear();
            }
        }
        if (eol != 0) {
            impl->m_sourceInfoReady = true;
            pa_threaded_mainloop_signal(impl->m_loop, 0);
        }
    }

    static void onSubscribe(pa_context* context, pa_subscription_event_type_t event,
                            std::uint32_t index, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SOURCE) return;
        pa_operation* const operation = pa_context_get_source_info_by_index(
            context, index, &Impl::onSourceInfo, impl);
        if (operation == nullptr) {
            impl->m_gainReady.store(false, std::memory_order_release);
            impl->m_spectrum.clear();
            return;
        }
        pa_operation_unref(operation);
    }

    static void onRead(pa_stream* stream, std::size_t, void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        while (pa_stream_readable_size(stream) > 0) {
            const void* data = nullptr;
            std::size_t byteCount = 0;
            if (pa_stream_peek(stream, &data, &byteCount) < 0) return;
            if (byteCount == 0) return;
            if (data != nullptr) {
                constexpr std::uint32_t kFrameBytes = kChannels * static_cast<std::uint32_t>(sizeof(float));
                const float* const samples = static_cast<const float*>(data);
                if (impl->m_gainReady.load(std::memory_order_acquire)) {
                    impl->m_spectrum.append(
                        samples, static_cast<std::uint32_t>(byteCount / kFrameBytes), kChannels,
                        impl->m_leftScale.load(std::memory_order_acquire),
                        impl->m_rightScale.load(std::memory_order_acquire));
                }
            }
            pa_stream_drop(stream);
        }
    }

    pa_threaded_mainloop* m_loop = nullptr;
    pa_context* m_context = nullptr;
    pa_stream* m_stream = nullptr;
    std::string m_defaultSink;
    std::string m_monitorSource;
    bool m_serverInfoReady = false;
    bool m_sourceInfoReady = false;
    std::atomic<float> m_leftScale { 1.0f };
    std::atomic<float> m_rightScale { 1.0f };
    std::atomic<bool> m_gainReady { false };

    mutable WRAudioSpectrum m_spectrum;
};

WRAudioTap::WRAudioTap() : m_impl(std::make_unique<Impl>()) {}
WRAudioTap::~WRAudioTap() = default;
bool WRAudioTap::start(QString* error) { return m_impl->start(error); }
void WRAudioTap::stop() { m_impl->stop(); }
bool WRAudioTap::isRunning() const { return m_impl->isRunning(); }
bool WRAudioTap::copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const {
    return m_impl->copySpectrum(left, right);
}
