#include "mirage_display_broker.h"

#include "codec.hpp"
#include "common/net.hpp"
#include "common/outbox.hpp"
#include "common/util.hpp"
#include "protocol.hpp"
#include "sync_fanout.h"

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <memory>
#include <new>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

/*
 * Broker routing core (include/mirage_display_broker.h): binds the 0600 AF_UNIX
 * SOCK_SEQPACKET endpoint, validates peers with SO_PEERCRED, routes one producer
 * to many display consumers per stable output id, negotiates format/modifier
 * intersections, and fans out DMA-BUF and sync descriptors without touching
 * pixel data.
 *
 * Threading: a single dispatch thread owns every peer and route; the sync fanout
 * state is not internally synchronized and must be used from that thread only.
 */

namespace {

constexpr uint32_t kBrokerMaxPeers = 32U;
constexpr uint32_t kBrokerMaxRoutes = 128U;
constexpr uint32_t kBrokerDefaultRoutes = 16U;
using ProtocolString = std::unique_ptr<char, decltype(&md_protocol_free_string)>;

}  // namespace

typedef enum md_broker_role {
    MD_BROKER_ROLE_NONE = 0,
    MD_BROKER_ROLE_DISPLAY = 1,
    MD_BROKER_ROLE_PRODUCER = 2,
} md_broker_role_t;

typedef struct md_broker_peer md_broker_peer_t;
typedef struct md_broker_route md_broker_route_t;
typedef struct md_broker_fanout md_broker_fanout_t;

struct md_broker_peer {
    int fd{-1};
    md_broker_role_t role{MD_BROKER_ROLE_NONE};
    bool hello_done{false};
    bool ready{false};
    uint16_t minor{0U};
    uint64_t features{0U};
    uint64_t id{0U};
    uint32_t next_serial{1U};
    std::string client_name{};
    std::string client_version{};
    md_broker_route_t* route{nullptr};
    bool display_pool_sent{false};
    bool display_unbind_pending{false};
    uint64_t display_unbind_generation{0U};

    std::string output_stable_id{};
    std::string output_name{};
    md_output_info_t output{};
    md_consumer_caps_t caps{};
    std::vector<md_format_cap_t> cap_formats{};

    std::string producer_stable_id{};
    std::string producer_kind{};
    md_producer_info_t producer_info{};
    std::vector<md_format_cap_t> producer_formats{};

    md_outbox_t outbox{};

    md_broker_peer() { md_outbox_init(&outbox); }
    ~md_broker_peer();
};

struct md_broker_route {
    uint64_t output_id{0U};
    std::string stable_id{};
    md_broker_peer_t* display{nullptr};
    std::array<md_broker_peer_t*, kBrokerMaxPeers> displays{};
    uint32_t display_count{0U};
    md_broker_peer_t* producer{nullptr};
    bool producer_gpu_bound{false};
    md_format_cap_t selected_format{};
    bool format_selected{false};
    bool output_config_sent{false};
    /* Last WINDOW_STATE flags received from a display. Cached because window
     * state is a persistent fact (unlike pointer events): a display may report
     * it before the producer connects, and the value is replayed when the
     * producer (re)establishes the route. */
    uint32_t window_state{0U};
    bool pool_active{false};
    bool retire_pending{false};
    bool unbind_pending{false};
    uint64_t pending_unbind_generation{0U};
    uint64_t pool_generation{0U};
    md_buffer_pool_t pool{};
    bool config_active{false};
    md_display_config_t config{};
    bool unbind_retiring_old_producer{false};

    ~md_broker_route() {
        /* The broker owns the DMA-BUF descriptors after OFFER_BUFFERS. */
        md_close_pool(&pool);
    }
};

struct md_broker_fanout {
    std::unique_ptr<md_broker_fanout_t> next{};
    md_sync_fanout_t* sync{nullptr};
    uint32_t display_count{0U};
    std::array<md_broker_peer_t*, kBrokerMaxPeers> displays{};

    ~md_broker_fanout() {
        if (sync != nullptr) {
            md_sync_fanout_free(sync);
        }
    }
};

struct md_broker {
    int listen_fd{-1};
    bool listening{false};
    std::atomic_bool stopping{false};
    std::string socket_path{};
    std::string server_name{};
    std::string server_version{};
    uint64_t features{0U};
    uint32_t max_routes{0U};
    uint64_t next_output_id{1U};
    uint64_t next_peer_id{1U};
    std::array<std::unique_ptr<md_broker_peer_t>, kBrokerMaxPeers> peers{};
    std::array<std::unique_ptr<md_broker_route_t>, kBrokerMaxRoutes> routes{};
    uint32_t route_count{0U};
    std::unique_ptr<md_broker_fanout_t> fanouts{};
    /* Host notification hooks (see md_broker_options_t); the broker never
     * owns or frees user_data. */
    void (*on_window_state)(void* user_data, const char* stable_id, uint32_t flags){nullptr};
    void (*on_output_added)(void* user_data, const md_output_info_t* output){nullptr};
    void (*on_output_updated)(void* user_data, const md_output_info_t* output){nullptr};
    void (*on_output_removed)(void* user_data, const char* stable_id){nullptr};
    void* user_data{nullptr};
};

static void poll_fanouts(md_broker_t* broker) {
    std::unique_ptr<md_broker_fanout_t>* link = &broker->fanouts;
    while (*link) {
        md_broker_fanout_t* const fanout = link->get();
        const int rc = md_sync_fanout_poll(fanout->sync);
        if (rc == 0) {
            link = &fanout->next;
            continue;
        }
        /* Removing the owning link destroys the completed fanout and its
         * syncobj exactly once; peers retain only non-owning references. */
        *link = std::move(fanout->next);
    }
}

static void abandon_peer_fanouts(md_broker_t* broker, md_broker_peer_t* peer) {
    for (md_broker_fanout_t* fanout = broker->fanouts.get(); fanout != nullptr;
         fanout = fanout->next.get()) {
        for (uint32_t i = 0; i < fanout->display_count; ++i) {
            if (fanout->displays[i] == peer) md_sync_fanout_abandon(fanout->sync, i);
        }
    }
}

static void free_fanouts(md_broker_t* broker) {
    while (broker->fanouts) {
        std::unique_ptr<md_broker_fanout_t> fanout = std::move(broker->fanouts);
        broker->fanouts = std::move(fanout->next);
        /* Detach before destruction so shutdown has the same iterative bound
         * as polling even when many frames are awaiting consumer release. */
        fanout->next.reset();
    }
}

static void clear_peer_data(md_broker_peer_t* peer) {
    peer->client_name.clear();
    peer->client_version.clear();
    peer->output_stable_id.clear();
    peer->output_name.clear();
    peer->cap_formats.clear();
    peer->producer_stable_id.clear();
    peer->producer_kind.clear();
    peer->producer_formats.clear();
    peer->output = {};
    peer->caps = {};
    peer->producer_info = {};
}

md_broker_peer::~md_broker_peer() {
    if (fd >= 0) {
        close(fd);
    }
    md_outbox_clear(&outbox);
    clear_peer_data(this);
}

