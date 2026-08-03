#define _GNU_SOURCE

#include "mirage_display_broker.h"

#include "codec.h"
#include "common/net.h"
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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MD_BROKER_MAX_PEERS 32u
#define MD_BROKER_DEFAULT_ROUTES 16u

typedef enum md_broker_role {
    MD_BROKER_ROLE_NONE = 0,
    MD_BROKER_ROLE_DISPLAY = 1,
    MD_BROKER_ROLE_PRODUCER = 2,
} md_broker_role_t;

typedef struct md_broker_peer md_broker_peer_t;
typedef struct md_broker_route md_broker_route_t;
typedef struct md_broker_fanout md_broker_fanout_t;

struct md_broker_peer {
    int fd;
    md_broker_role_t role;
    bool hello_done;
    bool ready;
    uint16_t minor;
    uint64_t features;
    uint64_t id;
    uint32_t next_serial;
    char* client_name;
    char* client_version;
    md_broker_route_t* route;
    bool display_pool_sent;
    bool display_unbind_pending;
    uint64_t display_unbind_generation;

    char* output_stable_id;
    char* output_name;
    md_output_info_t output;
    md_consumer_caps_t caps;
    md_format_cap_t* cap_formats;

    char* producer_stable_id;
    char* producer_kind;
    md_producer_info_t producer_info;
    md_format_cap_t* producer_formats;

    md_outbox_t outbox;
};

struct md_broker_route {
    uint64_t output_id;
    char* stable_id;
    md_broker_peer_t* display;
    md_broker_peer_t* displays[MD_BROKER_MAX_PEERS];
    uint32_t display_count;
    md_broker_peer_t* producer;
    md_format_cap_t selected_format;
    bool format_selected;
    bool output_config_sent;
    bool pool_active;
    bool retire_pending;
    bool unbind_pending;
    uint64_t pending_unbind_generation;
    uint64_t pool_generation;
    md_buffer_pool_t pool;
    bool config_active;
    md_display_config_t config;
    bool unbind_retiring_old_producer;
};

struct md_broker_fanout {
    md_broker_fanout_t* next;
    md_sync_fanout_t* sync;
    uint32_t display_count;
    md_broker_peer_t* displays[MD_BROKER_MAX_PEERS];
};

struct md_broker {
    int listen_fd;
    bool listening;
    atomic_bool stopping;
    char* socket_path;
    char* server_name;
    char* server_version;
    uint64_t features;
    uint32_t max_routes;
    uint64_t next_output_id;
    uint64_t next_peer_id;
    md_broker_peer_t* peers[MD_BROKER_MAX_PEERS];
    md_broker_route_t* routes;
    uint32_t route_count;
    md_broker_fanout_t* fanouts;
};
static void poll_fanouts(md_broker_t* broker) {
    md_broker_fanout_t** link = &broker->fanouts;
    while (*link != NULL) {
        md_broker_fanout_t* fanout = *link;
        int rc = md_sync_fanout_poll(fanout->sync);
        if (rc == 0) {
            link = &fanout->next;
            continue;
        }
        *link = fanout->next;
        md_sync_fanout_free(fanout->sync);
        free(fanout);
    }
}

static void abandon_peer_fanouts(md_broker_t* broker, md_broker_peer_t* peer) {
    for (md_broker_fanout_t* fanout = broker->fanouts; fanout != NULL;
         fanout = fanout->next) {
        for (uint32_t i = 0; i < fanout->display_count; ++i) {
            if (fanout->displays[i] == peer) md_sync_fanout_abandon(fanout->sync, i);
        }
    }
}

static void free_fanouts(md_broker_t* broker) {
    md_broker_fanout_t* fanout = broker->fanouts;
    while (fanout != NULL) {
        md_broker_fanout_t* next = fanout->next;
        md_sync_fanout_free(fanout->sync);
        free(fanout);
        fanout = next;
    }
    broker->fanouts = NULL;
}

static void clear_peer_data(md_broker_peer_t* peer) {
    free(peer->client_name);
    free(peer->client_version);
    free(peer->output_stable_id);
    free(peer->output_name);
    free(peer->cap_formats);
    free(peer->producer_stable_id);
    free(peer->producer_kind);
    free(peer->producer_formats);
    peer->client_name = NULL;
    peer->client_version = NULL;
    peer->output_stable_id = NULL;
    peer->output_name = NULL;
    peer->cap_formats = NULL;
    peer->producer_stable_id = NULL;
    peer->producer_kind = NULL;
    peer->producer_formats = NULL;
    memset(&peer->output, 0, sizeof(peer->output));
    memset(&peer->caps, 0, sizeof(peer->caps));
    memset(&peer->producer_info, 0, sizeof(peer->producer_info));
}

static void free_peer(md_broker_peer_t* peer) {
    if (peer == NULL) return;
    if (peer->fd >= 0) close(peer->fd);
    md_outbox_clear(&peer->outbox);
    clear_peer_data(peer);
    free(peer);
}

static int peer_index(const md_broker_t* broker, const md_broker_peer_t* peer) {
    for (size_t i = 0; i < MD_BROKER_MAX_PEERS; ++i) {
        if (broker->peers[i] == peer) return (int)i;
    }
    return -1;
}

static void remove_peer_slot(md_broker_t* broker, md_broker_peer_t* peer) {
    int index = peer_index(broker, peer);
    if (index >= 0) broker->peers[(size_t)index] = NULL;
}

static void free_route(md_broker_route_t* route) {
    if (route == NULL) return;
    md_close_pool(&route->pool);
    free(route->stable_id);
    route->stable_id = NULL;
}

static void remove_route(md_broker_t* broker, md_broker_route_t* route) {
    if (route == NULL) return;
    size_t index = (size_t)(route - broker->routes);
    if (index >= broker->route_count) return;
    free_route(route);
    if (index + 1u < broker->route_count) {
        broker->routes[index] = broker->routes[broker->route_count - 1u];
        md_broker_route_t* moved = &broker->routes[index];
        if (moved->display != NULL) moved->display->route = moved;
        if (moved->producer != NULL) moved->producer->route = moved;
    }
    --broker->route_count;
}

static md_broker_route_t* find_route(const md_broker_t* broker, const char* stable_id) {
    for (uint32_t i = 0; i < broker->route_count; ++i) {
        if (strcmp(broker->routes[i].stable_id, stable_id) == 0) return &broker->routes[i];
    }
    return NULL;
}

