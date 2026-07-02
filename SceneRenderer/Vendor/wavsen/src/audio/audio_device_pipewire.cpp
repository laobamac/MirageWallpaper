module;

#include <cmath>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import pipewire;
import :core;

namespace wavsen::audio {

namespace {

std::once_flag g_pw_init_once;
void ensure_pw_init() {
    std::call_once(g_pw_init_once, [] { pw_init(nullptr, nullptr); });
}

constexpr std::uint32_t kDefaultRate     = 48000;
constexpr std::uint32_t kDefaultChannels = 2;
constexpr std::uint32_t kQuantum         = 1024;

float clamp_volume_scale(float v) {
    if (! std::isfinite(v) || v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

} // namespace

class AudioDevice::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (is_inited()) return true;

        ensure_pw_init();

        loop_ = pw_thread_loop_new("wavsen-audio", nullptr);
        if (! loop_) {
            rstd::log::error("wavsen::audio: pw_thread_loop_new failed");
            return false;
        }

        if (pw_thread_loop_start(loop_) < 0) {
            rstd::log::error("wavsen::audio: pw_thread_loop_start failed");
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
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Music",
            PW_KEY_APP_NAME,       "wavsen",
            PW_KEY_NODE_NAME,      "wavsen-out",
            PW_KEY_NODE_DESCRIPTION, "wavsen audio output",
            nullptr);
        pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", kQuantum, kDefaultRate);
        pw_properties_setf(props, PW_KEY_NODE_RATE,    "1/%u",  kDefaultRate);

        stream_ = pw_stream_new_simple(
            pw_thread_loop_get_loop(loop_),
            "wavsen-out",
            props,
            &stream_events,
            this);
        if (! stream_) {
            pw_thread_loop_unlock(loop_);
            rstd::log::error("wavsen::audio: pw_stream_new_simple failed");
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

        if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags,
                              params, 1) < 0)
        {
            rstd::log::error("wavsen::audio: pw_stream_connect failed");
            pw_stream_destroy(stream_);
            stream_ = nullptr;
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }

        pw_thread_loop_unlock(loop_);

        desc_ = { kDefaultChannels, kDefaultRate };

        {
            std::lock_guard<std::mutex> lk(channels_mu_);
            for (auto& c : channels_) {
                c->pass_desc(desc_);
            }
        }

        rstd::log::info("wavsen::audio: pipewire device inited ({} ch @ {} Hz)",
                        desc_.channels, desc_.sample_rate);
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

    void start() {
        if (! stream_) return;
        pw_thread_loop_lock(loop_);
        pw_stream_set_active(stream_, true);
        pw_thread_loop_unlock(loop_);
    }

    void stop() {
        if (! stream_) return;
        pw_thread_loop_lock(loop_);
        pw_stream_set_active(stream_, false);
        pw_thread_loop_unlock(loop_);
    }

    void mount(std::unique_ptr<IPullChannel> ch) {
        if (is_inited()) {
            ch->pass_desc(desc_);
        }
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.push_back(std::move(ch));
    }

    void unmount_all() {
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.clear();
    }

    DeviceDesc desc() const { return desc_; }

    float volume() const { return volume_.load(std::memory_order_relaxed); }
    bool  muted()  const { return muted_.load(std::memory_order_relaxed); }
    void  set_volume(float v) { volume_.store(v, std::memory_order_relaxed); }
    void  set_muted(bool m)   { muted_.store(m,  std::memory_order_relaxed); }
    float volume_scale() const { return volume_scale_.load(std::memory_order_relaxed); }
    void  set_volume_scale(float v) {
        v = clamp_volume_scale(v);
        volume_scale_epoch_.fetch_add(1, std::memory_order_acq_rel);
        volume_scale_target_.store(v, std::memory_order_relaxed);
        volume_scale_step_.store(0.0f, std::memory_order_relaxed);
        volume_scale_frames_left_.store(0, std::memory_order_relaxed);
        volume_scale_.store(v, std::memory_order_relaxed);
        volume_scale_epoch_.fetch_add(1, std::memory_order_release);
    }
    void set_volume_scale(float v, std::uint32_t fade_ms) {
        v = clamp_volume_scale(v);
        if (fade_ms == 0) {
            set_volume_scale(v);
            return;
        }
        const auto rate = desc_.sample_rate != 0 ? desc_.sample_rate : kDefaultRate;
        auto fade_frames64 = static_cast<std::uint64_t>(rate) * fade_ms / 1000ULL;
        if (fade_frames64 == 0) {
            set_volume_scale(v);
            return;
        }
        if (fade_frames64 > 0xffffffffULL) fade_frames64 = 0xffffffffULL;
        const auto fade_frames = static_cast<std::uint32_t>(fade_frames64);
        const auto current     = volume_scale_.load(std::memory_order_relaxed);
        if (current == v) {
            set_volume_scale(v);
            return;
        }

        volume_scale_epoch_.fetch_add(1, std::memory_order_acq_rel);
        volume_scale_target_.store(v, std::memory_order_relaxed);
        volume_scale_step_.store((v - current) / static_cast<float>(fade_frames),
                                 std::memory_order_relaxed);
        volume_scale_frames_left_.store(fade_frames, std::memory_order_relaxed);
        volume_scale_epoch_.fetch_add(1, std::memory_order_release);
    }

