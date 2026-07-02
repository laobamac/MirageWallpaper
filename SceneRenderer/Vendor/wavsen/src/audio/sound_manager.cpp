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
};

SoundManager::SoundManager() : impl_(std::make_unique<Impl>()) {}
SoundManager::~SoundManager() = default;

void SoundManager::mount(std::unique_ptr<SoundStream> ss) {
    if (!ss) return;
    impl_->device.mount(std::make_unique<StreamPullChannel>(std::move(ss)));
}

void SoundManager::unmount_all() { impl_->device.unmount_all(); }

bool SoundManager::init() {
    if (muted()) {
        rstd::log::info("wavsen::audio: muted, not initializing device");
        return false;
    }
    return impl_->device.init();
}

bool SoundManager::is_inited() const { return impl_->device.is_inited(); }

void SoundManager::play()  { impl_->device.start(); }
void SoundManager::pause() { impl_->device.stop(); }

float SoundManager::volume() const     { return impl_->device.volume(); }
bool  SoundManager::muted() const      { return impl_->device.muted(); }
void  SoundManager::set_volume(float v) { impl_->device.set_volume(v); }
float SoundManager::volume_scale() const { return impl_->device.volume_scale(); }
void  SoundManager::set_volume_scale(float v) { impl_->device.set_volume_scale(v); }
void  SoundManager::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->device.set_volume_scale(v, fade_ms);
}

void SoundManager::set_muted(bool m) {
    impl_->device.set_muted(m);
    if (!m) {
        // re-init the device if previously muted-uninited
        impl_->device.init();
    } else {
        impl_->device.uninit();
    }
}

} // namespace wavsen::audio