static bool route_has_display(const md_broker_route_t* route, const md_broker_peer_t* peer) {
    if (route == NULL || peer == NULL) return false;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i] == peer) return true;
    }
    return false;
}

static int route_add_display(md_broker_route_t* route, md_broker_peer_t* peer) {
    if (route == NULL || peer == NULL || route->display_count >= MD_BROKER_MAX_PEERS ||
        route_has_display(route, peer)) return MD_ERR_STATE;
    route->displays[route->display_count++] = peer;
    if (route->display == NULL) route->display = peer;
    peer->display_pool_sent = false;
    peer->display_unbind_pending = false;
    peer->display_unbind_generation = 0;
    return MD_OK;
}

static void route_remove_display(md_broker_route_t* route, md_broker_peer_t* peer) {
    if (route == NULL || peer == NULL) return;
    uint32_t index = route->display_count;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i] == peer) {
            index = i;
            break;
        }
    }
    if (index == route->display_count) return;
    if (index + 1u < route->display_count) {
        memmove(&route->displays[index], &route->displays[index + 1u],
                sizeof(route->displays[0]) * (route->display_count - index - 1u));
    }
    --route->display_count;
    route->displays[route->display_count] = NULL;
    route->display = route->display_count > 0 ? route->displays[0] : NULL;
}

static bool route_all_unbound(const md_broker_route_t* route) {
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i]->display_unbind_pending) return false;
    }
    return true;
}

static bool reclaim_idle_route(md_broker_t* broker) {
    for (uint32_t i = 0; i < broker->route_count; ++i) {
        md_broker_route_t* candidate = &broker->routes[i];
        if (candidate->display == NULL && candidate->producer == NULL &&
            !candidate->pool_active && !candidate->unbind_pending) {
            remove_route(broker, candidate);
            return true;
        }
    }
    return false;
}

static md_broker_route_t* create_route(md_broker_t* broker, const char* stable_id) {
    if (broker->route_count >= broker->max_routes && !reclaim_idle_route(broker)) {
        return NULL;
    }
    md_broker_route_t* resized = realloc(
        broker->routes, sizeof(*broker->routes) * (size_t)(broker->route_count + 1u));
    if (resized == NULL) return NULL;
    broker->routes = resized;
    md_broker_route_t* route = &broker->routes[broker->route_count++];
    memset(route, 0, sizeof(*route));
    md_init_pool(&route->pool);
    route->stable_id = md_strdup(stable_id);
    if (route->stable_id == NULL) {
        --broker->route_count;
        return NULL;
    }
    route->output_id = broker->next_output_id++;
    if (route->output_id == 0) route->output_id = broker->next_output_id++;
    return route;
}

static int queue_peer(md_broker_peer_t* peer, uint16_t opcode, uint16_t flags,
                      const uint8_t* payload, size_t payload_size, int* fds, size_t fd_count);

static int send_error(md_broker_peer_t* peer, uint32_t code, bool fatal, const char* message) {
    uint8_t payload[MD_WIRE_MAX_PAYLOAD];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_write_u32(&writer, code) != 0 || md_write_u32(&writer, fatal ? 1u : 0u) != 0 ||
        md_write_string(&writer, message != NULL ? message : "broker error") != 0) {
        return MD_ERR_PROTOCOL;
    }
    return queue_peer(peer, MD_OP_ERROR, 0, payload, writer.size, NULL, 0);
}

static int encode_welcome(md_broker_t* broker, uint8_t* payload, size_t capacity, size_t* size) {
    md_writer_t writer;
    md_writer_init(&writer, payload, capacity);
    if (md_write_u16(&writer, MIRAGE_DISPLAY_PROTOCOL_MINOR) != 0 ||
        md_write_u16(&writer, 0) != 0 || md_write_u64(&writer, broker->features) != 0 ||
        md_write_string(&writer, broker->server_name) != 0 ||
        md_write_string(&writer, broker->server_version) != 0) {
        return MD_ERR_NOMEM;
    }
    *size = writer.size;
    return MD_OK;
}

static int encode_u64_payload(uint64_t value, uint8_t* payload, size_t capacity, size_t* size) {
    md_writer_t writer;
    md_writer_init(&writer, payload, capacity);
    if (md_write_u64(&writer, value) != 0) return MD_ERR_NOMEM;
    *size = writer.size;
    return MD_OK;
}

static int encode_output_config(const md_producer_config_t* config, uint8_t* payload,
                                size_t capacity, size_t* size) {
    md_writer_t writer;
    md_writer_init(&writer, payload, capacity);
    if (md_write_u32(&writer, config->physical_width) != 0 ||
        md_write_u32(&writer, config->physical_height) != 0 ||
        md_write_u32(&writer, config->refresh_mhz) != 0 ||
        md_write_u32(&writer, (uint32_t)config->transform) != 0 ||
        md_write_u32(&writer, config->fourcc) != 0 ||
        md_write_u32(&writer, config->plane_count) != 0 ||
        md_write_u64(&writer, config->modifier) != 0) {
        return MD_ERR_NOMEM;
    }
    *size = writer.size;
    return MD_OK;
}

static int encode_producer_accepted(uint64_t producer_id, uint64_t output_id,
                                    uint8_t* payload, size_t capacity, size_t* size) {
    md_writer_t writer;
    md_writer_init(&writer, payload, capacity);
    if (md_write_u64(&writer, producer_id) != 0 || md_write_u64(&writer, output_id) != 0) {
        return MD_ERR_NOMEM;
    }
    *size = writer.size;
    return MD_OK;
}


static int send_encoded(md_broker_peer_t* peer, uint16_t opcode, const uint8_t* payload,
                        size_t payload_size) {
    return queue_peer(peer, opcode, 0, payload, payload_size, NULL, 0);
}

static int parse_hello(const md_packet_t* packet, md_broker_role_t* role, uint16_t* minor,
                       uint64_t* features, char** name, char** version) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t role_value;
    uint16_t reserved;
    uint16_t advertised_minor;
    int rc = md_read_u32(&reader, &role_value);
    if (rc == 0) rc = md_read_u16(&reader, &reserved);
    if (rc == 0) rc = md_read_u16(&reader, &advertised_minor);
    if (rc == 0) rc = md_read_u64(&reader, features);
    if (rc == 0) rc = md_read_string(&reader, name);
    if (rc == 0) rc = md_read_string(&reader, version);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0 || reserved != 0 || advertised_minor > MIRAGE_DISPLAY_PROTOCOL_MINOR ||
        (role_value != 1 && role_value != 2)) {
        free(*name);
        free(*version);
        *name = NULL;
        *version = NULL;
        return MD_ERR_PROTOCOL;
    }
    *role = (md_broker_role_t)role_value;
    *minor = advertised_minor;
    return MD_OK;
}

