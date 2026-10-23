#include "WRAudioTap.h"

#include "WRAudioSpectrum.h"

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/string.h>

#include <QDebug>

#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kQuantumFrames = 1024;
}

class WRAudioTap::Impl final {
public:
    ~Impl() { stop(); }

    bool start(QString* error) {
        if (isRunning()) return true;
        static std::once_flag initOnce;
        std::call_once(initOnce, [] { pw_init(nullptr, nullptr); });
        m_loop = pw_thread_loop_new("mirage-web-renderer-audio", nullptr);
        if (m_loop == nullptr) {
            *error = QStringLiteral("PipeWire thread loop creation failed");
            return false;
        }
        if (pw_thread_loop_start(m_loop) < 0) {
            *error = QStringLiteral("PipeWire thread loop start failed");
            destroyLoop();
            return false;
        }
        static const pw_stream_events events {
            .version = PW_VERSION_STREAM_EVENTS,
            .destroy = nullptr,
            .state_changed = &Impl::onStateChanged,
            .control_info = nullptr,
            .io_changed = nullptr,
            .param_changed = nullptr,
            .add_buffer = nullptr,
            .remove_buffer = nullptr,
            .process = &Impl::onProcess,
            .drained = nullptr,
            .command = nullptr,
            .trigger_done = nullptr,
        };
        pw_thread_loop_lock(m_loop);
        m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
        if (m_context == nullptr) {
            *error = QStringLiteral("PipeWire context creation failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        m_core = pw_context_connect(m_context, nullptr, 0);
        if (m_core == nullptr) {
            *error = QStringLiteral("PipeWire core connection failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
        if (m_registry == nullptr) {
            *error = QStringLiteral("PipeWire registry creation failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        static const pw_registry_events registryEvents {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global = &Impl::onGlobal,
            .global_remove = &Impl::onGlobalRemove,
        };
        if (pw_registry_add_listener(m_registry, &m_registryListener,
                                     &registryEvents, this) < 0) {
            *error = QStringLiteral("PipeWire registry listener creation failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }

        pw_properties* const properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "Mirage WebRenderer",
            PW_KEY_NODE_NAME, "mirage-web-renderer-audio",
            PW_KEY_NODE_DESCRIPTION, "Mirage WebRenderer system output audio response",
            // PipeWire otherwise autoconnects this input stream to the default
            // microphone/source. This property selects the monitor of the
            // default playback sink, matching wavsen's system-output capture.
            PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr);
        if (properties == nullptr) {
            *error = QStringLiteral("PipeWire stream properties creation failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        pw_properties_setf(properties, PW_KEY_NODE_LATENCY, "%u/%u", kQuantumFrames, kSampleRate);
        m_stream = pw_stream_new(m_core, "Mirage WebRenderer spectrum", properties);
        if (m_stream == nullptr) {
            *error = QStringLiteral("PipeWire stream creation failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        pw_stream_add_listener(m_stream, &m_streamListener, &events, this);
        std::array<std::uint8_t, 1024> podBuffer {};
        spa_pod_builder builder {};
        builder.data = podBuffer.data();
        builder.size = podBuffer.size();
        spa_audio_info_raw info {};
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        info.rate = kSampleRate;
        info.channels = kChannels;
        const spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
        const pw_stream_flags flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
        if (pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
            *error = QStringLiteral("PipeWire monitor connection failed");
            pw_thread_loop_unlock(m_loop);
            stop();
            return false;
        }
        m_streamNodeId = pw_stream_get_node_id(m_stream);
        pw_thread_loop_unlock(m_loop);
        return true;
    }

    void stop() {
        if (m_loop == nullptr) return;
        pw_thread_loop_lock(m_loop);
        if (m_stream != nullptr) {
            pw_stream_destroy(m_stream);
            m_stream = nullptr;
        }
        if (m_sinkNode != nullptr) {
            pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(m_sinkNode)));
            m_sinkNode = nullptr;
        }
        if (m_registry != nullptr) {
            pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(m_registry)));
            m_registry = nullptr;
        }
        if (m_core != nullptr) {
            if (pw_core_disconnect(m_core) < 0) {
                qWarning("WebRenderer: PipeWire core disconnect failed");
            }
            m_core = nullptr;
        }
        if (m_context != nullptr) {
            pw_context_destroy(m_context);
            m_context = nullptr;
        }
        pw_thread_loop_unlock(m_loop);
        destroyLoop();
        m_spectrum.clear();
        m_gainReady.store(false, std::memory_order_release);
    }

    bool isRunning() const { return m_loop != nullptr && m_stream != nullptr; }
    bool copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const {
        return m_spectrum.copy(left, right);
    }

private:
    // Registry link events and stream state events have no ordering guarantee.
    // Retain the known link IDs so either event can complete the binding to
    // the exact sink node that feeds this capture stream.
    void bindConnectedSink() {
        if (m_streamNodeId == SPA_ID_INVALID || m_sinkNode != nullptr) return;
        for (const std::array<std::uint32_t, 3>& link : m_links) {
            if (link[1] != m_streamNodeId) continue;
            m_sinkNodeId = link[2];
            m_sinkNode = static_cast<pw_node*>(pw_registry_bind(
                m_registry, m_sinkNodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
            if (m_sinkNode == nullptr) {
                m_sinkNodeId = SPA_ID_INVALID;
                return;
            }
            static const pw_node_events nodeEvents {
                .version = PW_VERSION_NODE_EVENTS,
                .info = nullptr,
                .param = &Impl::onNodeParam,
            };
            if (pw_node_add_listener(m_sinkNode, &m_sinkNodeListener,
                                     &nodeEvents, this) < 0) {
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(m_sinkNode)));
                m_sinkNode = nullptr;
                m_sinkNodeId = SPA_ID_INVALID;
                return;
            }
            std::uint32_t ids[] = { SPA_PARAM_Props };
            const int subscribeResult = pw_node_subscribe_params(m_sinkNode, ids, 1);
            const int enumerateResult =
                pw_node_enum_params(m_sinkNode, 0, SPA_PARAM_Props, 0, 1, nullptr);
            if (subscribeResult < 0 || enumerateResult < 0) {
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(m_sinkNode)));
                m_sinkNode = nullptr;
                m_sinkNodeId = SPA_ID_INVALID;
            }
            return;
        }
    }

    static void onGlobal(void* user, std::uint32_t id, std::uint32_t,
                         const char* type, std::uint32_t, const spa_dict* props) {
        Impl* const impl = static_cast<Impl*>(user);
        if (impl->m_stream == nullptr || impl->m_registry == nullptr ||
            std::strcmp(type, PW_TYPE_INTERFACE_Link) != 0 || props == nullptr) {
            return;
        }
        const char* const inputText = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        const char* const outputText = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        if (inputText == nullptr || outputText == nullptr) return;
        std::uint32_t inputNode = SPA_ID_INVALID;
        std::uint32_t outputNode = SPA_ID_INVALID;
        if (!spa_atou32(inputText, &inputNode, 10) ||
            !spa_atou32(outputText, &outputNode, 10)) {
            return;
        }
        impl->m_links.push_back({id, inputNode, outputNode});
        impl->bindConnectedSink();
    }

    static void onGlobalRemove(void* user, std::uint32_t id) {
        Impl* const impl = static_cast<Impl*>(user);
        impl->m_links.erase(
            std::remove_if(impl->m_links.begin(), impl->m_links.end(),
                           [id](const std::array<std::uint32_t, 3>& link) {
                               return link[0] == id;
                           }),
            impl->m_links.end());
        const bool stillLinked = std::any_of(
            impl->m_links.begin(), impl->m_links.end(),
            [impl](const std::array<std::uint32_t, 3>& link) {
                return link[1] == impl->m_streamNodeId && link[2] == impl->m_sinkNodeId;
            });
        if (id != impl->m_sinkNodeId && stillLinked) return;
        if (impl->m_sinkNode != nullptr) {
            pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(impl->m_sinkNode)));
            impl->m_sinkNode = nullptr;
        }
        impl->m_sinkNodeId = SPA_ID_INVALID;
        impl->m_gainReady.store(false, std::memory_order_release);
        impl->m_spectrum.clear();
        impl->bindConnectedSink();
    }

    static void onNodeParam(void* user, int, std::uint32_t id, std::uint32_t,
                            std::uint32_t, const spa_pod* param) {
        Impl* const impl = static_cast<Impl*>(user);
        if (id != SPA_PARAM_Props || param == nullptr) return;
        const spa_pod_prop* const muteProperty =
            spa_pod_find_prop(param, nullptr, SPA_PROP_monitorMute);
        const spa_pod_prop* const volumeProperty =
            spa_pod_find_prop(param, nullptr, SPA_PROP_monitorVolumes);
        if (muteProperty == nullptr || volumeProperty == nullptr) {
            impl->m_gainReady.store(false, std::memory_order_release);
            impl->m_spectrum.clear();
            return;
        }
        bool monitorMute = false;
        std::array<float, 2> values;
        const int muteResult = spa_pod_get_bool(&muteProperty->value, &monitorMute);
        const std::uint32_t copied = spa_pod_copy_array_full(
            &volumeProperty->value, SPA_TYPE_Float, sizeof(float),
            values.data(), values.size());
        if (muteResult < 0 || monitorMute || copied != values.size()) {
            impl->m_gainReady.store(false, std::memory_order_release);
            impl->m_spectrum.clear();
            return;
        }
        if (!(values[0] > 0.0f) || !(values[1] > 0.0f)) {
            impl->m_gainReady.store(false, std::memory_order_release);
            impl->m_spectrum.clear();
            return;
        }
        impl->m_leftScale.store(1.0f / values[0], std::memory_order_release);
        impl->m_rightScale.store(1.0f / values[1], std::memory_order_release);
        impl->m_gainReady.store(true, std::memory_order_release);
    }

    void destroyLoop() {
        pw_thread_loop_stop(m_loop);
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }

    static void onStateChanged(void* user, pw_stream_state, pw_stream_state state, const char*) {
        // Stream state is intentionally informational. PipeWire's process
        // callback is the authoritative signal that a monitor is producing data.
        if (state == PW_STREAM_STATE_ERROR) return;
        Impl* const impl = static_cast<Impl*>(user);
        impl->m_streamNodeId = pw_stream_get_node_id(impl->m_stream);
        impl->bindConnectedSink();
    }

    static void onProcess(void* user) {
        Impl* const impl = static_cast<Impl*>(user);
        pw_buffer* const buffer = pw_stream_dequeue_buffer(impl->m_stream);
        if (buffer == nullptr || buffer->buffer == nullptr || buffer->buffer->n_datas == 0) return;
        spa_data& data = buffer->buffer->datas[0];
        if (data.data == nullptr || data.chunk == nullptr) {
            if (pw_stream_queue_buffer(impl->m_stream, buffer) < 0) {
                impl->m_gainReady.store(false, std::memory_order_release);
            }
            return;
        }
        const std::uint32_t stride = data.chunk->stride > 0
            ? static_cast<std::uint32_t>(data.chunk->stride)
            : kChannels * static_cast<std::uint32_t>(sizeof(float));
        const std::uint32_t offset = data.chunk->offset % data.maxsize;
        const std::uint32_t bytes = std::min(data.chunk->size, data.maxsize - offset);
        const std::byte* const raw = static_cast<const std::byte*>(data.data);
        const float* const samples = static_cast<const float*>(static_cast<const void*>(raw + offset));
        if (impl->m_gainReady.load(std::memory_order_acquire)) {
            impl->m_spectrum.append(samples, bytes / stride,
                                    stride / static_cast<std::uint32_t>(sizeof(float)),
                                    impl->m_leftScale.load(std::memory_order_acquire),
                                    impl->m_rightScale.load(std::memory_order_acquire));
        }
        if (pw_stream_queue_buffer(impl->m_stream, buffer) < 0) {
            impl->m_gainReady.store(false, std::memory_order_release);
        }
    }

    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_registry* m_registry = nullptr;
    pw_stream* m_stream = nullptr;
    pw_node* m_sinkNode = nullptr;
    std::uint32_t m_streamNodeId = SPA_ID_INVALID;
    std::uint32_t m_sinkNodeId = SPA_ID_INVALID;
    std::vector<std::array<std::uint32_t, 3>> m_links;
    spa_hook m_registryListener {};
    spa_hook m_streamListener {};
    spa_hook m_sinkNodeListener {};
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
