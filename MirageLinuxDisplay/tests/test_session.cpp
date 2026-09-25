#include "mirage_display.h"

#include "codec.hpp"
#include "protocol.hpp"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Unit tests for the shared client handshake state machine and connection
 * lifecycle.
 */

typedef struct mock_broker {
    int server_fd{};
    int client_fd{};
    pthread_t thread{};
    bool saw_motion{};
    bool saw_button{};
    bool saw_axis{};
    bool saw_window_state{};
    bool saw_unbind_done{};
} mock_broker_t;

typedef struct client_observer {
    md_display_t* display{};
    unsigned connected{};
    unsigned buffers_ready{};
    unsigned configs{};
    unsigned frames{};
    unsigned buffers_releasing{};
    unsigned disconnected{};
    uint64_t output_id{};
} client_observer_t;

static void send_writer(int fd, uint16_t opcode, uint32_t serial, md_writer_t* writer,
                        const int* fds, size_t fd_count) {
    const uint16_t minor = opcode == MD_OP_WELCOME ? 0U : MIRAGE_DISPLAY_PROTOCOL_MINOR;
    assert(md_codec_send(fd, minor, opcode, 0, serial, writer->data, writer->size,
                         fds, fd_count) == 0);
}

static md_packet_t receive_opcode(int fd, uint16_t opcode) {
    md_packet_t packet;
    for (;;) {
        int rc = md_codec_recv(fd, &packet);
        if (rc == 1) break;
        assert(rc == 0);
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        assert(poll(&pfd, 1, 3000) == 1);
    }
    assert(packet.opcode == opcode);
    return packet;
}

static void* broker_main(void* opaque) {
    mock_broker_t* const broker = static_cast<mock_broker_t*>(opaque);
    int client = broker->server_fd;

    md_packet_t packet = receive_opcode(client, MD_OP_HELLO);
    assert(packet.fd_count == 0);
    md_reader_t reader;
    md_reader_init(&reader, packet.payload, packet.payload_size);
    uint32_t role;
    assert(md_read_u32(&reader, &role) == 0 && role == 1);
    md_packet_close_fds(&packet);

    uint8_t payload[1024];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u16(&writer, MIRAGE_DISPLAY_PROTOCOL_MINOR) == 0);
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u64(&writer, MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                  MD_FEATURE_POINTER_AXIS | MD_FEATURE_WINDOW_STATE) == 0);
    assert(md_write_string(&writer, "mock-broker") == 0);
    assert(md_write_string(&writer, "0.2") == 0);
    send_writer(client, MD_OP_WELCOME, 1, &writer, NULL, 0);

    packet = receive_opcode(client, MD_OP_REGISTER_OUTPUT);
    md_packet_close_fds(&packet);

    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u64(&writer, 42) == 0);
    send_writer(client, MD_OP_OUTPUT_ACCEPTED, 2, &writer, NULL, 0);

    packet = receive_opcode(client, MD_OP_CONSUMER_CAPS);
    md_packet_close_fds(&packet);

    int buffer_fds[3];
    for (size_t i = 0; i < 3; ++i) {
        buffer_fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(buffer_fds[i] >= 0);
    }
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u64(&writer, 7) == 0);
    assert(md_write_u32(&writer, 3) == 0);
    assert(md_write_u32(&writer, 1920) == 0);
    assert(md_write_u32(&writer, 1080) == 0);
    assert(md_write_u32(&writer, UINT32_C(0x34325258)) == 0);
    assert(md_write_u32(&writer, 1) == 0);
    assert(md_write_u64(&writer, 0) == 0);
    assert(md_write_u32(&writer, 3) == 0);
    for (size_t i = 0; i < 3; ++i) {
        assert(md_write_u32(&writer, 7680) == 0);
        assert(md_write_u32(&writer, 0) == 0);
        assert(md_write_u64(&writer, UINT64_C(8294400)) == 0);
    }
    send_writer(client, MD_OP_BIND_BUFFERS, 3, &writer, buffer_fds, 3);
    for (size_t i = 0; i < 3; ++i) close(buffer_fds[i]);

    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u64(&writer, 1) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 1920.0f) == 0);
    assert(md_write_f32(&writer, 1080.0f) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 1920.0f) == 0);
    assert(md_write_f32(&writer, 1080.0f) == 0);
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_f32(&writer, 0.1f) == 0);
    assert(md_write_f32(&writer, 0.2f) == 0);
    assert(md_write_f32(&writer, 0.3f) == 0);
    assert(md_write_f32(&writer, 1.0f) == 0);
    send_writer(client, MD_OP_SET_CONFIG, 4, &writer, NULL, 0);

    int frame_fds[2];
    for (size_t i = 0; i < 2; ++i) {
        frame_fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(frame_fds[i] >= 0);
    }
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u64(&writer, 7) == 0);
    assert(md_write_u32(&writer, 1) == 0);
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_u64(&writer, 99) == 0);
    send_writer(client, MD_OP_FRAME_READY, 5, &writer, frame_fds, 2);
    assert(close(frame_fds[0]) == 0);
    assert(close(frame_fds[1]) == 0);

    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u64(&writer, 7) == 0);
    send_writer(client, MD_OP_UNBIND, 6, &writer, NULL, 0);

    while (!broker->saw_unbind_done) {
        struct pollfd pfd = {.fd = client, .events = POLLIN, .revents = 0};
        assert(poll(&pfd, 1, 3000) == 1);
        int rc = md_codec_recv(client, &packet);
        assert(rc == 1);
        switch (packet.opcode) {
        case MD_OP_POINTER_MOTION: broker->saw_motion = true; break;
        case MD_OP_POINTER_BUTTON: broker->saw_button = true; break;
        case MD_OP_POINTER_AXIS: broker->saw_axis = true; break;
        case MD_OP_WINDOW_STATE: broker->saw_window_state = true; break;
        case MD_OP_UNBIND_DONE: broker->saw_unbind_done = true; break;
        default: assert(false);
        }
        md_packet_close_fds(&packet);
    }

    assert(broker->saw_motion);
    assert(broker->saw_button);
    assert(broker->saw_axis);
    assert(broker->saw_window_state);
    assert(close(client) == 0);
    return nullptr;
}

