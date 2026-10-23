#include "mirage_display_producer.h"

#include "codec.hpp"
#include "common/handshake.hpp"
#include "common/util.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <utility>
#include <vector>

/*
 * Producer-side library (include/mirage_display_producer.h): the renderer-role
 * mirror of the consumer session, adding buffer lending, frame submission, and
 * generation retirement.
 *
 * Frame synchronization descriptors are consumed by md_producer_submit_frame on
 * every return path; pool descriptors are borrowed and duplicated for queued
 * sends, so the pool itself always remains owned by the producer.
 */

struct md_producer final : mirage::ClientSession {
    explicit md_producer(const md_producer_callbacks_t* callbacks)
        : ClientSession(MD_CLIENT_ROLE_PRODUCER,
                        MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                            MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS |
                            MD_FEATURE_WINDOW_STATE | MD_FEATURE_TARGET_GPU_BINDING),
          producer_id_(0U),
          output_id_(0U),
          pool_offered_(false),
          retire_pending_(false),
          gpu_bound_(false),
          pool_generation_(0U),
          pool_buffer_count_(0U) {
        std::memset(&callbacks_, 0, sizeof(callbacks_));
        std::memset(&info_, 0, sizeof(info_));
        if (callbacks != nullptr) {
            callbacks_ = *callbacks;
        }
    }

    ~md_producer() override {
        close_producer();
        clear_connection_data();
    }

    md_result_t configure_connection(const char* socket_path, const char* client_name,
                                     const char* client_version,
                                     const md_producer_info_t* info) {
        if (!valid_info(info)) {
            return MD_ERR_INVALID;
        }
        try {
            std::string next_stable_output_id(info->stable_output_id);
            std::string next_kind(info->kind);
            std::vector<md_format_cap_t> next_formats(info->formats,
                                                       info->formats + info->format_count);
            const md_result_t identity_result = set_identity(socket_path, client_name, client_version);
            if (identity_result != MD_OK) {
                return identity_result;
            }
            stable_output_id_ = std::move(next_stable_output_id);
            kind_ = std::move(next_kind);
            formats_ = std::move(next_formats);
        } catch (const std::bad_alloc&) {
            clear_connection_data();
            return MD_ERR_NOMEM;
        }

        info_ = *info;
        info_.stable_output_id = stable_output_id_.c_str();
        info_.kind = kind_.c_str();
        info_.formats = formats_.data();
        return MD_OK;
    }


/*
 * Starts a new connection over a pathname or @-prefixed abstract AF_UNIX
 * socket.  Identity strings and the producer info are copied into session-owned
 * storage before the shared handshake begins.
 */

    md_result_t begin_path_connection(const char* socket_path, const char* client_name,
                                      const char* client_version,
                                      const md_producer_info_t* info) {
        if (connection_state() != MD_CONNECTION_DISCONNECTED) {
            return MD_ERR_STATE;
        }
        const md_result_t configuration_result =
            configure_connection(socket_path, client_name, client_version, info);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        const md_result_t connection_result = begin_connect();
        if (connection_result != MD_OK) {
            return connection_result;
        }
        producer_id_ = 0U;
        output_id_ = 0U;
        return MD_OK;
    }


/*
 * Adopts an already-connected SOCK_SEQPACKET descriptor (broker handoff,
 * socket activation, or tests); ownership transfers to the producer on success.
 */

    md_result_t begin_adopted_connection(const std::int32_t connected_fd,
                                         const char* client_name,
                                         const char* client_version,
                                         const md_producer_info_t* info) {
        if (connected_fd == mirage::kInvalidFd ||
            connection_state() != MD_CONNECTION_DISCONNECTED) {
            return MD_ERR_STATE;
        }
        const md_result_t configuration_result =
            configure_connection("", client_name, client_version, info);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        const md_result_t connection_result = begin_connected_fd(connected_fd);
        if (connection_result != MD_OK) {
            return connection_result;
        }
        producer_id_ = 0U;
        output_id_ = 0U;
        return MD_OK;
    }


/*
 * Blocking convenience wrapper around configure_connection + connect() for
 * command-line tools and tests.
 */

