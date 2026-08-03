#ifndef MIRAGE_DISPLAY_PRODUCER_H
#define MIRAGE_DISPLAY_PRODUCER_H

#include "mirage_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_producer md_producer_t;

typedef struct md_producer_info {
    const char* stable_output_id;
    const char* kind;
    uint32_t drm_render_major;
    uint32_t drm_render_minor;
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
    const md_format_cap_t* formats;
    uint32_t format_count;
} md_producer_info_t;

typedef struct md_producer_config {
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t refresh_mhz;
    md_transform_t transform;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
} md_producer_config_t;

typedef struct md_producer_callbacks {
    void (*on_connected)(void* user_data, uint64_t producer_id, uint64_t output_id);
    void (*on_output_config)(void* user_data, const md_producer_config_t* config);
    void (*on_retire_buffers)(void* user_data, uint64_t generation);
    void (*on_pointer_enter)(void* user_data, const md_pointer_enter_t* event);
    void (*on_pointer_leave)(void* user_data, uint64_t timestamp_us);
    void (*on_pointer_motion)(void* user_data, const md_pointer_motion_t* event);
    void (*on_pointer_button)(void* user_data, const md_pointer_button_t* event);
    void (*on_pointer_axis)(void* user_data, const md_pointer_axis_t* event);
    void (*on_disconnected)(void* user_data, md_result_t reason, const char* message);
    void* user_data;
} md_producer_callbacks_t;

md_producer_t* md_producer_new(const md_producer_callbacks_t* callbacks);
void md_producer_free(md_producer_t* producer);

int md_producer_begin_connect(md_producer_t* producer, const char* socket_path,
                              const char* client_name, const char* client_version,
                              const md_producer_info_t* info);
int md_producer_begin_connected_fd(md_producer_t* producer, int connected_fd,
                                  const char* client_name, const char* client_version,
                                  const md_producer_info_t* info);
int md_producer_advance_handshake(md_producer_t* producer);
int md_producer_connect(md_producer_t* producer, const char* socket_path,
                        const char* client_name, const char* client_version,
                        const md_producer_info_t* info, int timeout_ms);

void md_producer_close(md_producer_t* producer);
int md_producer_get_fd(const md_producer_t* producer);
md_connection_state_t md_producer_connection_state(const md_producer_t* producer);
md_handshake_state_t md_producer_handshake_state(const md_producer_t* producer);
bool md_producer_wants_writable(const md_producer_t* producer);
int md_producer_handle_writable(md_producer_t* producer);
int md_producer_dispatch(md_producer_t* producer);

/* Pool FDs are borrowed and duplicated internally for a queued send. */
int md_producer_offer_buffers(md_producer_t* producer, const md_buffer_pool_t* pool);
int md_producer_set_config(md_producer_t* producer, const md_display_config_t* config);
/* Both frame FDs are consumed by this call, including on errors. */
int md_producer_submit_frame(md_producer_t* producer, uint64_t generation,
                             uint32_t buffer_index, uint64_t sequence,
                             int acquire_sync_fd, int release_syncobj_fd);
int md_producer_retire_done(md_producer_t* producer, uint64_t generation);

#ifdef __cplusplus
}
#endif

#endif