static int peer_index(const md_broker_t* broker, const md_broker_peer_t* peer) {
    for (size_t i = 0; i < broker->peers.size(); ++i) {
        if (broker->peers[i].get() == peer) return static_cast<int>(i);
    }
    return -1;
}

static void remove_peer_slot(md_broker_t* broker, md_broker_peer_t* peer) {
    const int index = peer_index(broker, peer);
    if (index >= 0) {
        broker->peers[static_cast<size_t>(index)].reset();
    }
}

static void remove_route(md_broker_t* broker, md_broker_route_t* route) {
    if (route == nullptr) return;
    for (size_t index = 0; index < broker->routes.size(); ++index) {
        if (broker->routes[index].get() == route) {
            broker->routes[index].reset();
            --broker->route_count;
            return;
        }
    }
}

static md_broker_route_t* find_route(const md_broker_t* broker, const char* stable_id) {
    for (size_t index = 0; index < broker->routes.size(); ++index) {
        md_broker_route_t* const route = broker->routes[index].get();
        if (route != nullptr && route->stable_id == stable_id) {
            return route;
        }
    }
    return nullptr;
}

static bool route_has_display(const md_broker_route_t* route, const md_broker_peer_t* peer) {
    if (route == nullptr || peer == nullptr) return false;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i] == peer) return true;
    }
    return false;
}

static int route_add_display(md_broker_route_t* route, md_broker_peer_t* peer) {
    if (route == nullptr || peer == nullptr || route->display_count >= kBrokerMaxPeers ||
        route_has_display(route, peer)) return MD_ERR_STATE;
    route->displays[route->display_count++] = peer;
    if (route->display == nullptr) route->display = peer;
    peer->display_pool_sent = false;
    peer->display_unbind_pending = false;
    peer->display_unbind_generation = 0U;
    return MD_OK;
}

static void route_remove_display(md_broker_route_t* route, md_broker_peer_t* peer) {
    if (route == nullptr || peer == nullptr) return;
    uint32_t index = route->display_count;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i] == peer) {
            index = i;
            break;
        }
    }
    if (index == route->display_count) return;
    if (index + 1u < route->display_count) {
        for (uint32_t destination = index; destination + 1U < route->display_count;
             ++destination) {
            route->displays[destination] = route->displays[destination + 1U];
        }
    }
    --route->display_count;
    route->displays[route->display_count] = nullptr;
    route->display = route->display_count > 0U ? route->displays[0] : nullptr;
}

static bool route_all_unbound(const md_broker_route_t* route) {
    for (uint32_t i = 0; i < route->display_count; ++i) {
        if (route->displays[i]->display_unbind_pending) return false;
    }
    return true;
}

static bool reclaim_idle_route(md_broker_t* broker) {
    for (size_t index = 0; index < broker->routes.size(); ++index) {
        md_broker_route_t* const candidate = broker->routes[index].get();
        if (candidate != nullptr && candidate->display == nullptr && candidate->producer == nullptr &&
            !candidate->pool_active && !candidate->unbind_pending) {
            remove_route(broker, candidate);
            return true;
        }
    }
    return false;
}

static md_broker_route_t* create_route(md_broker_t* broker, const char* stable_id) {
    if (broker->route_count >= broker->max_routes && !reclaim_idle_route(broker)) {
        return nullptr;
    }
    for (size_t index = 0; index < broker->routes.size(); ++index) {
        if (broker->routes[index]) continue;
        std::unique_ptr<md_broker_route_t> route{new (std::nothrow) md_broker_route_t{}};
        if (!route) return nullptr;
        md_init_pool(&route->pool);
        route->stable_id = stable_id;
        route->output_id = broker->next_output_id++;
        if (route->output_id == 0U) route->output_id = broker->next_output_id++;
        md_broker_route_t* const result = route.get();
        broker->routes[index] = std::move(route);
        ++broker->route_count;
        return result;
    }
    return nullptr;
}


/*
 * Queues one outbound message with the peer's monotonically increasing serial;
 * descriptor ownership transfers to the outbox on every path.
 */
static int queue_peer(md_broker_peer_t* peer, uint16_t opcode, uint16_t flags,
                      const uint8_t* payload, size_t payload_size, int* fds, size_t fd_count);

static int send_error(md_broker_peer_t* peer, uint32_t code, bool fatal, const char* message) {
    uint8_t payload[MD_WIRE_MAX_PAYLOAD];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_write_u32(&writer, code) != 0 || md_write_u32(&writer, fatal ? 1u : 0u) != 0 ||
        md_write_string(&writer, message != nullptr ? message : "broker error") != 0) {
        return MD_ERR_PROTOCOL;
    }
    return queue_peer(peer, MD_OP_ERROR, 0, payload, writer.size, nullptr, 0U);
}

static int encode_welcome(md_broker_t* broker, uint16_t selected_minor,
                          uint8_t* payload, size_t capacity, size_t* size) {
    md_writer_t writer;
    md_writer_init(&writer, payload, capacity);
    if (md_write_u16(&writer, selected_minor) != 0 ||
        md_write_u16(&writer, 0) != 0 || md_write_u64(&writer, broker->features) != 0 ||
        md_write_string(&writer, broker->server_name.c_str()) != 0 ||
        md_write_string(&writer, broker->server_version.c_str()) != 0) {
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
        md_write_u32(&writer, config->logical_width) != 0 ||
        md_write_u32(&writer, config->logical_height) != 0 ||
        md_write_u32(&writer, config->refresh_mhz) != 0 ||
        md_write_u32(&writer, static_cast<uint32_t>(config->transform)) != 0 ||
        md_write_u32(&writer, config->fourcc) != 0 ||
        md_write_u32(&writer, config->plane_count) != 0 ||
        md_write_u64(&writer, config->modifier) != 0 ||
        md_write_u32(&writer, config->target_drm_render_major) != 0 ||
        md_write_u32(&writer, config->target_drm_render_minor) != 0 ||
        md_write_u32(&writer, config->target_gpu_flags) != 0 ||
        md_write_bytes(&writer, config->target_device_uuid,
                       sizeof(config->target_device_uuid)) != 0 ||
        md_write_bytes(&writer, config->target_driver_uuid,
                       sizeof(config->target_driver_uuid)) != 0) {
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
    return queue_peer(peer, opcode, 0, payload, payload_size, nullptr, 0U);
}

static int parse_hello(const md_packet_t* packet, md_broker_role_t* role, uint16_t* minor,
                       uint64_t* features, char** name, char** version) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t role_value;
    uint16_t reserved;
    uint16_t min_minor;
    uint16_t max_minor;
    int rc = md_read_u32(&reader, &role_value);
    if (rc == 0) rc = md_read_u16(&reader, &reserved);
    if (rc == 0) rc = md_read_u16(&reader, &min_minor);
    if (rc == 0) rc = md_read_u16(&reader, &max_minor);
    if (rc == 0) rc = md_read_u64(&reader, features);
    if (rc == 0) rc = md_read_string(&reader, name);
    if (rc == 0) rc = md_read_string(&reader, version);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0 || reserved != 0 || min_minor > max_minor ||
        min_minor != MIRAGE_DISPLAY_PROTOCOL_MINOR ||
        max_minor != MIRAGE_DISPLAY_PROTOCOL_MINOR ||
        (role_value != 1 && role_value != 2)) {
        md_protocol_free_string(*name);
        md_protocol_free_string(*version);
        *name = nullptr;
        *version = nullptr;
        return MD_ERR_PROTOCOL;
    }
    *role = static_cast<md_broker_role_t>(role_value);
    *minor = max_minor < MIRAGE_DISPLAY_PROTOCOL_MINOR ? max_minor : MIRAGE_DISPLAY_PROTOCOL_MINOR;
    return MD_OK;
}

