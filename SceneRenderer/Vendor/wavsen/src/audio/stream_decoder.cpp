module wavsen.audio;

import rstd.cppstd;
import rstd;
import rstd.log;
import :byte_stream;  // IByteStream
import :core;         // DeviceDesc
import :mixer;        // SoundStream
import :file;
import avutil;
import avcodec;
import avformat;
import swresample;

namespace wavsen::audio {

namespace {

constexpr std::size_t kAvioBuf = 32 * 1024;

int avio_read_cb(void* opaque, std::uint8_t* buf, int sz) {
    auto* s = static_cast<IByteStream*>(opaque);
    auto  r = s->read(reinterpret_cast<rstd::u8*>(buf),
                      static_cast<rstd::usize>(sz));
    if (r.is_err()) return AVERROR_EOF;
    auto n = std::move(r).unwrap();
    if (n == 0) return AVERROR_EOF;
    return static_cast<int>(n);
}

std::int64_t avio_seek_cb(void* opaque, std::int64_t off, int whence) {
    auto* s = static_cast<IByteStream*>(opaque);
    if (whence == AVSEEK_SIZE) return -1;
    rstd::io::SeekFrom from = (whence == rstd::sys::libc::SEEK_SET)
        ? rstd::io::SeekFrom::from_start(static_cast<rstd::u64>(off))
        : (whence == rstd::sys::libc::SEEK_CUR
            ? rstd::io::SeekFrom::from_current(off)
            : rstd::io::SeekFrom::from_end(off));
    auto r = s->seek(from);
    if (r.is_err()) return -1;
    return static_cast<std::int64_t>(std::move(r).unwrap());
}

} // namespace

class StreamDecoder::Impl {
public:
    ~Impl() { teardown(); }

