export module wavsen.audio:file;

import rstd.cppstd;
import :byte_stream;  // IByteStream
import :core;         // DeviceDesc
import :mixer;        // SoundStream (for make_stream factory)

export namespace wavsen::audio {

// libav*-backed audio decoder + resampler. Reads bytes from `IByteStream`
// (via a custom AVIOContext), decodes via libavformat/libavcodec, and
// resamples through libswresample to the device's negotiated f32 LE
// interleaved format.
class StreamDecoder {
public:
    StreamDecoder();
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&)            = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;
    StreamDecoder(StreamDecoder&&) noexcept;
    StreamDecoder& operator=(StreamDecoder&&) noexcept;

    // Open the source. Returns false on parser/codec error; details logged
    // via rstd::log.
    auto open(std::shared_ptr<IByteStream> src, const DeviceDesc& target) -> bool;

    // Update target descriptor (channels / sample rate). Caller invokes
    // this after the audio device negotiates a different format than what
    // was originally requested.
    void retarget(const DeviceDesc& target);

    // Pull `frames` interleaved f32 frames into `dst`. Returns frames
    // actually produced (less than `frames` only on EOF).
    auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t;

    // Seek to a stream-time offset. Re-baselines internal PTS tracking
    // and clears the EOF flag. Returns false if no source is open or
    // av_seek_frame fails.
    auto seek_to(double seconds) -> bool;

    // PTS in seconds of the most recently decoded frame, derived from
    // best_effort_timestamp * av_q2d(stream.time_base). Returns 0.0 before
    // the first frame is decoded.
    auto current_pts_seconds() const -> double;

    // True once av_read_frame returned AVERROR_EOF (latched).
    auto is_eof() const -> bool;

    // Source stream characteristics. Both return 0 before a successful open.
    auto sample_rate() const -> std::uint32_t;
    auto channels()    const -> std::uint32_t;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Construct a libav*-backed SoundStream from a byte source. Decodes any
// container/codec libavformat understands and resamples to `desc`.
auto make_stream(std::shared_ptr<IByteStream> source, const SoundStream::Desc& desc)
    -> std::unique_ptr<SoundStream>;

} // namespace wavsen::audio
