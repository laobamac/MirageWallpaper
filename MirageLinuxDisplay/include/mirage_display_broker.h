#ifndef MIRAGE_DISPLAY_BROKER_H
#define MIRAGE_DISPLAY_BROKER_H

#include "mirage_display.h"

/*
 * Public C ABI for the MirageLinuxDisplay broker routing core.
 *
 * The broker binds the 0600 AF_UNIX SOCK_SEQPACKET endpoint, validates peers via
 * SO_PEERCRED, routes one producer to many display consumers per stable output
 * id, and forwards packets and descriptors without ever copying pixel data.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Broker options cross the C ABI and therefore use the common 8-byte layout. */
#pragma pack(push, 8)

typedef struct md_broker md_broker_t;

typedef struct md_broker_options {
    /* Borrowed, NUL-terminated values copied by md_broker_new(). */
    const char* socket_path;
    const char* server_name;
    const char* server_version;
    uint64_t features;
    uint32_t max_routes;
    void (*on_output_added)(void* user_data, const md_output_info_t* output);
    void (*on_output_updated)(void* user_data, const md_output_info_t* output);
    /* stable_id is borrowed only for the callback duration. */
    void (*on_output_removed)(void* user_data, const char* stable_id);
    /* Optional host notification for desktop window-state changes received
     * from a display. Invoked on the broker dispatch thread; stable_id is
     * borrowed for the duration of the call, so the host must copy it if it
     * is kept. The host uses the facts to drive its own playback policy. */
    void (*on_window_state)(void* user_data, const char* stable_id, uint32_t flags);
    /* Borrowed opaque context passed to every host callback; never freed. */
    void* user_data;
} md_broker_options_t;

/* Creates an unbound broker owned by the caller. The options strings are copied. */
md_broker_t* md_broker_new(const md_broker_options_t* options);
void md_broker_free(md_broker_t* broker);

/* Binds the AF_UNIX SOCK_SEQPACKET endpoint and starts accepting peers. */
md_result_t md_broker_listen(md_broker_t* broker);
void md_broker_stop(md_broker_t* broker);

/*
 * Polls the listener and all active peers for up to timeout_ms. A negative
 * timeout blocks until an event. This is suitable for a dedicated MirageQt
 * event thread; the broker remains independent of X11 and Wayland.
 */
int32_t md_broker_dispatch(md_broker_t* broker, int32_t timeout_ms);

int32_t md_broker_get_fd(const md_broker_t* broker);
const char* md_broker_socket_path(const md_broker_t* broker);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
