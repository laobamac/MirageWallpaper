#define _GNU_SOURCE

#include "mirage_display.h"

#include "codec.h"
#include "common/handshake.h"
#include "common/outbox.h"
#include "common/util.h"
#include "protocol.h"
#include "sync_fanout.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct md_display {
    md_client_t client;
    md_display_callbacks_t callbacks;
    uint64_t output_id;
    char* stable_id;
    char* output_name;
    md_output_info_t output;
    md_consumer_caps_t caps;
    md_format_cap_t* formats;

    bool pool_active;
    bool in_unbind_callback;
    bool unbind_deferred;
    uint64_t pending_unbind_generation;
    md_buffer_pool_t pool;
};

static void free_connection_data(md_display_t* display) {
    md_client_clear_identity(&display->client);
    free(display->stable_id);
    free(display->output_name);
    free(display->formats);
    display->stable_id = NULL;
    display->output_name = NULL;
    display->formats = NULL;
    memset(&display->output, 0, sizeof(display->output));
    memset(&display->caps, 0, sizeof(display->caps));
}
static void release_pool(md_display_t* display, bool notify) {
    if (!display->pool_active) return;
    if (notify && display->callbacks.on_buffers_releasing != NULL) {
        display->callbacks.on_buffers_releasing(display->callbacks.user_data, &display->pool);
    }
    md_close_pool(&display->pool);
    display->pool_active = false;
}

static void abandon_pool(md_display_t* display, bool notify_if_needed) {
    bool notify = notify_if_needed && display->pending_unbind_generation == 0;
    release_pool(display, notify);
    display->in_unbind_callback = false;
    display->unbind_deferred = false;
    display->pending_unbind_generation = 0;
}
static void display_notify_disconnected(void* object, md_result_t reason,
                                        const char* message) {
    md_display_t* display = object;
    abandon_pool(display, true);
    if (!display->client.disconnected_notified && display->callbacks.on_disconnected != NULL) {
        display->client.disconnected_notified = true;
        display->callbacks.on_disconnected(display->callbacks.user_data, reason, message);
    }
}

static int fail_session(md_display_t* display, md_result_t reason, const char* message) {
    return md_client_disconnect(&display->client, reason, message);
}

static int display_encode_register(void* object, md_writer_t* writer) {
    md_display_t* display = object;
    return md_proto_encode_register_output(writer, &display->output);
}

static int display_encode_caps(void* object, md_writer_t* writer) {
    md_display_t* display = object;
    return md_proto_encode_consumer_caps(writer, &display->caps);
}

static int display_apply_accepted(void* object, const md_packet_t* packet) {
    md_display_t* display = object;
    uint64_t output_id;
    if (md_proto_decode_output_accepted(packet->payload, packet->payload_size,
                                        &output_id) != 0 ||
        output_id == 0) {
        return MD_ERR_PROTOCOL;
    }
    display->output_id = output_id;
    return MD_OK;
}

static void display_on_ready(void* object) {
    md_display_t* display = object;
    if (display->callbacks.on_connected != NULL) {
        display->callbacks.on_connected(display->callbacks.user_data, display->output_id);
    }
}

static const md_client_ops_t display_client_ops = {
    .register_opcode = MD_OP_REGISTER_OUTPUT,
    .accepted_opcode = MD_OP_OUTPUT_ACCEPTED,
    .caps_opcode = MD_OP_CONSUMER_CAPS,
    .encode_register = display_encode_register,
    .encode_caps = display_encode_caps,
    .apply_accepted = display_apply_accepted,
    .on_ready = display_on_ready,
    .notify_disconnected = display_notify_disconnected,
};
static bool valid_output(const md_output_info_t* output) {
    return output != NULL && output->stable_id != NULL && output->name != NULL &&
           output->physical_width > 0 && output->physical_height > 0 &&
           output->logical_width > 0 && output->logical_height > 0 &&
           output->scale_120 > 0 && output->transform <= MD_TRANSFORM_FLIPPED_270;
}