static int parse_output(const md_packet_t* packet, md_output_info_t* output, char** stable_id,
                        char** name) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t transform;
    *output = {};
    int rc = md_read_string(&reader, stable_id);
    if (rc == 0) rc = md_read_string(&reader, name);
    if (rc == 0) rc = md_read_u32(&reader, &output->physical_width);
    if (rc == 0) rc = md_read_u32(&reader, &output->physical_height);
    if (rc == 0) rc = md_read_u32(&reader, &output->logical_width);
    if (rc == 0) rc = md_read_u32(&reader, &output->logical_height);
    if (rc == 0) rc = md_read_i32(&reader, &output->logical_x);
    if (rc == 0) rc = md_read_i32(&reader, &output->logical_y);
    if (rc == 0) rc = md_read_u32(&reader, &output->scale_120);
    if (rc == 0) rc = md_read_u32(&reader, &output->refresh_mhz);
    if (rc == 0) rc = md_read_u32(&reader, &transform);
    if (rc == 0) rc = md_read_u32(&reader, &output->drm_render_major);
    if (rc == 0) rc = md_read_u32(&reader, &output->drm_render_minor);
    if (rc == 0) rc = md_read_u64(&reader, &output->input_caps);
    if (rc == 0) rc = md_reader_finish(&reader);
    if (rc != 0 || *stable_id == nullptr || (*stable_id)[0] == '\0' || *name == nullptr ||
        (*name)[0] == '\0' || output->physical_width == 0 || output->physical_height == 0 ||
        output->logical_width == 0 || output->logical_height == 0 || output->scale_120 == 0 ||
        transform > MD_TRANSFORM_FLIPPED_270) {
        md_protocol_free_string(*stable_id);
        md_protocol_free_string(*name);
        *stable_id = nullptr;
        *name = nullptr;
        return MD_ERR_PROTOCOL;
    }
    output->stable_id = *stable_id;
    output->name = *name;
    output->transform = static_cast<md_transform_t>(transform);
    return MD_OK;
}

static int parse_caps(const md_packet_t* packet, md_consumer_caps_t* caps,
                      std::vector<md_format_cap_t>* formats) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t count;
    *caps = {};
    formats->clear();
    int rc = md_read_u64(&reader, &caps->sync_caps);
    if (rc == 0) rc = md_read_u64(&reader, &caps->color_caps);
    if (rc == 0) rc = md_read_u32(&reader, &caps->max_width);
    if (rc == 0) rc = md_read_u32(&reader, &caps->max_height);
    if (rc == 0) rc = md_read_bytes(&reader, caps->device_uuid, sizeof(caps->device_uuid));
    if (rc == 0) rc = md_read_bytes(&reader, caps->driver_uuid, sizeof(caps->driver_uuid));
    if (rc == 0) rc = md_read_u32(&reader, &count);
    if (rc != 0 || count > MIRAGE_DISPLAY_MAX_FORMATS) return MD_ERR_PROTOCOL;
    formats->resize(count);
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
        formats->clear();
        return rc;
    }
    caps->formats = formats->data();
    caps->format_count = count;
    return MD_OK;
}

static int parse_producer(const md_packet_t* packet, md_producer_info_t* info,
                          char** stable_id, char** kind,
                          std::vector<md_format_cap_t>* formats) {
    md_reader_t reader;
    md_reader_init(&reader, packet->payload, packet->payload_size);
    uint32_t count;
    *info = {};
    formats->clear();
    int rc = md_read_string(&reader, stable_id);
    if (rc == 0) rc = md_read_string(&reader, kind);
    if (rc == 0) rc = md_read_u32(&reader, &info->drm_render_major);
    if (rc == 0) rc = md_read_u32(&reader, &info->drm_render_minor);
    if (rc == 0) rc = md_read_bytes(&reader, info->device_uuid, sizeof(info->device_uuid));
    if (rc == 0) rc = md_read_bytes(&reader, info->driver_uuid, sizeof(info->driver_uuid));
    if (rc == 0) rc = md_read_u32(&reader, &count);
    if (rc != 0 || *stable_id == nullptr || (*stable_id)[0] == '\0' || *kind == nullptr ||
        (*kind)[0] == '\0' || count > MIRAGE_DISPLAY_MAX_FORMATS) {
        md_protocol_free_string(*stable_id);
        md_protocol_free_string(*kind);
        *stable_id = nullptr;
        *kind = nullptr;
        return MD_ERR_PROTOCOL;
    }
    formats->resize(count);
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
        md_protocol_free_string(*stable_id);
        md_protocol_free_string(*kind);
        formats->clear();
        *stable_id = nullptr;
        *kind = nullptr;
        return rc != 0 ? rc : MD_ERR_PROTOCOL;
    }
    info->stable_output_id = *stable_id;
    info->kind = *kind;
    info->formats = formats->data();
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
    if (display == nullptr || candidate == nullptr) return false;
    for (uint32_t c = 0; c < display->caps.format_count; ++c) {
        const md_format_cap_t* right = &display->cap_formats[c];
        if (candidate->fourcc == right->fourcc && candidate->plane_count == right->plane_count &&
            candidate->modifier == right->modifier) return true;
    }
    return false;
}

/* UUID capability fields are optional. A UUID is present only when at least
 * one of all sixteen wire bytes is non-zero; checking a prefix would make a
 * valid GPU identity indistinguishable from an absent one. */
static bool uuid_is_present(const uint8_t uuid[16]) {
    for (uint32_t index = 0U; index < 16U; ++index) {
        if (uuid[index] != 0U) return true;
    }
    return false;
}


/*
 * Negotiates the format/modifier intersection: a producer candidate is
 * selected only when every bound, ready display supports the exact
 * (fourcc, plane_count, modifier) triple.
 */
