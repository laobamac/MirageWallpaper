#ifndef MIRAGE_DISPLAY_CODEC_H
#define MIRAGE_DISPLAY_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define MD_WIRE_MAGIC UINT32_C(0x3150444d)
#define MD_WIRE_HEADER_SIZE 24u
#define MD_WIRE_MAX_PACKET 65536u
#define MD_WIRE_MAX_PAYLOAD (MD_WIRE_MAX_PACKET - MD_WIRE_HEADER_SIZE)
#define MD_WIRE_MAX_FDS 16u

#define MD_PACKET_OPTIONAL UINT16_C(1)

typedef struct md_packet {
    uint16_t major;
    uint16_t minor;
    uint16_t opcode;
    uint16_t flags;
    uint32_t serial;
    size_t payload_size;
    uint8_t payload[MD_WIRE_MAX_PAYLOAD];
    size_t fd_count;
    int fds[MD_WIRE_MAX_FDS];
} md_packet_t;

void md_packet_init(md_packet_t* packet);
void md_packet_close_fds(md_packet_t* packet);

/* Returns 1 on packet, 0 on EAGAIN, or negative errno. */
int md_codec_recv(int fd, md_packet_t* packet);

/* Returns 0 on success, 1 on EAGAIN, or negative errno. */
int md_codec_send(int fd, uint16_t minor, uint16_t opcode, uint16_t flags,
                  uint32_t serial, const void* payload, size_t payload_size,
                  const int* fds, size_t fd_count);

#endif
