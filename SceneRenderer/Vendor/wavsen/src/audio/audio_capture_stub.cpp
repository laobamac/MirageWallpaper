// audio_capture_stub.cpp — null AudioCapture backend. Reports a steady
// zero-valued spectrum so audio-responsive scene effects render as if the
// audio loopback were silent. Pair with the stub AudioDevice backend.

module;

#include <memory>

module wavsen.audio;

import rstd.cppstd;
import :capture;

namespace wavsen::audio
{

class AudioCapture::Impl {
public:
    bool init() { return true; }
    void uninit() {}
    bool is_inited() const { return true; }
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;

bool AudioCapture::init(bool) { return impl_->init(); }
void AudioCapture::uninit() { impl_->uninit(); }
bool AudioCapture::is_inited() const { return impl_->is_inited(); }

bool AudioCapture::snapshot(AudioSpectrum& out) const {
    // Always zeroed — consumers see "silence". Returning false means
    // "no buffer processed yet" so downstream effects that fall back to
    // a static visual when capture is uninitialised behave nicely.
    out.clear();
    return false;
}

} // namespace wavsen::audio