    std::uint64_t stream_position_frames() const {
        if (! stream_) return 0;
        pw_time t {};
        if (pw_stream_get_time_n(stream_, &t, sizeof(t)) < 0) return 0;
        if (t.ticks <= static_cast<std::uint64_t>(t.delay)) return 0;
        return t.ticks - static_cast<std::uint64_t>(t.delay);
    }

private:
    void apply_output_gain(float* out_f, std::uint32_t n_frames, std::uint32_t channels) {
        const float volume = volume_.load(std::memory_order_relaxed);
        auto        left   = volume_scale_frames_left_.load(std::memory_order_relaxed);
        if (left == 0) {
            const float gain =
                volume * volume_scale_.load(std::memory_order_relaxed);
            const auto total_samples = static_cast<std::size_t>(n_frames) * channels;
            for (std::size_t i = 0; i < total_samples; ++i) {
                out_f[i] *= gain;
            }
            return;
        }

        const auto  epoch  = volume_scale_epoch_.load(std::memory_order_acquire);
        const float step   = volume_scale_step_.load(std::memory_order_relaxed);
        const float target = volume_scale_target_.load(std::memory_order_relaxed);
        float       scale  = volume_scale_.load(std::memory_order_relaxed);
        for (std::uint32_t frame = 0; frame < n_frames; ++frame) {
            const float gain = volume * scale;
            const auto  base = static_cast<std::size_t>(frame) * channels;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                out_f[base + ch] *= gain;
            }
            if (left > 0) {
                --left;
                scale = left == 0 ? target : scale + step;
            }
        }
        if ((epoch & 1u) == 0 &&
            volume_scale_epoch_.load(std::memory_order_acquire) == epoch) {
            volume_scale_.store(scale, std::memory_order_relaxed);
            volume_scale_frames_left_.store(left, std::memory_order_relaxed);
        }
    }

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

        const auto channels = self->desc_.channels;
        const auto stride   = channels * static_cast<std::uint32_t>(sizeof(float));
        auto*      out_f    = static_cast<float*>(sb->datas[0].data);

        std::uint32_t n_frames = sb->datas[0].maxsize / stride;
        if (b->requested != 0) {
            const auto req = static_cast<std::uint32_t>(b->requested);
            if (req < n_frames) n_frames = req;
        }

        const auto total_samples = static_cast<std::size_t>(n_frames) * channels;
        std::memset(out_f, 0, total_samples * sizeof(float));

        if (! self->muted_.load(std::memory_order_relaxed)) {
            std::vector<float> scratch(total_samples);

            {
                std::lock_guard<std::mutex> lk(self->channels_mu_);
                for (auto& ch : self->channels_) {
                    std::memset(scratch.data(), 0, total_samples * sizeof(float));
                    const auto produced = ch->next_pcm(scratch.data(), n_frames);
                    const auto produced_samples = static_cast<std::size_t>(produced) * channels;
                    for (std::size_t i = 0; i < produced_samples; ++i) {
                        out_f[i] += scratch[i];
                    }
                }
            }
            self->apply_output_gain(out_f, n_frames, channels);
        }

        sb->datas[0].chunk->offset = 0;
        sb->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
        sb->datas[0].chunk->size   = n_frames * stride;

        pw_stream_queue_buffer(self->stream_, b);
    }

    static void on_state_changed(void* /*user*/, ::pw_stream_state /*old*/,
                                 ::pw_stream_state state, const char* error) {
        switch (state) {
        case PW_STREAM_STATE_ERROR:
            rstd::log::error("wavsen::audio: stream ERROR{}",
                             error ? std::string(": ") + error : std::string{});
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            rstd::log::debug("wavsen::audio: stream UNCONNECTED");
            break;
        case PW_STREAM_STATE_CONNECTING:
            rstd::log::debug("wavsen::audio: stream CONNECTING");
            break;
        case PW_STREAM_STATE_PAUSED:
            rstd::log::debug("wavsen::audio: stream PAUSED");
            break;
        case PW_STREAM_STATE_STREAMING:
            rstd::log::debug("wavsen::audio: stream STREAMING");
            break;
        }
    }

    ::pw_thread_loop* loop_   = nullptr;
    ::pw_stream*      stream_ = nullptr;
    DeviceDesc        desc_ {};

    std::mutex                                 channels_mu_;
    std::vector<std::unique_ptr<IPullChannel>> channels_;

    std::atomic<float> volume_ { 1.0f };
    std::atomic<float> volume_scale_ { 1.0f };
    std::atomic<float> volume_scale_target_ { 1.0f };
    std::atomic<float> volume_scale_step_ { 0.0f };
    std::atomic<std::uint32_t> volume_scale_frames_left_ { 0 };
    std::atomic<std::uint32_t> volume_scale_epoch_ { 0 };
    std::atomic<bool>  muted_ { false };
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() = default;

bool AudioDevice::init()                  { return impl_->init(); }
void AudioDevice::uninit()                { impl_->uninit(); }
bool AudioDevice::is_inited() const       { return impl_->is_inited(); }
void AudioDevice::start()                 { impl_->start(); }
void AudioDevice::stop()                  { impl_->stop(); }
void AudioDevice::mount(std::unique_ptr<IPullChannel> ch) { impl_->mount(std::move(ch)); }
void AudioDevice::unmount_all()           { impl_->unmount_all(); }
float AudioDevice::volume() const         { return impl_->volume(); }
bool  AudioDevice::muted()  const         { return impl_->muted(); }
void  AudioDevice::set_volume(float v)    { impl_->set_volume(v); }
void  AudioDevice::set_muted(bool m)      { impl_->set_muted(m); }
float AudioDevice::volume_scale() const   { return impl_->volume_scale(); }
void  AudioDevice::set_volume_scale(float v) { impl_->set_volume_scale(v); }
void  AudioDevice::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->set_volume_scale(v, fade_ms);
}
DeviceDesc AudioDevice::desc() const      { return impl_->desc(); }
std::uint64_t AudioDevice::stream_position_frames() const {
    return impl_->stream_position_frames();
}

} // namespace wavsen::audio
