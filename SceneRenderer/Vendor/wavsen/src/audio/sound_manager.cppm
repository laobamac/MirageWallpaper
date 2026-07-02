export module wavsen.audio:mixer;

import rstd.cppstd;
import :byte_stream;  // IByteStream

export namespace wavsen::audio {

// A mountable PCM source. Data callback fills `frames` interleaved frames
// of the device's negotiated format (f32 little-endian) and channel count.
class SoundStream {
public:
    struct Desc {
        std::uint32_t channels;
        std::uint32_t sample_rate;
    };

    SoundStream()                              = default;
    virtual ~SoundStream()                     = default;
    SoundStream(const SoundStream&)            = delete;
    SoundStream& operator=(const SoundStream&) = delete;

    virtual auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t = 0;
    virtual void pass_desc(const Desc&)                                     = 0;

    // 3D-audio hooks — no-op in 0.1; spatial backend will override these in
    // a future iteration. Coordinates are listener-relative (right-handed,
    // metres). See plans/wavsen-...md "推后" section.
    virtual void set_position(float /*x*/, float /*y*/, float /*z*/) {}
    virtual void set_listener_position(float /*x*/, float /*y*/, float /*z*/) {}
};

// Owns the output audio device and mounted streams. Mixing happens in the
// audio thread via the device's data callback. Mute / volume are atomic
// so UI threads can poke them without locks.
class SoundManager {
public:
    SoundManager();
    ~SoundManager();
    SoundManager(const SoundManager&)            = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    void mount(std::unique_ptr<SoundStream>);
    void unmount_all();

    auto init() -> bool;
    auto is_inited() const -> bool;
    void play();
    void pause();

    auto volume() const -> float;
    auto muted() const -> bool;
    void set_volume(float v);
    void set_muted(bool m);
    auto volume_scale() const -> float;
    void set_volume_scale(float v);
    void set_volume_scale(float v, std::uint32_t fade_ms);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavsen::audio
