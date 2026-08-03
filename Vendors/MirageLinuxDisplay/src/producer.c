#define _GNU_SOURCE

#include "mirage_display_producer.h"

#include "codec.h"
#include "common/handshake.h"
#include "common/outbox.h"
#include "common/util.h"
#include "protocol.h"

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

struct md_producer {
    md_client_t client;
    md_producer_callbacks_t callbacks;
    uint64_t producer_id;
    uint64_t output_id;
    char* stable_output_id;
    char* kind;
    md_producer_info_t info;
    md_format_cap_t* formats;

    bool pool_offered;
    bool retire_pending;
    uint64_t pool_generation;
    uint32_t pool_buffer_count;
};

static void clear_connect_args(md_producer_t* producer) {
    md_client_clear_identity(&producer->client);
    free(producer->stable_output_id);
    free(producer->kind);
    free(producer->formats);
    producer->stable_output_id = NULL;
    producer->kind = NULL;
    producer->formats = NULL;
    memset(&producer->info, 0, sizeof(producer->info));
}

static void producer_notify_disconnected(void* object, md_result_t reason,
                                         const char* message) {
    md_producer_t* producer = object;
    producer->pool_offered = false;
    producer->retire_pending = false;
    producer->pool_generation = 0;
    producer->pool_buffer_count = 0;
    if (!producer->client.disconnected_notified && producer->callbacks.on_disconnected != NULL) {
        producer->client.disconnected_notified = true;
        producer->callbacks.on_disconnected(producer->callbacks.user_data, reason, message);
    }
}

static int fail_producer(md_producer_t* producer, md_result_t reason, const char* message) {
    return md_client_disconnect(&producer->client, reason, message);
}

static int producer_encode_register(void* object, md_writer_t* writer) {
    md_producer_t* producer = object;
    return md_proto_encode_register_producer(writer, &producer->info);
}

static int producer_apply_accepted(void* object, const md_packet_t* packet) {
    md_producer_t* producer = object;
    uint64_t producer_id;
    uint64_t output_id;
    if (md_proto_decode_producer_accepted(packet->payload, packet->payload_size,
                                          &producer_id, &output_id) != 0 ||
        producer_id == 0 || output_id == 0) {
        return MD_ERR_PROTOCOL;
    }
    producer->producer_id = producer_id;
    producer->output_id = output_id;
    return MD_OK;
}

static void producer_on_ready(void* object) {
    md_producer_t* producer = object;
    if (producer->callbacks.on_connected != NULL) {
        producer->callbacks.on_connected(producer->callbacks.user_data,
                                         producer->producer_id, producer->output_id);
    }
}

