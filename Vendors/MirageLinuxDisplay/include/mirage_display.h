#ifndef MIRAGE_DISPLAY_H
#define MIRAGE_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIRAGE_DISPLAY_VERSION_MAJOR 0
#define MIRAGE_DISPLAY_VERSION_MINOR 1
#define MIRAGE_DISPLAY_VERSION_PATCH 0

#define MIRAGE_DISPLAY_PROTOCOL_MAJOR 1
#define MIRAGE_DISPLAY_PROTOCOL_MINOR 0

#define MIRAGE_DISPLAY_MAX_BUFFERS 4
#define MIRAGE_DISPLAY_MAX_PLANES 4
#define MIRAGE_DISPLAY_MAX_FORMATS 256

typedef struct md_display md_display_t;

typedef enum md_result {
    MD_OK = 0,
    MD_ERR_INVALID = -1,
    MD_ERR_NOMEM = -2,
    MD_ERR_STATE = -3,
    MD_ERR_IO = -4,
    MD_ERR_PROTOCOL = -5,
    MD_ERR_WOULD_BLOCK = -6,
    MD_ERR_DISCONNECTED = -7,
    MD_ERR_UNSUPPORTED = -8,
} md_result_t;

typedef enum md_connection_state {
    MD_CONNECTION_DISCONNECTED = 0,
    MD_CONNECTION_CONNECTING = 1,
    MD_CONNECTION_HANDSHAKING = 2,
    MD_CONNECTION_READY = 3,
    MD_CONNECTION_DEAD = 4,
} md_connection_state_t;

typedef enum md_handshake_state {
    MD_HANDSHAKE_IDLE = 0,
    MD_HANDSHAKE_CONNECTING = 1,
    MD_HANDSHAKE_HELLO_SEND = 2,
    MD_HANDSHAKE_WELCOME_WAIT = 3,
    MD_HANDSHAKE_REGISTER_SEND = 4,
    MD_HANDSHAKE_ACCEPT_WAIT = 5,
    MD_HANDSHAKE_CAPS_SEND = 6,
    MD_HANDSHAKE_READY = 7,
} md_handshake_state_t;

enum {
    MD_HANDSHAKE_DONE = 1,
    MD_HANDSHAKE_NEED_READ = 2,
    MD_HANDSHAKE_NEED_WRITE = 3,
    MD_HANDSHAKE_PROGRESS = 4,
};

enum {
    MD_FEATURE_EXPLICIT_SYNC = UINT64_C(1) << 0,
    MD_FEATURE_DRM_MODIFIERS = UINT64_C(1) << 1,
    MD_FEATURE_MULTIPLANE = UINT64_C(1) << 2,
    MD_FEATURE_POINTER_AXIS = UINT64_C(1) << 3,
    MD_FEATURE_WINDOW_STATE = UINT64_C(1) << 4,
    MD_FEATURE_COLOR_METADATA = UINT64_C(1) << 5,
};

enum {
    MD_INPUT_POINTER_ENTER_LEAVE = UINT64_C(1) << 0,
    MD_INPUT_POINTER_MOTION = UINT64_C(1) << 1,
    MD_INPUT_POINTER_BUTTON = UINT64_C(1) << 2,
    MD_INPUT_POINTER_AXIS = UINT64_C(1) << 3,
    MD_INPUT_NON_CONSUMING = UINT64_C(1) << 4,
};

typedef enum md_transform {
    MD_TRANSFORM_NORMAL = 0,
    MD_TRANSFORM_90 = 1,
    MD_TRANSFORM_180 = 2,
    MD_TRANSFORM_270 = 3,
    MD_TRANSFORM_FLIPPED = 4,
    MD_TRANSFORM_FLIPPED_90 = 5,
    MD_TRANSFORM_FLIPPED_180 = 6,
    MD_TRANSFORM_FLIPPED_270 = 7,
} md_transform_t;

typedef enum md_button_state {
    MD_BUTTON_RELEASED = 0,
    MD_BUTTON_PRESSED = 1,
} md_button_state_t;

typedef enum md_axis_source {
    MD_AXIS_WHEEL = 0,
    MD_AXIS_FINGER = 1,
    MD_AXIS_CONTINUOUS = 2,
} md_axis_source_t;

typedef struct md_rect {
    float x;
    float y;
    float width;
    float height;
} md_rect_t;

typedef struct md_format_cap {
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
} md_format_cap_t;

typedef struct md_output_info {
    const char* stable_id;
    const char* name;
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t scale_120;
    uint32_t refresh_mhz;
    md_transform_t transform;
    uint32_t drm_render_major;
    uint32_t drm_render_minor;
    uint64_t input_caps;
} md_output_info_t;

typedef struct md_consumer_caps {
    uint64_t features;
    uint64_t sync_caps;
    uint64_t color_caps;
    uint32_t max_width;
    uint32_t max_height;
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
    const md_format_cap_t* formats;
    uint32_t format_count;
} md_consumer_caps_t;

typedef struct md_plane {
    int fd;
    uint32_t stride;
    uint32_t offset;
    uint64_t size;
} md_plane_t;

/* Borrowed by callbacks. The library owns plane FDs until unbind/close. */
typedef struct md_buffer_pool {
    uint64_t generation;
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
    md_plane_t planes[MIRAGE_DISPLAY_MAX_BUFFERS][MIRAGE_DISPLAY_MAX_PLANES];
} md_buffer_pool_t;

