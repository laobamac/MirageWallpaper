#define _GNU_SOURCE

#include "codec.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

static void write_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & UINT16_C(0xff));
    out[1] = (uint8_t)((value >> 8) & UINT16_C(0xff));
}

static void write_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & UINT32_C(0xff));
    out[1] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    out[2] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    out[3] = (uint8_t)((value >> 24) & UINT32_C(0xff));
}

static uint16_t read_u16(const uint8_t* in) {
    return (uint16_t)((uint16_t)in[0] | (uint16_t)((uint16_t)in[1] << 8));
}

static uint32_t read_u32(const uint8_t* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

void md_packet_init(md_packet_t* packet) {
    if (packet == NULL) return;
    memset(packet, 0, sizeof(*packet));
    for (size_t i = 0; i < MD_WIRE_MAX_FDS; ++i) packet->fds[i] = -1;
}

void md_packet_close_fds(md_packet_t* packet) {
    if (packet == NULL) return;
    for (size_t i = 0; i < packet->fd_count; ++i) {
        if (packet->fds[i] >= 0) close(packet->fds[i]);
        packet->fds[i] = -1;
    }
    packet->fd_count = 0;
}

int md_codec_send(int fd, uint16_t minor, uint16_t opcode, uint16_t flags,
                  uint32_t serial, const void* payload, size_t payload_size,
                  const int* fds, size_t fd_count) {
    if (fd < 0) return -EBADF;
    if (payload_size > MD_WIRE_MAX_PAYLOAD || fd_count > MD_WIRE_MAX_FDS) return -EMSGSIZE;
    if ((payload_size > 0 && payload == NULL) || (fd_count > 0 && fds == NULL)) return -EINVAL;

    uint8_t header[MD_WIRE_HEADER_SIZE] = {0};
    write_u32(header, MD_WIRE_MAGIC);
    write_u16(header + 4, 1);
    write_u16(header + 6, minor);
    write_u16(header + 8, opcode);
    write_u16(header + 10, flags);
    write_u32(header + 12, (uint32_t)payload_size);
    write_u16(header + 16, (uint16_t)fd_count);
    write_u16(header + 18, 0);
    write_u32(header + 20, serial);

    struct iovec iov[2] = {
        {.iov_base = header, .iov_len = sizeof(header)},
        {.iov_base = (void*)payload, .iov_len = payload_size},
    };

    union {
        char bytes[CMSG_SPACE(sizeof(int) * MD_WIRE_MAX_FDS)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = payload_size > 0 ? 2u : 1u;

    if (fd_count > 0) {
        msg.msg_control = control.bytes;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }

    ssize_t result;
    do {
        result = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -errno;
    }
    const size_t expected = MD_WIRE_HEADER_SIZE + payload_size;
    return (size_t)result == expected ? 0 : -EIO;
}

int md_codec_recv(int fd, md_packet_t* packet) {
    if (fd < 0 || packet == NULL) return -EINVAL;
    md_packet_init(packet);

    uint8_t raw[MD_WIRE_MAX_PACKET];
    union {
        char bytes[CMSG_SPACE(sizeof(int) * MD_WIRE_MAX_FDS)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof(control));

    struct iovec iov = {.iov_base = raw, .iov_len = sizeof(raw)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);

    ssize_t result;
    do {
        result = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -errno;
    }
    if (result == 0) return -ECONNRESET;

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        const char* control_begin = control.bytes;
        const char* control_end = control.bytes + msg.msg_controllen;
        const char* cmsg_end = (const char*)cmsg + cmsg->cmsg_len;
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg_end > control_end ||
            (const char*)cmsg < control_begin) {
            md_packet_close_fds(packet);
            return -EPROTO;
        }
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0) {
            md_packet_close_fds(packet);
            return -EPROTO;
        }
        size_t count = bytes / sizeof(int);
        if (packet->fd_count + count > MD_WIRE_MAX_FDS) {
            const int* received = (const int*)CMSG_DATA(cmsg);
            for (size_t i = 0; i < count; ++i) close(received[i]);
            md_packet_close_fds(packet);
            return -EMSGSIZE;
        }
        memcpy(packet->fds + packet->fd_count, CMSG_DATA(cmsg), bytes);
        packet->fd_count += count;
    }

    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        md_packet_close_fds(packet);
        return -EMSGSIZE;
    }
    if ((size_t)result < MD_WIRE_HEADER_SIZE) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }

    if (read_u32(raw) != MD_WIRE_MAGIC || read_u16(raw + 4) != 1 || read_u16(raw + 18) != 0) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }

    const uint32_t payload_size = read_u32(raw + 12);
    const uint16_t declared_fds = read_u16(raw + 16);
    if (payload_size > MD_WIRE_MAX_PAYLOAD ||
        (size_t)result != MD_WIRE_HEADER_SIZE + (size_t)payload_size ||
        (size_t)declared_fds != packet->fd_count) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }

    packet->major = read_u16(raw + 4);
    packet->minor = read_u16(raw + 6);
    packet->opcode = read_u16(raw + 8);
    packet->flags = read_u16(raw + 10);
    packet->serial = read_u32(raw + 20);
    packet->payload_size = payload_size;
    if (payload_size > 0) memcpy(packet->payload, raw + MD_WIRE_HEADER_SIZE, payload_size);
    return 1;
}
