export module wavsen.video:video_decoder;

import rstd.cppstd;
import rstd;
import vulkan;
import :vk_device;        // Error, Producer

export namespace wavsen::video {

struct Nv12Frame {
    // Layout: Y plane (`width * height` bytes) directly followed by
    // interleaved UV plane (`width * height / 2` bytes). Total size is
    // therefore `width * height * 3 / 2`.
    std::vector<std::uint8_t> data;
    std::uint32_t             width  { 0 };
    std::uint32_t             height { 0 };
    // Stream-time PTS in seconds; -1.0 if unavailable.
    double                    pts_seconds { -1.0 };
    // Source colorspace / range — caller feeds these into the YuvToRgba
    // colour matrix builder. Defaults to BT.709 limited range when the
    // stream doesn't tag them.
    std::uint32_t             colorspace { 0 };
    std::uint32_t             color_range { 0 };
};

// Probe result for VideoDecoder::probe_native.
struct ProbeResult {
    std::uint32_t width;
    std::uint32_t height;
};

// Outcome of a successful frame pull. `Error` is reserved for the Err
// arm of Result; clean stream end is Eof in the Ok arm.
enum class NextFrame {
    Ok,
    Eof,
};

// View onto the AVVkFrame yielded by `next_vk_frame` — one entry per
// plane. Pointers alias the underlying AVVkFrame; valid until the next
// call to `next_vk_frame`.
struct VkFrameView {
    VkImage*       img;
    VkImageLayout* layout;
    VkSemaphore*   sem;
    std::uint64_t* sem_value;
    std::uint32_t* queue_family;
    std::uint32_t  plane_count;
    std::uint32_t  width;
    std::uint32_t  height;
    double         pts_seconds;
    std::uint32_t  colorspace  { 0 };
    std::uint32_t  color_range { 0 };
    std::uint32_t  bit_depth   { 8 };
};

// User selection for the hwaccel chain. Auto resolves to [Vulkan, Vaapi]
// (then sw decode if both fail).
enum class HwAccel {
    Auto    = 0,
    Vulkan  = 1,
    Vaapi   = 2,
    None    = 3,
};

struct OpenOpts {
    HwAccel     hwaccel { HwAccel::Auto };
    // DRM render node (e.g. "/dev/dri/renderD128") for AV_HWDEVICE_TYPE_VAAPI.
    // Empty → FFmpeg picks the default.
    std::string render_node;
};

// Caller-supplied byte source for stream-based decoder construction.
// Maps onto libavformat's AVIOContext callbacks. Methods are invoked
// from the decoder thread; implementations don't need internal locks.
// Lifetime: VideoDecoder takes ownership and destroys after close.
struct IInputStream {
    virtual ~IInputStream() = default;
    // Read up to `size` bytes into `buf`. Return bytes read; 0 = EOF;
    // negative = error (mapped to AVERROR_EOF / AVERROR(EIO)).
    virtual int     read(std::uint8_t* buf, int size) = 0;
    // `whence` mirrors libavformat: SEEK_SET / SEEK_CUR / SEEK_END, and
    // AVSEEK_SIZE (0x10000) which must return the total stream size.
    // Negative return on failure.
    virtual std::int64_t seek(std::int64_t offset, int whence) = 0;
};

// Hands out a fresh IInputStream per call (cursor at 0). open_from_stream
// invokes this once per hwaccel trial so each attempt sees a pristine
// libavformat probe without depending on a working seek on a shared stream.
using InputStreamFactory = std::function<std::unique_ptr<IInputStream>()>;

// Which pump method the caller should drive for the next frame. Each
// frame is decoded into exactly one of the three concrete views.
enum class FrameKind {
    Sw          = 0,  // call next_frame(Nv12Frame&)
    VulkanShared = 1, // call next_vk_frame(VkFrameView&)
    VaapiDrm    = 2,  // call next_drm_frame(DrmFrameView&)
};

// Mirror of one AVDRMPlaneDescriptor entry — which `object_index` of
// the parent DrmFrameView's `objects[]` it lives in, plus byte offset
// and row pitch into that fd's buffer.
struct DrmPlane {
    std::uint32_t object_index;
    std::uint64_t offset;
    std::uint64_t pitch;
};

// Mirror of one AVDRMLayerDescriptor — describes a logical image
// (drm_fourcc + N planes). NV12 always lands as either:
//   1 layer / 2 planes (Y + UV interleaved into the same DRM_FORMAT_NV12)
//   or 2 layers / 1 plane each (Y as DRM_FORMAT_R8, UV as DRM_FORMAT_GR88).
struct DrmLayer {
    std::uint32_t fourcc;
    std::uint32_t plane_count;
    DrmPlane      planes[4];
};

struct DrmObject {
    int            fd;          // dup'd; owned by VideoDecoder until next pull
    std::uint64_t  size;
    std::uint64_t  format_modifier;
};

// VAAPI surface mapped to DRM_PRIME via av_hwframe_map. Snapshot copy:
// safe to read after the next next_drm_frame() call (but the fds it
// references are unref'd then, so consumers must dup or finish using
// them within the cycle).
struct DrmFrameView {
    std::uint32_t object_count { 0 };
    DrmObject     objects[4]   {};
    std::uint32_t layer_count  { 0 };
    DrmLayer      layers[4]    {};
    std::uint32_t width        { 0 };
    std::uint32_t height       { 0 };
    double        pts_seconds  { -1.0 };
    std::uint32_t colorspace   { 0 };
    std::uint32_t color_range  { 0 };
    std::uint32_t bit_depth    { 8 };
};

class VideoDecoder {
public:
    // Read the native video resolution from the file's first video
    // stream without committing to a decoder.
    static auto probe_native(const std::string& path) -> rstd::Result<ProbeResult, Error>;

