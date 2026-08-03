#ifndef MIRAGE_DISPLAY_COMMON_HANDSHAKE_H
#define MIRAGE_DISPLAY_COMMON_HANDSHAKE_H

#include "mirage_display.h"
#include "outbox.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Handshake messages are far below the 64 KiB wire cap; a modest fixed
 * buffer keeps every client object compact. */
#define MD_HANDSHAKE_BUFFER_SIZE 4096u

enum {
    MD_CLIENT_ROLE_DISPLAY = 1,
    MD_CLIENT_ROLE_PRODUCER = 2,
};

/* Role-specific parts of the common client handshake. */
typedef struct md_client_ops {
    uint16_t register_opcode;   /* REGISTER_OUTPUT or REGISTER_PRODUCER */
    uint16_t accepted_opcode;   /* OUTPUT_ACCEPTED or PRODUCER_ACCEPTED */
    uint16_t caps_opcode;       /* CONSUMER_CAPS, or 0 when the role sends none */
    int (*encode_register)(void* object, md_writer_t* writer);
    int (*encode_caps)(void* object, md_writer_t* writer);
    int (*apply_accepted)(void* object, const md_packet_t* packet);
    void (*on_ready)(void* object);
    void (*notify_disconnected)(void* object, md_result_t reason, const char* message);
} md_client_ops_t;

/* Transport, handshake and outbox state shared by the display consumer and
 * render producer clients. Embedded as the first member of each role. */
typedef struct md_client {
    int fd;
    md_connection_state_t connection_state;
    md_handshake_state_t handshake_state;
    uint16_t selected_minor;
    uint64_t negotiated_features;
    uint32_t next_serial;
    bool disconnected_notified;
    uint32_t role;
    uint64_t advertised_features;

    char* socket_path;
    char* client_name;
    char* client_version;

    uint16_t handshake_opcode;
    uint32_t handshake_serial;
    size_t handshake_size;
    uint8_t handshake_payload[MD_HANDSHAKE_BUFFER_SIZE];

    md_outbox_t outbox;

    const md_client_ops_t* ops;
    void* object;
} md_client_t;

void md_client_init(md_client_t* client, uint32_t role, const md_client_ops_t* ops,
                    void* object);

/* Stores and duplicates the connection identity strings. */
int md_client_set_identity(md_client_t* client, const char* socket_path,
                           const char* client_name, const char* client_version);
void md_client_clear_identity(md_client_t* client);

/* Starts connecting to the stored socket path or adopts an already-connected
 * AF_UNIX SOCK_SEQPACKET descriptor. */
int md_client_begin_connect(md_client_t* client);
int md_client_begin_connected_fd(md_client_t* client, int connected_fd);

/* Drives the handshake state machine. */
int md_client_advance_handshake(md_client_t* client);

/* Blocking handshake convenience for command-line tools and tests. */
int md_client_connect(md_client_t* client, int timeout_ms);

/* Closes the session, clearing the outbox and resetting transport state. */
void md_client_close(md_client_t* client);

/* Fails the session: closes the socket, clears the outbox, marks the state
 * dead and notifies the role. Returns the failure reason. */
int md_client_disconnect(md_client_t* client, md_result_t reason, const char* message);

/* Flushes the outbox. See md_outbox_flush for the return contract. */
int md_client_flush_outbox(md_client_t* client);

#endif
