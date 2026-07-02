export module wavsen.audio:capture;

import rstd.cppstd;

export namespace wavsen::audio
{

// 64-bin perceptual magnitude spectrum, EMA-smoothed. Values are mapped to
// 0..1 for audio-responsive consumers. `bins` is kept as an alias of `average`.
// `publish_ms` is a steady_clock timestamp (ms since epoch) of the last
// RT-side update, used by readers to detect stale snapshots. Zero means
// "never primed".
struct AudioSpectrum {
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    std::array<float, 64> average {};
    std::array<float, 64> bins {};
    std::int64_t          publish_ms { 0 };

    void clear() {
        left.fill(0.0f);
        right.fill(0.0f);
        average.fill(0.0f);
        bins.fill(0.0f);
        publish_ms = 0;
    }
};

// Taps the system default sink's monitor source, runs a 4096-point
// Hann-windowed FFT per stereo channel on the audio thread, merges
// magnitudes into 64 calibrated WE-style bands, EMA-smooths, and publishes a
// lock-free snapshot for renderers.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    auto init() -> bool;
    void uninit();
    auto is_inited() const -> bool;

    // Lock-free read. Returns true if at least one capture buffer has
    // been processed; out is zero-filled until then.
    bool snapshot(AudioSpectrum& out) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavsen::audio