    md_result_t connect_path(const char* socket_path, const char* client_name,
                             const char* client_version, const md_producer_info_t* info,
                             const std::int32_t timeout_ms) {
        const md_result_t configuration_result =
            configure_connection(socket_path, client_name, client_version, info);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        producer_id_ = 0U;
        output_id_ = 0U;
        return connect(timeout_ms);
    }


/*
 * Closes the session and resets pool state.  Pool descriptors belong to the
 * producer and are not owned here; the caller keeps them.
 */

    void close_producer() noexcept {
        close();
        producer_id_ = 0U;
        output_id_ = 0U;
        pool_offered_ = false;
        retire_pending_ = false;
        gpu_bound_ = false;
        pool_generation_ = 0U;
        pool_buffer_count_ = 0U;
    }

    [[nodiscard]] bool wants_writable() const noexcept {
        return has_pending_output();
    }

    md_result_t handle_writable() {
        if (connection_state() != MD_CONNECTION_READY) {
            return MD_ERR_STATE;
        }
        const md_result_t result = flush_messages();
        return result == MD_ERR_WOULD_BLOCK ? MD_OK : result;
    }


/*
 * Drains every currently readable packet (OUTPUT_CONFIG, RETIRE_BUFFERS, and
 * pointer events) and invokes the borrowed callbacks.
 */

    std::int32_t dispatch() {
        if (connection_state() != MD_CONNECTION_READY) {
            return MD_ERR_STATE;
        }
        std::int32_t count = 0;
        for (;;) {
            md_packet_t packet;
            const std::int32_t receive_result = md_codec_recv(fd(), &packet);
            if (receive_result == 0) {
                break;
            }
            if (receive_result < 0) {
                return fail_producer(md_map_io_error(receive_result), "producer receive failed");
            }
            const md_result_t packet_result = process_packet(&packet);
            md_packet_close_fds(&packet);
            if (packet_result != MD_OK) {
                return packet_result;
            }
            ++count;
        }
        const md_result_t flush_result = flush_messages();
        if (flush_result != MD_OK && flush_result != MD_ERR_WOULD_BLOCK) {
            return flush_result;
        }
        return count;
    }


/*
 * Lends a pool to the broker.  Pool descriptors are borrowed from the caller
 * and duplicated with F_DUPFD_CLOEXEC because a queued send must survive while
 * the caller may replace its pool; the pool itself remains producer-owned.
 */

    md_result_t offer_buffers(const md_buffer_pool_t* pool) {
        if (pool == nullptr || !gpu_bound_ || pool_offered_) {
            return MD_ERR_STATE;
        }
        std::array<std::uint8_t, 1024U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_offer_buffers(&writer, pool) != 0) {
            return MD_ERR_INVALID;
        }
        const std::size_t fd_count =
            static_cast<std::size_t>(pool->buffer_count) * pool->plane_count;
        if (fd_count > MD_WIRE_MAX_FDS) {
            return MD_ERR_INVALID;
        }

        std::array<std::int32_t, MD_WIRE_MAX_FDS> duplicated_fds{};
        std::fill(duplicated_fds.begin(), duplicated_fds.end(), mirage::kInvalidFd);
        std::size_t index = 0U;
        for (std::uint32_t buffer = 0U; buffer < pool->buffer_count; ++buffer) {
            for (std::uint32_t plane = 0U; plane < pool->plane_count; ++plane) {
                const std::int32_t source_fd = pool->planes[buffer][plane].fd;
                if (source_fd == mirage::kInvalidFd) {
                    md_close_fds(duplicated_fds.data(), index);
                    return MD_ERR_INVALID;
                }
                const std::int32_t duplicate = ::fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
                if (duplicate == mirage::kInvalidFd) {
                    md_close_fds(duplicated_fds.data(), index);
                    return MD_ERR_IO;
                }
                duplicated_fds[index] = duplicate;
                ++index;
            }
        }
        const md_result_t result = send_owned(MD_OP_OFFER_BUFFERS, payload.data(), writer.size,
                                              duplicated_fds.data(), fd_count);
        if (result == MD_OK) {
            pool_offered_ = true;
            pool_generation_ = pool->generation;
            pool_buffer_count_ = pool->buffer_count;
        }
        return result;
    }

    md_result_t bind_gpu(const md_producer_gpu_info_t* gpu) {
        if (gpu == nullptr || gpu_bound_ || connection_state() != MD_CONNECTION_READY) {
            return MD_ERR_STATE;
        }
        std::array<std::uint8_t, 64U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_producer_gpu_bound(&writer, gpu) != 0) return MD_ERR_INVALID;
        const md_result_t result = send_owned(MD_OP_PRODUCER_GPU_BOUND, payload.data(),
                                              writer.size, nullptr, 0U);
        if (result == MD_OK) {
            gpu_bound_ = true;
            info_.drm_render_major = gpu->drm_render_major;
            info_.drm_render_minor = gpu->drm_render_minor;
            std::memcpy(info_.device_uuid, gpu->device_uuid, sizeof(info_.device_uuid));
            std::memcpy(info_.driver_uuid, gpu->driver_uuid, sizeof(info_.driver_uuid));
        }
        return result;
    }


