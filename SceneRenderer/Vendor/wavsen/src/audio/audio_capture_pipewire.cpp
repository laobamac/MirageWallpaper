module;

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <chrono>
#include <cstring>

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

        stream_ = pw_stream_new_simple(
            pw_thread_loop_get_loop(loop_),
            "wavsen-capture",
            props,
            &stream_events,
            this);
        if (! stream_) {
            pw_thread_loop_unlock(loop_);
            rstd::log::error("wavsen::audio: capture pw_stream_new_simple failed");
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }

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
            pw_stream_destroy(stream_);
            stream_ = nullptr;
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }

        pw_thread_loop_unlock(loop_);

        rstd::log::info("wavsen::audio: capture inited (monitor sink, "
                        "{} ch @ {} Hz)", kDefaultChannels, kDefaultRate);
        return true;
    }

    void uninit() {
        if (stream_) {
            pw_thread_loop_lock(loop_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
            pw_thread_loop_unlock(loop_);
        }
        if (loop_) {
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
    }

    bool is_inited() const { return loop_ != nullptr && stream_ != nullptr; }

    bool snapshot(AudioSpectrum& out) const {
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
    static void on_process(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (! self->stream_) return;

        pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
        if (! b) return;

        auto* sb = b->buffer;
        if (! sb || sb->n_datas == 0 || ! sb->datas[0].data) {
            pw_stream_queue_buffer(self->stream_, b);
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

        self->ingest(src, n_frames, channels);

        pw_stream_queue_buffer(self->stream_, b);
    }

    static void on_state_changed(void* /*user*/, ::pw_stream_state /*old*/,
                                 ::pw_stream_state state, const char* error) {
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

    void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels) {
        for (std::uint32_t f = 0; f < n_frames; ++f) {
            const std::uint32_t base  = f * channels;
            const float         left  = channels > 0 ? src[base] : 0.f;
            const float         right = channels > 1 ? src[base + 1] : left;
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

        const float norm = 2.0f / static_cast<float>(dsp::kFftSize);
        const auto  raw =
            dsp::analyze_stereo_spectrum(buf_left.data(), buf_right.data(), band_layout_, norm);
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
    ::pw_stream*      stream_ = nullptr;

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
