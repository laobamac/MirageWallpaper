#ifndef MIRAGE_DISPLAY_PRODUCER_H
#define MIRAGE_DISPLAY_PRODUCER_H

#include "mirage_display.h"

/*
 * Public C ABI for the MirageLinuxDisplay producer (renderer) library.
 *
 * Renderers use this library to advertise a stable output and format
 * capabilities, lend DMA-BUF buffer pools, submit frames with explicit
 * synchronization, and retire buffer generations.  It shares the handshake and
 * transport rules of the consumer library.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Producer DTOs have an explicit layout for C/C++ and FFI consumers. */
#pragma pack(push, 8)

typedef struct md_producer md_producer_t;

typedef struct md_producer_info {
    /* Borrowed, NUL-terminated strings and a borrowed format_count array. */
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
    /* Logical wallpaper item dimensions supplied by the display consumer.
     * Producers may use these as their buffer dimensions when the desktop's
     * private scene-graph backing store is intentionally oversampled. */
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t refresh_mhz;
    md_transform_t transform;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
    /* Target GPU identity supplied before renderer resources are created. */
    uint32_t target_drm_render_major;
    uint32_t target_drm_render_minor;
    uint32_t target_gpu_flags;
    uint8_t target_device_uuid[16];
    uint8_t target_driver_uuid[16];
} md_producer_config_t;

typedef struct md_producer_gpu_info {
    /* Actual GPU identity after Vulkan/EGL/VA-API resources are created. */
    uint32_t drm_render_major;
    uint32_t drm_render_minor;
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
} md_producer_gpu_info_t;

typedef struct md_producer_callbacks {
    void (*on_connected)(void* user_data, uint64_t producer_id, uint64_t output_id);
    void (*on_output_config)(void* user_data, const md_producer_config_t* config);
    void (*on_retire_buffers)(void* user_data, uint64_t generation);
    void (*on_pointer_enter)(void* user_data, const md_pointer_enter_t* event);
    void (*on_pointer_leave)(void* user_data, uint64_t timestamp_us);
    void (*on_pointer_motion)(void* user_data, const md_pointer_motion_t* event);
    void (*on_pointer_button)(void* user_data, const md_pointer_button_t* event);
    void (*on_pointer_axis)(void* user_data, const md_pointer_axis_t* event);
    /* Reports the desktop window facts computed by the display adapter. The
     * flags are the WINDOW_STATE bit field (0x1 covered, 0x2 focus lost,
     * 0x4 maximized, 0x8 fullscreen); the callback runs on the session's
     * dispatch thread and the flags value is borrowed for the call. */
    void (*on_window_state)(void* user_data, uint32_t flags);
    void (*on_disconnected)(void* user_data, md_result_t reason, const char* message);
    /* Borrowed opaque callback context. The library never frees it. */
    void* user_data;
} md_producer_callbacks_t;

/* Allocates a producer owned by the caller; release it with md_producer_free(). */
md_producer_t* md_producer_new(const md_producer_callbacks_t* callbacks);
void md_producer_free(md_producer_t* producer);

md_result_t md_producer_begin_connect(md_producer_t* producer, const char* socket_path,
                                      const char* client_name, const char* client_version,
                                      const md_producer_info_t* info);
md_result_t md_producer_begin_connected_fd(md_producer_t* producer, int32_t connected_fd,
                                           const char* client_name, const char* client_version,
                                           const md_producer_info_t* info);
/* Returns an MD_HANDSHAKE_* progress value or a negative md_result_t value. */
int32_t md_producer_advance_handshake(md_producer_t* producer);
md_result_t md_producer_connect(md_producer_t* producer, const char* socket_path,
                                const char* client_name, const char* client_version,
                                const md_producer_info_t* info, int32_t timeout_ms);

void md_producer_close(md_producer_t* producer);
int32_t md_producer_get_fd(const md_producer_t* producer);
md_connection_state_t md_producer_connection_state(const md_producer_t* producer);
md_handshake_state_t md_producer_handshake_state(const md_producer_t* producer);
uint8_t md_producer_wants_writable(const md_producer_t* producer);
md_result_t md_producer_handle_writable(md_producer_t* producer);
int32_t md_producer_dispatch(md_producer_t* producer);

/* Pool FDs are borrowed and duplicated internally for a queued send. */
md_result_t md_producer_offer_buffers(md_producer_t* producer, const md_buffer_pool_t* pool);
/* Confirms the GPU that owns renderer resources; must precede buffer offers. */
md_result_t md_producer_bind_gpu(md_producer_t* producer,
                                 const md_producer_gpu_info_t* gpu);
md_result_t md_producer_set_config(md_producer_t* producer,
                                   const md_display_config_t* config);
/* Both frame FDs are consumed by this call, including on errors. */
md_result_t md_producer_submit_frame(md_producer_t* producer, uint64_t generation,
                                     uint32_t buffer_index, uint64_t sequence,
                                     int32_t acquire_sync_fd,
                                     int32_t release_syncobj_fd);
md_result_t md_producer_retire_done(md_producer_t* producer, uint64_t generation);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