typedef struct md_display_config {
    uint64_t generation;
    md_rect_t source;
    md_rect_t destination;
    md_transform_t transform;
    float clear_color[4];
} md_display_config_t;

typedef struct md_pointer_enter {
    float x;
    float y;
    uint64_t timestamp_us;
} md_pointer_enter_t;

typedef struct md_pointer_motion {
    float x;
    float y;
    uint64_t timestamp_us;
    uint32_t modifiers;
} md_pointer_motion_t;

typedef struct md_pointer_button {
    float x;
    float y;
    uint32_t button;
    md_button_state_t state;
    uint64_t timestamp_us;
    uint32_t modifiers;
} md_pointer_button_t;

typedef struct md_pointer_axis {
    float x;
    float y;
    float delta_x;
    float delta_y;
    md_axis_source_t source;
    uint64_t timestamp_us;
    uint32_t modifiers;
} md_pointer_axis_t;

/* Ownership of both FDs transfers to on_frame. Close them on every path. */
typedef struct md_frame {
    uint64_t buffer_generation;
    uint32_t buffer_index;
    uint64_t sequence;
    int acquire_sync_fd;
    int release_syncobj_fd;
} md_frame_t;

typedef struct md_display_callbacks {
    void (*on_connected)(void* user_data, uint64_t output_id);
    void (*on_buffers_ready)(void* user_data, const md_buffer_pool_t* pool);
    /*
     * Called before a pool is released. Call md_display_defer_unbind() from
     * this callback when GPU references must be destroyed asynchronously.
     * The pool and its FDs remain valid until md_display_finish_unbind().
     */
    void (*on_buffers_releasing)(void* user_data, const md_buffer_pool_t* pool);
    void (*on_config)(void* user_data, const md_display_config_t* config);
    void (*on_frame)(void* user_data, const md_frame_t* frame);
    void (*on_disconnected)(void* user_data, md_result_t reason, const char* message);
    void* user_data;
} md_display_callbacks_t;

md_display_t* md_display_new(const md_display_callbacks_t* callbacks);
void md_display_free(md_display_t* display);

int md_display_begin_connect(md_display_t* display, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_output_info_t* output,
                             const md_consumer_caps_t* caps);

/*
 * Starts the same handshake on an already-connected AF_UNIX SOCK_SEQPACKET FD.
 * On success ownership of connected_fd transfers to the display. This supports
 * broker handoff, socket activation and tests that cannot create pathname
 * sockets. The function enables O_NONBLOCK and FD_CLOEXEC.
 */
int md_display_begin_connected_fd(md_display_t* display, int connected_fd,
                                  const char* client_name, const char* client_version,
                                  const md_output_info_t* output,
                                  const md_consumer_caps_t* caps);
int md_display_advance_handshake(md_display_t* display);

/* Blocking convenience for command-line tools and tests. */
int md_display_connect(md_display_t* display, const char* socket_path,
                       const char* client_name, const char* client_version,
                       const md_output_info_t* output,
                       const md_consumer_caps_t* caps, int timeout_ms);

void md_display_close(md_display_t* display);
int md_display_get_fd(const md_display_t* display);
md_connection_state_t md_display_connection_state(const md_display_t* display);
md_handshake_state_t md_display_handshake_state(const md_display_t* display);
uint64_t md_display_output_id(const md_display_t* display);

/* Dispatches all currently readable packets. Returns packet count or an error. */
int md_display_dispatch(md_display_t* display);
bool md_display_wants_writable(const md_display_t* display);
int md_display_handle_writable(md_display_t* display);

/*
 * Defers the current broker-requested UNBIND. This is only valid from inside
 * on_buffers_releasing. By default UNBIND is completed when the callback
 * returns, preserving the synchronous C API behavior.
 */
int md_display_defer_unbind(md_display_t* display);
/* Returns zero when no deferred UNBIND is pending. */
uint64_t md_display_pending_unbind_generation(const md_display_t* display);
/*
 * Completes a deferred UNBIND after host GPU references are gone. The pool FDs
 * are closed and UNBIND_DONE is sent or queued. Call on the display's event
 * thread, not directly from a Qt Quick render-thread callback.
 */
int md_display_finish_unbind(md_display_t* display, uint64_t generation);

int md_display_update_output(md_display_t* display, const md_output_info_t* output);
int md_display_send_pointer_enter(md_display_t* display, float x, float y,
                                  uint64_t timestamp_us);
int md_display_send_pointer_leave(md_display_t* display, uint64_t timestamp_us);
int md_display_send_pointer_motion(md_display_t* display, float x, float y,
                                   uint64_t timestamp_us, uint32_t modifiers);
int md_display_send_pointer_button(md_display_t* display, float x, float y,
                                   uint32_t button, md_button_state_t state,
                                   uint64_t timestamp_us, uint32_t modifiers);
int md_display_send_pointer_axis(md_display_t* display, float x, float y,
                                 float delta_x, float delta_y,
                                 md_axis_source_t source, uint64_t timestamp_us,
                                 uint32_t modifiers);
int md_display_send_window_state(md_display_t* display, uint32_t flags);

/* CPU fallback for a release syncobj. Both descriptors are consumed. */
int md_display_signal_release_syncobj(int release_syncobj_fd);
/* Connect a completed sync_file to a release syncobj. Both descriptors are consumed. */
int md_display_release_after_sync_file(int release_syncobj_fd, int sync_file_fd);

#ifdef __cplusplus
}
#endif

#endif