static bool valid_caps(const md_consumer_caps_t* caps) {
    if (caps == NULL || caps->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (caps->format_count > 0 && caps->formats == NULL)) return false;
    for (uint32_t i = 0; i < caps->format_count; ++i) {
        if (caps->formats[i].plane_count < 1 ||
            caps->formats[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES) return false;
    }
    return true;
}

static int copy_connect_args(md_display_t* display, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_output_info_t* output, const md_consumer_caps_t* caps) {
    if (!valid_output(output) || !valid_caps(caps)) return MD_ERR_INVALID;
    int rc = md_client_set_identity(&display->client, socket_path, client_name, client_version);
    if (rc != MD_OK) return rc;
    free(display->stable_id);
    free(display->output_name);
    free(display->formats);
    display->stable_id = md_strdup(output->stable_id);
    display->output_name = md_strdup(output->name);
    if (display->stable_id == NULL || display->output_name == NULL) {
        free_connection_data(display);
        return MD_ERR_NOMEM;
    }

    display->output = *output;
    display->output.stable_id = display->stable_id;
    display->output.name = display->output_name;
    display->caps = *caps;
    display->caps.features |= MD_FEATURE_EXPLICIT_SYNC;
    display->client.advertised_features = display->caps.features;
    if (caps->format_count > 0) {
        display->formats = malloc(sizeof(*display->formats) * caps->format_count);
        if (display->formats == NULL) {
            free_connection_data(display);
            return MD_ERR_NOMEM;
        }
        memcpy(display->formats, caps->formats, sizeof(*display->formats) * caps->format_count);
        display->caps.formats = display->formats;
    }
    return MD_OK;
}

md_display_t* md_display_new(const md_display_callbacks_t* callbacks) {
    md_display_t* display = calloc(1, sizeof(*display));
    if (display == NULL) return NULL;
    if (callbacks != NULL) display->callbacks = *callbacks;
    md_client_init(&display->client, MD_CLIENT_ROLE_DISPLAY, &display_client_ops, display);
    md_init_pool(&display->pool);
    return display;
}

void md_display_free(md_display_t* display) {
    if (display == NULL) return;
    md_display_close(display);
    free_connection_data(display);
    free(display);
}

int md_display_begin_connect(md_display_t* display, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_output_info_t* output, const md_consumer_caps_t* caps) {
    if (display == NULL || display->client.connection_state != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    int rc = copy_connect_args(display, socket_path, client_name, client_version, output, caps);
    if (rc != MD_OK) return rc;
    rc = md_client_begin_connect(&display->client);
    if (rc != MD_OK) return rc;
    display->output_id = 0;
    return MD_OK;
}

int md_display_begin_connected_fd(md_display_t* display, int connected_fd,
                                  const char* client_name, const char* client_version,
                                  const md_output_info_t* output,
                                  const md_consumer_caps_t* caps) {
    if (display == NULL || connected_fd < 0 ||
        display->client.connection_state != MD_CONNECTION_DISCONNECTED) return MD_ERR_STATE;
    int rc = copy_connect_args(display, "", client_name, client_version, output, caps);
    if (rc != MD_OK) return rc;
    rc = md_client_begin_connected_fd(&display->client, connected_fd);
    if (rc != MD_OK) return rc;
    display->output_id = 0;
    return MD_OK;
}

int md_display_advance_handshake(md_display_t* display) {
    if (display == NULL) return MD_ERR_STATE;
    return md_client_advance_handshake(&display->client);
}

int md_display_connect(md_display_t* display, const char* socket_path,
                       const char* client_name, const char* client_version,
                       const md_output_info_t* output, const md_consumer_caps_t* caps,
                       int timeout_ms) {
    if (display == NULL) return MD_ERR_STATE;
    int rc = copy_connect_args(display, socket_path, client_name, client_version, output, caps);
    if (rc != MD_OK) return rc;
    display->output_id = 0;
    return md_client_connect(&display->client, timeout_ms);
}

void md_display_close(md_display_t* display) {
    if (display == NULL) return;
    md_client_close(&display->client);
    abandon_pool(display, true);
    display->output_id = 0;
}
int md_display_get_fd(const md_display_t* display) { return display != NULL ? display->client.fd : -1; }
md_connection_state_t md_display_connection_state(const md_display_t* display) {
    return display != NULL ? display->client.connection_state : MD_CONNECTION_DEAD;
}
md_handshake_state_t md_display_handshake_state(const md_display_t* display) {
    return display != NULL ? display->client.handshake_state : MD_HANDSHAKE_IDLE;
}
uint64_t md_display_output_id(const md_display_t* display) {
    return display != NULL ? display->output_id : 0;
}

static bool coalescible(uint16_t opcode) {
    return opcode == MD_OP_POINTER_MOTION || opcode == MD_OP_UPDATE_OUTPUT ||
           opcode == MD_OP_WINDOW_STATE;
}

static int flush_outbox(md_display_t* display) {
    int rc = md_client_flush_outbox(&display->client);
    if (rc == MD_ERR_WOULD_BLOCK) return MD_ERR_WOULD_BLOCK;
    if (rc < 0) return fail_session(display, md_map_io_error(rc), "outbox send failed");
    return MD_OK;
}

static int queue_message(md_display_t* display, uint16_t opcode, uint16_t flags,
                         const uint8_t* payload, size_t size) {
    if (display == NULL || display->client.connection_state != MD_CONNECTION_READY) {
        return MD_ERR_STATE;
    }
    uint32_t serial = display->client.next_serial++;
    int rc = md_outbox_send_or_queue(&display->client.outbox, display->client.fd,
                                     display->client.selected_minor, opcode, flags, serial,
                                     payload, size, NULL, 0, coalescible(opcode));
    if (rc == MD_OK || rc == MD_ERR_WOULD_BLOCK || rc == MD_ERR_NOMEM ||
        rc == MD_ERR_INVALID) {
        return rc;
    }
    return fail_session(display, md_map_io_error(rc), "request send failed");
}

bool md_display_wants_writable(const md_display_t* display) {
    return display != NULL && display->client.outbox.head != NULL;
}

int md_display_handle_writable(md_display_t* display) {
    if (display == NULL || display->client.connection_state != MD_CONNECTION_READY) {
        return MD_ERR_STATE;
    }
    int rc = flush_outbox(display);
    return rc == MD_ERR_WOULD_BLOCK ? MD_OK : rc;
}
int md_display_defer_unbind(md_display_t* display) {
    if (display == NULL) return MD_ERR_INVALID;
    if (!display->in_unbind_callback || display->pending_unbind_generation == 0) {
        return MD_ERR_STATE;
    }
    display->unbind_deferred = true;
    return MD_OK;
}

uint64_t md_display_pending_unbind_generation(const md_display_t* display) {
    if (display == NULL || !display->unbind_deferred) return 0;
    return display->pending_unbind_generation;
}

static int complete_unbind(md_display_t* display, uint64_t generation) {
    if (display == NULL || generation == 0 || !display->pool_active ||
        display->pending_unbind_generation != generation) {
        return MD_ERR_STATE;
    }
    release_pool(display, false);
    display->in_unbind_callback = false;
    display->unbind_deferred = false;
    display->pending_unbind_generation = 0;

    uint8_t payload[8];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_u64(&writer, generation) != 0) return MD_ERR_PROTOCOL;
    return queue_message(display, MD_OP_UNBIND_DONE, 0, payload, writer.size);
}

int md_display_finish_unbind(md_display_t* display, uint64_t generation) {
    if (display == NULL) return MD_ERR_INVALID;
    if (!display->unbind_deferred) return MD_ERR_STATE;
    return complete_unbind(display, generation);
}

static int process_packet(md_display_t* display, md_packet_t* packet) {
    if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
        packet->minor != display->client.selected_minor) {
        return fail_session(display, MD_ERR_PROTOCOL, "wire version changed during session");
    }
    if (packet->opcode == MD_OP_ERROR) {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "error has FDs");
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed error packet");
        }
        md_result_t reason = error.fatal ? MD_ERR_PROTOCOL : MD_ERR_IO;
        int rc = error.fatal ? fail_session(display, reason, error.message) : MD_OK;
        md_proto_error_clear(&error);
        return rc;
    }

    if (display->pending_unbind_generation != 0) {
        return fail_session(display, MD_ERR_PROTOCOL,
                            "packet received before deferred unbind completed");
    }

    switch (packet->opcode) {
    case MD_OP_BIND_BUFFERS: {
        if (display->pool_active) return fail_session(display, MD_ERR_PROTOCOL, "pool rebound without unbind");
        md_buffer_pool_t pool;
        if (md_proto_decode_bind_buffers(packet->payload, packet->payload_size, &pool) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed buffer pool");
        }
        size_t expected = (size_t)pool.buffer_count * (size_t)pool.plane_count;
        if (packet->fd_count != expected || pool.generation == 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "buffer pool FD count mismatch");
        }
        size_t index = 0;
        for (uint32_t b = 0; b < pool.buffer_count; ++b) {
            for (uint32_t p = 0; p < pool.plane_count; ++p) {
                pool.planes[b][p].fd = packet->fds[index];
                packet->fds[index++] = -1;
            }
        }
        packet->fd_count = 0;
        display->pool = pool;
        display->pool_active = true;
        if (display->callbacks.on_buffers_ready != NULL) {
            display->callbacks.on_buffers_ready(display->callbacks.user_data, &display->pool);
        }
        return MD_OK;
    }
    case MD_OP_SET_CONFIG: {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "config has FDs");
        md_display_config_t config;
        if (md_proto_decode_config(packet->payload, packet->payload_size, &config) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed display config");
        }
        if (display->callbacks.on_config != NULL) {
            display->callbacks.on_config(display->callbacks.user_data, &config);
        }
        return MD_OK;
    }
    case MD_OP_FRAME_READY: {
        if (packet->fd_count != 2) {
            if (packet->fd_count >= 1 && packet->fds[0] >= 0) {
                close(packet->fds[0]);
                packet->fds[0] = -1;
            }
            if (packet->fd_count >= 2 && packet->fds[1] >= 0) {
                (void)md_display_signal_release_syncobj_on_node(
                    packet->fds[1], display->output.drm_render_major,
                    display->output.drm_render_minor);
                packet->fds[1] = -1;
            }
            return fail_session(display, MD_ERR_PROTOCOL, "frame FD count mismatch");
        }
        md_frame_t frame;
        if (md_proto_decode_frame(packet->payload, packet->payload_size, &frame) != 0) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return fail_session(display, MD_ERR_PROTOCOL, "malformed frame");
        }
        if (!display->pool_active || frame.buffer_generation != display->pool.generation) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return MD_OK;
        }
        if (frame.buffer_index >= display->pool.buffer_count) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return fail_session(display, MD_ERR_PROTOCOL, "frame buffer index out of range");
        }
        frame.acquire_sync_fd = packet->fds[0];
        frame.release_syncobj_fd = packet->fds[1];
        packet->fds[0] = -1; packet->fds[1] = -1; packet->fd_count = 0;
        if (display->callbacks.on_frame != NULL) {
            display->callbacks.on_frame(display->callbacks.user_data, &frame);
        } else {
            close(frame.acquire_sync_fd);
            close(frame.release_syncobj_fd);
        }
        return MD_OK;
    }
    case MD_OP_UNBIND: {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "unbind has FDs");
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            !display->pool_active || generation != display->pool.generation) {
            return fail_session(display, MD_ERR_PROTOCOL, "invalid unbind generation");
        }
        if (display->pending_unbind_generation != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "overlapping unbind");
        }
        display->pending_unbind_generation = generation;
        display->in_unbind_callback = true;
        display->unbind_deferred = false;
        if (display->callbacks.on_buffers_releasing != NULL) {
            display->callbacks.on_buffers_releasing(display->callbacks.user_data, &display->pool);
        }
        display->in_unbind_callback = false;
        if (display->unbind_deferred) return MD_OK;
        return complete_unbind(display, generation);
    }
    default:
        if ((packet->flags & MD_PACKET_OPTIONAL) != 0) return MD_OK;
        return fail_session(display, MD_ERR_PROTOCOL, "unknown required opcode");
    }
}

