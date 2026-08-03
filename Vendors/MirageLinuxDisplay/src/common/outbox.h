#ifndef MIRAGE_DISPLAY_COMMON_OUTBOX_H
#define MIRAGE_DISPLAY_COMMON_OUTBOX_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MD_OUTBOX_LIMIT 64u

typedef struct md_out_message {
    struct md_out_message* next;
    uint16_t opcode;
    uint16_t flags;
    uint32_t serial;
    size_t payload_size;
    size_t fd_count;
    int fds[MD_WIRE_MAX_FDS];
    uint8_t payload[];
} md_out_message_t;

typedef struct md_outbox {
    md_out_message_t* head;
    md_out_message_t* tail;
    size_t count;
} md_outbox_t;

void md_outbox_init(md_outbox_t* outbox);

/* Closes owned FDs and frees every queued message. */
void md_outbox_clear(md_outbox_t* outbox);

/*
 * Sends a message immediately when the queue is empty, otherwise queues it.
 * `fds` is consumed on every return path (each entry becomes -1). When
 * `coalesce_tail` is set and the queued tail carries the same opcode and
 * payload size, the tail payload is replaced instead of appending; this only
 * applies to fd-less messages. Returns MD_OK, MD_ERR_WOULD_BLOCK (queue full),
 * MD_ERR_NOMEM, MD_ERR_INVALID, or a negative errno from the transport.
 */
int md_outbox_send_or_queue(md_outbox_t* outbox, int fd, uint16_t minor,
                            uint16_t opcode, uint16_t flags, uint32_t serial,
                            const uint8_t* payload, size_t payload_size,
                            int* fds, size_t fd_count, bool coalesce_tail);

/* Flushes queued messages until empty. Returns MD_OK, MD_ERR_WOULD_BLOCK
 * while the socket is busy, or a negative errno from the transport. */
int md_outbox_flush(md_outbox_t* outbox, int fd, uint16_t minor);

#endif
