#include "codec.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * Implementation of the fixed 24-byte packet header codec and the SCM_RIGHTS
 * send/recv helpers.
 *
 * The 65536-byte packet cap lets the payload array live inside md_packet_t
 * without heap allocation.  Recv returns exactly one ordered packet per call on
 * the nonblocking SOCK_SEQPACKET socket; send duplicates nothing and transfers
 * descriptor ownership to the kernel on success.
 */

namespace {

void write_u16(std::uint8_t* const out, const std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & UINT16_C(0xff));
    out[1] = static_cast<std::uint8_t>((value >> 8U) & UINT16_C(0xff));
}

void write_u32(std::uint8_t* const out, const std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & UINT32_C(0xff));
    out[1] = static_cast<std::uint8_t>((value >> 8U) & UINT32_C(0xff));
    out[2] = static_cast<std::uint8_t>((value >> 16U) & UINT32_C(0xff));
    out[3] = static_cast<std::uint8_t>((value >> 24U) & UINT32_C(0xff));
}

std::uint16_t read_u16(const std::uint8_t* const input) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[0]) |
                                      (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* const input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

}  // namespace

void md_packet_init(md_packet_t* const packet) {
    if (packet == nullptr) {
        return;
    }

    packet->major = 0U;
    packet->minor = 0U;
    packet->opcode = 0U;
    packet->flags = 0U;
    packet->serial = 0U;
    packet->payload_size = 0U;
    std::fill(std::begin(packet->payload), std::end(packet->payload), UINT8_C(0));
    packet->fd_count = 0U;
    std::fill(std::begin(packet->fds), std::end(packet->fds), mirage::kInvalidFd);
}

void md_packet_close_fds(md_packet_t* const packet) {
    if (packet == nullptr) {
        return;
    }

    md_close_fds(packet->fds, packet->fd_count);
    packet->fd_count = 0U;
}


/*
 * Sends one ordered packet plus optional SCM_RIGHTS descriptors.  Returns 0 on
 * success, 1 when the nonblocking socket would block (descriptor ownership stays
 * with the caller for the outbox to retain), or -errno.  The 24-byte header is
 * written with explicit little-endian helpers and validated by md_codec_recv.
 */
std::int32_t md_codec_send(const std::int32_t fd, const std::uint16_t minor,
                           const std::uint16_t opcode, const std::uint16_t flags,
                           const std::uint32_t serial, const void* const payload,
                           const std::size_t payload_size, const std::int32_t* const fds,
                           const std::size_t fd_count) {
    if (fd == mirage::kInvalidFd) {
        return -EBADF;
    }
    if (payload_size > MD_WIRE_MAX_PAYLOAD || fd_count > MD_WIRE_MAX_FDS) {
        return -EMSGSIZE;
    }
    if ((payload_size != 0U && payload == nullptr) || (fd_count != 0U && fds == nullptr)) {
        return -EINVAL;
    }

    std::array<std::uint8_t, MD_WIRE_HEADER_SIZE> header{};
    write_u32(header.data(), MD_WIRE_MAGIC);
    write_u16(header.data() + 4U, 1U);
    write_u16(header.data() + 6U, minor);
    write_u16(header.data() + 8U, opcode);
    write_u16(header.data() + 10U, flags);
    write_u32(header.data() + 12U, static_cast<std::uint32_t>(payload_size));
    write_u16(header.data() + 16U, static_cast<std::uint16_t>(fd_count));
    write_u16(header.data() + 18U, 0U);
    write_u32(header.data() + 20U, serial);

    std::array<iovec, 2U> iov{};
    iov[0].iov_base = header.data();
    iov[0].iov_len = header.size();
    /* POSIX declares iov_base mutable even though sendmsg never writes payload. */
    iov[1].iov_base = const_cast<void*>(payload);
    iov[1].iov_len = payload_size;

    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(std::int32_t) * MD_WIRE_MAX_FDS)>
        control{};
    msghdr message;
    std::memset(&message, 0, sizeof(message));
    message.msg_iov = iov.data();
    message.msg_iovlen = payload_size != 0U ? 2U : 1U;

    if (fd_count != 0U) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(std::int32_t) * fd_count);
        cmsghdr* const cmsg = CMSG_FIRSTHDR(&message);
        if (cmsg == nullptr) {
            return -EIO;
        }
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(std::int32_t) * fd_count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(std::int32_t) * fd_count);
    }

    ssize_t result = 0;
    do {
        result = ::sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        const std::int32_t error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return 1;
        }
        return -error;
    }

    const std::size_t expected_size = MD_WIRE_HEADER_SIZE + payload_size;
    return static_cast<std::size_t>(result) == expected_size ? 0 : -EIO;
}


