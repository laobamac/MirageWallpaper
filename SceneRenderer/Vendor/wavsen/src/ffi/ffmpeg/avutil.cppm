module;

#include <vulkan/vulkan.h>

extern "C" {
#include <errno.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#if defined(__APPLE__)
#include <libavutil/hwcontext_videotoolbox.h>
#endif
#if defined(WAVSEN_HAS_VAAPI)
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace _wv_avutil {
inline constexpr int           k_AV_TIME_BASE              = AV_TIME_BASE;
inline constexpr int           k_AV_ERROR_MAX_STRING_SIZE  = AV_ERROR_MAX_STRING_SIZE;
inline constexpr int64_t       k_AV_NOPTS_VALUE            = AV_NOPTS_VALUE;
inline constexpr int           k_AVERROR_EOF               = AVERROR_EOF;
inline constexpr int           k_EAGAIN                    = EAGAIN;
// AV_PIX_FMT_P010 / P016 are AV_PIX_FMT_NE-style endian-alias macros
// (NOT enumerators), so they must be captured + #undef'd like ints.
inline constexpr AVPixelFormat k_AV_PIX_FMT_P010           = AV_PIX_FMT_P010;
inline constexpr AVPixelFormat k_AV_PIX_FMT_P016           = AV_PIX_FMT_P016;
}

#undef AV_TIME_BASE
#undef AV_ERROR_MAX_STRING_SIZE
#undef AV_NOPTS_VALUE
#undef AVERROR_EOF
#undef AVERROR
#undef EAGAIN
#undef AV_PIX_FMT_P010
#undef AV_PIX_FMT_P016

export module avutil;

export {

inline constexpr int           AV_TIME_BASE              = _wv_avutil::k_AV_TIME_BASE;
inline constexpr int           AV_ERROR_MAX_STRING_SIZE  = _wv_avutil::k_AV_ERROR_MAX_STRING_SIZE;
inline constexpr int64_t       AV_NOPTS_VALUE            = _wv_avutil::k_AV_NOPTS_VALUE;
inline constexpr int           AVERROR_EOF               = _wv_avutil::k_AVERROR_EOF;
inline constexpr int           EAGAIN                    = _wv_avutil::k_EAGAIN;
inline constexpr AVPixelFormat AV_PIX_FMT_P010           = _wv_avutil::k_AV_PIX_FMT_P010;
inline constexpr AVPixelFormat AV_PIX_FMT_P016           = _wv_avutil::k_AV_PIX_FMT_P016;
inline constexpr int AVERROR(int e) noexcept { return -e; }

using ::AVRational;
using ::AVMediaType;
using ::AVPixelFormat;
using ::AVPixFmtDescriptor;
using ::AVSampleFormat;
using ::AVColorSpace;
using ::AVColorRange;
using ::AVChannelOrder;
using ::AVChannel;
using ::AVChannelLayout;
using ::AVFrame;
using ::AVBufferRef;
using ::AVHWDeviceType;
using ::AVHWDeviceContext;
using ::AVHWFramesContext;
using ::AVVulkanDeviceContext;
using ::AVVulkanDeviceQueueFamily;
using ::AVVulkanFramesContext;
using ::AVVkFrame;
using ::AVVkFrameFlags;
using ::AV_VK_FRAME_FLAG_NONE;
using ::AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
#if defined(WAVSEN_HAS_VAAPI)
using ::AVVAAPIDeviceContext;
using ::AVVAAPIFramesContext;
#endif
using ::AVDRMObjectDescriptor;
using ::AVDRMPlaneDescriptor;
using ::AVDRMLayerDescriptor;
using ::AVDRMFrameDescriptor;
using ::AVDRMDeviceContext;
inline constexpr int AV_DRM_MAX_PLANES_C = ::AV_DRM_MAX_PLANES;

using ::AVMEDIA_TYPE_UNKNOWN;
using ::AVMEDIA_TYPE_VIDEO;
using ::AVMEDIA_TYPE_AUDIO;
using ::AVMEDIA_TYPE_DATA;
using ::AVMEDIA_TYPE_SUBTITLE;

using ::AV_PIX_FMT_NONE;
using ::AV_PIX_FMT_RGBA;
using ::AV_PIX_FMT_NV12;
using ::AV_PIX_FMT_VULKAN;
#if defined(__APPLE__)
using ::AV_PIX_FMT_VIDEOTOOLBOX;
#endif
using ::AV_PIX_FMT_YUV420P;
#if defined(WAVSEN_HAS_VAAPI)
using ::AV_PIX_FMT_VAAPI;
#endif
using ::AV_PIX_FMT_DRM_PRIME;

using ::AVCOL_SPC_BT709;
using ::AVCOL_SPC_BT2020_CL;
using ::AVCOL_SPC_BT2020_NCL;
using ::AVCOL_SPC_BT470BG;
using ::AVCOL_SPC_SMPTE170M;
using ::AVCOL_RANGE_JPEG;
using ::AVCOL_RANGE_MPEG;

using ::AV_HWDEVICE_TYPE_VULKAN;
using ::AV_HWDEVICE_TYPE_DRM;
#if defined(__APPLE__)
using ::AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#endif
#if defined(WAVSEN_HAS_VAAPI)
using ::AV_HWDEVICE_TYPE_VAAPI;
#endif

using ::AV_HWFRAME_MAP_READ;
using ::AV_HWFRAME_MAP_WRITE;
using ::AV_HWFRAME_MAP_OVERWRITE;
using ::AV_HWFRAME_MAP_DIRECT;

using ::AV_CHANNEL_ORDER_UNSPEC;

using ::AV_SAMPLE_FMT_FLT;
using ::AV_SAMPLE_FMT_S16;

using ::av_frame_alloc;
using ::av_frame_free;
using ::av_frame_unref;
using ::av_frame_clone;
using ::av_frame_ref;

using ::av_buffer_ref;
using ::av_buffer_unref;
using ::av_buffer_alloc;

using ::av_channel_layout_copy;
using ::av_channel_layout_default;
using ::av_channel_layout_uninit;

using ::av_hwdevice_ctx_alloc;
using ::av_hwdevice_ctx_create;
using ::av_hwdevice_ctx_init;
using ::av_hwframe_ctx_init;
using ::av_hwframe_transfer_data;
using ::av_hwframe_map;

using ::av_get_pix_fmt_name;
using ::av_pix_fmt_desc_get;

using ::av_malloc;
using ::av_free;
using ::av_freep;

using ::av_image_get_buffer_size;
using ::av_image_fill_arrays;

using ::av_strerror;

// `::av_q2d` is `static inline` in <libavutil/rational.h> — internal
// linkage, so it can't be redeclared at module scope as `av_q2d` or
// re-exported via `using ::av_q2d`. Wrap it under a sub-namespace
// (different qualified name → no redeclaration), out-of-line so the
// `::av_q2d` call resolves in this module's TU and importers reach it
// via a real symbol rather than inline expansion.
namespace ffi {
    double av_q2d(AVRational a) noexcept;
}

}

namespace ffi {
    double av_q2d(AVRational a) noexcept { return ::av_q2d(a); }
}