static bool formats_intersect(const md_broker_route_t* route, md_format_cap_t* selected) {
    if (route == nullptr || route->producer == nullptr || route->display_count == 0U) return false;
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


/*
 * Derives the producer's OUTPUT_CONFIG from the primary display's geometry and
 * the negotiated format, then marks the route as configured.
 */
static int send_output_config(md_broker_route_t* route) {
    if (route == nullptr || route->display == nullptr || route->producer == nullptr) return MD_ERR_STATE;
    /* A new target configuration invalidates resources created under the old
     * consumer identity. The producer must bind again before offering or
     * submitting another pool; stale GPU ownership is never retained. */
    route->producer_gpu_bound = false;
    /* The target node is mandatory in protocol v1.1: producers must select
     * this exact consumer GPU before they create DMA-BUF resources. */
    if (route->display->output.drm_render_major == 0U ||
        route->display->output.drm_render_minor < 128U ||
        route->display->output.drm_render_minor > 255U) {
        return MD_ERR_UNSUPPORTED;
    }
    md_broker_peer_t* producer = route->producer;
    if (!formats_intersect(route, &route->selected_format)) return MD_ERR_UNSUPPORTED;
    route->format_selected = true;
    md_producer_config_t config = {
        .physical_width = route->display->output.physical_width,
        .physical_height = route->display->output.physical_height,
        .logical_width = route->display->output.logical_width,
        .logical_height = route->display->output.logical_height,
        .refresh_mhz = route->display->output.refresh_mhz,
        .transform = route->display->output.transform,
        .fourcc = route->selected_format.fourcc,
        .plane_count = route->selected_format.plane_count,
        .modifier = route->selected_format.modifier,
        .target_drm_render_major = route->display->output.drm_render_major,
        .target_drm_render_minor = route->display->output.drm_render_minor,
        .target_gpu_flags = MD_TARGET_GPU_RENDER_NODE_VALID,
        /* UUID bytes remain zero when their valid flag is absent. This is a
         * defined v1.1 wire representation, never uninitialized padding. */
        .target_device_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                               0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
        .target_driver_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                               0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    if (uuid_is_present(route->display->caps.device_uuid)) {
        config.target_gpu_flags |= MD_TARGET_GPU_DEVICE_UUID_VALID;
        std::memcpy(config.target_device_uuid, route->display->caps.device_uuid,
                    sizeof(config.target_device_uuid));
    }
    if (uuid_is_present(route->display->caps.driver_uuid)) {
        config.target_gpu_flags |= MD_TARGET_GPU_DRIVER_UUID_VALID;
        std::memcpy(config.target_driver_uuid, route->display->caps.driver_uuid,
                    sizeof(config.target_driver_uuid));
    }
    uint8_t payload[128];
    size_t payload_size = 0;
    if (encode_output_config(&config, payload, sizeof(payload), &payload_size) != MD_OK) {
        return MD_ERR_PROTOCOL;
    }
    int rc = send_encoded(producer, MD_OP_OUTPUT_CONFIG, payload, payload_size);
    if (rc == MD_OK) route->output_config_sent = true;
    /* Replay the cached window-state flags whenever the route configuration is
     * (re)established, so a producer that connects after the display always
     * learns the current desktop facts instead of waiting for the next change.
     * The default 0 (desktop focused, nothing covering) is correct for a
     * producer that connected before any display reported state. */
    if (rc == MD_OK) {
        uint8_t state_payload[8];
        md_writer_t state_writer;
        md_writer_init(&state_writer, state_payload, sizeof(state_payload));
        if (md_proto_encode_u32(&state_writer, route->window_state) == 0) {
            rc = queue_peer(producer, MD_OP_PRODUCER_WINDOW_STATE, 0, state_payload,
                            state_writer.size, nullptr, 0U);
        }
    }
    return rc;
}


/*
 * Forwards the active pool to one display as BIND_BUFFERS.  Pool descriptors
 * are duplicated for the outbound message because the route owns them until the
 * pool is retired; a display receives the pool at most once per generation.
 */
static int send_pool_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == nullptr || display == nullptr || !route_has_display(route, display) ||
        !route->pool_active) return MD_ERR_STATE;
    if (display->display_unbind_pending || display->display_pool_sent) return MD_OK;
    const size_t count = static_cast<size_t>(route->pool.buffer_count) *
                         static_cast<size_t>(route->pool.plane_count);
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
    if (route == nullptr || !route->pool_active) return MD_ERR_STATE;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_pool_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}


/*
 * Starts pool replacement for one display: sends UNBIND and records the
 * pending generation.  The pool stays valid until every display sends
 * UNBIND_DONE (see route_all_unbound).
 */
static int send_unbind_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == nullptr || display == nullptr || !route_has_display(route, display) ||
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
    if (route == nullptr || !route->pool_active) return MD_ERR_STATE;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_unbind_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}

static int send_config_to_display(md_broker_route_t* route, md_broker_peer_t* display) {
    if (route == nullptr || display == nullptr || !display->ready || !route->config_active) {
        return MD_OK;
    }
    uint8_t payload[128];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_config(&writer, &route->config) != 0) return MD_ERR_PROTOCOL;
    return queue_peer(display, MD_OP_SET_CONFIG, 0, payload, writer.size, nullptr, 0U);
}

static int send_config_to_displays(md_broker_route_t* route) {
    if (route == nullptr) return MD_ERR_INVALID;
    for (uint32_t i = 0; i < route->display_count; ++i) {
        int rc = send_config_to_display(route, route->displays[i]);
        if (rc != MD_OK) return rc;
    }
    return MD_OK;
}

static int send_retire_to_producer(md_broker_route_t* route) {
    if (route == nullptr || route->producer == nullptr || !route->pool_active) return MD_ERR_STATE;
    uint8_t payload[8];
    size_t payload_size = 0;
    if (encode_u64_payload(route->pool_generation, payload, sizeof(payload), &payload_size) != MD_OK) {
        return MD_ERR_PROTOCOL;
    }
    route->retire_pending = true;
    return send_encoded(route->producer, MD_OP_RETIRE_BUFFERS, payload, payload_size);
}


/*
 * Binds the pool to all displays once the route has a producer, a display,
 * and an active pool, and no unbind/retire is in flight.
 */
static int maybe_bind_pool(md_broker_route_t* route) {
    if (route == nullptr || route->display == nullptr || route->producer == nullptr ||
        !route->pool_active || route->unbind_pending || route->retire_pending) return MD_OK;
    return send_pool_to_displays(route);
}

/* A rejected producer frame still owns exactly one acquire FD and one release
 * syncobj FD. Both must be consumed together so invalid generations or frame
 * metadata cannot leave the producer's buffer slot permanently blocked. */
static int discard_producer_frame(md_broker_route_t* route, md_packet_t* packet) {
    const int close_result = close(packet->fds[0]);
    packet->fds[0] = -1;
    const md_result_t signal_result = md_display_signal_release_syncobj_on_node(
        packet->fds[1], route->producer->producer_info.drm_render_major,
        route->producer->producer_info.drm_render_minor);
    packet->fds[1] = -1;
    return close_result == 0 && signal_result == MD_OK ? MD_OK : MD_ERR_IO;
}


/*
 * Routes one display-role packet: registration, consumer caps, unbind
 * acknowledgement, pointer input, window state, and geometry updates.  Each case
 * validates role state before mutating the route.
 */
