module;

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

namespace _wv_avformat {
inline constexpr int k_AVSEEK_FLAG_BACKWARD  = AVSEEK_FLAG_BACKWARD;
inline constexpr int k_AVSEEK_FLAG_ANY       = AVSEEK_FLAG_ANY;
inline constexpr int k_AVSEEK_SIZE           = AVSEEK_SIZE;
inline constexpr int k_AVFMT_FLAG_CUSTOM_IO  = AVFMT_FLAG_CUSTOM_IO;
}

#undef AVSEEK_FLAG_BACKWARD
#undef AVSEEK_FLAG_ANY
#undef AVSEEK_SIZE
#undef AVFMT_FLAG_CUSTOM_IO

export module avformat;

export {

inline constexpr int AVSEEK_FLAG_BACKWARD  = _wv_avformat::k_AVSEEK_FLAG_BACKWARD;
inline constexpr int AVSEEK_FLAG_ANY       = _wv_avformat::k_AVSEEK_FLAG_ANY;
inline constexpr int AVSEEK_SIZE           = _wv_avformat::k_AVSEEK_SIZE;
inline constexpr int AVFMT_FLAG_CUSTOM_IO  = _wv_avformat::k_AVFMT_FLAG_CUSTOM_IO;

using ::AVFormatContext;
using ::AVStream;
using ::AVIOContext;

using ::avformat_alloc_context;
using ::avformat_close_input;
using ::avformat_find_stream_info;
using ::avformat_open_input;
using ::av_find_best_stream;
using ::av_read_frame;
using ::av_seek_frame;

using ::avio_alloc_context;
using ::avio_context_free;

}
