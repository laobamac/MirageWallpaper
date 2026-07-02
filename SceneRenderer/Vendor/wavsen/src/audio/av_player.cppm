export module wavsen.audio:av_sync;

import rstd.cppstd;
import rstd;
import :byte_stream;  // IByteStream

export namespace wavsen::audio {

// Error type for AvPlayer::open. Mirrors the small-string Error pattern
// used elsewhere in wavsen (no rich type — just a printable reason).
struct AvPlayerError {
    std::string message;
};

// Single-stream audio playback aimed at A/V sync. Owns its own
// AudioDevice (independent of any SoundManager) so it can expose a
// stable master clock derived from pw_stream_get_time_n.
//
// Threading: open/play/pause/seek_to_start/set_volume/set_muted are all
// callable from the main thread. current_time_seconds is lock-free and
// safe to call from any thread (e.g. from inside a Presenter callback).
class AvPlayer {
public:
    static auto open(std::shared_ptr<IByteStream> src)
        -> rstd::Result<std::unique_ptr<AvPlayer>, AvPlayerError>;

    ~AvPlayer();
    AvPlayer(const AvPlayer&)            = delete;
    AvPlayer& operator=(const AvPlayer&) = delete;

    void play();
    void pause();
    bool is_paused() const;

    // Reset playback to t=0. Call from the video plugin's loop boundary
    // (NextFrame::Eof) after the video decoder seeks back to the start.
    // The clock will re-anchor on the next data callback.
    void seek_to_start();

    // PTS in seconds of the audio sample currently being played by the
    // device. Returns NaN before the device is primed; the caller should
    // treat NaN as "fall back to wall-clock pacing".
    double current_time_seconds() const;

    // 0..1 linear gain. Atomic; safe from any thread.
    void set_volume(float v);
    void set_muted(bool m);
    auto volume_scale() const -> float;
    void set_volume_scale(float v);
    void set_volume_scale(float v, std::uint32_t fade_ms);

    // True once the decoder reached EOF *and* the device has had time
    // to drain the last enqueued frames.
    bool is_eof() const;

private:
    AvPlayer();
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavsen::audio
