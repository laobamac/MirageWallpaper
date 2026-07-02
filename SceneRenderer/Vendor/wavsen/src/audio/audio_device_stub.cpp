// audio_device_stub.cpp — silent AudioDevice backend used on platforms that
// don't have a wired-up native audio backend (initially macOS).
//
// Goals:
//   - Satisfy the AudioDevice public API so wavsen::audio links cleanly.
//   - Drive a background thread that pulls PCM from mounted IPullChannel
//     consumers at a steady cadence so AvPlayer (which uses
//     stream_position_frames as its master clock) progresses, but discard
//     the resulting samples instead of routing them anywhere.
//   - Report a sensible DeviceDesc (48 kHz stereo f32 interleaved) — this
//     matches what the pulse backend negotiates by default.
//
// SceneWallpaper.cpp tolerates a silent audio path (see references in the
// SceneRenderer port plan); this is the minimum viable backend.

module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :core;

namespace wavsen::audio
{

namespace
{
constexpr std::uint32_t kStubRate     = 48000;
constexpr std::uint32_t kStubChannels = 2;
constexpr std::uint32_t kStubQuantum  = 1024; // frames per pull tick

float clamp_volume_scale(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}
} // namespace

class AudioDevice::Impl {
public:
    Impl()  = default;
    ~Impl() { uninit(); }

    bool init() {
        if (inited_.load(std::memory_order_acquire)) return true;
        desc_ = DeviceDesc { kStubChannels, kStubRate };
        inited_.store(true, std::memory_order_release);
        return true;
    }

    void uninit() {
        if (! inited_.load(std::memory_order_acquire)) return;
        stop();
        unmount_all();
        inited_.store(false, std::memory_order_release);
    }

    bool is_inited() const { return inited_.load(std::memory_order_acquire); }

    void start() {
        if (running_.exchange(true)) return;
        thr_ = std::thread([this] { pump_loop(); });
    }

    void stop() {
        if (! running_.exchange(false)) return;
        if (thr_.joinable()) thr_.join();
    }

    void mount(std::unique_ptr<IPullChannel> ch) {
        if (! ch) return;
        ch->pass_desc(desc_);
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.push_back(std::move(ch));
    }

    void unmount_all() {
        std::lock_guard<std::mutex> lk(channels_mu_);
        channels_.clear();
    }

    float volume() const { return volume_.load(std::memory_order_relaxed); }
    bool  muted() const { return muted_.load(std::memory_order_relaxed); }
    void  set_volume(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        volume_.store(v, std::memory_order_relaxed);
    }
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }

    float volume_scale() const { return volume_scale_.load(std::memory_order_relaxed); }
    void  set_volume_scale(float v) {
        volume_scale_.store(clamp_volume_scale(v), std::memory_order_relaxed);
    }
    void set_volume_scale(float v, std::uint32_t /*fade_ms*/) {
        // No real device → no need to honour fades; jump straight to target.
        set_volume_scale(v);
    }

    DeviceDesc desc() const { return desc_; }

    std::uint64_t stream_position_frames() const {
        return position_frames_.load(std::memory_order_relaxed);
    }

private:
    void pump_loop() {
        std::vector<std::uint8_t> scratch(static_cast<std::size_t>(kStubQuantum) * desc_.channels *
                                          sizeof(float));
        const auto tick_period =
            std::chrono::microseconds((1'000'000ull * kStubQuantum) / desc_.sample_rate);
        auto next = std::chrono::steady_clock::now();
        while (running_.load(std::memory_order_acquire)) {
            // Pull from every mounted channel so AvPlayer's queues drain and
            // the master clock advances; discard the produced PCM.
            {
                std::lock_guard<std::mutex> lk(channels_mu_);
                for (auto& ch : channels_) {
                    if (ch) ch->next_pcm(scratch.data(), kStubQuantum);
                }
            }
            position_frames_.fetch_add(kStubQuantum, std::memory_order_relaxed);
            next += tick_period;
            std::this_thread::sleep_until(next);
        }
    }

    std::atomic<bool> inited_ { false };
    std::atomic<bool> running_ { false };
    std::thread       thr_;

    DeviceDesc desc_ { kStubChannels, kStubRate };

    std::mutex                                 channels_mu_;
    std::vector<std::unique_ptr<IPullChannel>> channels_;

    std::atomic<float> volume_ { 1.0f };
    std::atomic<float> volume_scale_ { 1.0f };
    std::atomic<bool>  muted_ { false };

    std::atomic<std::uint64_t> position_frames_ { 0 };
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() = default;

bool          AudioDevice::init() { return impl_->init(); }
void          AudioDevice::uninit() { impl_->uninit(); }
bool          AudioDevice::is_inited() const { return impl_->is_inited(); }
void          AudioDevice::start() { impl_->start(); }
void          AudioDevice::stop() { impl_->stop(); }
void          AudioDevice::mount(std::unique_ptr<IPullChannel> ch) { impl_->mount(std::move(ch)); }
void          AudioDevice::unmount_all() { impl_->unmount_all(); }
float         AudioDevice::volume() const { return impl_->volume(); }
bool          AudioDevice::muted() const { return impl_->muted(); }
void          AudioDevice::set_volume(float v) { impl_->set_volume(v); }
void          AudioDevice::set_muted(bool m) { impl_->set_muted(m); }
float         AudioDevice::volume_scale() const { return impl_->volume_scale(); }
void          AudioDevice::set_volume_scale(float v) { impl_->set_volume_scale(v); }
void          AudioDevice::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->set_volume_scale(v, fade_ms);
}
DeviceDesc    AudioDevice::desc() const { return impl_->desc(); }
std::uint64_t AudioDevice::stream_position_frames() const {
    return impl_->stream_position_frames();
}

} // namespace wavsen::audio
