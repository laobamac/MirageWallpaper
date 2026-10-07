#ifndef MIRAGE_DISPLAY_CODEC_HPP
#define MIRAGE_DISPLAY_CODEC_HPP

#include <cstddef>
#include <cstdint>

/*
 * Wire packet codec shared by broker, display, and producer.
 *
 * md_packet_t is a fixed-size trivial DTO so SCM_RIGHTS descriptors can cross
 * roles without allocation.  Whoever receives a packet owns every descriptor in
 * its fd array and must close any descriptor not explicitly handed to a callback
 * or queue.
 */

inline constexpr std::uint32_t MD_WIRE_MAGIC = UINT32_C(0x3150444d);
inline constexpr std::size_t MD_WIRE_HEADER_SIZE = 24U;
inline constexpr std::size_t MD_WIRE_MAX_PACKET = 65536U;
inline constexpr std::size_t MD_WIRE_MAX_PAYLOAD = MD_WIRE_MAX_PACKET - MD_WIRE_HEADER_SIZE;
inline constexpr std::size_t MD_WIRE_MAX_FDS = 16U;

inline constexpr std::uint16_t MD_PACKET_OPTIONAL = UINT16_C(1);

/*
 * Packet storage intentionally remains a fixed, trivial protocol DTO: broker,
 * display, and producer transfer SCM_RIGHTS ownership through these fields.
 * md_packet_close_fds() releases any descriptors not explicitly transferred.
 */
struct md_packet_t {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t opcode;
    std::uint16_t flags;
    std::uint32_t serial;
    std::size_t payload_size;
    std::uint8_t payload[MD_WIRE_MAX_PAYLOAD];
    std::size_t fd_count;
    std::int32_t fds[MD_WIRE_MAX_FDS];
};

void md_packet_init(md_packet_t* packet);
void md_packet_close_fds(md_packet_t* packet);

/* Returns one packet, zero when the nonblocking socket has no packet, or -errno. */
std::int32_t md_codec_recv(std::int32_t fd, md_packet_t* packet);

/* Returns zero on success, one when the socket would block, or -errno. */
std::int32_t md_codec_send(std::int32_t fd, std::uint16_t minor, std::uint16_t opcode,
                           std::uint16_t flags, std::uint32_t serial,
                           const void* payload, std::size_t payload_size,
                           const std::int32_t* fds, std::size_t fd_count);

#endif