static int parse_output(const md_packet_t* packet, md_output_info_t* output, char** stable_id,
                        char** name) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t transform;
    memset(output, 0, sizeof(*output));
    int rc = md_read_string(&reader, stable_id);
    if (rc == 0) rc = md_read_string(&reader, name);
    if (rc == 0) rc = md_read_u32(&reader, &output->physical_width);
    if (rc == 0) rc = md_read_u32(&reader, &output->physical_height);
    if (rc == 0) rc = md_read_u32(&reader, &output->logical_width);
    if (rc == 0) rc = md_read_u32(&reader, &output->logical_height);
    if (rc == 0) rc = md_read_u32(&reader, &output->scale_120);
    if (rc == 0) rc = md_read_u32(&reader, &output->refresh_mhz);
    if (rc == 0) rc = md_read_u32(&reader, &transform);
    if (rc == 0) rc = md_read_u32(&reader, &output->drm_render_major);
    if (rc == 0) rc = md_read_u32(&reader, &output->drm_render_minor);
    if (rc == 0) rc = md_read_u64(&reader, &output->input_caps);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0 || *stable_id == NULL || (*stable_id)[0] == '\0' || *name == NULL ||
        (*name)[0] == '\0' || output->physical_width == 0 || output->physical_height == 0 ||
        output->logical_width == 0 || output->logical_height == 0 || output->scale_120 == 0 ||
        transform > MD_TRANSFORM_FLIPPED_270) {
        free(*stable_id);
        free(*name);
        *stable_id = NULL;
        *name = NULL;
        return MD_ERR_PROTOCOL;
    }
    output->stable_id = *stable_id;
    output->name = *name;
    output->transform = (md_transform_t)transform;
    return MD_OK;
}

static int parse_caps(const md_packet_t* packet, md_consumer_caps_t* caps,
                      md_format_cap_t** formats) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t count;
    memset(caps, 0, sizeof(*caps));
    int rc = md_read_u64(&reader, &caps->sync_caps);
    if (rc == 0) rc = md_read_u64(&reader, &caps->color_caps);
    if (rc == 0) rc = md_read_u32(&reader, &caps->max_width);
    if (rc == 0) rc = md_read_u32(&reader, &caps->max_height);
    if (rc == 0) rc = md_read_bytes(&reader, caps->device_uuid, sizeof(caps->device_uuid));
    if (rc == 0) rc = md_read_bytes(&reader, caps->driver_uuid, sizeof(caps->driver_uuid));
    if (rc == 0) rc = md_read_u32(&reader, &count);
    if (rc != 0 || count > MIRAGE_DISPLAY_MAX_FORMATS) return MD_ERR_PROTOCOL;
    if (count > 0) {
        *formats = calloc(count, sizeof(**formats));
        if (*formats == NULL) return MD_ERR_NOMEM;
    }
    for (uint32_t i = 0; rc == 0 && i < count; ++i) {
        rc = md_read_u32(&reader, &(*formats)[i].fourcc);
        if (rc == 0) rc = md_read_u32(&reader, &(*formats)[i].plane_count);
        if (rc == 0) rc = md_read_u64(&reader, &(*formats)[i].modifier);
        if (rc == 0 && ((*formats)[i].plane_count == 0 ||
                        (*formats)[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES)) {
            rc = MD_ERR_PROTOCOL;
        }
    }
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0) {
        free(*formats);
        *formats = NULL;
        return rc;
    }
    caps->formats = *formats;
    caps->format_count = count;
    return MD_OK;
}

static int parse_producer(const md_packet_t* packet, md_producer_info_t* info,
                          char** stable_id, char** kind, md_format_cap_t** formats) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t count;
    memset(info, 0, sizeof(*info));
    int rc = md_read_string(&reader, stable_id);
    if (rc == 0) rc = md_read_string(&reader, kind);
    if (rc == 0) rc = md_read_u32(&reader, &info->drm_render_major);
    if (rc == 0) rc = md_read_u32(&reader, &info->drm_render_minor);
    if (rc == 0) rc = md_read_bytes(&reader, info->device_uuid, sizeof(info->device_uuid));
    if (rc == 0) rc = md_read_bytes(&reader, info->driver_uuid, sizeof(info->driver_uuid));
    if (rc == 0) rc = md_read_u32(&reader, &count);
    if (rc != 0 || *stable_id == NULL || (*stable_id)[0] == '\0' || *kind == NULL ||
        (*kind)[0] == '\0' || count > MIRAGE_DISPLAY_MAX_FORMATS) {
        free(*stable_id);
        free(*kind);
        *stable_id = NULL;
        *kind = NULL;
        return MD_ERR_PROTOCOL;
    }
    if (count > 0) {
        *formats = calloc(count, sizeof(**formats));
        if (*formats == NULL) return MD_ERR_NOMEM;
    }
    for (uint32_t i = 0; rc == 0 && i < count; ++i) {
        rc = md_read_u32(&reader, &(*formats)[i].fourcc);
        if (rc == 0) rc = md_read_u32(&reader, &(*formats)[i].plane_count);
        if (rc == 0) rc = md_read_u64(&reader, &(*formats)[i].modifier);
        if (rc == 0 && ((*formats)[i].plane_count == 0 ||
                        (*formats)[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES)) {
            rc = MD_ERR_PROTOCOL;
        }
    }
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0 || count == 0) {
        free(*stable_id);
        free(*kind);
        free(*formats);
        *stable_id = NULL;
        *kind = NULL;
        *formats = NULL;
        return rc != 0 ? rc : MD_ERR_PROTOCOL;
    }
    info->stable_output_id = *stable_id;
    info->kind = *kind;
    info->formats = *formats;
    info->format_count = count;
    return MD_OK;
}

static int queue_peer(md_broker_peer_t* peer, uint16_t opcode, uint16_t flags,
                      const uint8_t* payload, size_t payload_size, int* fds, size_t fd_count) {
    uint32_t serial = peer->next_serial++;
    int rc = md_outbox_send_or_queue(&peer->outbox, peer->fd, peer->minor, opcode, flags,
                                     serial, payload, payload_size, fds, fd_count, false);
    if (rc == MD_OK || rc == MD_ERR_WOULD_BLOCK || rc == MD_ERR_NOMEM ||
        rc == MD_ERR_INVALID) {
        return rc;
    }
    return MD_ERR_IO;
}

