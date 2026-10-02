module;

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/string.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#include "audio_capture_dsp.hpp"

module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import pipewire;
import :capture;

namespace wavsen::audio {

namespace {

std::once_flag g_pw_init_once_capture;
void ensure_pw_init() {
    std::call_once(g_pw_init_once_capture, [] { pw_init(nullptr, nullptr); });
}

constexpr std::uint32_t kDefaultRate     = 48000;
constexpr std::uint32_t kDefaultChannels = 2;
constexpr std::uint32_t kQuantum         = 1024;

} // namespace

class AudioCapture::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (is_inited()) return true;

        ensure_pw_init();

        loop_ = pw_thread_loop_new("wavsen-capture", nullptr);
        if (! loop_) {
            rstd::log::error("wavsen::audio: capture pw_thread_loop_new failed");
            return false;
        }
        if (pw_thread_loop_start(loop_) < 0) {
            rstd::log::error("wavsen::audio: capture pw_thread_loop_start failed");
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }

        static const ::pw_stream_events stream_events = {
            .version       = PW_VERSION_STREAM_EVENTS,
            .destroy       = nullptr,
            .state_changed = &Impl::on_state_changed,
            .control_info  = nullptr,
            .io_changed    = nullptr,
            .param_changed = nullptr,
            .add_buffer    = nullptr,
            .remove_buffer = nullptr,
            .process       = &Impl::on_process,
            .drained       = nullptr,
            .command       = nullptr,
            .trigger_done  = nullptr,
        };

        pw_thread_loop_lock(loop_);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (! context_) {
            rstd::log::error("wavsen::audio: capture pw_context_new failed");
            pw_thread_loop_unlock(loop_);
            uninit();
            return false;
        }
        core_ = pw_context_connect(context_, nullptr, 0);
        if (! core_) {
            rstd::log::error("wavsen::audio: capture pw_context_connect failed");
            pw_thread_loop_unlock(loop_);
            uninit();
            return false;
        }
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (! registry_) {
            rstd::log::error("wavsen::audio: capture pw_core_get_registry failed");
            pw_thread_loop_unlock(loop_);
            uninit();
            return false;
        }
        static const pw_registry_events registry_events {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global = &Impl::on_global,
            .global_remove = &Impl::on_global_remove,
        };
        if (pw_registry_add_listener(registry_, &registry_listener_,
                                     &registry_events, this) < 0) {
            rstd::log::error("wavsen::audio: capture registry listener failed");
            pw_thread_loop_unlock(loop_);
            uninit();
            return false;
        }