/*
 * Receives exactly one packet into a caller-owned md_packet_t.  Returns 1 on
 * success, 0 when the nonblocking socket has no packet, or -errno.  The received
 * descriptors move into packet->fds; every rejection path closes them so no
 * SCM_RIGHTS descriptor can escape an invalid packet.
 */
std::int32_t md_codec_recv(const std::int32_t fd, md_packet_t* const packet) {
    if (fd == mirage::kInvalidFd || packet == nullptr) {
        return -EINVAL;
    }
    md_packet_init(packet);

    std::array<std::uint8_t, MD_WIRE_MAX_PACKET> raw{};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(std::int32_t) * MD_WIRE_MAX_FDS)>
        control{};
    iovec iov;
    std::memset(&iov, 0, sizeof(iov));
    iov.iov_base = raw.data();
    iov.iov_len = raw.size();

    msghdr message;
    std::memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1U;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t result = 0;
    do {
        result = ::recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        const std::int32_t error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return 0;
        }
        return -error;
    }
    if (result == 0) {
        return -ECONNRESET;
    }

    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }

        const std::byte* const control_begin = control.data();
        const std::byte* const control_end = control.data() + message.msg_controllen;
        const std::byte* const cmsg_begin =
            static_cast<const std::byte*>(static_cast<const void*>(cmsg));
        const std::byte* const cmsg_end = cmsg_begin + cmsg->cmsg_len;
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg_begin < control_begin ||
            cmsg_end > control_end) {
            md_packet_close_fds(packet);
            return -EPROTO;
        }

        const std::size_t byte_count = cmsg->cmsg_len - CMSG_LEN(0);
        if (byte_count % sizeof(std::int32_t) != 0U) {
            md_packet_close_fds(packet);
            return -EPROTO;
        }
        const std::size_t received_count = byte_count / sizeof(std::int32_t);
        if (packet->fd_count + received_count > MD_WIRE_MAX_FDS) {
            const std::int32_t* const received =
                static_cast<const std::int32_t*>(
                    static_cast<const void*>(CMSG_DATA(cmsg)));
            for (std::size_t index = 0U; index < received_count; ++index) {
                const std::int32_t close_result = ::close(received[index]);
                if (close_result != 0) {
                    /* SCM_RIGHTS ownership still ends after this close attempt. */
                }
            }
            md_packet_close_fds(packet);
            return -EMSGSIZE;
        }
        std::memcpy(packet->fds + packet->fd_count, CMSG_DATA(cmsg), byte_count);
        packet->fd_count += received_count;
    }

    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        md_packet_close_fds(packet);
        return -EMSGSIZE;
    }
    if (static_cast<std::size_t>(result) < MD_WIRE_HEADER_SIZE) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }
    if (read_u32(raw.data()) != MD_WIRE_MAGIC || read_u16(raw.data() + 4U) != 1U ||
        read_u16(raw.data() + 18U) != 0U) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }

    const std::uint32_t payload_size = read_u32(raw.data() + 12U);
    const std::uint16_t declared_fds = read_u16(raw.data() + 16U);
    if (payload_size > MD_WIRE_MAX_PAYLOAD ||
        static_cast<std::size_t>(result) != MD_WIRE_HEADER_SIZE + payload_size ||
        static_cast<std::size_t>(declared_fds) != packet->fd_count) {
        md_packet_close_fds(packet);
        return -EPROTO;
    }

    packet->major = read_u16(raw.data() + 4U);
    packet->minor = read_u16(raw.data() + 6U);
    packet->opcode = read_u16(raw.data() + 8U);
    packet->flags = read_u16(raw.data() + 10U);
    packet->serial = read_u32(raw.data() + 20U);
    packet->payload_size = payload_size;
    std::copy_n(raw.data() + MD_WIRE_HEADER_SIZE, payload_size, packet->payload);
    return 1;
}