    bool open(std::shared_ptr<IByteStream> src, const DeviceDesc& target) {
        teardown();
        src_    = std::move(src);
        target_ = target;

        avio_buf_ = static_cast<std::uint8_t*>(av_malloc(kAvioBuf));
        if (!avio_buf_) {
            rstd::log::error("wavsen::audio: av_malloc avio buffer failed");
            return false;
        }

        avio_ = avio_alloc_context(avio_buf_, kAvioBuf,
                                   /*write_flag=*/0,
                                   /*opaque=*/src_.get(),
                                   &avio_read_cb,
                                   /*write_packet=*/nullptr,
                                   &avio_seek_cb);
        if (!avio_) {
            rstd::log::error("wavsen::audio: avio_alloc_context failed");
            return false;
        }

        fmt_ctx_         = avformat_alloc_context();
        fmt_ctx_->pb     = avio_;
        fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr) != 0) {
            rstd::log::error("wavsen::audio: avformat_open_input failed");
            // avformat_open_input frees fmt_ctx_ on failure.
            fmt_ctx_ = nullptr;
            return false;
        }
        if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
            rstd::log::error("wavsen::audio: avformat_find_stream_info failed");
            return false;
        }

        const AVCodec* dec   = nullptr;
        int            sidx  = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO,
                                                   -1, -1, &dec, 0);
        if (sidx < 0 || !dec) {
            rstd::log::error("wavsen::audio: no audio stream / codec");
            return false;
        }
        stream_idx_ = sidx;

        cctx_ = avcodec_alloc_context3(dec);
        if (!cctx_) {
            rstd::log::error("wavsen::audio: avcodec_alloc_context3 failed");
            return false;
        }
        if (avcodec_parameters_to_context(cctx_, fmt_ctx_->streams[sidx]->codecpar) < 0) {
            rstd::log::error("wavsen::audio: avcodec_parameters_to_context failed");
            return false;
        }
        if (avcodec_open2(cctx_, dec, nullptr) < 0) {
            rstd::log::error("wavsen::audio: avcodec_open2 failed");
            return false;
        }

        pkt_   = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!pkt_ || !frame_) {
            rstd::log::error("wavsen::audio: alloc packet/frame failed");
            return false;
        }

        return setup_resampler();
    }

    void retarget(const DeviceDesc& target) {
        if (cctx_ == nullptr) {
            target_ = target;
            return;
        }
        if (target.channels == target_.channels && target.sample_rate == target_.sample_rate) {
            return;
        }
        target_ = target;
        if (swr_) {
            swr_free(&swr_);
        }
        setup_resampler();
        // Drop any pending resampled frames; the new format is incompatible.
        pending_offset_ = 0;
        pending_frames_ = 0;
    }

    std::uint64_t next_pcm(void* dst, std::uint32_t frames) {
        if (cctx_ == nullptr || swr_ == nullptr) return 0;

        auto*       out      = static_cast<std::uint8_t*>(dst);
        const auto  bps      = sizeof(float) * target_.channels;
        std::uint32_t produced = 0;

        while (produced < frames) {
            // Flush any leftover from prior decode round.
            if (pending_frames_ > 0) {
                const auto take = std::min<std::uint32_t>(frames - produced, pending_frames_);
                std::memcpy(out + produced * bps,
                            pending_buf_.data() + pending_offset_ * bps,
                            take * bps);
                produced        += take;
                pending_offset_ += take;
                pending_frames_ -= take;
                continue;
            }

            // Need a fresh decoded frame.
            if (!pull_decoded_frame()) {
                if (eof_) break;
                continue; // try again (decode may still be priming)
            }

            // Resample the new frame into pending_buf_.
            const std::int64_t max_out = swr_get_out_samples(swr_, frame_->nb_samples);
            if (max_out <= 0) continue;

            const std::size_t need_bytes = static_cast<std::size_t>(max_out) * bps;
            if (pending_buf_.size() < need_bytes) {
                pending_buf_.resize(need_bytes);
            }
            std::uint8_t* out_ptr   = pending_buf_.data();
            const int     converted = swr_convert(swr_, &out_ptr,
                                                  static_cast<int>(max_out),
                                                  const_cast<const std::uint8_t**>(frame_->extended_data),
                                                  frame_->nb_samples);
            if (converted < 0) {
                rstd::log::warn("wavsen::audio: swr_convert error");
                continue;
            }
            pending_offset_ = 0;
            pending_frames_ = static_cast<std::uint32_t>(converted);
        }

        return produced;
    }

    bool seek_to(double seconds) {
        if (!fmt_ctx_ || stream_idx_ < 0) return false;
        const AVStream* st = fmt_ctx_->streams[stream_idx_];
        const auto      tb = st->time_base;
        const auto      ts = static_cast<std::int64_t>(seconds / ffi::av_q2d(tb));
        if (av_seek_frame(fmt_ctx_, stream_idx_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            rstd::log::warn("wavsen::audio: av_seek_frame failed");
            return false;
        }
        if (cctx_) avcodec_flush_buffers(cctx_);
        if (swr_)  swr_free(&swr_);
        setup_resampler();
        pending_offset_   = 0;
        pending_frames_   = 0;
        eof_              = false;
        last_pts_seconds_ = seconds;
        return true;
    }

    double current_pts_seconds() const { return last_pts_seconds_; }
    bool   is_eof()              const { return eof_; }

    std::uint32_t sample_rate() const {
        return cctx_ ? static_cast<std::uint32_t>(cctx_->sample_rate) : 0;
    }
    std::uint32_t channels() const {
        return cctx_ ? static_cast<std::uint32_t>(cctx_->ch_layout.nb_channels) : 0;
    }

private:
    bool pull_decoded_frame() {
        for (;;) {
            int rc = avcodec_receive_frame(cctx_, frame_);
            if (rc == 0) {
                // Capture PTS of the freshly decoded frame for clock query.
                const auto pts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? frame_->best_effort_timestamp
                                 : frame_->pts;
                if (pts != AV_NOPTS_VALUE && fmt_ctx_ && stream_idx_ >= 0) {
                    last_pts_seconds_ =
                        static_cast<double>(pts) *
                        ffi::av_q2d(fmt_ctx_->streams[stream_idx_]->time_base);
                }
                return true;
            }
            if (rc == AVERROR(EAGAIN)) {
                // Need more input.
                rc = av_read_frame(fmt_ctx_, pkt_);
                if (rc == AVERROR_EOF) {
                    avcodec_send_packet(cctx_, nullptr); // flush
                    eof_ = true;
                    continue;
                }
                if (rc < 0) {
                    rstd::log::warn("wavsen::audio: av_read_frame error");
                    eof_ = true;
                    return false;
                }
                if (pkt_->stream_index != stream_idx_) {
                    av_packet_unref(pkt_);
                    continue;
                }
                avcodec_send_packet(cctx_, pkt_);
                av_packet_unref(pkt_);
                continue;
            }
            if (rc == AVERROR_EOF) {
                eof_ = true;
                return false;
            }
            rstd::log::warn("wavsen::audio: avcodec_receive_frame error");
            return false;
        }
    }

    bool setup_resampler() {
        AVChannelLayout out_layout {};
        av_channel_layout_default(&out_layout, static_cast<int>(target_.channels));

        AVChannelLayout in_layout {};
        if (cctx_->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_copy(&in_layout, &cctx_->ch_layout);
        } else {
            av_channel_layout_default(&in_layout, cctx_->ch_layout.nb_channels);
        }

        if (swr_alloc_set_opts2(&swr_,
                                &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(target_.sample_rate),
                                &in_layout,  cctx_->sample_fmt,  cctx_->sample_rate,
                                /*log_offset=*/0, /*log_ctx=*/nullptr) < 0)
        {
            av_channel_layout_uninit(&out_layout);
            av_channel_layout_uninit(&in_layout);
            rstd::log::error("wavsen::audio: swr_alloc_set_opts2 failed");
            return false;
        }
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&in_layout);

        if (swr_init(swr_) < 0) {
            rstd::log::error("wavsen::audio: swr_init failed");
            swr_free(&swr_);
            return false;
        }
        return true;
    }

    void teardown() {
        if (swr_)     swr_free(&swr_);
        if (frame_)   av_frame_free(&frame_);
        if (pkt_)     av_packet_free(&pkt_);
        if (cctx_)    avcodec_free_context(&cctx_);
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
        if (avio_)    {
            // avio_buf_ is owned by avio_; avio_context_free handles it iff
            // we set it. To be safe, free both explicitly.
            av_freep(&avio_->buffer);
            avio_context_free(&avio_);
        }
        avio_buf_ = nullptr;
        src_.reset();
        pending_offset_   = 0;
        pending_frames_   = 0;
        eof_              = false;
        last_pts_seconds_ = 0.0;
    }

    std::shared_ptr<IByteStream> src_;
    DeviceDesc                   target_ {};

    std::uint8_t*    avio_buf_ = nullptr;
    AVIOContext*     avio_     = nullptr;
    AVFormatContext* fmt_ctx_  = nullptr;
    AVCodecContext*  cctx_     = nullptr;
    SwrContext*      swr_      = nullptr;
    AVPacket*        pkt_      = nullptr;
    AVFrame*         frame_    = nullptr;
    int              stream_idx_ = -1;

    std::vector<std::uint8_t> pending_buf_;
    std::uint32_t             pending_offset_ = 0;
    std::uint32_t             pending_frames_ = 0;
    bool                      eof_            = false;
    double                    last_pts_seconds_ = 0.0;
};

