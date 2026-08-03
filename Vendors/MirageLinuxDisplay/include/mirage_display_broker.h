#ifndef MIRAGE_DISPLAY_BROKER_H
#define MIRAGE_DISPLAY_BROKER_H

#include "mirage_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_broker md_broker_t;

typedef struct md_broker_options {
    const char* socket_path;
    const char* server_name;
    const char* server_version;
    uint64_t features;
    uint32_t max_routes;
} md_broker_options_t;

/* Creates an unbound broker. The options strings are copied. */
md_broker_t* md_broker_new(const md_broker_options_t* options);
void md_broker_free(md_broker_t* broker);

/* Binds the AF_UNIX SOCK_SEQPACKET endpoint and starts accepting peers. */
int md_broker_listen(md_broker_t* broker);
void md_broker_stop(md_broker_t* broker);

/*
 * Polls the listener and all active peers for up to timeout_ms. A negative
 * timeout blocks until an event. This is suitable for a dedicated MirageQt
 * event thread; the broker remains independent of X11 and Wayland.
 */
int md_broker_dispatch(md_broker_t* broker, int timeout_ms);

int md_broker_get_fd(const md_broker_t* broker);
const char* md_broker_socket_path(const md_broker_t* broker);

#ifdef __cplusplus
}
#endif

#endif