/*
 * Forwards a placement/transform/clear-color change to the broker, which
 * relays it to every bound display as SET_CONFIG.
 */

    md_result_t set_config(const md_display_config_t* config) {
        if (config == nullptr || !pool_offered_ || retire_pending_) {
            return MD_ERR_STATE;
        }
        std::array<std::uint8_t, 128U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_config(&writer, config) != 0) {
            return MD_ERR_INVALID;
        }
        return send_owned(MD_OP_PRODUCER_SET_CONFIG, payload.data(), writer.size, nullptr, 0U);
    }


/*
 * Submits one frame.  Both sync descriptors are consumed on every return path,
 * including validation failures and a full outbox, so the caller can never
 * double-close them.  Stale generations and retired pools are rejected.
 */

    md_result_t submit_frame(const std::uint64_t generation, const std::uint32_t buffer_index,
                             const std::uint64_t sequence, const std::int32_t acquire_sync_fd,
                             const std::int32_t release_syncobj_fd) {
        std::array<std::int32_t, 2U> fds{acquire_sync_fd, release_syncobj_fd};
        if (acquire_sync_fd == mirage::kInvalidFd ||
            release_syncobj_fd == mirage::kInvalidFd || !pool_offered_ || retire_pending_ ||
            generation != pool_generation_ || buffer_index >= pool_buffer_count_) {
            md_close_fds(fds.data(), fds.size());
            return MD_ERR_STATE;
        }
        std::array<std::uint8_t, 32U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_producer_frame(&writer, generation, buffer_index, sequence) != 0) {
            md_close_fds(fds.data(), fds.size());
            return MD_ERR_INVALID;
        }
        return send_owned(MD_OP_PRODUCER_FRAME, payload.data(), writer.size, fds.data(),
                          fds.size());
    }


/*
 * Acknowledges RETIRE_BUFFERS after the producer stopped submitting and
 * destroyed its local pool; resets pool state so the next generation can be
 * offered.
 */

    md_result_t retire_done(const std::uint64_t generation) {
        if (!pool_offered_ || !retire_pending_ || generation != pool_generation_) {
            return MD_ERR_STATE;
        }
        std::array<std::uint8_t, 8U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_u64(&writer, generation) != 0) {
            return MD_ERR_INVALID;
        }
        const md_result_t result =
            send_owned(MD_OP_RETIRE_DONE, payload.data(), writer.size, nullptr, 0U);
        if (result == MD_OK) {
            pool_offered_ = false;
            retire_pending_ = false;
            pool_generation_ = 0U;
            pool_buffer_count_ = 0U;
        }
        return result;
    }

protected:

