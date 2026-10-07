#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>

static const char copyright_notice[] = "Copyright © 2026 王孝慈. All rights reserved.";

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    AVFormatContext *format = NULL;
    if (avformat_open_input(&format, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(format, NULL) < 0) return 3;
    const AVCodec *codec = NULL;
    int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0) index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0 || !codec) return 4;
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    if (!decoder || avcodec_parameters_to_context(decoder, format->streams[index]->codecpar) < 0 || avcodec_open2(decoder, codec, NULL) < 0) return 5;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int count = 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index) {
            if (avcodec_send_packet(decoder, packet) < 0) return 6;
            int result;
            while ((result = avcodec_receive_frame(decoder, frame)) >= 0) { ++count; av_frame_unref(frame); }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) return 7;
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder, NULL);
    while (avcodec_receive_frame(decoder, frame) >= 0) { ++count; av_frame_unref(frame); }
    printf("%s: %d decoded frames\n", codec->name, count);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return count > 0 ? 0 : 8;
}