static int handle_display_packet(md_broker_t* broker, md_broker_peer_t* peer,
                                  md_packet_t* packet) {
    md_broker_route_t* route = peer->route;
    switch (packet->opcode) {
    case MD_OP_REGISTER_OUTPUT: {
        if (peer->ready || !peer->output_stable_id.empty() || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_output_info_t output;
        char* stable_id = nullptr;
        char* name = nullptr;
        int rc = parse_output(packet, &output, &stable_id, &name);
        if (rc != MD_OK) return rc;
        ProtocolString stable_id_owner{stable_id, md_protocol_free_string};
        ProtocolString name_owner{name, md_protocol_free_string};
        route = find_route(broker, stable_id_owner.get());
        if (route == nullptr) route = create_route(broker, stable_id_owner.get());
        if (route == nullptr || route->display_count >= kBrokerMaxPeers) {
            return MD_ERR_STATE;
        }
        peer->output_stable_id = stable_id_owner.get();
        peer->output_name = name_owner.get();
        peer->output = output;
        peer->output.stable_id = peer->output_stable_id.c_str();
        peer->output.name = peer->output_name.c_str();
        const bool first_display = route->display_count == 0U;
        peer->route = route;
        rc = route_add_display(route, peer);
        if (rc != MD_OK) return rc;
        if (first_display && broker->on_output_added != nullptr) {
            broker->on_output_added(broker->user_data, &peer->output);
        }
        uint8_t payload[8];
        size_t payload_size = 0;
        if (encode_u64_payload(route->output_id, payload, sizeof(payload), &payload_size) != MD_OK) {
            return MD_ERR_PROTOCOL;
        }
        return send_encoded(peer, MD_OP_OUTPUT_ACCEPTED, payload, payload_size);
    }
    case MD_OP_CONSUMER_CAPS: {
        if (route == nullptr || !route_has_display(route, peer) || packet->fd_count != 0 ||
            peer->ready) return MD_ERR_PROTOCOL;
        int rc = parse_caps(packet, &peer->caps, &peer->cap_formats);
        if (rc != MD_OK) return rc;
        peer->ready = true;
        if (route->producer != nullptr && route->producer->ready) {
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
                route->producer_gpu_bound = false;
                rc = send_output_config(route);
                if (rc != MD_OK) return rc;
            }
        }
        rc = send_config_to_display(route, peer);
        if (rc != MD_OK) return rc;
        return maybe_bind_pool(route);
    }
    case MD_OP_UPDATE_OUTPUT: {
        if (route == nullptr || !route_has_display(route, peer) || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_reader_t reader;
        md_reader_init(&reader, packet->payload, packet->payload_size);
        const md_output_info_t previous_output = peer->output;
        uint32_t transform;
        int rc = md_read_u32(&reader, &peer->output.physical_width);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.physical_height);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.logical_width);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.logical_height);
        if (rc == 0) rc = md_read_i32(&reader, &peer->output.logical_x);
        if (rc == 0) rc = md_read_i32(&reader, &peer->output.logical_y);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.scale_120);
        if (rc == 0) rc = md_read_u32(&reader, &peer->output.refresh_mhz);
        if (rc == 0) rc = md_read_u32(&reader, &transform);
        if (rc == 0) rc = md_reader_finish(&reader);
        if (rc != 0 || transform > MD_TRANSFORM_FLIPPED_270) return MD_ERR_PROTOCOL;
        peer->output.transform = static_cast<md_transform_t>(transform);
        const bool geometry_changed = previous_output.physical_width != peer->output.physical_width ||
            previous_output.physical_height != peer->output.physical_height ||
            previous_output.logical_width != peer->output.logical_width ||
            previous_output.logical_height != peer->output.logical_height ||
            previous_output.logical_x != peer->output.logical_x ||
            previous_output.logical_y != peer->output.logical_y ||
            previous_output.scale_120 != peer->output.scale_120 ||
            previous_output.refresh_mhz != peer->output.refresh_mhz ||
            previous_output.transform != peer->output.transform;
        if (geometry_changed && broker->on_output_updated != nullptr) {
            broker->on_output_updated(broker->user_data, &peer->output);
        }
        if (route->display != peer) return MD_OK;
        route->output_config_sent = false;
        route->producer_gpu_bound = false;
        if (route->producer != nullptr && route->producer->ready) {
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
        if (route == nullptr || route->producer == nullptr || !route->producer->ready ||
            packet->fd_count != 0) return MD_ERR_STATE;
        uint16_t opcode = packet->opcode;
        if (opcode == MD_OP_POINTER_ENTER) opcode = MD_OP_PRODUCER_POINTER_ENTER;
        else if (opcode == MD_OP_POINTER_LEAVE) opcode = MD_OP_PRODUCER_POINTER_LEAVE;
        else if (opcode == MD_OP_POINTER_MOTION) opcode = MD_OP_PRODUCER_POINTER_MOTION;
        else if (opcode == MD_OP_POINTER_BUTTON) opcode = MD_OP_PRODUCER_POINTER_BUTTON;
        else opcode = MD_OP_PRODUCER_POINTER_AXIS;
        return queue_peer(route->producer, opcode, packet->flags, packet->payload,
                          packet->payload_size, nullptr, 0U);
    }
    case MD_OP_WINDOW_STATE: {
        if (route == nullptr || packet->fd_count != 0) return MD_ERR_STATE;
        uint32_t flags = 0U;
        if (md_proto_decode_window_state(packet->payload, packet->payload_size, &flags) != 0) {
            return MD_ERR_PROTOCOL;
        }
        /* Cache the latest flags and forward to the bound producer. Unlike a
         * pointer event, window state is a persistent fact: when no producer
         * is ready yet the value is kept and replayed by send_output_config
         * once the producer (re)connects. */
        route->window_state = flags;
        /* Notify the embedding host (MirageQt) so it can apply its own
         * playback policy; the callback runs on this dispatch thread and the
         * stable id is only borrowed for the call. */
        if (broker->on_window_state != nullptr) {
            broker->on_window_state(broker->user_data, route->stable_id.c_str(), flags);
        }
        if (route->producer != nullptr && route->producer->ready) {
            return queue_peer(route->producer, MD_OP_PRODUCER_WINDOW_STATE, packet->flags,
                              packet->payload, packet->payload_size, nullptr, 0U);
        }
        return MD_OK;
    }
    case MD_OP_UNBIND_DONE: {
        if (route == nullptr || !route_has_display(route, peer) || packet->fd_count != 0 ||
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
        if (route->pool_active && route->producer != nullptr && route->producer->ready &&
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


/*
 * Routes one producer-role packet: registration, buffer offers, frame
 * submission (fanned out to every display), configuration, and retirement
 * acknowledgements.
 */
static int handle_producer_packet(md_broker_t* broker, md_broker_peer_t* peer,
                                  md_packet_t* packet) {
    md_broker_route_t* route = peer->route;
    switch (packet->opcode) {
    case MD_OP_REGISTER_PRODUCER: {
        if (peer->ready || !peer->producer_stable_id.empty() || packet->fd_count != 0) {
            return MD_ERR_PROTOCOL;
        }
        md_producer_info_t info;
        char* stable_id = nullptr;
        char* kind = nullptr;
        std::vector<md_format_cap_t> formats{};
        int rc = parse_producer(packet, &info, &stable_id, &kind, &formats);
        if (rc != MD_OK) return rc;
        ProtocolString stable_id_owner{stable_id, md_protocol_free_string};
        ProtocolString kind_owner{kind, md_protocol_free_string};
        route = find_route(broker, stable_id_owner.get());
        if (route == nullptr) route = create_route(broker, stable_id_owner.get());
        if (route == nullptr || route->producer != nullptr) {
            return MD_ERR_STATE;
        }
        peer->producer_stable_id = stable_id_owner.get();
        peer->producer_kind = kind_owner.get();
        peer->producer_formats = std::move(formats);
        peer->producer_info = info;
        peer->producer_info.stable_output_id = peer->producer_stable_id.c_str();
        peer->producer_info.kind = peer->producer_kind.c_str();
        peer->producer_info.formats = peer->producer_formats.data();
        peer->route = route;
        route->producer = peer;
        /* A producer reconnect invalidates the old producer's pool. Keep the
         * consumer alive, but explicitly unbind it before accepting frames
         * from the new producer. The old descriptors are broker-owned and
         * can be closed immediately after the unbind packet is queued. */
        if (route->pool_active) {
            if (route->display_count > 0 && route->display->ready && !route->unbind_pending) {
                const int unbind_result = send_unbind_to_displays(route);
                if (unbind_result != MD_OK) return unbind_result;
                route->unbind_retiring_old_producer = true;
            }
            md_close_pool(&route->pool);
            route->pool_active = false;
            route->pool_generation = 0;
        }
        route->retire_pending = false;
        /* A reconnect must prove its GPU again; the old producer's identity
         * cannot authorize frames from the newly registered process. */
        route->producer_gpu_bound = false;
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
        if (rc == MD_OK && route->display != nullptr && route->display->ready &&
            !route->output_config_sent) {
            rc = send_output_config(route);
        }
        return rc;
    }
    case MD_OP_OFFER_BUFFERS: {
        if (route == nullptr || route->producer != peer || !peer->ready ||
            !route->producer_gpu_bound || route->pool_active) {
            return MD_ERR_PROTOCOL;
        }
        md_buffer_pool_t pool;
        int rc = md_proto_decode_bind_buffers(packet->payload, packet->payload_size, &pool);
        size_t expected = 0;
        if (rc == 0) {
            expected = static_cast<size_t>(pool.buffer_count) *
                       static_cast<size_t>(pool.plane_count);
        }
        if (rc != 0 || packet->fd_count != expected || expected > MD_WIRE_MAX_FDS ||
            !route->format_selected || pool.fourcc != route->selected_format.fourcc ||
            pool.plane_count != route->selected_format.plane_count ||
            pool.modifier != route->selected_format.modifier) return MD_ERR_PROTOCOL;
        for (uint32_t b = 0; b < pool.buffer_count; ++b) {
            for (uint32_t p = 0; p < pool.plane_count; ++p) {
                const size_t index = static_cast<size_t>(b) * pool.plane_count + p;
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
        if (route == nullptr || route->producer != peer || !route->pool_active ||
            !route->producer_gpu_bound || packet->fd_count != 2) {
            return MD_ERR_PROTOCOL;
        }
        md_frame_t frame;
        if (md_proto_decode_frame(packet->payload, packet->payload_size, &frame) != 0) {
            const int cleanup_result = discard_producer_frame(route, packet);
            if (cleanup_result != MD_OK) return cleanup_result;
            return MD_ERR_PROTOCOL;
        }
        if (frame.buffer_generation != route->pool_generation) {
            const int cleanup_result = discard_producer_frame(route, packet);
            if (cleanup_result != MD_OK) return cleanup_result;
            return MD_OK;
        }
        if (frame.buffer_index >= route->pool.buffer_count) {
            const int cleanup_result = discard_producer_frame(route, packet);
            if (cleanup_result != MD_OK) return cleanup_result;
            return MD_ERR_PROTOCOL;
        }
        std::array<md_broker_peer_t*, kBrokerMaxPeers> active_displays{};
        uint32_t active_count = 0;
        for (uint32_t i = 0; i < route->display_count; ++i) {
            md_broker_peer_t* display = route->displays[i];
            if (display->ready && !display->display_unbind_pending) {
                active_displays[active_count++] = display;
            }
        }
        if (active_count == 0) {
            const int cleanup_result = discard_producer_frame(route, packet);
            if (cleanup_result != MD_OK) return cleanup_result;
            return MD_OK;
        }

        if (active_count > 1) {
            std::array<int, kBrokerMaxPeers> release_fds{};
            md_sync_fanout_t* sync = nullptr;
            if (md_sync_fanout_create_on_node(
                    packet->fds[1], active_count, route->producer->producer_info.drm_render_major,
                    route->producer->producer_info.drm_render_minor, release_fds.data(), &sync) == MD_OK) {
                std::unique_ptr<md_broker_fanout_t> fanout{
                    new (std::nothrow) md_broker_fanout_t{}};
                if (!fanout) {
                    const int cleanup_result = discard_producer_frame(route, packet);
                    for (uint32_t i = 0; i < active_count; ++i) {
                        if (release_fds[i] >= 0) close(release_fds[i]);
                        md_sync_fanout_abandon(sync, i);
                    }
                    md_sync_fanout_free(sync);
                    return cleanup_result == MD_OK ? MD_ERR_NOMEM : cleanup_result;
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
                fanout->next = std::move(broker->fanouts);
                broker->fanouts = std::move(fanout);
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
            const int cleanup_result = discard_producer_frame(route, packet);
            return cleanup_result == MD_OK ? MD_OK : cleanup_result;
        }
        int release_fallback = fcntl(fds[1], F_DUPFD_CLOEXEC, 0);
        int rc = queue_peer(display, MD_OP_FRAME_READY, packet->flags,
                            packet->payload, packet->payload_size, fds, 2);
        if (release_fallback >= 0) {
            if (rc == MD_OK) {
                if (close(release_fallback) != 0) return MD_ERR_IO;
            } else {
                const md_result_t signal_result = md_display_signal_release_syncobj_on_node(
                    release_fallback, route->producer->producer_info.drm_render_major,
                    route->producer->producer_info.drm_render_minor);
                if (signal_result != MD_OK) return signal_result;
            }
        } else if (rc != MD_OK) {
            const md_result_t signal_result = md_display_signal_release_syncobj_on_node(
                packet->fds[1], route->producer->producer_info.drm_render_major,
                route->producer->producer_info.drm_render_minor);
            packet->fds[1] = -1;
            if (signal_result != MD_OK) return signal_result;
        }
        return rc == MD_OK ? MD_OK : MD_ERR_IO;
    }
    case MD_OP_PRODUCER_SET_CONFIG: {
        if (route == nullptr || route->producer != peer || packet->fd_count != 0 ||
            !peer->ready) return MD_ERR_STATE;
        md_display_config_t config;
        if (md_proto_decode_config(packet->payload, packet->payload_size, &config) != 0) {
            return MD_ERR_PROTOCOL;
        }
        route->config = config;
        route->config_active = true;
        return send_config_to_displays(route);
    }
    case MD_OP_PRODUCER_GPU_BOUND: {
        if (route == nullptr || route->producer != peer || packet->fd_count != 0 ||
            !peer->ready || route->producer_gpu_bound) return MD_ERR_PROTOCOL;
        md_producer_gpu_info_t gpu{};
        if (md_proto_decode_producer_gpu_bound(packet->payload, packet->payload_size, &gpu) != MD_OK) {
            return MD_ERR_PROTOCOL;
        }
        md_producer_config_t expected{};
        if (!route->output_config_sent || !route->display->ready ||
            !route->display->output.drm_render_minor ||
            !route->format_selected || !formats_intersect(route, &route->selected_format)) {
            return MD_ERR_STATE;
        }
        expected.target_drm_render_major = route->display->output.drm_render_major;
        expected.target_drm_render_minor = route->display->output.drm_render_minor;
        if (gpu.drm_render_major != expected.target_drm_render_major ||
            gpu.drm_render_minor != expected.target_drm_render_minor) {
            return MD_ERR_UNSUPPORTED;
        }
        if (uuid_is_present(route->display->caps.device_uuid) &&
            std::memcmp(gpu.device_uuid, route->display->caps.device_uuid,
                        sizeof(gpu.device_uuid)) != 0) return MD_ERR_UNSUPPORTED;
        if (uuid_is_present(route->display->caps.driver_uuid) &&
            std::memcmp(gpu.driver_uuid, route->display->caps.driver_uuid,
                        sizeof(gpu.driver_uuid)) != 0) return MD_ERR_UNSUPPORTED;
        peer->producer_info.drm_render_major = gpu.drm_render_major;
        peer->producer_info.drm_render_minor = gpu.drm_render_minor;
        std::memcpy(peer->producer_info.device_uuid, gpu.device_uuid,
                    sizeof(peer->producer_info.device_uuid));
        std::memcpy(peer->producer_info.driver_uuid, gpu.driver_uuid,
                    sizeof(peer->producer_info.driver_uuid));
        route->producer_gpu_bound = true;
        return MD_OK;
    }
    case MD_OP_RETIRE_DONE: {
        if (route == nullptr || route->producer != peer || packet->fd_count != 0 ||
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


/*
 * Top-level packet dispatcher: enforces the HELLO-first rule, then sends the
 * packet to the role handler matching the peer's registered role.
 */
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
        char* name = nullptr;
        char* version = nullptr;
        int rc = parse_hello(packet, &role, &minor, &features, &name, &version);
        if (rc != MD_OK) return rc;
        ProtocolString name_owner{name, md_protocol_free_string};
        ProtocolString version_owner{version, md_protocol_free_string};
        peer->role = role;
        peer->minor = minor;
        peer->features = features;
        peer->client_name = name_owner.get();
        peer->client_version = version_owner.get();
        peer->hello_done = true;
        uint8_t payload[MD_WIRE_MAX_PAYLOAD];
        size_t payload_size = 0;
        rc = encode_welcome(broker, peer->minor, payload, sizeof(payload), &payload_size);
        if (rc == MD_OK) rc = send_encoded(peer, MD_OP_WELCOME, payload, payload_size);
        return rc;
    }
    if (peer->role == MD_BROKER_ROLE_DISPLAY) return handle_display_packet(broker, peer, packet);
    if (peer->role == MD_BROKER_ROLE_PRODUCER) return handle_producer_packet(broker, peer, packet);
    return MD_ERR_PROTOCOL;
}

static int detach_peer_from_route(md_broker_t* broker, md_broker_peer_t* peer) {
    md_broker_route_t* route = peer->route;
    if (route == nullptr) return MD_OK;
    int result = MD_OK;
    if (route_has_display(route, peer)) {
        const bool was_last_display = route->display_count == 1U;
        route_remove_display(route, peer);
        if (was_last_display && broker->on_output_removed != nullptr) {
            broker->on_output_removed(broker->user_data, route->stable_id.c_str());
        }
        /* Keep the producer and its broker-owned pool alive. A replacement
         * display can bind the exact same generation without forcing the
         * renderer to allocate again. */
        route->unbind_pending = !route_all_unbound(route);
    }
    if (route->producer == peer) {
        if (route->pool_active) {
            if (route->display_count > 0 && route->display->ready && !route->unbind_pending) {
                const int unbind_result = send_unbind_to_displays(route);
                if (unbind_result != MD_OK) result = unbind_result;
            }
            route->unbind_retiring_old_producer = route->unbind_pending;
        }
        route->producer = nullptr;
        /* The producer owns the authoritative generation. Once it is gone,
         * retain only the route identity, not stale DMA-BUF descriptors. */
        md_close_pool(&route->pool);
        route->pool_active = false;
        route->retire_pending = false;
        /* A disconnected producer no longer owns the GPU advertised by the
         * route, so a replacement must complete GPU binding from scratch. */
        route->producer_gpu_bound = false;
        route->output_config_sent = false;
        route->format_selected = false;
        route->pool_generation = 0;
        route->config_active = false;
    }
    peer->route = nullptr;
    /* Keep an empty route so a peer that restarts with the same stable id can
     * reclaim its output id and negotiated lifecycle. It is bounded by the
     * broker's max_routes setting and is reclaimed when the broker itself is
     * destroyed. */
    return result;
}

static int disconnect_peer(md_broker_t* broker, md_broker_peer_t* peer) {
    if (peer == nullptr) return MD_OK;
    abandon_peer_fanouts(broker, peer);
    const int detach_result = detach_peer_from_route(broker, peer);
    remove_peer_slot(broker, peer);
    return detach_result;
}


/*
 * Accepts one peer on the listener and validates its credentials with
 * SO_PEERCRED: any UID different from the broker's own is rejected before any
 * protocol byte is processed.  The peer starts in an empty slot and waits for
 * HELLO.
 */
static md_broker_peer_t* accept_peer(md_broker_t* broker) {
    const int fd = accept4(broker->listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return nullptr;
    struct ucred credentials;
    socklen_t credential_size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) != 0 ||
        credential_size != sizeof(credentials) || credentials.uid != getuid()) {
        close(fd);
        return nullptr;
    }
    size_t slot = broker->peers.size();
    for (size_t i = 0; i < broker->peers.size(); ++i) {
        if (!broker->peers[i]) {
            slot = i;
            break;
        }
    }
    if (slot == broker->peers.size()) {
        close(fd);
        return nullptr;
    }
    std::unique_ptr<md_broker_peer_t> peer{new (std::nothrow) md_broker_peer_t{}};
    if (!peer) {
        close(fd);
        return nullptr;
    }
    peer->fd = fd;
    md_broker_peer_t* const result = peer.get();
    broker->peers[slot] = std::move(peer);
    return result;
}

md_broker_t* md_broker_new(const md_broker_options_t* options) {
    if (options == nullptr || options->socket_path == nullptr || options->socket_path[0] == '\0') {
        return nullptr;
    }
    try {
        std::unique_ptr<md_broker_t> broker{new (std::nothrow) md_broker_t{}};
        if (!broker) return nullptr;
        broker->socket_path = options->socket_path;
        broker->server_name = options->server_name != nullptr ? options->server_name : "mirage-display";
        broker->server_version =
            options->server_version != nullptr ? options->server_version : "0.1";
        broker->features = options->features != 0U
                               ? options->features
                               : MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                                     MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS |
                                     MD_FEATURE_WINDOW_STATE | MD_FEATURE_COLOR_METADATA;
        broker->max_routes =
            options->max_routes == 0U ? kBrokerDefaultRoutes : options->max_routes;
        if (broker->max_routes > kBrokerMaxRoutes) {
            return nullptr;
        }
        broker->on_window_state = options->on_window_state;
        broker->on_output_added = options->on_output_added;
        broker->on_output_updated = options->on_output_updated;
        broker->on_output_removed = options->on_output_removed;
        broker->user_data = options->user_data;
        return broker.release();
    } catch (const std::bad_alloc&) {
        /* The C ABI cannot propagate allocation exceptions, so construction
         * reports its established null result instead. */
        return nullptr;
    }
}

void md_broker_stop(md_broker_t* broker) {
    if (broker == nullptr) return;
    broker->stopping.store(true);
}

void md_broker_free(md_broker_t* broker) {
    if (broker == nullptr) return;
    md_broker_stop(broker);
    for (size_t index = 0; index < broker->peers.size(); ++index) {
        broker->peers[index].reset();
    }
    free_fanouts(broker);
    for (size_t index = 0; index < broker->routes.size(); ++index) {
        broker->routes[index].reset();
    }
    if (broker->listen_fd >= 0) close(broker->listen_fd);
    if (!broker->socket_path.empty() && broker->socket_path[0] != '@') {
        unlink(broker->socket_path.c_str());
    }
    delete broker;
}


/*
 * Binds the AF_UNIX SOCK_SEQPACKET endpoint with mode 0600 (pathname sockets
 * are unlinked on stop/free) and starts accepting peers.
 */
md_result_t md_broker_listen(md_broker_t* broker) {
    if (broker == nullptr || broker->listening) return MD_ERR_STATE;
    const bool abstract = broker->socket_path[0] == '@';
    if (!abstract) {
        struct stat existing;
        if (lstat(broker->socket_path.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) return MD_ERR_IO;
            if (unlink(broker->socket_path.c_str()) != 0) return MD_ERR_IO;
        } else if (errno != ENOENT) {
            return MD_ERR_IO;
        }
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return MD_ERR_IO;
    struct sockaddr_un address;
    socklen_t address_length{};
    const int address_result =
        md_fill_unix_address(broker->socket_path.c_str(), &address, &address_length);
    if (address_result != MD_OK) {
        close(fd);
        return static_cast<md_result_t>(address_result);
    }
    if (bind(fd, std::bit_cast<sockaddr*>(&address), address_length) != 0) {
        int saved_errno = errno;
        close(fd);
        if (!abstract) unlink(broker->socket_path.c_str());
        errno = saved_errno;
        return MD_ERR_IO;
    }
    if (!abstract && chmod(broker->socket_path.c_str(), 0600) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(broker->socket_path.c_str());
        errno = saved_errno;
        return MD_ERR_IO;
    }
    if (listen(fd, static_cast<int>(kBrokerMaxPeers)) != 0) {
        int saved_errno = errno;
        close(fd);
        if (!abstract) unlink(broker->socket_path.c_str());
        errno = saved_errno;
        return MD_ERR_IO;
    }
    broker->listen_fd = fd;
    broker->listening = true;
    broker->stopping.store(false);
    return MD_OK;
}

int32_t md_broker_get_fd(const md_broker_t* broker) {
    return broker != nullptr ? broker->listen_fd : -1;
}

const char* md_broker_socket_path(const md_broker_t* broker) {
    return broker != nullptr ? broker->socket_path.c_str() : nullptr;
}


/*
 * Polls the listener and every active peer for up to timeout_ms, accepting new
 * peers, draining readable packets, flushing writable outboxes, and polling
 * completed fanouts.  A negative timeout blocks until an event; the returned
 * count includes accepted peers and handled packets.
 */
int32_t md_broker_dispatch(md_broker_t* broker, const int32_t timeout_ms) {
    if (broker == nullptr || !broker->listening) return MD_ERR_STATE;
    if (broker->stopping.load()) return MD_ERR_DISCONNECTED;
    try {
        poll_fanouts(broker);

        std::array<pollfd, 1U + kBrokerMaxPeers> descriptors{};
        std::array<md_broker_peer_t*, kBrokerMaxPeers> owners{};
        nfds_t count = 1;
        descriptors[0] = pollfd{broker->listen_fd, POLLIN, 0};
        size_t owner_count = 0;
        for (size_t i = 0; i < broker->peers.size(); ++i) {
            md_broker_peer_t* const peer = broker->peers[i].get();
            if (peer == nullptr) continue;
            const short events =
                static_cast<short>(POLLIN | (peer->outbox.has_pending() ? POLLOUT : 0));
            descriptors[count] = pollfd{peer->fd, events, 0};
            owners[owner_count++] = peer;
            ++count;
        }
        int poll_result;
        do {
            poll_result = poll(descriptors.data(), count, timeout_ms);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) return MD_ERR_IO;
        if (poll_result == 0) return 0;
        int handled = 0;
        if ((descriptors[0].revents & POLLIN) != 0) {
            for (;;) {
                md_broker_peer_t* const peer = accept_peer(broker);
                if (peer == nullptr) break;
                ++handled;
            }
        }
        for (size_t i = 0; i < owner_count; ++i) {
            md_broker_peer_t* const peer = owners[i];
            if (peer_index(broker, peer) < 0) continue;
            const size_t descriptor_index = i + 1U;
            const short revents = descriptors[descriptor_index].revents;
            bool disconnect = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (!disconnect && (revents & POLLIN) != 0) {
                md_packet_t packet;
                const int receive_result = md_codec_recv(peer->fd, &packet);
                if (receive_result < 0) {
                    disconnect = true;
                } else if (receive_result == 1) {
                    const int result = handle_peer_packet(broker, peer, &packet);
                    md_packet_close_fds(&packet);
                    ++handled;
                    if (result < 0) {
                        const int send_result = send_error(
                            peer, static_cast<uint32_t>(-result), true, "broker protocol error");
                        if (send_result != MD_OK) {
                            disconnect = true;
                        }
                        disconnect = true;
                    }
                }
            }
            if (!disconnect && (revents & POLLOUT) != 0 && peer_index(broker, peer) >= 0) {
                if (flush_peer(peer) < 0) disconnect = true;
            }
            if (disconnect && peer_index(broker, peer) >= 0) {
                const int disconnect_result = disconnect_peer(broker, peer);
                if (disconnect_result != MD_OK) return disconnect_result;
            }
        }
        poll_fanouts(broker);
        return handled;
    } catch (const std::bad_alloc&) {
        /* C callers receive the module's established error code instead of a
         * C++ exception when protocol-owned STL storage cannot grow. */
        return MD_ERR_NOMEM;
    }
}