StreamDecoder::StreamDecoder() : impl_(std::make_unique<Impl>()) {}
StreamDecoder::~StreamDecoder() = default;
StreamDecoder::StreamDecoder(StreamDecoder&&) noexcept = default;
StreamDecoder& StreamDecoder::operator=(StreamDecoder&&) noexcept = default;

bool StreamDecoder::open(std::shared_ptr<IByteStream> src, const DeviceDesc& target) {
    return impl_->open(std::move(src), target);
}

void StreamDecoder::retarget(const DeviceDesc& target) {
    impl_->retarget(target);
}

std::uint64_t StreamDecoder::next_pcm(void* dst, std::uint32_t frames) {
    return impl_->next_pcm(dst, frames);
}

bool          StreamDecoder::seek_to(double s)             { return impl_->seek_to(s); }
double        StreamDecoder::current_pts_seconds() const   { return impl_->current_pts_seconds(); }
bool          StreamDecoder::is_eof() const                { return impl_->is_eof(); }
std::uint32_t StreamDecoder::sample_rate() const           { return impl_->sample_rate(); }
std::uint32_t StreamDecoder::channels()    const           { return impl_->channels(); }

namespace {

// SoundStream backed by libav* decoder. Created via make_stream(); the
// CubebDevice pulls PCM through next_pcm in the audio thread.
class DecoderStream : public SoundStream {
public:
    explicit DecoderStream(StreamDecoder dec)
        : dec_(std::move(dec)) {}

    auto next_pcm(void* dst, std::uint32_t frames) -> std::uint64_t override {
        return dec_.next_pcm(dst, frames);
    }
    void pass_desc(const Desc& d) override {
        dec_.retarget({ d.channels, d.sample_rate });
    }

private:
    StreamDecoder dec_;
};

} // namespace

auto make_stream(std::shared_ptr<IByteStream> source, const SoundStream::Desc& desc)
    -> std::unique_ptr<SoundStream> {
    StreamDecoder dec;
    if (!dec.open(std::move(source), { desc.channels, desc.sample_rate })) {
        return nullptr;
    }
    return std::make_unique<DecoderStream>(std::move(dec));
}

} // namespace wavsen::audio