static int flush_peer(md_broker_peer_t* peer) {
    int rc = md_outbox_flush(&peer->outbox, peer->fd, peer->minor);
    if (rc == MD_ERR_WOULD_BLOCK) return MD_ERR_WOULD_BLOCK;
    return rc < 0 ? MD_ERR_IO : MD_OK;
}
static bool format_supported_by_display(const md_broker_peer_t* display,
                                        const md_format_cap_t* candidate) {
    if (display == NULL || candidate == NULL) return false;
    for (uint32_t c = 0; c < display->caps.format_count; ++c) {
        const md_format_cap_t* right = &display->cap_formats[c];
        if (candidate->fourcc == right->fourcc && candidate->plane_count == right->plane_count &&
            candidate->modifier == right->modifier) return true;
    }
    return false;
}

static bool formats_intersect(const md_broker_route_t* route, md_format_cap_t* selected) {
    if (route == NULL || route->producer == NULL || route->display_count == 0) return false;
    const md_broker_peer_t* producer = route->producer;
    for (uint32_t p = 0; p < producer->producer_info.format_count; ++p) {
        const md_format_cap_t* candidate = &producer->producer_formats[p];
        bool supported = true;
        for (uint32_t d = 0; d < route->display_count; ++d) {
            if (!route->displays[d]->ready ||
                !format_supported_by_display(route->displays[d], candidate)) {
                supported = false;
                break;
            }
        }
        if (supported) {
            *selected = *candidate;
            return true;
        }
    }
    return false;
}

static int send_output_config(md_broker_route_t* route) {
    if (route == NULL || route->display == NULL || route->producer == NULL) return MD_ERR_STATE;
    md_broker_peer_t* producer = route->producer;
    if (!formats_intersect(route, &route->selected_format)) return MD_ERR_UNSUPPORTED;
    route->format_selected = true;
    md_producer_config_t config = {
        .physical_width = route->display->output.physical_width,
        .physical_height = route->display->output.physical_height,
        .refresh_mhz = route->display->output.refresh_mhz,
        .transform = route->display->output.transform,
        .fourcc = route->selected_format.fourcc,
        .plane_count = route->selected_format.plane_count,
        .modifier = route->selected_format.modifier,
    };
    uint8_t payload[64];
    size_t payload_size = 0;
    if (encode_output_config(&config, payload, sizeof(payload), &payload_size) != MD_OK) {
        return MD_ERR_PROTOCOL;
    }
    int rc = send_encoded(producer, MD_OP_OUTPUT_CONFIG, payload, payload_size);
    if (rc == MD_OK) route->output_config_sent = true;
    return rc;
}

static int send_pool_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == NULL || display == NULL || !route_has_display(route, display) ||
        !route->pool_active) return MD_ERR_STATE;
    if (display->display_unbind_pending || display->display_pool_sent) return MD_OK;
    size_t count = (size_t)route->pool.buffer_count * (size_t)route->pool.plane_count;
    int fds[MD_WIRE_MAX_FDS];
    int source_fds[MD_WIRE_MAX_FDS];
    if (count > MD_WIRE_MAX_FDS) return MD_ERR_IO;
    size_t index = 0;
    for (uint32_t b = 0; b < route->pool.buffer_count; ++b) {
        for (uint32_t p = 0; p < route->pool.plane_count; ++p) {
            source_fds[index++] = route->pool.planes[b][p].fd;
        }
    }
    if (md_duplicate_fds(source_fds, count, fds) != MD_OK) {
        return MD_ERR_IO;
    }
    uint8_t payload[MD_WIRE_MAX_PAYLOAD];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_offer_buffers(&writer, &route->pool) != 0) {
        md_close_fds(fds, count);
        return MD_ERR_PROTOCOL;
    }
    int rc = queue_peer(display, MD_OP_BIND_BUFFERS, 0, payload, writer.size, fds, count);
    if (rc != MD_OK) md_close_fds(fds, count);
    else display->display_pool_sent = true;
    return rc;
}

static int send_pool_to_displays(md_broker_route_t* route) {
    if (route == NULL || !route->pool_active) return MD_ERR_STATE;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_pool_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}

static int send_unbind_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == NULL || display == NULL || !route_has_display(route, display) ||
        !route->pool_active) return MD_ERR_STATE;
    if (display->display_unbind_pending) return MD_OK;
    uint8_t payload[8];
    size_t payload_size = 0;
    if (encode_u64_payload(route->pool_generation, payload, sizeof(payload), &payload_size) != MD_OK) {
        return MD_ERR_PROTOCOL;
    }
    int rc = send_encoded(display, MD_OP_UNBIND, payload, payload_size);
    if (rc == MD_OK) {
        display->display_unbind_pending = true;
        display->display_unbind_generation = route->pool_generation;
        route->unbind_pending = true;
        route->pending_unbind_generation = route->pool_generation;
    }
    return rc;
}

static int send_unbind_to_displays(md_broker_route_t* route) {
    if (route == NULL || !route->pool_active) return MD_ERR_STATE;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_unbind_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}

static int send_config_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == NULL || display == NULL || !display->ready || !route->config_active) {
        return MD_OK;
    }
    uint8_t payload[128];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_config(&writer, &route->config) != 0) return MD_ERR_PROTOCOL;
    return queue_peer(display, MD_OP_SET_CONFIG, 0, payload, writer.size, NULL, 0);
}

static int send_config_to_displays(md_broker_route_t* route) {
    if (route == NULL) return MD_ERR_INVALID;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_config_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}

static int send_retire_to_producer(md_broker_route_t* route) {
    if (route == NULL || route->producer == NULL || !route->pool_active) return MD_ERR_STATE;
    uint8_t payload[8];
    size_t payload_size = 0;
    if (encode_u64_payload(route->pool_generation, payload, sizeof(payload), &payload_size) != MD_OK) {
        return MD_ERR_PROTOCOL;
    }
    route->retire_pending = true;
    return send_encoded(route->producer, MD_OP_RETIRE_BUFFERS, payload, payload_size);
}

