#define _GNU_SOURCE

#include "outbox.h"

#include "mirage_display.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void close_fds(int* fds, size_t count) {
    if (fds == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

void md_outbox_init(md_outbox_t* outbox) {
    outbox->head = NULL;
    outbox->tail = NULL;
    outbox->count = 0;
}

void md_outbox_clear(md_outbox_t* outbox) {
    md_out_message_t* message = outbox->head;
    while (message != NULL) {
        md_out_message_t* next = message->next;
        close_fds(message->fds, message->fd_count);
        free(message);
        message = next;
    }
    outbox->head = NULL;
    outbox->tail = NULL;
    outbox->count = 0;
}

int md_outbox_send_or_queue(md_outbox_t* outbox, int fd, uint16_t minor,
                            uint16_t opcode, uint16_t flags, uint32_t serial,
                            const uint8_t* payload, size_t payload_size,
                            int* fds, size_t fd_count, bool coalesce_tail) {
    if (fd < 0 || payload_size > MD_WIRE_MAX_PAYLOAD || fd_count > MD_WIRE_MAX_FDS) {
        close_fds(fds, fd_count);
        return MD_ERR_INVALID;
    }
    if (outbox->head == NULL) {
        int rc = md_codec_send(fd, minor, opcode, flags, serial, payload, payload_size,
                               fds, fd_count);
        if (rc == 0) {
            close_fds(fds, fd_count);
            return MD_OK;
        }
        if (rc < 0) {
            close_fds(fds, fd_count);
            return rc;
        }
    }
    if (coalesce_tail && fd_count == 0 && outbox->tail != NULL &&
        outbox->tail->opcode == opcode && outbox->tail->payload_size == payload_size) {
        if (payload_size > 0) memcpy(outbox->tail->payload, payload, payload_size);
        return MD_OK;
    }
    if (outbox->count >= MD_OUTBOX_LIMIT) {
        close_fds(fds, fd_count);
        return MD_ERR_WOULD_BLOCK;
    }
    md_out_message_t* message = malloc(sizeof(*message) + payload_size);
    if (message == NULL) {
        close_fds(fds, fd_count);
        return MD_ERR_NOMEM;
    }
    message->next = NULL;
    message->opcode = opcode;
    message->flags = flags;
    message->serial = serial;
    message->payload_size = payload_size;
    message->fd_count = fd_count;
    for (size_t i = 0; i < MD_WIRE_MAX_FDS; ++i) message->fds[i] = -1;
    if (payload_size > 0) memcpy(message->payload, payload, payload_size);
    for (size_t i = 0; i < fd_count; ++i) {
        message->fds[i] = fds[i];
        fds[i] = -1;
    }
    if (outbox->tail != NULL) outbox->tail->next = message;
    else outbox->head = message;
    outbox->tail = message;
    ++outbox->count;
    return MD_OK;
}

int md_outbox_flush(md_outbox_t* outbox, int fd, uint16_t minor) {
    while (outbox->head != NULL) {
        md_out_message_t* message = outbox->head;
        int rc = md_codec_send(fd, minor, message->opcode, message->flags,
                               message->serial, message->payload, message->payload_size,
                               message->fds, message->fd_count);
        if (rc == 1) return MD_ERR_WOULD_BLOCK;
        if (rc < 0) return rc;
        close_fds(message->fds, message->fd_count);
        outbox->head = message->next;
        if (outbox->head == NULL) outbox->tail = NULL;
        --outbox->count;
        free(message);
    }
    return MD_OK;
}