        auto* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE,       "Audio",
            PW_KEY_MEDIA_CATEGORY,   "Capture",
            PW_KEY_MEDIA_ROLE,       "Music",
            PW_KEY_APP_NAME,         "wavsen",
            PW_KEY_NODE_NAME,        "wavsen-capture",
            PW_KEY_NODE_DESCRIPTION, "wavsen audio response capture",
            PW_KEY_STREAM_CAPTURE_SINK, "true",
            nullptr);
        pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", kQuantum, kDefaultRate);

        stream_ = pw_stream_new(core_, "wavsen-capture", props);
        if (! stream_) {
            pw_thread_loop_unlock(loop_);
            rstd::log::error("wavsen::audio: capture pw_stream_new_simple failed");
            uninit();
            return false;
        }
        pw_stream_add_listener(stream_, &stream_listener_, &stream_events, this);

        std::uint8_t   pod_buffer[1024];
        spa_pod_builder b {};
        b.data = pod_buffer;
        b.size = sizeof(pod_buffer);

        spa_audio_info_raw info {};
        info.format   = SPA_AUDIO_FORMAT_F32_LE;
        info.rate     = kDefaultRate;
        info.channels = kDefaultChannels;

        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

        const auto flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS);

        if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags,
                              params, 1) < 0)
        {
            rstd::log::error("wavsen::audio: capture pw_stream_connect failed");
            pw_thread_loop_unlock(loop_);
            uninit();
            return false;
        }
        stream_node_id_ = pw_stream_get_node_id(stream_);

        pw_thread_loop_unlock(loop_);

        rstd::log::info("wavsen::audio: capture inited (monitor sink, "
                        "{} ch @ {} Hz)", kDefaultChannels, kDefaultRate);
        return true;
    }

    void uninit() {
        if (loop_) {
            pw_thread_loop_lock(loop_);
            if (stream_) {
                pw_stream_destroy(stream_);
                stream_ = nullptr;
            }
            if (sink_node_) {
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(sink_node_)));
                sink_node_ = nullptr;
            }
            if (registry_) {
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(registry_)));
                registry_ = nullptr;
            }
            if (core_) {
                if (pw_core_disconnect(core_) < 0) {
                    rstd::log::error("wavsen::audio: capture pw_core_disconnect failed");
                }
                core_ = nullptr;
            }
            if (context_) {
                pw_context_destroy(context_);
                context_ = nullptr;
            }
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        gain_ready_.store(false, std::memory_order_release);
    }

    bool is_inited() const { return loop_ != nullptr && stream_ != nullptr; }

    bool snapshot(AudioSpectrum& out) const {
        if (! gain_ready_.load(std::memory_order_acquire)) {
            out.clear();
            return false;
        }
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

private:
    // Link discovery and stream-node assignment are asynchronous. Keep the
    // exact graph links so either event can bind the sink that owns the
    // monitor ports feeding this capture stream.
    void bind_connected_sink() {
        if (stream_node_id_ == SPA_ID_INVALID || sink_node_ != nullptr) return;
        for (const std::array<std::uint32_t, 3>& link : links_) {
            if (link[1] != stream_node_id_) continue;
            sink_node_id_ = link[2];
            sink_node_ = static_cast<pw_node*>(pw_registry_bind(
                registry_, sink_node_id_, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
            if (! sink_node_) {
                sink_node_id_ = SPA_ID_INVALID;
                return;
            }
            static const pw_node_events node_events {
                .version = PW_VERSION_NODE_EVENTS,
                .info = nullptr,
                .param = &Impl::on_node_param,
            };
            if (pw_node_add_listener(sink_node_, &sink_node_listener_,
                                     &node_events, this) < 0) {
                rstd::log::error("wavsen::audio: monitor node listener failed");
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(sink_node_)));
                sink_node_ = nullptr;
                sink_node_id_ = SPA_ID_INVALID;
                return;
            }
            std::uint32_t ids[] = { SPA_PARAM_Props };
            const int subscribe_result = pw_node_subscribe_params(sink_node_, ids, 1);
            const int enumerate_result =
                pw_node_enum_params(sink_node_, 0, SPA_PARAM_Props, 0, 1, nullptr);
            if (subscribe_result < 0 || enumerate_result < 0) {
                rstd::log::error("wavsen::audio: monitor volume subscription failed");
                pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(sink_node_)));
                sink_node_ = nullptr;
                sink_node_id_ = SPA_ID_INVALID;
            }
            return;
        }
    }

    static void on_global(void* user, std::uint32_t id, std::uint32_t,
                          const char* type, std::uint32_t, const spa_dict* props) {
        auto* self = static_cast<Impl*>(user);
        if (! self->stream_ || ! self->registry_ || ! props ||
            std::strcmp(type, PW_TYPE_INTERFACE_Link) != 0) return;
        const char* input_text = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        const char* output_text = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        if (! input_text || ! output_text) return;
        std::uint32_t input_node = SPA_ID_INVALID;
        std::uint32_t output_node = SPA_ID_INVALID;
        if (! spa_atou32(input_text, &input_node, 10) ||
            ! spa_atou32(output_text, &output_node, 10)) return;
        self->links_.push_back({ id, input_node, output_node });
        self->bind_connected_sink();
    }

    static void on_global_remove(void* user, std::uint32_t id) {
        auto* self = static_cast<Impl*>(user);
        self->links_.erase(
            std::remove_if(self->links_.begin(), self->links_.end(),
                           [id](const std::array<std::uint32_t, 3>& link) {
                               return link[0] == id;
                           }),
            self->links_.end());
        const bool still_linked = std::any_of(
            self->links_.begin(), self->links_.end(),
            [self](const std::array<std::uint32_t, 3>& link) {
                return link[1] == self->stream_node_id_ && link[2] == self->sink_node_id_;
            });
        if (id != self->sink_node_id_ && still_linked) return;
        if (self->sink_node_) {
            pw_proxy_destroy(static_cast<pw_proxy*>(static_cast<void*>(self->sink_node_)));
            self->sink_node_ = nullptr;
        }
        self->sink_node_id_ = SPA_ID_INVALID;
        self->gain_ready_.store(false, std::memory_order_release);
        self->bind_connected_sink();
    }

    static void on_node_param(void* user, int, std::uint32_t id, std::uint32_t,
                              std::uint32_t, const spa_pod* param) {
        auto* self = static_cast<Impl*>(user);
        if (id != SPA_PARAM_Props || ! param) return;
        const spa_pod_prop* mute_prop = spa_pod_find_prop(param, nullptr, SPA_PROP_monitorMute);
        const spa_pod_prop* volume_prop = spa_pod_find_prop(param, nullptr, SPA_PROP_monitorVolumes);
        if (! mute_prop || ! volume_prop) {
            self->gain_ready_.store(false, std::memory_order_release);
            return;
        }
        bool monitor_mute = false;
        std::array<float, 2> values;
        const int mute_result = spa_pod_get_bool(&mute_prop->value, &monitor_mute);
        const std::uint32_t copied = spa_pod_copy_array_full(
            &volume_prop->value, SPA_TYPE_Float, sizeof(float),
            values.data(), values.size());
        if (mute_result < 0 || monitor_mute || copied != values.size() ||
            !(values[0] > 0.0f) || !(values[1] > 0.0f)) {
            self->gain_ready_.store(false, std::memory_order_release);
            return;
        }
        self->left_scale_.store(1.0f / values[0], std::memory_order_release);
        self->right_scale_.store(1.0f / values[1], std::memory_order_release);
        self->gain_ready_.store(true, std::memory_order_release);
    }

    static void on_process(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (! self->stream_) return;

        pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
        if (! b) return;

        auto* sb = b->buffer;
        if (! sb || sb->n_datas == 0 || ! sb->datas[0].data) {
            if (pw_stream_queue_buffer(self->stream_, b) < 0) {
                self->gain_ready_.store(false, std::memory_order_release);
            }
            return;
        }

        auto& d         = sb->datas[0];
        const auto stride = d.chunk->stride > 0
                                ? static_cast<std::uint32_t>(d.chunk->stride)
                                : kDefaultChannels * static_cast<std::uint32_t>(sizeof(float));
        const auto channels = stride / static_cast<std::uint32_t>(sizeof(float));
        const std::uint32_t offset = d.chunk->offset % d.maxsize;
        const std::uint32_t bytes  = std::min(d.chunk->size, d.maxsize - offset);
        const auto* src = reinterpret_cast<const float*>(
            static_cast<const std::uint8_t*>(d.data) + offset);
        const std::uint32_t n_frames = bytes / stride;

        if (self->gain_ready_.load(std::memory_order_acquire)) {
            self->ingest(src, n_frames, channels,
                         self->left_scale_.load(std::memory_order_acquire),
                         self->right_scale_.load(std::memory_order_acquire));
        }

        if (pw_stream_queue_buffer(self->stream_, b) < 0) {
            self->gain_ready_.store(false, std::memory_order_release);
        }
    }

    static void on_state_changed(void* user, ::pw_stream_state /*old*/,
                                 ::pw_stream_state state, const char* error) {
        auto* self = static_cast<Impl*>(user);
        if (self->stream_) {
            self->stream_node_id_ = pw_stream_get_node_id(self->stream_);
            self->bind_connected_sink();
        }
        switch (state) {
        case PW_STREAM_STATE_ERROR:
            rstd::log::error("wavsen::audio: capture stream ERROR{}",
                             error ? std::string(": ") + error : std::string{});
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            rstd::log::debug("wavsen::audio: capture stream UNCONNECTED");
            break;
        case PW_STREAM_STATE_CONNECTING:
            rstd::log::debug("wavsen::audio: capture stream CONNECTING");
            break;
        case PW_STREAM_STATE_PAUSED:
            rstd::log::debug("wavsen::audio: capture stream PAUSED");
            break;
        case PW_STREAM_STATE_STREAMING:
            rstd::log::debug("wavsen::audio: capture stream STREAMING");
            break;
        }
    }

    void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels,
                float left_scale, float right_scale) {
        for (std::uint32_t f = 0; f < n_frames; ++f) {
            const std::uint32_t base  = f * channels;
            const float         left  = channels > 0 ? src[base] * left_scale : 0.f;
            const float         right = channels > 1 ? src[base + 1] * right_scale : left;
            ring_left_[ring_head_]    = left;
            ring_right_[ring_head_]   = right;
            ring_head_                = (ring_head_ + 1) % dsp::kFftSize;
            if (samples_filled_ < dsp::kFftSize) ++samples_filled_;
            ++samples_since_fft_;
        }

        if (samples_filled_ < dsp::kFftSize || samples_since_fft_ < dsp::kHopSize) return;
        samples_since_fft_ = 0;

        std::array<std::complex<float>, dsp::kFftSize> buf_left;
        std::array<std::complex<float>, dsp::kFftSize> buf_right;
        for (std::size_t i = 0; i < dsp::kFftSize; ++i) {
            const std::size_t idx = (ring_head_ + i) % dsp::kFftSize;
            const float       w   = dsp::hann_window(i, dsp::kFftSize);
            buf_left[i]           = std::complex<float>(ring_left_[idx] * w, 0.f);
            buf_right[i]          = std::complex<float>(ring_right_[idx] * w, 0.f);
        }

        dsp::fft_inplace(buf_left.data(), dsp::kFftSize);
        dsp::fft_inplace(buf_right.data(), dsp::kFftSize);

        const auto  raw =
            dsp::analyze_stereo_spectrum(buf_left.data(), buf_right.data(), band_layout_,
                                         dsp::kFftAmplitudeNorm);
        const auto dt_sec = static_cast<float>(dsp::kHopSize) / static_cast<float>(kDefaultRate);
        const auto bands  = dsp::smooth_spectrum(raw, smoothed_, dt_sec);

        AudioSpectrum out {};
        for (std::size_t k = 0; k < dsp::kNumBins; ++k) {
            out.left[k]    = bands.left[k];
            out.right[k]   = bands.right[k];
            out.average[k] = bands.average[k];
            out.bins[k]    = bands.average[k];
        }
        out.publish_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

        seq_.fetch_add(1, std::memory_order_release);
        std::memcpy(&published_, &out, sizeof(AudioSpectrum));
        seq_.fetch_add(1, std::memory_order_release);
    }

    ::pw_thread_loop* loop_   = nullptr;
    ::pw_context* context_ = nullptr;
    ::pw_core* core_ = nullptr;
    ::pw_registry* registry_ = nullptr;
    ::pw_stream*      stream_ = nullptr;
    ::pw_node* sink_node_ = nullptr;
    std::uint32_t stream_node_id_ = SPA_ID_INVALID;
    std::uint32_t sink_node_id_ = SPA_ID_INVALID;
    std::vector<std::array<std::uint32_t, 3>> links_;
    spa_hook registry_listener_ {};
    spa_hook stream_listener_ {};
    spa_hook sink_node_listener_ {};
    std::atomic<float> left_scale_ { 1.0f };
    std::atomic<float> right_scale_ { 1.0f };
    std::atomic<bool> gain_ready_ { false };

    std::array<float, dsp::kFftSize> ring_left_ {};
    std::array<float, dsp::kFftSize> ring_right_ {};
    std::size_t                      ring_head_         = 0;
    std::size_t                      samples_filled_    = 0;
    std::size_t                      samples_since_fft_ = 0;
    dsp::BandLayout                  band_layout_ { dsp::make_we_layout(kDefaultRate) };
    dsp::SpectrumBands               smoothed_ {};

    mutable std::atomic<std::uint32_t> seq_ { 0 };
    AudioSpectrum                       published_ {};
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;

bool AudioCapture::init(bool enable_system_capture) {
    return ! enable_system_capture || impl_->init();
}
void AudioCapture::uninit()         { impl_->uninit(); }
bool AudioCapture::is_inited() const { return impl_->is_inited(); }
bool AudioCapture::snapshot(AudioSpectrum& out) const { return impl_->snapshot(out); }

} // namespace wavsen::audio