static void broker_start(mock_broker_t* broker) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) == 0);
    broker->server_fd = sockets[0];
    broker->client_fd = sockets[1];
    assert(pthread_create(&broker->thread, NULL, broker_main, broker) == 0);
}

static void on_connected(void* opaque, uint64_t output_id) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->connected;
    observer->output_id = output_id;
}

static void on_buffers_ready(void* opaque, const md_buffer_pool_t* pool) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->buffers_ready;
    assert(pool->generation == 7);
    assert(pool->buffer_count == 3);
    assert(pool->planes[0][0].fd >= 0);
}

static void on_buffers_releasing(void* opaque, const md_buffer_pool_t* pool) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->buffers_releasing;
    assert(pool->generation == 7);
    assert(pool->planes[2][0].fd >= 0);
    assert(md_display_defer_unbind(observer->display) == MD_OK);
}

static void on_config(void* opaque, const md_display_config_t* config) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->configs;
    assert(config->destination.width == 1920.0f);
    assert(config->clear_color[3] == 1.0f);
}

static void on_frame(void* opaque, const md_frame_t* frame) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->frames;
    assert(frame->buffer_generation == 7);
    assert(frame->buffer_index == 1);
    assert(frame->sequence == 99);
    assert(frame->acquire_sync_fd >= 0);
    assert(frame->release_syncobj_fd >= 0);
    assert(close(frame->acquire_sync_fd) == 0);
    assert(close(frame->release_syncobj_fd) == 0);
}

static void on_disconnected(void* opaque, md_result_t, const char*) {
    client_observer_t* const observer = static_cast<client_observer_t*>(opaque);
    ++observer->disconnected;
}

