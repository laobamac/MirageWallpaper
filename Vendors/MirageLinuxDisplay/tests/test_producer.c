#define _GNU_SOURCE

#include "mirage_display_producer.h"

#include "codec.h"
#include "protocol.h"

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
#include <unistd.h>

typedef struct mock_broker {
    int server_fd;
    int client_fd;
    pthread_t thread;
    bool saw_offer;
    bool saw_frame;
    bool saw_config;
    bool saw_retire_done;
} mock_broker_t;

typedef struct observer {
    unsigned connected;
    unsigned configs;
    unsigned retire;
    unsigned enter;
    unsigned leave;
    unsigned motion;
    unsigned button;
    unsigned axis;
    uint64_t producer_id;
    uint64_t output_id;
} observer_t;

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

static void send_packet(int fd, uint16_t opcode, uint32_t serial,
                        const uint8_t* data, size_t size) {
    assert(md_codec_send(fd, 0, opcode, 0, serial, data, size, NULL, 0) == 0);
}

static void send_welcome(int fd) {
    uint8_t payload[256];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u64(&writer, MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                  MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS) == 0);
    assert(md_write_string(&writer, "mock-broker") == 0);
    assert(md_write_string(&writer, "0.1") == 0);
    send_packet(fd, MD_OP_WELCOME, 1, payload, writer.size);
}

static void send_config(int fd) {
    uint8_t payload[64];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    assert(md_write_u32(&writer, 1920) == 0);
    assert(md_write_u32(&writer, 1080) == 0);
    assert(md_write_u32(&writer, 60000) == 0);
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_u32(&writer, UINT32_C(0x34325258)) == 0);
    assert(md_write_u32(&writer, 1) == 0);
    assert(md_write_u64(&writer, 0) == 0);
    send_packet(fd, MD_OP_OUTPUT_CONFIG, 4, payload, writer.size);
}

static void send_inputs(int fd) {
    uint8_t payload[64];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_proto_encode_pointer_enter(&writer, 10.0f, 20.0f, 1) == 0);
    send_packet(fd, MD_OP_PRODUCER_POINTER_ENTER, 5, payload, writer.size);
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_proto_encode_pointer_leave(&writer, 2) == 0);
    send_packet(fd, MD_OP_PRODUCER_POINTER_LEAVE, 6, payload, writer.size);
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_proto_encode_pointer_motion(&writer, 11.0f, 21.0f, 3, 4) == 0);
    send_packet(fd, MD_OP_PRODUCER_POINTER_MOTION, 7, payload, writer.size);
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_proto_encode_pointer_button(&writer, 11.0f, 21.0f, 0x110,
                                          MD_BUTTON_PRESSED, 4, 0) == 0);
    send_packet(fd, MD_OP_PRODUCER_POINTER_BUTTON, 8, payload, writer.size);
    md_writer_init(&writer, payload, sizeof(payload));
    assert(md_proto_encode_pointer_axis(&writer, 11.0f, 21.0f, 0.0f, 1.0f,
                                        MD_AXIS_WHEEL, 5, 0) == 0);
    send_packet(fd, MD_OP_PRODUCER_POINTER_AXIS, 9, payload, writer.size);
}

static void* broker_main(void* opaque) {
    mock_broker_t* broker = opaque;
    int fd = broker->server_fd;
    md_packet_t packet = receive_opcode(fd, MD_OP_HELLO);
    md_reader_t reader; md_reader_init(&reader, packet.payload, packet.payload_size);
    uint32_t role;
    assert(md_read_u32(&reader, &role) == 0 && role == 2);
    md_packet_close_fds(&packet);
    send_welcome(fd);

    packet = receive_opcode(fd, MD_OP_REGISTER_PRODUCER);
    md_packet_close_fds(&packet);
    uint8_t accepted[16];
    md_writer_t writer; md_writer_init(&writer, accepted, sizeof(accepted));
    assert(md_write_u64(&writer, 10) == 0);
    assert(md_write_u64(&writer, 20) == 0);
    send_packet(fd, MD_OP_PRODUCER_ACCEPTED, 2, accepted, writer.size);

    packet = receive_opcode(fd, MD_OP_OFFER_BUFFERS);
    assert(packet.fd_count == 3);
    broker->saw_offer = true;
    md_packet_close_fds(&packet);
    packet = receive_opcode(fd, MD_OP_PRODUCER_SET_CONFIG);
    assert(packet.fd_count == 0);
    md_display_config_t producer_config;
    assert(md_proto_decode_config(packet.payload, packet.payload_size, &producer_config) == 0);
    assert(producer_config.clear_color[2] == 0.3f);
    broker->saw_config = true;
    md_packet_close_fds(&packet);
    packet = receive_opcode(fd, MD_OP_PRODUCER_FRAME);
    assert(packet.fd_count == 2);
    broker->saw_frame = true;
    md_packet_close_fds(&packet);

    send_config(fd);
    send_inputs(fd);
    uint8_t retire[8];
    md_writer_init(&writer, retire, sizeof(retire));
    assert(md_write_u64(&writer, 7) == 0);
    send_packet(fd, MD_OP_RETIRE_BUFFERS, 10, retire, writer.size);

    packet = receive_opcode(fd, MD_OP_RETIRE_DONE);
    broker->saw_retire_done = true;
    md_packet_close_fds(&packet);
    close(fd);
    return NULL;
}