/*
 * Role hooks: a producer registers itself, waits for PRODUCER_ACCEPTED, and
 * has no separate caps step because capabilities ride in REGISTER_PRODUCER.
 */

    [[nodiscard]] std::uint16_t register_opcode() const noexcept override {
        return MD_OP_REGISTER_PRODUCER;
    }

    [[nodiscard]] std::uint16_t accepted_opcode() const noexcept override {
        return MD_OP_PRODUCER_ACCEPTED;
    }

    [[nodiscard]] std::uint16_t caps_opcode() const noexcept override {
        return 0U;
    }

    std::int32_t encode_registration(md_writer_t* const writer) override {
        return md_proto_encode_register_producer(writer, &info_);
    }

    std::int32_t encode_caps(md_writer_t* const) override {
        return MD_ERR_STATE;
    }

    std::int32_t apply_accepted(const md_packet_t& packet) override {
        std::uint64_t accepted_producer_id = 0U;
        std::uint64_t accepted_output_id = 0U;
        if (md_proto_decode_producer_accepted(packet.payload, packet.payload_size,
                                              &accepted_producer_id,
                                              &accepted_output_id) != 0 ||
            accepted_producer_id == 0U || accepted_output_id == 0U) {
            return MD_ERR_PROTOCOL;
        }
        producer_id_ = accepted_producer_id;
        output_id_ = accepted_output_id;
        return MD_OK;
    }

    void on_ready() override {
        if (callbacks_.on_connected != nullptr) {
            callbacks_.on_connected(callbacks_.user_data, producer_id_, output_id_);
        }
    }

    void on_disconnected(const md_result_t reason, const char* const message) override {
        pool_offered_ = false;
        retire_pending_ = false;
        gpu_bound_ = false;
        pool_generation_ = 0U;
        pool_buffer_count_ = 0U;
        if (callbacks_.on_disconnected != nullptr) {
            callbacks_.on_disconnected(callbacks_.user_data, reason, message);
        }
    }

private:

/*
 * Requires a non-empty format list so the broker always has a capability
 * intersection to negotiate with.
 */

    static bool valid_info(const md_producer_info_t* const info) {
        if (info == nullptr || info->stable_output_id == nullptr || info->kind == nullptr ||
            info->format_count == 0U || info->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
            info->formats == nullptr) {
            return false;
        }
        for (std::uint32_t index = 0U; index < info->format_count; ++index) {
            if (info->formats[index].plane_count == 0U ||
                info->formats[index].plane_count > MIRAGE_DISPLAY_MAX_PLANES) {
                return false;
            }
        }
        return true;
    }

    void clear_connection_data() noexcept {
        clear_identity();
        stable_output_id_.clear();
        kind_.clear();
        formats_.clear();
        std::memset(&info_, 0, sizeof(info_));
    }

    std::int32_t fail_producer(const md_result_t reason, const char* const message) {
        return disconnect(reason, message);
    }

    md_result_t flush_messages() {
        const std::int32_t result = flush_outbox();
        if (result == MD_ERR_WOULD_BLOCK) {
            return MD_ERR_WOULD_BLOCK;
        }
        if (result < 0) {
            return static_cast<md_result_t>(
                fail_producer(md_map_io_error(result), "producer outbox failed"));
        }
        return MD_OK;
    }

    /* send_owned consumes every supplied descriptor, including rejected submissions. */
    md_result_t send_owned(const std::uint16_t opcode, const std::uint8_t* const payload,
                           const std::size_t payload_size, std::int32_t* const fds,
                           const std::size_t fd_count) {
        if (connection_state() != MD_CONNECTION_READY) {
            md_close_fds(fds, fd_count);
            return MD_ERR_STATE;
        }
        if (fd_count > MD_WIRE_MAX_FDS) {
            md_close_fds(fds, fd_count);
            return MD_ERR_INVALID;
        }
        const std::int32_t result =
            send_or_queue(opcode, 0U, payload, payload_size, fds, fd_count, false);
        if (result == MD_OK || result == MD_ERR_WOULD_BLOCK || result == MD_ERR_NOMEM ||
            result == MD_ERR_INVALID) {
            return static_cast<md_result_t>(result);
        }
        return static_cast<md_result_t>(
            fail_producer(md_map_io_error(result), "producer request failed"));
    }

    md_result_t process_packet(md_packet_t* const packet) {
        if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
            packet->minor != selected_minor()) {
            return static_cast<md_result_t>(
                fail_producer(MD_ERR_PROTOCOL, "producer wire version changed"));
        }
        if (packet->fd_count != 0U) {
            return static_cast<md_result_t>(
                fail_producer(MD_ERR_PROTOCOL, "unexpected producer event FDs"));
        }
        if (packet->opcode == MD_OP_ERROR) {
            md_proto_error_t error;
            if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
                return static_cast<md_result_t>(
                    fail_producer(MD_ERR_PROTOCOL, "malformed broker error"));
            }
            const md_result_t result = error.fatal != 0U
                                           ? static_cast<md_result_t>(fail_producer(
                                                 MD_ERR_PROTOCOL, error.message))
                                           : MD_OK;
            md_proto_error_clear(&error);
            return result;
        }

        switch (packet->opcode) {
            case MD_OP_OUTPUT_CONFIG: {
                md_producer_config_t config;
                if (md_proto_decode_output_config(packet->payload, packet->payload_size,
                                                  &config) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed output config"));
                }
                if (callbacks_.on_output_config != nullptr) {
                    callbacks_.on_output_config(callbacks_.user_data, &config);
                }
                return MD_OK;
            }
            case MD_OP_RETIRE_BUFFERS: {
                std::uint64_t generation = 0U;
                if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) !=
                        0 ||
                    !pool_offered_ || retire_pending_ || generation != pool_generation_) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "invalid retire generation"));
                }
                retire_pending_ = true;
                if (callbacks_.on_retire_buffers != nullptr) {
                    callbacks_.on_retire_buffers(callbacks_.user_data, generation);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_POINTER_ENTER: {
                md_pointer_enter_t event;
                if (md_proto_decode_pointer_enter(packet->payload, packet->payload_size,
                                                  &event) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed pointer enter"));
                }
                if (callbacks_.on_pointer_enter != nullptr) {
                    callbacks_.on_pointer_enter(callbacks_.user_data, &event);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_POINTER_LEAVE: {
                std::uint64_t timestamp = 0U;
                if (md_proto_decode_pointer_leave(packet->payload, packet->payload_size,
                                                  &timestamp) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed pointer leave"));
                }
                if (callbacks_.on_pointer_leave != nullptr) {
                    callbacks_.on_pointer_leave(callbacks_.user_data, timestamp);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_POINTER_MOTION: {
                md_pointer_motion_t event;
                if (md_proto_decode_pointer_motion(packet->payload, packet->payload_size,
                                                   &event) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed pointer motion"));
                }
                if (callbacks_.on_pointer_motion != nullptr) {
                    callbacks_.on_pointer_motion(callbacks_.user_data, &event);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_POINTER_BUTTON: {
                md_pointer_button_t event;
                if (md_proto_decode_pointer_button(packet->payload, packet->payload_size,
                                                   &event) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed pointer button"));
                }
                if (callbacks_.on_pointer_button != nullptr) {
                    callbacks_.on_pointer_button(callbacks_.user_data, &event);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_POINTER_AXIS: {
                md_pointer_axis_t event;
                if (md_proto_decode_pointer_axis(packet->payload, packet->payload_size,
                                                 &event) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed pointer axis"));
                }
                if (callbacks_.on_pointer_axis != nullptr) {
                    callbacks_.on_pointer_axis(callbacks_.user_data, &event);
                }
                return MD_OK;
            }
            case MD_OP_PRODUCER_WINDOW_STATE: {
                std::uint32_t flags = 0U;
                if (md_proto_decode_window_state(packet->payload, packet->payload_size,
                                                 &flags) != 0) {
                    return static_cast<md_result_t>(
                        fail_producer(MD_ERR_PROTOCOL, "malformed window state"));
                }
                if (callbacks_.on_window_state != nullptr) {
                    callbacks_.on_window_state(callbacks_.user_data, flags);
                }
                return MD_OK;
            }
            default:
                if ((packet->flags & MD_PACKET_OPTIONAL) != 0U) {
                    return MD_OK;
                }
                return static_cast<md_result_t>(
                    fail_producer(MD_ERR_PROTOCOL, "unknown required producer opcode"));
        }
    }

    md_producer_callbacks_t callbacks_;
    std::uint64_t producer_id_;
    std::uint64_t output_id_;
    std::string stable_output_id_;
    std::string kind_;
    md_producer_info_t info_;
    std::vector<md_format_cap_t> formats_;
    bool pool_offered_;
    bool retire_pending_;
    bool gpu_bound_;
    std::uint64_t pool_generation_;
    std::uint32_t pool_buffer_count_;
};


/*
 * C ABI entry points below are thin wrappers over the C++ session object; null
 * handles map to documented errors instead of crashing.
 */

extern "C" md_producer_t* md_producer_new(const md_producer_callbacks_t* const callbacks) {
    try {
        return new md_producer(callbacks);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

extern "C" void md_producer_free(md_producer_t* const producer) {
    delete producer;
}

extern "C" md_result_t md_producer_begin_connect(
    md_producer_t* const producer, const char* const socket_path, const char* const client_name,
    const char* const client_version, const md_producer_info_t* const info) {
    return producer == nullptr ? MD_ERR_STATE
                               : producer->begin_path_connection(socket_path, client_name,
                                                                 client_version, info);
}

extern "C" md_result_t md_producer_begin_connected_fd(
    md_producer_t* const producer, const std::int32_t connected_fd,
    const char* const client_name, const char* const client_version,
    const md_producer_info_t* const info) {
    return producer == nullptr ? MD_ERR_STATE
                               : producer->begin_adopted_connection(connected_fd, client_name,
                                                                    client_version, info);
}

extern "C" int32_t md_producer_advance_handshake(md_producer_t* const producer) {
    return producer == nullptr ? MD_ERR_STATE : producer->advance_handshake();
}

extern "C" md_result_t md_producer_connect(
    md_producer_t* const producer, const char* const socket_path, const char* const client_name,
    const char* const client_version, const md_producer_info_t* const info,
    const std::int32_t timeout_ms) {
    return producer == nullptr
               ? MD_ERR_STATE
               : producer->connect_path(socket_path, client_name, client_version, info, timeout_ms);
}

extern "C" void md_producer_close(md_producer_t* const producer) {
    if (producer != nullptr) {
        producer->close_producer();
    }
}

extern "C" std::int32_t md_producer_get_fd(const md_producer_t* const producer) {
    return producer == nullptr ? mirage::kInvalidFd : producer->fd();
}

extern "C" md_connection_state_t
md_producer_connection_state(const md_producer_t* const producer) {
    return producer == nullptr ? MD_CONNECTION_DEAD : producer->connection_state();
}

extern "C" md_handshake_state_t
md_producer_handshake_state(const md_producer_t* const producer) {
    return producer == nullptr ? MD_HANDSHAKE_IDLE : producer->handshake_state();
}

extern "C" std::uint8_t md_producer_wants_writable(const md_producer_t* const producer) {
    return producer != nullptr && producer->wants_writable() ? UINT8_C(1) : UINT8_C(0);
}

extern "C" md_result_t md_producer_handle_writable(md_producer_t* const producer) {
    return producer == nullptr ? MD_ERR_STATE : producer->handle_writable();
}

extern "C" std::int32_t md_producer_dispatch(md_producer_t* const producer) {
    return producer == nullptr ? MD_ERR_STATE : producer->dispatch();
}

extern "C" md_result_t md_producer_offer_buffers(md_producer_t* const producer,
                                                  const md_buffer_pool_t* const pool) {
    return producer == nullptr ? MD_ERR_STATE : producer->offer_buffers(pool);
}

extern "C" md_result_t md_producer_bind_gpu(
    md_producer_t* const producer, const md_producer_gpu_info_t* const gpu) {
    return producer == nullptr ? MD_ERR_STATE : producer->bind_gpu(gpu);
}

extern "C" md_result_t md_producer_set_config(md_producer_t* const producer,
                                               const md_display_config_t* const config) {
    return producer == nullptr ? MD_ERR_STATE : producer->set_config(config);
}

extern "C" md_result_t md_producer_submit_frame(
    md_producer_t* const producer, const std::uint64_t generation,
    const std::uint32_t buffer_index, const std::uint64_t sequence,
    const std::int32_t acquire_sync_fd, const std::int32_t release_syncobj_fd) {
    if (producer == nullptr) {
        std::array<std::int32_t, 2U> fds{acquire_sync_fd, release_syncobj_fd};
        md_close_fds(fds.data(), fds.size());
        return MD_ERR_STATE;
    }
    return producer->submit_frame(generation, buffer_index, sequence, acquire_sync_fd,
                                  release_syncobj_fd);
}

extern "C" md_result_t md_producer_retire_done(md_producer_t* const producer,
                                                const std::uint64_t generation) {
    return producer == nullptr ? MD_ERR_STATE : producer->retire_done(generation);
}
