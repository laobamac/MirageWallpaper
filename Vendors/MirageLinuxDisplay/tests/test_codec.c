#define _GNU_SOURCE

#include "codec.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_round_trip_with_fd(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);

    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
    assert(md_codec_send(sockets[0], 0, 0x1234, MD_PACKET_OPTIONAL, 77,
                         payload, sizeof(payload), &pipe_fds[0], 1) == 0);

    md_packet_t packet;
    assert(md_codec_recv(sockets[1], &packet) == 1);
    assert(packet.major == 1);
    assert(packet.minor == 0);
    assert(packet.opcode == 0x1234);
    assert(packet.flags == MD_PACKET_OPTIONAL);
    assert(packet.serial == 77);
    assert(packet.payload_size == sizeof(payload));
    assert(memcmp(packet.payload, payload, sizeof(payload)) == 0);
    assert(packet.fd_count == 1);
    assert(packet.fds[0] >= 0);
    assert((fcntl(packet.fds[0], F_GETFD) & FD_CLOEXEC) != 0);

    md_packet_close_fds(&packet);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(sockets[0]);
    close(sockets[1]);
}

static void write_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void test_declared_fd_mismatch(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    uint8_t header[MD_WIRE_HEADER_SIZE] = {0};
    write_u32(header, MD_WIRE_MAGIC);
    write_u16(header + 4, 1);
    write_u16(header + 6, 0);
    write_u16(header + 8, 9);
    write_u32(header + 12, 0);
    write_u16(header + 16, 1);
    write_u32(header + 20, 1);
    struct iovec iov = {.iov_base = header, .iov_len = sizeof(header)};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    ssize_t sent = sendmsg(sockets[0], &message, MSG_NOSIGNAL);
    if (sent != (ssize_t)sizeof(header)) perror("sendmsg malformed header");
    assert(sent == (ssize_t)sizeof(header));

    md_packet_t packet;
    assert(md_codec_recv(sockets[1], &packet) == -EPROTO);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_would_block(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    md_packet_t packet;
    assert(md_codec_recv(sockets[0], &packet) == 0);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void) {
    test_round_trip_with_fd();
    test_declared_fd_mismatch();
    test_would_block();
    return 0;
}
