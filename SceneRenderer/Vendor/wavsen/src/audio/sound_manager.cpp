module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :core;
import :mixer;

namespace wavsen::audio {

namespace {

// Adapter exposing a SoundStream to AudioDevice's IPullChannel interface.
class StreamPullChannel : public IPullChannel {
public:
    explicit StreamPullChannel(std::unique_ptr<SoundStream> ss)
        : ss_(std::move(ss)) {}

    auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t override {
        return ss_->next_pcm(dst, frames);
    }
    void pass_desc(const DeviceDesc& d) override {
        ss_->pass_desc({ d.channels, d.sample_rate });
    }

private:
    std::unique_ptr<SoundStream> ss_;
};

} // namespace

class SoundManager::Impl {
public:
    AudioDevice device;
    std::recursive_mutex lifecycle_mutex;
    std::size_t          stream_count { 0 };
    bool                 playback_requested { false };
    std::uint32_t offline_rate { 0 };
    std::vector<std::unique_ptr<SoundStream>> offline_streams;
};

SoundManager::SoundManager() : impl_(std::make_unique<Impl>()) {}
SoundManager::~SoundManager() = default;

void SoundManager::mount(std::unique_ptr<SoundStream> ss) {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (!ss) return;
    if (impl_->offline_rate) {
        ss->pass_desc({ 2, impl_->offline_rate });
        impl_->offline_streams.push_back(std::move(ss));
        return;
    }
    impl_->device.mount(std::make_unique<StreamPullChannel>(std::move(ss)));
    ++impl_->stream_count;
    if (impl_->playback_requested && init()) impl_->device.start();
}

void SoundManager::unmount_all() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->device.stop();
    impl_->device.unmount_all();
    impl_->stream_count = 0;
    impl_->offline_streams.clear();
    impl_->device.uninit();
}

bool SoundManager::init() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (impl_->offline_rate) return false;
    if (impl_->stream_count == 0) return false;
    if (muted()) {
        rstd::log::info("wavsen::audio: muted, not initializing device");
        return false;
    }
    return impl_->device.init();
}

void SoundManager::set_offline(std::uint32_t sample_rate) {
    unmount_all();
    std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->offline_rate = sample_rate;
}

std::vector<float> SoundManager::render_offline(std::uint32_t frames) {
    std::lock_guard lock(impl_->lifecycle_mutex);
    std::vector<float> output(static_cast<std::size_t>(frames) * 2, 0.0f);
    std::vector<float> scratch(output.size(), 0.0f);
    for (auto& stream : impl_->offline_streams) {
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        const auto count = std::min<std::uint64_t>(frames, stream->next_pcm(scratch.data(), frames));
        for (std::size_t i = 0; i < count * 2; ++i) output[i] += scratch[i];
    }
    const float gain = muted() ? 0.0f : volume() * volume_scale();
    for (auto& sample : output) sample = std::clamp(sample * gain, -1.0f, 1.0f);
    return output;
}

bool SoundManager::is_inited() const { return impl_->device.is_inited(); }

void SoundManager::play() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->playback_requested = true;
    if (init()) impl_->device.start();
}
void SoundManager::pause() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->playback_requested = false;
    impl_->device.stop();
}

float SoundManager::volume() const     { return impl_->device.volume(); }
bool  SoundManager::muted() const      { return impl_->device.muted(); }
void  SoundManager::set_volume(float v) { impl_->device.set_volume(v); }
float SoundManager::volume_scale() const { return impl_->device.volume_scale(); }
void  SoundManager::set_volume_scale(float v) { impl_->device.set_volume_scale(v); }
void  SoundManager::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->device.set_volume_scale(v, fade_ms);
}

void SoundManager::set_muted(bool m) {
    std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->device.set_muted(m);
    if (!m) {
        if (impl_->playback_requested && init()) impl_->device.start();
    } else {
        impl_->device.uninit();
    }
}

} // namespace wavsen::audio
