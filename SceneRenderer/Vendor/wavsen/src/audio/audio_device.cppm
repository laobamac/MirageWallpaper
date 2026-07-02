export module wavsen.audio:core;

import rstd.cppstd;

export namespace wavsen::audio {

// Negotiated stream format — wavsen always asks the backend for f32 interleaved.
struct DeviceDesc {
    std::uint32_t channels;
    std::uint32_t sample_rate;
};

// Pulled by the audio thread to fill an output buffer. Implementations
// must NOT block (the backend data thread is realtime). `frames` is the
// number of interleaved frames to write into `dst`.
class IPullChannel {
public:
    virtual ~IPullChannel() = default;
    virtual auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t = 0;
    virtual void pass_desc(const DeviceDesc&)                               = 0;
};

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&)            = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    auto init() -> bool;
    void uninit();
    auto is_inited() const -> bool;

    void start();
    void stop();

    void mount(std::unique_ptr<IPullChannel>);
    void unmount_all();

    auto volume() const -> float;
    auto muted() const -> bool;
    void set_volume(float v);
    void set_muted(bool m);
    auto volume_scale() const -> float;
    void set_volume_scale(float v);
    void set_volume_scale(float v, std::uint32_t fade_ms);

    auto desc() const -> DeviceDesc;

    // Frames the audio device has actually played back since the stream
    // was created. Used by AvPlayer as the master clock for A/V sync.
    // Returns 0 before init() / on query failure / before primed.
    auto stream_position_frames() const -> std::uint64_t;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavsen::audio