static int maybe_bind_pool(md_broker_route_t* route) {
    if (route == NULL || route->display == NULL || route->producer == NULL ||
        !route->pool_active || route->unbind_pending || route->retire_pending) return MD_OK;
    return send_pool_to_displays(route);
}

static int handle_display_packet(md_broker_t* broker, md_broker_peer_t* peer,
                                  md_packet_t* packet) {
    md_broker_route_t* route = peer->route;
    switch (packet->opcode) {
    case MD_OP_REGISTER_OUTPUT: {
        if (peer->ready || peer->output_stable_id != NULL || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_output_info_t output;
        char* stable_id = NULL;
        char* name = NULL;
        int rc = parse_output(packet, &output, &stable_id, &name);
        if (rc != MD_OK) return rc;
        route = find_route(broker, stable_id);
        if (route == NULL) route = create_route(broker, stable_id);
        if (route == NULL || route->display_count >= MD_BROKER_MAX_PEERS) {
            free(stable_id);
            free(name);
            return MD_ERR_STATE;
        }
        peer->output_stable_id = stable_id;
        peer->output_name = name;
        peer->output = output;
        peer->output.stable_id = peer->output_stable_id;
        peer->output.name = peer->output_name;
        peer->route = route;
        rc = route_add_display(route, peer);
        if (rc != MD_OK) return rc;
        uint8_t payload[8];
        size_t payload_size = 0;
        if (encode_u64_payload(route->output_id, payload, sizeof(payload), &payload_size) != MD_OK) {
            return MD_ERR_PROTOCOL;
        }
        return send_encoded(peer, MD_OP_OUTPUT_ACCEPTED, payload, payload_size);
    }
    case MD_OP_CONSUMER_CAPS: {
        if (route == NULL || !route_has_display(route, peer) || packet->fd_count != 0 ||
            peer->cap_formats != NULL) return MD_ERR_PROTOCOL;
        int rc = parse_caps(packet, &peer->caps, &peer->cap_formats);
        if (rc != MD_OK) return rc;
        peer->ready = true;
        if (route->producer != NULL && route->producer->ready) {
            md_format_cap_t selected;
            if (!formats_intersect(route, &selected)) return MD_ERR_UNSUPPORTED;
            bool format_changed = route->format_selected &&
                (route->selected_format.fourcc != selected.fourcc ||
                 route->selected_format.plane_count != selected.plane_count ||
                 route->selected_format.modifier != selected.modifier);
            if (format_changed && route->pool_active) {
                route->unbind_retiring_old_producer = false;
                rc = send_unbind_to_displays(route);
                if (rc != MD_OK) return rc;
            } else if (!route->output_config_sent || format_changed) {
                route->output_config_sent = false;
                rc = send_output_config(route);
                if (rc != MD_OK) return rc;
            }
        }
        rc = send_config_to_display(route, peer);
        if (rc != MD_OK) return rc;
        return maybe_bind_pool(route);
    }
    case MD_OP_UPDATE_OUTPUT: {
        if (route == NULL || !route_has_display(route, peer) || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_reader_t reader;
        md_reader_init(&reader, packet->payload, packet->payload_size);
        uint32_t transform;
        int rc = md_read_u32(&reader, &peer->output.physical_width);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.physical_height);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.logical_width);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.logical_height);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.scale_120);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.refresh_mhz);
        if (rc == 0) rc = md_read_u32(&reader, &transform);
        if (rc == 0) rc = md_reader_finish(&reader);
        if (rc != 0 || transform > MD_TRANSFORM_FLIPPED_270) return MD_ERR_PROTOCOL;
        peer->output.transform = (md_transform_t)transform;
        if (route->display != peer) return MD_OK;
        route->output_config_sent = false;
        if (route->producer != NULL && route->producer->ready) {
            if (route->pool_active && !route->unbind_pending) {
                route->unbind_retiring_old_producer = false;
                return send_unbind_to_displays(route);
            }
            return send_output_config(route);
        }
        return MD_OK;
    }
    case MD_OP_POINTER_ENTER:
    case MD_OP_POINTER_LEAVE:
    case MD_OP_POINTER_MOTION:
    case MD_OP_POINTER_BUTTON:
    case MD_OP_POINTER_AXIS: {
        if (route == NULL || route->producer == NULL || !route->producer->ready ||
            packet->fd_count != 0) return MD_ERR_STATE;
        uint16_t opcode = packet->opcode;
        if (opcode == MD_OP_POINTER_ENTER) opcode = MD_OP_PRODUCER_POINTER_ENTER;
        else if (opcode == MD_OP_POINTER_LEAVE) opcode = MD_OP_PRODUCER_POINTER_LEAVE;
        else if (opcode == MD_OP_POINTER_MOTION) opcode = MD_OP_PRODUCER_POINTER_MOTION;
        else if (opcode == MD_OP_POINTER_BUTTON) opcode = MD_OP_PRODUCER_POINTER_BUTTON;
        else opcode = MD_OP_PRODUCER_POINTER_AXIS;
        return queue_peer(route->producer, opcode, packet->flags, packet->payload,
                          packet->payload_size, NULL, 0);
    }
    case MD_OP_WINDOW_STATE:
        return MD_OK;
    case MD_OP_UNBIND_DONE: {
        if (route == NULL || !route_has_display(route, peer) || packet->fd_count != 0 ||
            !peer->display_unbind_pending) return MD_ERR_PROTOCOL;
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            generation != peer->display_unbind_generation) return MD_ERR_PROTOCOL;
        peer->display_unbind_pending = false;
        peer->display_unbind_generation = 0;
        peer->display_pool_sent = false;
        route->unbind_pending = !route_all_unbound(route);
        if (route->unbind_pending) return MD_OK;
        route->pending_unbind_generation = 0;
        if (route->unbind_retiring_old_producer) {
            route->unbind_retiring_old_producer = false;
            return route->pool_active ? maybe_bind_pool(route) : MD_OK;
        }
        if (route->pool_active && route->producer != NULL && route->producer->ready &&
            !route->retire_pending) {
            return send_retire_to_producer(route);
        }
        if (!route->pool_active) {
            route->pool_generation = 0;
            md_close_pool(&route->pool);
        }
        return MD_OK;
    }
    default:
        return (packet->flags & MD_PACKET_OPTIONAL) != 0 ? MD_OK : MD_ERR_PROTOCOL;
    }
}