static void on_connected(void* opaque, uint64_t producer_id, uint64_t output_id) {
    observer_t* observer = opaque;
    ++observer->connected;
    observer->producer_id = producer_id;
    observer->output_id = output_id;
}
static void on_config(void* opaque, const md_producer_config_t* config) {
    observer_t* observer = opaque;
    ++observer->configs;
    assert(config->physical_width == 1920);
    assert(config->fourcc == UINT32_C(0x34325258));
}
static void on_retire(void* opaque, uint64_t generation) {
    observer_t* observer = opaque;
    ++observer->retire;
    assert(generation == 7);
}
static void on_enter(void* opaque, const md_pointer_enter_t* event) {
    observer_t* observer = opaque; ++observer->enter; assert(event->x == 10.0f);
}
static void on_leave(void* opaque, uint64_t timestamp) {
    observer_t* observer = opaque; ++observer->leave; assert(timestamp == 2);
}
static void on_motion(void* opaque, const md_pointer_motion_t* event) {
    observer_t* observer = opaque; ++observer->motion; assert(event->modifiers == 4);
}
static void on_button(void* opaque, const md_pointer_button_t* event) {
    observer_t* observer = opaque; ++observer->button; assert(event->button == 0x110);
}
static void on_axis(void* opaque, const md_pointer_axis_t* event) {
    observer_t* observer = opaque; ++observer->axis; assert(event->delta_y == 1.0f);
}

int main(void) {
    mock_broker_t broker = {0};
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    broker.server_fd = sockets[0];
    broker.client_fd = sockets[1];
    assert(pthread_create(&broker.thread, NULL, broker_main, &broker) == 0);

    observer_t observer = {0};
    md_producer_callbacks_t callbacks = {
        .on_connected = on_connected,
        .on_output_config = on_config,
        .on_retire_buffers = on_retire,
        .on_pointer_enter = on_enter,
        .on_pointer_leave = on_leave,
        .on_pointer_motion = on_motion,
        .on_pointer_button = on_button,
        .on_pointer_axis = on_axis,
        .user_data = &observer,
    };
    md_producer_t* producer = md_producer_new(&callbacks);
    assert(producer != NULL);
    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258), .plane_count = 1, .modifier = 0,
    };
    md_producer_info_t info = {
        .stable_output_id = "test-output",
        .kind = "scene",
        .formats = &format,
        .format_count = 1,
    };
    assert(md_producer_begin_connected_fd(producer, broker.client_fd, "test-producer", "0.1",
                                          &info) == MD_OK);
    broker.client_fd = -1;
    for (;;) {
        int handshake = md_producer_advance_handshake(producer);
        if (handshake == MD_HANDSHAKE_DONE) break;
        assert(handshake > 0);
        if (handshake == MD_HANDSHAKE_PROGRESS) continue;
        struct pollfd pfd = {
            .fd = md_producer_get_fd(producer),
            .events = handshake == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN,
            .revents = 0,
        };
        assert(poll(&pfd, 1, 3000) == 1);
    }
    assert(observer.connected == 1);
    assert(observer.producer_id == 10 && observer.output_id == 20);

    md_buffer_pool_t pool = {0};
    pool.generation = 7;
    pool.buffer_count = 3;
    pool.width = 1920;
    pool.height = 1080;
    pool.fourcc = UINT32_C(0x34325258);
    pool.plane_count = 1;
    for (uint32_t i = 0; i < 3; ++i) {
        pool.planes[i][0].fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        pool.planes[i][0].stride = 7680;
        pool.planes[i][0].size = UINT64_C(8294400);
        assert(pool.planes[i][0].fd >= 0);
    }
    assert(md_producer_offer_buffers(producer, &pool) == MD_OK);
    for (uint32_t i = 0; i < 3; ++i) close(pool.planes[i][0].fd);

    md_display_config_t config = {
        .generation = 1,
        .source = {.x = 0.0f, .y = 0.0f, .width = 1920.0f, .height = 1080.0f},
        .destination = {.x = 0.0f, .y = 0.0f, .width = 1920.0f, .height = 1080.0f},
        .transform = MD_TRANSFORM_NORMAL,
        .clear_color = {0.1f, 0.2f, 0.3f, 1.0f},
    };
    assert(md_producer_set_config(producer, &config) == MD_OK);

    int frame_fds[2];
    assert(pipe2(frame_fds, O_CLOEXEC) == 0);
    int release_fd = frame_fds[0];
    int acquire_fd = frame_fds[1];
    assert(md_producer_submit_frame(producer, 7, 1, 99, acquire_fd, release_fd) == MD_OK);
    assert(fcntl(acquire_fd, F_GETFD) == -1);
    assert(fcntl(release_fd, F_GETFD) == -1);

    while (observer.retire == 0) {
        struct pollfd pfd = {.fd = md_producer_get_fd(producer), .events = POLLIN, .revents = 0};
        assert(poll(&pfd, 1, 3000) == 1);
        assert(md_producer_dispatch(producer) >= 0);
    }
    assert(observer.configs == 1);
    assert(observer.enter == 1 && observer.leave == 1 && observer.motion == 1);
    assert(observer.button == 1 && observer.axis == 1);
    assert(md_producer_retire_done(producer, 7) == MD_OK);

    assert(pthread_join(broker.thread, NULL) == 0);
    assert(broker.saw_offer && broker.saw_config && broker.saw_frame && broker.saw_retire_done);
    md_producer_close(producer);
    md_producer_free(producer);
    if (broker.client_fd >= 0) close(broker.client_fd);
    return 0;
}