int main(void) {
    mock_broker_t broker{};
    broker_start(&broker);

    client_observer_t observer{};
    md_display_callbacks_t callbacks{};
    callbacks.on_connected = on_connected;
    callbacks.on_buffers_ready = on_buffers_ready;
    callbacks.on_buffers_releasing = on_buffers_releasing;
    callbacks.on_config = on_config;
    callbacks.on_frame = on_frame;
    callbacks.on_disconnected = on_disconnected;
    callbacks.user_data = &observer;
    md_display_t* display = md_display_new(&callbacks);
    assert(display != NULL);
    observer.display = display;
    assert(md_display_defer_unbind(display) == MD_ERR_STATE);
    assert(md_display_finish_unbind(display, 7) == MD_ERR_STATE);

    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258),
        .plane_count = 1,
        .modifier = 0,
    };
    md_output_info_t output{};
    output.stable_id = "test-output";
    output.name = "Mock Display";
    output.physical_width = 1920U;
    output.physical_height = 1080U;
    output.logical_width = 1920U;
    output.logical_height = 1080U;
    output.scale_120 = 120U;
    output.refresh_mhz = 60000U;
    output.transform = MD_TRANSFORM_NORMAL;
    output.drm_render_major = 226U;
    output.drm_render_minor = 128U;
    output.input_caps = MD_INPUT_POINTER_MOTION | MD_INPUT_POINTER_BUTTON |
                        MD_INPUT_POINTER_AXIS | MD_INPUT_NON_CONSUMING;
    md_consumer_caps_t caps{};
    caps.features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_POINTER_AXIS | MD_FEATURE_WINDOW_STATE;
    caps.sync_caps = 1U;
    caps.max_width = 8192U;
    caps.max_height = 8192U;
    caps.formats = &format;
    caps.format_count = 1U;

    md_result_t begin_rc = md_display_begin_connected_fd(display, broker.client_fd,
                                                          "test-client", "0.2",
                                                          &output, &caps);
    if (begin_rc != MD_OK) fprintf(stderr, "begin connected fd failed: %d\n", begin_rc);
    assert(begin_rc == MD_OK);
    broker.client_fd = -1;
    for (;;) {
        int32_t handshake = md_display_advance_handshake(display);
        if (handshake == MD_HANDSHAKE_DONE) break;
        assert(handshake > 0);
        if (handshake == MD_HANDSHAKE_PROGRESS) continue;
        struct pollfd pfd{
            .fd = md_display_get_fd(display),
            .events = static_cast<short>(handshake == MD_HANDSHAKE_NEED_WRITE
                                             ? POLLOUT
                                             : POLLIN),
            .revents = 0,
        };
        assert(poll(&pfd, 1, 3000) == 1);
    }
    assert(observer.connected == 1);
    assert(observer.output_id == 42);

    assert(md_display_send_pointer_motion(display, 100.0f, 200.0f, 1000, 0) == MD_OK);
    assert(md_display_send_pointer_button(display, 100.0f, 200.0f, 0x110,
                                          MD_BUTTON_PRESSED, 1001, 0) == MD_OK);
    assert(md_display_send_pointer_axis(display, 100.0f, 200.0f, 0.0f, 1.0f,
                                        MD_AXIS_WHEEL, 1002, 0) == MD_OK);
    assert(md_display_send_window_state(display, 3) == MD_OK);

    while (observer.buffers_releasing == 0) {
        struct pollfd pfd = {.fd = md_display_get_fd(display), .events = POLLIN, .revents = 0};
        assert(poll(&pfd, 1, 3000) == 1);
        assert(md_display_dispatch(display) >= 0);
    }

    assert(observer.buffers_ready == 1);
    assert(observer.configs == 1);
    assert(observer.frames == 1);
    assert(observer.buffers_releasing == 1);
    assert(observer.disconnected == 0);
    assert(md_display_pending_unbind_generation(display) == 7);
    assert(md_display_finish_unbind(display, 8) == MD_ERR_STATE);
    assert(md_display_finish_unbind(display, 7) == MD_OK);
    assert(md_display_pending_unbind_generation(display) == 0);

    assert(pthread_join(broker.thread, NULL) == 0);
    md_display_close(display);
    md_display_free(display);
    if (broker.client_fd >= 0) close(broker.client_fd);
    return 0;
}