static const md_client_ops_t producer_client_ops = {
    .register_opcode = MD_OP_REGISTER_PRODUCER,
    .accepted_opcode = MD_OP_PRODUCER_ACCEPTED,
    .caps_opcode = 0,
    .encode_register = producer_encode_register,
    .encode_caps = NULL,
    .apply_accepted = producer_apply_accepted,
    .on_ready = producer_on_ready,
    .notify_disconnected = producer_notify_disconnected,
};
static bool valid_info(const md_producer_info_t* info) {
    if (info == NULL || info->stable_output_id == NULL || info->kind == NULL ||
        info->format_count == 0 || info->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        info->formats == NULL) return false;
    for (uint32_t i = 0; i < info->format_count; ++i) {
        if (info->formats[i].plane_count == 0 ||
            info->formats[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES) return false;
    }
    return true;
}

static int copy_connect_args(md_producer_t* producer, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_producer_info_t* info) {
    if (!valid_info(info)) return MD_ERR_INVALID;
    int rc = md_client_set_identity(&producer->client, socket_path, client_name, client_version);
    if (rc != MD_OK) return rc;
    free(producer->stable_output_id);
    free(producer->kind);
    free(producer->formats);
    producer->stable_output_id = md_strdup(info->stable_output_id);
    producer->kind = md_strdup(info->kind);
    if (producer->stable_output_id == NULL || producer->kind == NULL) {
        clear_connect_args(producer);
        return MD_ERR_NOMEM;
    }
    producer->formats = malloc(sizeof(*producer->formats) * info->format_count);
    if (producer->formats == NULL) {
        clear_connect_args(producer);
        return MD_ERR_NOMEM;
    }
    memcpy(producer->formats, info->formats, sizeof(*producer->formats) * info->format_count);
    producer->info = *info;
    producer->info.stable_output_id = producer->stable_output_id;
    producer->info.kind = producer->kind;
    producer->info.formats = producer->formats;
    return MD_OK;
}

md_producer_t* md_producer_new(const md_producer_callbacks_t* callbacks) {
    md_producer_t* producer = calloc(1, sizeof(*producer));
    if (producer == NULL) return NULL;
    if (callbacks != NULL) producer->callbacks = *callbacks;
    md_client_init(&producer->client, MD_CLIENT_ROLE_PRODUCER, &producer_client_ops, producer);
    producer->client.advertised_features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                           MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS;
    return producer;
}

void md_producer_free(md_producer_t* producer) {
    if (producer == NULL) return;
    md_producer_close(producer);
    clear_connect_args(producer);
    free(producer);
}

int md_producer_begin_connect(md_producer_t* producer, const char* socket_path,
                              const char* client_name, const char* client_version,
                              const md_producer_info_t* info) {
    if (producer == NULL || producer->client.connection_state != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    int rc = copy_connect_args(producer, socket_path, client_name, client_version, info);
    if (rc != MD_OK) return rc;
    rc = md_client_begin_connect(&producer->client);
    if (rc != MD_OK) return rc;
    producer->producer_id = 0;
    producer->output_id = 0;
    return MD_OK;
}

int md_producer_begin_connected_fd(md_producer_t* producer, int connected_fd,
                                   const char* client_name, const char* client_version,
                                   const md_producer_info_t* info) {
    if (producer == NULL || connected_fd < 0 ||
        producer->client.connection_state != MD_CONNECTION_DISCONNECTED) return MD_ERR_STATE;
    int rc = copy_connect_args(producer, "", client_name, client_version, info);
    if (rc != MD_OK) return rc;
    rc = md_client_begin_connected_fd(&producer->client, connected_fd);
    if (rc != MD_OK) return rc;
    producer->producer_id = 0;
    producer->output_id = 0;
    return MD_OK;
}

int md_producer_advance_handshake(md_producer_t* producer) {
    if (producer == NULL) return MD_ERR_STATE;
    return md_client_advance_handshake(&producer->client);
}

int md_producer_connect(md_producer_t* producer, const char* socket_path,
                        const char* client_name, const char* client_version,
                        const md_producer_info_t* info, int timeout_ms) {
    if (producer == NULL) return MD_ERR_STATE;
    int rc = copy_connect_args(producer, socket_path, client_name, client_version, info);
    if (rc != MD_OK) return rc;
    producer->producer_id = 0;
    producer->output_id = 0;
    return md_client_connect(&producer->client, timeout_ms);
}

void md_producer_close(md_producer_t* producer) {
    if (producer == NULL) return;
    md_client_close(&producer->client);
    producer->producer_id = 0;
    producer->output_id = 0;
    producer->pool_offered = false;
    producer->retire_pending = false;
    producer->pool_generation = 0;
    producer->pool_buffer_count = 0;
}
int md_producer_get_fd(const md_producer_t* producer) { return producer != NULL ? producer->client.fd : -1; }
md_connection_state_t md_producer_connection_state(const md_producer_t* producer) {
    return producer != NULL ? producer->client.connection_state : MD_CONNECTION_DEAD;
}
md_handshake_state_t md_producer_handshake_state(const md_producer_t* producer) {
    return producer != NULL ? producer->client.handshake_state : MD_HANDSHAKE_IDLE;
}

static int flush_outbox(md_producer_t* producer) {
    int rc = md_client_flush_outbox(&producer->client);
    if (rc == MD_ERR_WOULD_BLOCK) return MD_ERR_WOULD_BLOCK;
    if (rc < 0) return fail_producer(producer, md_map_io_error(rc), "producer outbox failed");
    return MD_OK;
}

static int send_owned(md_producer_t* producer, uint16_t opcode,
                      const uint8_t* payload, size_t payload_size,
                      int* fds, size_t fd_count) {
    if (producer == NULL || producer->client.connection_state != MD_CONNECTION_READY) {
        md_close_fds(fds, fd_count);
        return MD_ERR_STATE;
    }
    if (fd_count > MD_WIRE_MAX_FDS) {
        md_close_fds(fds, fd_count);
        return MD_ERR_INVALID;
    }
    uint32_t serial = producer->client.next_serial++;
    int rc = md_outbox_send_or_queue(&producer->client.outbox, producer->client.fd,
                                     producer->client.selected_minor, opcode, 0, serial,
                                     payload, payload_size, fds, fd_count, false);
    if (rc == MD_OK || rc == MD_ERR_WOULD_BLOCK || rc == MD_ERR_NOMEM ||
        rc == MD_ERR_INVALID) {
        return rc;
    }
    return fail_producer(producer, md_map_io_error(rc), "producer request failed");
}
bool md_producer_wants_writable(const md_producer_t* producer) {
    return producer != NULL && producer->client.outbox.head != NULL;
}

int md_producer_handle_writable(md_producer_t* producer) {
    if (producer == NULL || producer->client.connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int rc = flush_outbox(producer);
    return rc == MD_ERR_WOULD_BLOCK ? MD_OK : rc;
}

static int process_packet(md_producer_t* producer, md_packet_t* packet) {
    if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
        packet->minor != producer->client.selected_minor) {
        return fail_producer(producer, MD_ERR_PROTOCOL, "producer wire version changed");
    }
    if (packet->fd_count != 0) {
        return fail_producer(producer, MD_ERR_PROTOCOL, "unexpected producer event FDs");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed broker error");
        }
        int rc = error.fatal ? fail_producer(producer, MD_ERR_PROTOCOL, error.message) : MD_OK;
        md_proto_error_clear(&error);
        return rc;
    }
    switch (packet->opcode) {
    case MD_OP_OUTPUT_CONFIG: {
        md_producer_config_t config;
        if (md_proto_decode_output_config(packet->payload, packet->payload_size, &config) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed output config");
        }
        if (producer->callbacks.on_output_config != NULL) {
            producer->callbacks.on_output_config(producer->callbacks.user_data, &config);
        }
        return MD_OK;
    }
    case MD_OP_RETIRE_BUFFERS: {
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            !producer->pool_offered || producer->retire_pending ||
            generation != producer->pool_generation) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "invalid retire generation");
        }
        producer->retire_pending = true;
        if (producer->callbacks.on_retire_buffers != NULL) {
            producer->callbacks.on_retire_buffers(producer->callbacks.user_data, generation);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_ENTER: {
        md_pointer_enter_t event;
        if (md_proto_decode_pointer_enter(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer enter");
        }
        if (producer->callbacks.on_pointer_enter != NULL) {
            producer->callbacks.on_pointer_enter(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_LEAVE: {
        uint64_t timestamp;
        if (md_proto_decode_pointer_leave(packet->payload, packet->payload_size, &timestamp) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer leave");
        }
        if (producer->callbacks.on_pointer_leave != NULL) {
            producer->callbacks.on_pointer_leave(producer->callbacks.user_data, timestamp);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_MOTION: {
        md_pointer_motion_t event;
        if (md_proto_decode_pointer_motion(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer motion");
        }
        if (producer->callbacks.on_pointer_motion != NULL) {
            producer->callbacks.on_pointer_motion(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_BUTTON: {
        md_pointer_button_t event;
        if (md_proto_decode_pointer_button(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer button");
        }
        if (producer->callbacks.on_pointer_button != NULL) {
            producer->callbacks.on_pointer_button(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_AXIS: {
        md_pointer_axis_t event;
        if (md_proto_decode_pointer_axis(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer axis");
        }
        if (producer->callbacks.on_pointer_axis != NULL) {
            producer->callbacks.on_pointer_axis(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    default:
        if ((packet->flags & MD_PACKET_OPTIONAL) != 0) return MD_OK;
        return fail_producer(producer, MD_ERR_PROTOCOL, "unknown required producer opcode");
    }
}

int md_producer_dispatch(md_producer_t* producer) {
    if (producer == NULL || producer->client.connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int count = 0;
    for (;;) {
        md_packet_t packet;
        int rc = md_codec_recv(producer->client.fd, &packet);
        if (rc == 0) break;
        if (rc < 0) return fail_producer(producer, md_map_io_error(rc), "producer receive failed");
        rc = process_packet(producer, &packet);
        md_packet_close_fds(&packet);
        if (rc < 0) return rc;
        ++count;
    }
    int rc = flush_outbox(producer);
    if (rc < 0 && rc != MD_ERR_WOULD_BLOCK) return rc;
    return count;
}

int md_producer_offer_buffers(md_producer_t* producer, const md_buffer_pool_t* pool) {
    if (producer == NULL || pool == NULL || producer->pool_offered) return MD_ERR_STATE;
    uint8_t payload[1024];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_offer_buffers(&writer, pool) != 0) return MD_ERR_INVALID;
    size_t fd_count = (size_t)pool->buffer_count * (size_t)pool->plane_count;
    if (fd_count > MD_WIRE_MAX_FDS) return MD_ERR_INVALID;
    int fds[MD_WIRE_MAX_FDS];
    for (size_t i = 0; i < MD_WIRE_MAX_FDS; ++i) fds[i] = -1;
    size_t index = 0;
    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (pool->planes[b][p].fd < 0) {
                md_close_fds(fds, index);
                return MD_ERR_INVALID;
            }
            fds[index] = fcntl(pool->planes[b][p].fd, F_DUPFD_CLOEXEC, 0);
            if (fds[index] < 0) {
                md_close_fds(fds, index);
                return MD_ERR_IO;
            }
            ++index;
        }
    }
    int rc = send_owned(producer, MD_OP_OFFER_BUFFERS, payload, writer.size, fds, fd_count);
    if (rc == MD_OK) {
        producer->pool_offered = true;
        producer->pool_generation = pool->generation;
        producer->pool_buffer_count = pool->buffer_count;
    }
    return rc;
}

int md_producer_set_config(md_producer_t* producer, const md_display_config_t* config) {
    if (producer == NULL || config == NULL || !producer->pool_offered ||
        producer->retire_pending) return MD_ERR_STATE;
    uint8_t payload[128];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_config(&writer, config) != 0) return MD_ERR_INVALID;
    return send_owned(producer, MD_OP_PRODUCER_SET_CONFIG, payload, writer.size, NULL, 0);
}

int md_producer_submit_frame(md_producer_t* producer, uint64_t generation,
                             uint32_t buffer_index, uint64_t sequence,
                             int acquire_sync_fd, int release_syncobj_fd) {
    int fds[2] = {acquire_sync_fd, release_syncobj_fd};
    if (producer == NULL || acquire_sync_fd < 0 || release_syncobj_fd < 0 ||
        !producer->pool_offered || producer->retire_pending ||
        generation != producer->pool_generation ||
        buffer_index >= producer->pool_buffer_count) {
        md_close_fds(fds, 2);
        return MD_ERR_STATE;
    }
    uint8_t payload[32];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_producer_frame(&writer, generation, buffer_index, sequence) != 0) {
        md_close_fds(fds, 2);
        return MD_ERR_INVALID;
    }
    return send_owned(producer, MD_OP_PRODUCER_FRAME, payload, writer.size, fds, 2);
}

int md_producer_retire_done(md_producer_t* producer, uint64_t generation) {
    if (producer == NULL || !producer->pool_offered || !producer->retire_pending ||
        generation != producer->pool_generation) {
        return MD_ERR_STATE;
    }
    uint8_t payload[8];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_u64(&writer, generation) != 0) return MD_ERR_INVALID;
    int rc = send_owned(producer, MD_OP_RETIRE_DONE, payload, writer.size, NULL, 0);
    if (rc == MD_OK) {
        producer->pool_offered = false;
        producer->retire_pending = false;
        producer->pool_generation = 0;
        producer->pool_buffer_count = 0;
    }
    return rc;
}