    // `target_w`/`target_h` are the wallpaper extent. Both are rounded
    // up to even pixel boundaries (NV12 chroma is 4:2:0). Setting
    // `loop=true` causes EOF to seek back to the start automatically.
    static auto open(const std::string& path,
                     std::uint32_t target_w, std::uint32_t target_h, bool loop)
        -> rstd::Result<std::unique_ptr<VideoDecoder>, Error>;

    // Shared-device variant: bring up an FFmpeg hwdevice (Vulkan and/or
    // VAAPI per `opts.hwaccel`) on top of the Producer's VkInstance.
    //   Auto    → try Vulkan first, then VAAPI, then fall back to sw.
    //   Vulkan  → only Vulkan (then sw).
    //   Vaapi   → only VAAPI (then sw).
    //   None    → sw decode unconditionally.
    // The caller uses `kind()` to discover which pump to drive.
    static auto open_with_vk(const std::string& path,
                             std::uint32_t target_w, std::uint32_t target_h, bool loop,
                             const Producer& vk,
                             const OpenOpts& opts = {})
        -> rstd::Result<std::unique_ptr<VideoDecoder>, Error>;

    // Stream-based open. Wraps a fresh IInputStream from `make_stream` as
    // an AVIOContext and feeds it to libavformat. `vk` is optional; when
    // non-null behaves like open_with_vk (full hwaccel chain), when null
    // behaves like open (sw decode only). `make_stream` may be invoked
    // multiple times — once per hwaccel trial plus once for the final sw
    // fallback. The surviving stream is destroyed when the returned
    // VideoDecoder is destroyed.
    static auto open_from_stream(InputStreamFactory make_stream,
                                 std::uint32_t target_w, std::uint32_t target_h, bool loop,
                                 const Producer* vk = nullptr,
                                 const OpenOpts& opts = {})
        -> rstd::Result<std::unique_ptr<VideoDecoder>, Error>;

    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&)            = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    auto next_frame(Nv12Frame& out) -> rstd::Result<NextFrame, Error>;

    // Which pump matches the active backend. `using_vk_frames()` is the
    // legacy boolean accessor — true iff kind() == VulkanShared.
    FrameKind kind() const           { return kind_; }
    bool      using_vk_frames() const { return kind_ == FrameKind::VulkanShared; }

    auto next_vk_frame(VkFrameView& out) -> rstd::Result<NextFrame, Error>;
    auto next_drm_frame(DrmFrameView& out) -> rstd::Result<NextFrame, Error>;

    std::uint32_t width() const  { return target_w_; }
    std::uint32_t height() const { return target_h_; }
    void          set_loop(bool loop) { loop_ = loop; }

    struct State;

private:
    VideoDecoder() = default;

    // Internal input descriptor: exactly one of `path` (file-based, used
    // by open / open_with_vk) or `stream` (AVIOContext-based, used by
    // open_from_stream) is populated. `stream` is moved into State on
    // success so it outlives the libavformat context.
    struct InputSpec {
        std::string                    path;
        std::unique_ptr<IInputStream>  stream;
    };

    // Internal builder. `pre_built_hwdev` is AVBufferRef* type-erased to
    // void*; `requested_kind` records the hw mode the trial loop picked
    // so the per-frame pump knows which side-data to extract.
    static std::unique_ptr<VideoDecoder>
    build_internal(InputSpec input,
                   std::uint32_t target_w, std::uint32_t target_h,
                   bool loop, void* pre_built_hwdev,
                   FrameKind requested_kind,
                   Error* err);

    // Internal frame-pull helpers using the legacy in/out style. The
    // returned int encodes: 0 = ok, 1 = eof, -1 = error.
    int next_frame_(Nv12Frame& out, Error* err);
    int next_vk_frame_(VkFrameView& out, Error* err);
    int next_drm_frame_(DrmFrameView& out, Error* err);

    std::unique_ptr<State> st_;
    std::uint32_t target_w_ { 0 };
    std::uint32_t target_h_ { 0 };
    bool          loop_     { false };
    FrameKind     kind_     { FrameKind::Sw };
};

} // namespace wavsen::video
