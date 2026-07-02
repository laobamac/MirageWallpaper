module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :byte_stream;
import :core;          // AudioDevice, IPullChannel, DeviceDesc
import :file;          // StreamDecoder
import :av_sync;

namespace wavsen::audio {

namespace {

// Internal pull channel for AvPlayer. Calls into StreamDecoder and
// stamps the master-clock anchor on the first frame after open / seek.
class AvPullChannel : public IPullChannel {
public:
    AvPullChannel(StreamDecoder*               decoder,
                  std::atomic<double>*         pts_at_anchor,
                  std::atomic<std::uint64_t>*  device_pos_at_anchor,
                  std::atomic<bool>*           needs_reanchor,
                  std::atomic<bool>*           anchored,
                  std::function<std::uint64_t()> device_pos_now)
        : decoder_(decoder),
          pts_at_anchor_(pts_at_anchor),
          device_pos_at_anchor_(device_pos_at_anchor),
          needs_reanchor_(needs_reanchor),
          anchored_(anchored),
          device_pos_now_(std::move(device_pos_now)) {}

    auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t override {
        const auto produced = decoder_->next_pcm(dst, frames);
        if (produced > 0 && needs_reanchor_->load(std::memory_order_acquire)) {
            // Anchor: at the moment this batch enters the device, the
            // decoder's most-recent PTS corresponds to the device's
            // current playback position.
            pts_at_anchor_->store(decoder_->current_pts_seconds(),
                                  std::memory_order_relaxed);
            device_pos_at_anchor_->store(device_pos_now_(),
                                         std::memory_order_relaxed);
            anchored_->store(true, std::memory_order_release);
            needs_reanchor_->store(false, std::memory_order_release);
        }
        return produced;
    }

    void pass_desc(const DeviceDesc& d) override {
        decoder_->retarget(d);
    }

private:
    StreamDecoder*                 decoder_;
    std::atomic<double>*           pts_at_anchor_;
    std::atomic<std::uint64_t>*    device_pos_at_anchor_;
    std::atomic<bool>*             needs_reanchor_;
    std::atomic<bool>*             anchored_;
    std::function<std::uint64_t()> device_pos_now_;
};

} // namespace

class AvPlayer::Impl {
public:
    AudioDevice    device;
    StreamDecoder* decoder_ptr = nullptr;  // owned by AvPullChannel via decoder_storage
    // The decoder must outlive the audio stream (which calls into it).
    // Owned via unique_ptr so the AvPullChannel can hold a stable pointer.
    std::unique_ptr<StreamDecoder> decoder_storage;

    std::atomic<double>        pts_at_anchor        { 0.0 };
    std::atomic<std::uint64_t> device_pos_at_anchor { 0 };
    std::atomic<bool>          needs_reanchor       { true };
    std::atomic<bool>          anchored             { false };
    std::atomic<bool>          paused               { true };
};

AvPlayer::AvPlayer() : impl_(std::make_unique<Impl>()) {}
AvPlayer::~AvPlayer() = default;

auto AvPlayer::open(std::shared_ptr<IByteStream> src)
    -> rstd::Result<std::unique_ptr<AvPlayer>, AvPlayerError>
{
    auto p = std::unique_ptr<AvPlayer>(new AvPlayer());

    if (!p->impl_->device.init()) {
        return rstd::Err(AvPlayerError{ "audio device init failed" });
    }

    const auto desc = p->impl_->device.desc();

    p->impl_->decoder_storage = std::make_unique<StreamDecoder>();
    if (!p->impl_->decoder_storage->open(std::move(src), desc)) {
        return rstd::Err(AvPlayerError{ "audio decoder open failed" });
    }
    p->impl_->decoder_ptr = p->impl_->decoder_storage.get();

    rstd::log::info(
        "wavsen::audio::AvPlayer: opened ({} ch @ {} Hz device, source {} ch @ {} Hz)",
        desc.channels, desc.sample_rate,
        p->impl_->decoder_ptr->channels(),
        p->impl_->decoder_ptr->sample_rate());

    auto* dev = &p->impl_->device;
    auto channel = std::make_unique<AvPullChannel>(
        p->impl_->decoder_ptr,
        &p->impl_->pts_at_anchor,
        &p->impl_->device_pos_at_anchor,
        &p->impl_->needs_reanchor,
        &p->impl_->anchored,
        [dev]() { return dev->stream_position_frames(); });
    p->impl_->device.mount(std::move(channel));

    return rstd::Ok(std::move(p));
}

void AvPlayer::play() {
    impl_->device.start();
    impl_->paused.store(false, std::memory_order_relaxed);
}

void AvPlayer::pause() {
    impl_->device.stop();
    impl_->paused.store(true, std::memory_order_relaxed);
}

bool AvPlayer::is_paused() const {
    return impl_->paused.load(std::memory_order_relaxed);
}

void AvPlayer::seek_to_start() {
    const bool was_playing = !is_paused();
    impl_->device.stop();
    if (impl_->decoder_ptr) {
        impl_->decoder_ptr->seek_to(0.0);
    }
    impl_->anchored.store(false, std::memory_order_release);
    impl_->needs_reanchor.store(true, std::memory_order_release);
    if (was_playing) {
        impl_->device.start();
    }
}

double AvPlayer::current_time_seconds() const {
    if (!impl_->anchored.load(std::memory_order_acquire)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto sr = impl_->device.desc().sample_rate;
    if (sr == 0) return std::numeric_limits<double>::quiet_NaN();
    const auto played = impl_->device.stream_position_frames();
    const auto base   = impl_->device_pos_at_anchor.load(std::memory_order_relaxed);
    const auto pts0   = impl_->pts_at_anchor.load(std::memory_order_relaxed);
    // played may legally drop below base across some backends after stop/start;
    // saturate to anchor pts in that case.
    if (played < base) return pts0;
    return pts0 + static_cast<double>(played - base) / static_cast<double>(sr);
}

void AvPlayer::set_volume(float v) { impl_->device.set_volume(v); }
void AvPlayer::set_muted(bool m)   { impl_->device.set_muted(m); }
float AvPlayer::volume_scale() const { return impl_->device.volume_scale(); }
void AvPlayer::set_volume_scale(float v) { impl_->device.set_volume_scale(v); }
void AvPlayer::set_volume_scale(float v, std::uint32_t fade_ms) {
    impl_->device.set_volume_scale(v, fade_ms);
}

bool AvPlayer::is_eof() const {
    return impl_->decoder_ptr && impl_->decoder_ptr->is_eof();
}

} // namespace wavsen::audio