static int handle_producer_packet(md_broker_t* broker, md_broker_peer_t* peer,
                                  md_packet_t* packet) {
    md_broker_route_t* route = peer->route;
    switch (packet->opcode) {
    case MD_OP_REGISTER_PRODUCER: {
        if (peer->ready || peer->producer_stable_id != NULL || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_producer_info_t info;
        char* stable_id = NULL;
        char* kind = NULL;
        md_format_cap_t* formats = NULL;
        int rc = parse_producer(packet, &info, &stable_id, &kind, &formats);
        if (rc != MD_OK) return rc;
        route = find_route(broker, stable_id);
        if (route == NULL) route = create_route(broker, stable_id);
        if (route == NULL || route->producer != NULL) {
            free(stable_id);
            free(kind);
            free(formats);
            return MD_ERR_STATE;
        }
        peer->producer_stable_id = stable_id;
        peer->producer_kind = kind;
        peer->producer_formats = formats;
        peer->producer_info = info;
        peer->producer_info.stable_output_id = stable_id;
        peer->producer_info.kind = kind;
        peer->producer_info.formats = formats;
        peer->route = route;
        route->producer = peer;
        /* A producer reconnect invalidates the old producer's pool. Keep the
         * consumer alive, but explicitly unbind it before accepting frames
         * from the new producer. The old descriptors are broker-owned and
         * can be closed immediately after the unbind packet is queued. */
        if (route->pool_active) {
            if (route->display_count > 0 && route->display->ready && !route->unbind_pending) {
                (void)send_unbind_to_displays(route);
                route->unbind_retiring_old_producer = true;
            }
            md_close_pool(&route->pool);
            route->pool_active = false;
            route->pool_generation = 0;
        }
        route->retire_pending = false;
        route->output_config_sent = false;
        route->format_selected = false;
        peer->id = broker->next_peer_id++;
        if (peer->id == 0) peer->id = broker->next_peer_id++;
        uint8_t payload[16];
        size_t payload_size = 0;
        if (encode_producer_accepted(peer->id, route->output_id, payload, sizeof(payload),
                                     &payload_size) != MD_OK) return MD_ERR_PROTOCOL;
        rc = send_encoded(peer, MD_OP_PRODUCER_ACCEPTED, payload, payload_size);
        if (rc == MD_OK) peer->ready = true;
        if (rc == MD_OK && route->display != NULL && route->display->ready &&
            !route->output_config_sent) {
            rc = send_output_config(route);
        }
        return rc;
    }
    case MD_OP_OFFER_BUFFERS: {
        if (route == NULL || route->producer != peer || !peer->ready || route->pool_active) {
            return MD_ERR_PROTOCOL;
        }
        md_buffer_pool_t pool;
        int rc = md_proto_decode_bind_buffers(packet->payload, packet->payload_size, &pool);
        size_t expected = 0;
        if (rc == 0) expected = (size_t)pool.buffer_count * (size_t)pool.plane_count;
        if (rc != 0 || packet->fd_count != expected || expected > MD_WIRE_MAX_FDS ||
            !route->format_selected || pool.fourcc != route->selected_format.fourcc ||
            pool.plane_count != route->selected_format.plane_count ||
            pool.modifier != route->selected_format.modifier) return MD_ERR_PROTOCOL;
        for (uint32_t b = 0; b < pool.buffer_count; ++b) {
            for (uint32_t p = 0; p < pool.plane_count; ++p) {
                size_t index = (size_t)b * pool.plane_count + p;
                pool.planes[b][p].fd = packet->fds[index];
                packet->fds[index] = -1;
            }
        }
        route->pool = pool;
        route->pool_generation = pool.generation;
        route->pool_active = true;
        for (uint32_t i = 0; i < route->display_count; ++i) {
            route->displays[i]->display_pool_sent = false;
        }
        if (route->display_count > 0) return maybe_bind_pool(route);
        return MD_OK;
    }
    case MD_OP_PRODUCER_FRAME: {
        if (route == NULL || route->producer != peer || !route->pool_active ||
            packet->fd_count != 2) {
            return MD_ERR_PROTOCOL;
        }
        md_frame_t frame;
        if (md_proto_decode_frame(packet->payload, packet->payload_size, &frame) != 0) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            return MD_ERR_PROTOCOL;
        }
        if (frame.buffer_generation != route->pool_generation) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            return MD_OK;
        }
        if (frame.buffer_index >= route->pool.buffer_count) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            return MD_ERR_PROTOCOL;
        }
        md_broker_peer_t* active_displays[MD_BROKER_MAX_PEERS];
        uint32_t active_count = 0;
        for (uint32_t i = 0; i < route->display_count; ++i) {
            md_broker_peer_t* display = route->displays[i];
            if (display->ready && !display->display_unbind_pending) {
                active_displays[active_count++] = display;
            }
        }
        if (active_count == 0) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            return MD_OK;
        }

        if (active_count > 1) {
            int release_fds[MD_BROKER_MAX_PEERS];
            md_sync_fanout_t* sync = NULL;
            if (md_sync_fanout_create_on_node(
                    packet->fds[1], active_count, route->producer->producer_info.drm_render_major,
                    route->producer->producer_info.drm_render_minor, release_fds, &sync) == MD_OK) {
                md_broker_fanout_t* fanout = calloc(1, sizeof(*fanout));
                if (fanout == NULL) {
                    (void)md_display_signal_release_syncobj_on_node(
                        packet->fds[1], route->producer->producer_info.drm_render_major,
                        route->producer->producer_info.drm_render_minor);
                    packet->fds[1] = -1;
                    for (uint32_t i = 0; i < active_count; ++i) {
                        if (release_fds[i] >= 0) close(release_fds[i]);
                        md_sync_fanout_abandon(sync, i);
                    }
                    md_sync_fanout_free(sync);
                    return MD_ERR_NOMEM;
                }
                fanout->sync = sync;
                fanout->display_count = active_count;
                for (uint32_t i = 0; i < active_count; ++i) {
                    fanout->displays[i] = active_displays[i];
                    int acquire_fd = fcntl(packet->fds[0], F_DUPFD_CLOEXEC, 0);
                    int fds[2] = {acquire_fd, release_fds[i]};
                    release_fds[i] = -1;
                    if (acquire_fd < 0 ||
                        queue_peer(active_displays[i], MD_OP_FRAME_READY, packet->flags,
                                   packet->payload, packet->payload_size, fds, 2) != MD_OK) {
                        md_close_fds(fds, 2);
                        md_sync_fanout_abandon(sync, i);
                    }
                }
                fanout->next = broker->fanouts;
                broker->fanouts = fanout;
                return MD_OK;
            }

            /* A syncobj can only be delivered to one consumer directly. If
             * aggregation is unavailable on this GPU, preserve correctness
             * by rendering on the primary display and dropping this frame for
             * mirrors instead of duplicating the producer release FD. */
        }

        md_broker_peer_t* display = active_displays[0];
        int fds[2];
        if (md_duplicate_fds(packet->fds, 2, fds) != MD_OK) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            return MD_OK;
        }
        int release_fallback = fcntl(fds[1], F_DUPFD_CLOEXEC, 0);
        int rc = queue_peer(display, MD_OP_FRAME_READY, packet->flags,
                            packet->payload, packet->payload_size, fds, 2);
        if (release_fallback >= 0) {
            if (rc == MD_OK) {
                close(release_fallback);
            } else {
                (void)md_display_signal_release_syncobj_on_node(
                    release_fallback, route->producer->producer_info.drm_render_major,
                    route->producer->producer_info.drm_render_minor);
            }
        } else if (rc != MD_OK) {
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
        }
        return rc == MD_OK ? MD_OK : MD_ERR_IO;
    }
    case MD_OP_PRODUCER_SET_CONFIG: {
        if (route == NULL || route->producer != peer || packet->fd_count != 0 ||
            !peer->ready) return MD_ERR_STATE;
        md_display_config_t config;
        if (md_proto_decode_config(packet->payload, packet->payload_size, &config) != 0) {
            return MD_ERR_PROTOCOL;
        }
        route->config = config;
        route->config_active = true;
        return send_config_to_displays(route);
    }
    case MD_OP_RETIRE_DONE: {
        if (route == NULL || route->producer != peer || packet->fd_count != 0 ||
            !route->retire_pending) return MD_ERR_PROTOCOL;
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            generation != route->pool_generation) return MD_ERR_PROTOCOL;
        route->retire_pending = false;
        route->pool_active = false;
        md_close_pool(&route->pool);
        route->pool_generation = 0;
        route->output_config_sent = false;
        if (route->producer->ready && route->display_count > 0 && route->display->ready) {
            int rc = send_output_config(route);
            if (rc != MD_OK) return rc;
            return send_config_to_displays(route);
        }
        return MD_OK;
    }
    case MD_OP_GOODBYE:
        return MD_ERR_DISCONNECTED;
    default:
        return (packet->flags & MD_PACKET_OPTIONAL) != 0 ? MD_OK : MD_ERR_PROTOCOL;
    }
}

