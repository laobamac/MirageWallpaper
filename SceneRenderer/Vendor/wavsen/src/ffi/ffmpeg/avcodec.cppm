module;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
}

export module avcodec;

export {

using ::AVCodec;
using ::AVCodecContext;
using ::AVCodecParameters;
using ::AVCodecID;
using ::AVPacket;

using ::avcodec_alloc_context3;
using ::avcodec_default_get_format;
using ::avcodec_find_decoder;
using ::avcodec_find_decoder_by_name;
using ::avcodec_flush_buffers;
using ::avcodec_free_context;
using ::avcodec_get_hw_frames_parameters;
using ::avcodec_get_name;
using ::avcodec_open2;
using ::avcodec_parameters_to_context;
using ::avcodec_receive_frame;
using ::avcodec_send_packet;

using ::av_packet_alloc;
using ::av_packet_free;
using ::av_packet_unref;

}
