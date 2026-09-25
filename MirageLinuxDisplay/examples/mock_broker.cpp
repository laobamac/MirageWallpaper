#include "mirage_display.h"

#include "codec.hpp"
#include "protocol.hpp"

/* Keep assertions live even in Release builds (-DNDEBUG). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Mock mirage-display-v1 broker: exercises the codec and protocol encoders to
 * serve WELCOME/BIND_BUFFERS/FRAME_READY to a real consumer, for protocol
 * simulation and tests.
 */

static md_packet_t receive_packet(int fd) {
    md_packet_t packet;
    for (;;) {
        int rc = md_codec_recv(fd, &packet);
        if (rc == 1) return packet;
        assert(rc == 0);
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        assert(poll(&pfd, 1, -1) == 1);
    }
}

static void send_payload(int fd, uint16_t opcode, uint32_t serial,
                         const uint8_t* data, size_t size, const int* fds, size_t fd_count) {
    assert(md_codec_send(fd, 0, opcode, 0, serial, data, size, fds, fd_count) == 0);
}

static void send_welcome(int fd) {
    uint8_t data[512];
    md_writer_t writer;
    md_writer_init(&writer, data, sizeof(data));
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u16(&writer, 0) == 0);
    assert(md_write_u64(&writer, MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                  MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS) == 0);
    assert(md_write_string(&writer, "mirage-mock-broker") == 0);
    assert(md_write_string(&writer, "0.1") == 0);
    send_payload(fd, MD_OP_WELCOME, 1, data, writer.size, NULL, 0);
}

static void send_buffers(int fd) {
    uint8_t data[512];
    md_writer_t writer;
    md_writer_init(&writer, data, sizeof(data));
    assert(md_write_u64(&writer, 1) == 0);
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
    int fds[3];
    for (size_t i = 0; i < 3; ++i) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fds[i] >= 0);
    }
    send_payload(fd, MD_OP_BIND_BUFFERS, 3, data, writer.size, fds, 3);
    for (size_t i = 0; i < 3; ++i) close(fds[i]);
}

static void send_config(int fd) {
    uint8_t data[256];
    md_writer_t writer;
    md_writer_init(&writer, data, sizeof(data));
    assert(md_write_u64(&writer, 1) == 0);
    for (int i = 0; i < 2; ++i) {
        assert(md_write_f32(&writer, 0.0f) == 0);
        assert(md_write_f32(&writer, 0.0f) == 0);
        assert(md_write_f32(&writer, 1920.0f) == 0);
        assert(md_write_f32(&writer, 1080.0f) == 0);
    }
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 0.0f) == 0);
    assert(md_write_f32(&writer, 1.0f) == 0);
    send_payload(fd, MD_OP_SET_CONFIG, 4, data, writer.size, NULL, 0);
}

static void send_frame(int fd) {
    uint8_t data[64];
    md_writer_t writer;
    md_writer_init(&writer, data, sizeof(data));
    assert(md_write_u64(&writer, 1) == 0);
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_u32(&writer, 0) == 0);
    assert(md_write_u64(&writer, 1) == 0);
    int fds[2];
    for (size_t i = 0; i < 2; ++i) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fds[i] >= 0);
    }
    send_payload(fd, MD_OP_FRAME_READY, 5, data, writer.size, fds, 2);
    close(fds[0]);
    close(fds[1]);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s SOCKET_PATH\n", argv[0]);
        return 2;
    }
    unlink(argv[1]);
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(listen_fd >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(address.sun_path)) return 2;
    memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1u);
    assert(bind(listen_fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(chmod(argv[1], 0600) == 0);
    assert(listen(listen_fd, 1) == 0);
    printf("mock broker listening at %s\n", argv[1]);
    fflush(stdout);

    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    assert(client_fd >= 0);
    md_packet_t packet = receive_packet(client_fd);
    assert(packet.opcode == MD_OP_HELLO && packet.fd_count == 0);
    md_packet_close_fds(&packet);
    send_welcome(client_fd);

    packet = receive_packet(client_fd);
    assert(packet.opcode == MD_OP_REGISTER_OUTPUT && packet.fd_count == 0);
    md_packet_close_fds(&packet);
    uint8_t accepted[8];
    md_writer_t accepted_writer;
    md_writer_init(&accepted_writer, accepted, sizeof(accepted));
    assert(md_write_u64(&accepted_writer, 1) == 0);
    send_payload(client_fd, MD_OP_OUTPUT_ACCEPTED, 2, accepted, accepted_writer.size, NULL, 0);

    packet = receive_packet(client_fd);
    assert(packet.opcode == MD_OP_CONSUMER_CAPS && packet.fd_count == 0);
    md_packet_close_fds(&packet);
    send_buffers(client_fd);
    send_config(client_fd);
    send_frame(client_fd);
    uint8_t unbind[8];
    md_writer_t unbind_writer;
    md_writer_init(&unbind_writer, unbind, sizeof(unbind));
    assert(md_write_u64(&unbind_writer, 1) == 0);
    send_payload(client_fd, MD_OP_UNBIND, 6, unbind, unbind_writer.size, NULL, 0);
    printf("sent one buffer pool, config and frame\n");
    fflush(stdout);

    bool unbound = false;
    bool done = false;
    while (!done) {
        packet = receive_packet(client_fd);
        switch (packet.opcode) {
        case MD_OP_POINTER_MOTION:
        case MD_OP_POINTER_BUTTON:
        case MD_OP_POINTER_AXIS:
        case MD_OP_WINDOW_STATE:
            printf("received input opcode=0x%04x\n", packet.opcode);
            break;
        case MD_OP_GOODBYE:
            done = true;
            break;
        case MD_OP_UNBIND_DONE:
            unbound = true;
            printf("consumer acknowledged unbind\n");
            break;
        default:
            fprintf(stderr, "received opcode=0x%04x\n", packet.opcode);
            break;
        }
        md_packet_close_fds(&packet);
    }
    assert(unbound || done);
    close(client_fd);
    close(listen_fd);
    unlink(argv[1]);
    return 0;
}