static int handle_peer_packet(md_broker_t* broker, md_broker_peer_t* peer,
                              md_packet_t* packet) {
    if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR || packet->minor != peer->minor) {
        return MD_ERR_PROTOCOL;
    }
    if (!peer->hello_done) {
        if (packet->opcode != MD_OP_HELLO || packet->fd_count != 0) return MD_ERR_PROTOCOL;
        md_broker_role_t role;
        uint16_t minor;
        uint64_t features;
        char* name = NULL;
        char* version = NULL;
        int rc = parse_hello(packet, &role, &minor, &features, &name, &version);
        if (rc != MD_OK) return rc;
        peer->role = role;
        peer->minor = minor;
        peer->features = features;
        peer->client_name = name;
        peer->client_version = version;
        peer->hello_done = true;
        uint8_t payload[MD_WIRE_MAX_PAYLOAD];
        size_t payload_size = 0;
        rc = encode_welcome(broker, payload, sizeof(payload), &payload_size);
        if (rc == MD_OK) rc = send_encoded(peer, MD_OP_WELCOME, payload, payload_size);
        return rc;
    }
    if (peer->role == MD_BROKER_ROLE_DISPLAY) return handle_display_packet(broker, peer, packet);
    if (peer->role == MD_BROKER_ROLE_PRODUCER) return handle_producer_packet(broker, peer, packet);
    return MD_ERR_PROTOCOL;
}

static void detach_peer_from_route(md_broker_t* broker, md_broker_peer_t* peer) {
    (void)broker;
    md_broker_route_t* route = peer->route;
    if (route == NULL) return;
    if (route_has_display(route, peer)) {
        route_remove_display(route, peer);
        /* Keep the producer and its broker-owned pool alive. A replacement
         * display can bind the exact same generation without forcing the
         * renderer to allocate again. */
        route->unbind_pending = !route_all_unbound(route);
    }
    if (route->producer == peer) {
        if (route->pool_active) {
            if (route->display_count > 0 && route->display->ready && !route->unbind_pending) {
                (void)send_unbind_to_displays(route);
            }
            route->unbind_retiring_old_producer = route->unbind_pending;
        }
        route->producer = NULL;
        /* The producer owns the authoritative generation. Once it is gone,
         * retain only the route identity, not stale DMA-BUF descriptors. */
        md_close_pool(&route->pool);
        route->pool_active = false;
        route->retire_pending = false;
        route->output_config_sent = false;
        route->format_selected = false;
        route->pool_generation = 0;
        route->config_active = false;
    }
    peer->route = NULL;
    /* Keep an empty route so a peer that restarts with the same stable id can
     * reclaim its output id and negotiated lifecycle. It is bounded by the
     * broker's max_routes setting and is reclaimed when the broker itself is
     * destroyed. */
}

static void disconnect_peer(md_broker_t* broker, md_broker_peer_t* peer) {
    if (peer == NULL) return;
    abandon_peer_fanouts(broker, peer);
    detach_peer_from_route(broker, peer);
    remove_peer_slot(broker, peer);
    free_peer(peer);
}