int md_display_dispatch(md_display_t* display) {
    if (display == NULL || display->client.connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int count = 0;
    for (;;) {
        md_packet_t packet;
        int rc = md_codec_recv(display->client.fd, &packet);
        if (rc == 0) break;
        if (rc < 0) return fail_session(display, md_map_io_error(rc), "session receive failed");
        rc = process_packet(display, &packet);
        md_packet_close_fds(&packet);
        if (rc < 0) return rc;
        ++count;
    }
    int rc = flush_outbox(display);
    if (rc < 0 && rc != MD_ERR_WOULD_BLOCK) return rc;
    return count;
}

int md_display_update_output(md_display_t* display, const md_output_info_t* output) {
    if (!valid_output(output)) return MD_ERR_INVALID;
    uint8_t payload[128];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_update_output(&writer, output) != 0) return MD_ERR_INVALID;
    return queue_message(display, MD_OP_UPDATE_OUTPUT, 0, payload, writer.size);
}

int md_display_send_pointer_enter(md_display_t* display, float x, float y,
                                  uint64_t timestamp_us) {
    uint8_t payload[32];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_pointer_enter(&writer, x, y, timestamp_us) != 0) return MD_ERR_INVALID;
    return queue_message(display, MD_OP_POINTER_ENTER, 0, payload, writer.size);
}

int md_display_send_pointer_leave(md_display_t* display, uint64_t timestamp_us) {
    uint8_t payload[16];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_pointer_leave(&writer, timestamp_us) != 0) return MD_ERR_INVALID;
    return queue_message(display, MD_OP_POINTER_LEAVE, 0, payload, writer.size);
}

int md_display_send_pointer_motion(md_display_t* display, float x, float y,
                                   uint64_t timestamp_us, uint32_t modifiers) {
    uint8_t payload[32];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_pointer_motion(&writer, x, y, timestamp_us, modifiers) != 0) {
        return MD_ERR_INVALID;
    }
    return queue_message(display, MD_OP_POINTER_MOTION, 0, payload, writer.size);
}

int md_display_send_pointer_button(md_display_t* display, float x, float y, uint32_t button,
                                   md_button_state_t state, uint64_t timestamp_us,
                                   uint32_t modifiers) {
    if (state != MD_BUTTON_RELEASED && state != MD_BUTTON_PRESSED) return MD_ERR_INVALID;
    uint8_t payload[40];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_pointer_button(&writer, x, y, button, state, timestamp_us,
                                       modifiers) != 0) {
        return MD_ERR_INVALID;
    }
    return queue_message(display, MD_OP_POINTER_BUTTON, 0, payload, writer.size);
}

int md_display_send_pointer_axis(md_display_t* display, float x, float y, float delta_x,
                                 float delta_y, md_axis_source_t source,
                                 uint64_t timestamp_us, uint32_t modifiers) {
    if (source < MD_AXIS_WHEEL || source > MD_AXIS_CONTINUOUS) return MD_ERR_INVALID;
    uint8_t payload[48];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_pointer_axis(&writer, x, y, delta_x, delta_y, source,
                                     timestamp_us, modifiers) != 0) {
        return MD_ERR_INVALID;
    }
    return queue_message(display, MD_OP_POINTER_AXIS, 0, payload, writer.size);
}

int md_display_send_window_state(md_display_t* display, uint32_t flags) {
    uint8_t payload[8];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_u32(&writer, flags) != 0) return MD_ERR_INVALID;
    return queue_message(display, MD_OP_WINDOW_STATE, 0, payload, writer.size);
}