static md_broker_peer_t* accept_peer(md_broker_t* broker) {
    int fd = accept4(broker->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return NULL;
    struct ucred credentials;
    socklen_t credential_size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) != 0 ||
        credential_size != sizeof(credentials) || credentials.uid != getuid()) {
        close(fd);
        return NULL;
    }
    size_t slot = MD_BROKER_MAX_PEERS;
    for (size_t i = 0; i < MD_BROKER_MAX_PEERS; ++i) {
        if (broker->peers[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == MD_BROKER_MAX_PEERS) {
        close(fd);
        return NULL;
    }
    md_broker_peer_t* peer = calloc(1, sizeof(*peer));
    if (peer == NULL) {
        close(fd);
        return NULL;
    }
    peer->fd = fd;
    peer->minor = 0;
    peer->next_serial = 1;
    broker->peers[slot] = peer;
    return peer;
}

md_broker_t* md_broker_new(const md_broker_options_t* options) {
    if (options == NULL || options->socket_path == NULL || options->socket_path[0] == '\0') {
        return NULL;
    }
    md_broker_t* broker = calloc(1, sizeof(*broker));
    if (broker == NULL) return NULL;
    broker->listen_fd = -1;
    broker->socket_path = md_strdup(options->socket_path);
    broker->server_name = md_strdup(options->server_name != NULL ? options->server_name : "mirage-display");
    broker->server_version = md_strdup(options->server_version != NULL ? options->server_version : "0.1");
    broker->features = options->features != 0
                           ? options->features
                           : MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                 MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS |
                                 MD_FEATURE_WINDOW_STATE | MD_FEATURE_COLOR_METADATA;
    broker->max_routes = options->max_routes == 0 ? MD_BROKER_DEFAULT_ROUTES : options->max_routes;
    if (broker->max_routes > 128u || broker->socket_path == NULL || broker->server_name == NULL ||
        broker->server_version == NULL) {
        md_broker_free(broker);
        return NULL;
    }
    broker->next_output_id = 1;
    broker->next_peer_id = 1;
    atomic_init(&broker->stopping, false);
    return broker;
}

void md_broker_stop(md_broker_t* broker) {
    if (broker == NULL) return;
    atomic_store(&broker->stopping, true);
}

void md_broker_free(md_broker_t* broker) {
    if (broker == NULL) return;
    md_broker_stop(broker);
    for (size_t i = 0; i < MD_BROKER_MAX_PEERS; ++i) {
        if (broker->peers[i] != NULL) free_peer(broker->peers[i]);
    }
    free_fanouts(broker);
    for (uint32_t i = 0; i < broker->route_count; ++i) free_route(&broker->routes[i]);
    free(broker->routes);
    if (broker->listen_fd >= 0) close(broker->listen_fd);
    if (broker->socket_path != NULL && broker->socket_path[0] != '@') {
        unlink(broker->socket_path);
    }
    free(broker->socket_path);
    free(broker->server_name);
    free(broker->server_version);
    free(broker);
}

int md_broker_listen(md_broker_t* broker) {
    if (broker == NULL || broker->listening) return MD_ERR_STATE;
    bool abstract = broker->socket_path[0] == '@';
    if (!abstract) {
        struct stat existing;
        if (lstat(broker->socket_path, &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) return MD_ERR_IO;
            if (unlink(broker->socket_path) != 0) return MD_ERR_IO;
        } else if (errno != ENOENT) {
            return MD_ERR_IO;
        }
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return MD_ERR_IO;
    struct sockaddr_un address;
    socklen_t address_length;
    int address_result = md_fill_unix_address(broker->socket_path, &address, &address_length);
    if (address_result != MD_OK) {
        close(fd);
        return address_result;
    }
    if (bind(fd, (struct sockaddr*)&address, address_length) != 0) {
        int saved_errno = errno;
        close(fd);
        if (!abstract) unlink(broker->socket_path);
        errno = saved_errno;
        return MD_ERR_IO;
    }
    if (!abstract && chmod(broker->socket_path, 0600) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(broker->socket_path);
        errno = saved_errno;
        return MD_ERR_IO;
    }
    if (listen(fd, (int)MD_BROKER_MAX_PEERS) != 0) {
        int saved_errno = errno;
        close(fd);
        if (!abstract) unlink(broker->socket_path);
        errno = saved_errno;
        return MD_ERR_IO;
    }
    broker->listen_fd = fd;
    broker->listening = true;
    atomic_store(&broker->stopping, false);
    return MD_OK;
}

int md_broker_get_fd(const md_broker_t* broker) {
    return broker != NULL ? broker->listen_fd : -1;
}

const char* md_broker_socket_path(const md_broker_t* broker) {
    return broker != NULL ? broker->socket_path : NULL;
}

int md_broker_dispatch(md_broker_t* broker, int timeout_ms) {
    if (broker == NULL || !broker->listening) return MD_ERR_STATE;
    if (atomic_load(&broker->stopping)) return MD_ERR_DISCONNECTED;
    poll_fanouts(broker);

    struct pollfd descriptors[1 + MD_BROKER_MAX_PEERS];
    md_broker_peer_t* owners[MD_BROKER_MAX_PEERS];
    nfds_t count = 1;
    descriptors[0] = (struct pollfd){.fd = broker->listen_fd, .events = POLLIN, .revents = 0};
    size_t owner_count = 0;
    for (size_t i = 0; i < MD_BROKER_MAX_PEERS; ++i) {
        md_broker_peer_t* peer = broker->peers[i];
        if (peer == NULL) continue;
        descriptors[count] = (struct pollfd){
            .fd = peer->fd,
            .events = POLLIN | (peer->outbox.head != NULL ? POLLOUT : 0),
            .revents = 0,
        };
        owners[owner_count++] = peer;
        ++count;
    }
    int poll_result;
    do {
        poll_result = poll(descriptors, count, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) return MD_ERR_IO;
    if (poll_result == 0) return 0;
    int handled = 0;
    if ((descriptors[0].revents & POLLIN) != 0) {
        for (;;) {
            md_broker_peer_t* peer = accept_peer(broker);
            if (peer == NULL) break;
            ++handled;
        }
    }
    for (size_t i = 0; i < owner_count; ++i) {
        md_broker_peer_t* peer = owners[i];
        if (peer_index(broker, peer) < 0) continue;
        size_t descriptor_index = i + 1u;
        short revents = descriptors[descriptor_index].revents;
        bool disconnect = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (!disconnect && (revents & POLLIN) != 0) {
            md_packet_t packet;
            int receive_result = md_codec_recv(peer->fd, &packet);
            if (receive_result < 0) disconnect = true;
            else if (receive_result == 1) {
                int result = handle_peer_packet(broker, peer, &packet);
                md_packet_close_fds(&packet);
                ++handled;
                if (result < 0) {
                    (void)send_error(peer, (uint32_t)(-result), true, "broker protocol error");
                    disconnect = true;
                }
            }
        }
        if (!disconnect && (revents & POLLOUT) != 0 && peer_index(broker, peer) >= 0) {
            if (flush_peer(peer) < 0) disconnect = true;
        }
        if (disconnect && peer_index(broker, peer) >= 0) disconnect_peer(broker, peer);
    }
    poll_fanouts(broker);
    return handled;
}


