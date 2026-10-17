#include "mirage_display.h"

#include "codec.hpp"
#include "common/handshake.hpp"
#include "common/util.hpp"
#include "protocol.hpp"
#include "sync_fanout.h"

#include <array>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

/*
 * Consumer-side library (include/mirage_display.h): owns one display session,
 * drives the shared handshake state machine, dispatches broker packets to
 * borrowed callbacks, and tracks the active buffer pool.
 *
 * Threading: all public entry points except md_display_signal_release_syncobj
 * and md_display_release_after_sync_file must run on the display event thread.
 * A deferred unbind (md_display_defer_unbind / md_display_finish_unbind) lets a
 * host render thread destroy GPU references while this library keeps holding the
 * pool descriptors until finish_unbind runs on the event thread.
 */

struct md_display final : mirage::ClientSession {
    explicit md_display(const md_display_callbacks_t* callbacks)
        : ClientSession(MD_CLIENT_ROLE_DISPLAY, 0U),
          output_id_(0U),
          pool_active_(false),
          in_unbind_callback_(false),
          unbind_deferred_(false),
          pending_unbind_generation_(0U) {
        std::memset(&callbacks_, 0, sizeof(callbacks_));
        std::memset(&output_, 0, sizeof(output_));
        std::memset(&caps_, 0, sizeof(caps_));
        if (callbacks != nullptr) {
            callbacks_ = *callbacks;
        }
        md_init_pool(&pool_);
    }

    ~md_display() override {
        close_display();
        clear_connection_data();
    }

    md_result_t configure_connection(const char* socket_path, const char* client_name,
                                     const char* client_version,
                                     const md_output_info_t* output,
                                     const md_consumer_caps_t* caps) {
        if (!valid_output(output) || !valid_caps(caps)) {
            return MD_ERR_INVALID;
        }

        try {
            std::string next_stable_id(output->stable_id);
            std::string next_output_name(output->name);
            std::vector<md_format_cap_t> next_formats;
            if (caps->format_count != 0U) {
                next_formats.assign(caps->formats, caps->formats + caps->format_count);
            }
            const md_result_t identity_result = set_identity(socket_path, client_name, client_version);
            if (identity_result != MD_OK) {
                return identity_result;
            }

            stable_id_ = std::move(next_stable_id);
            output_name_ = std::move(next_output_name);
            formats_ = std::move(next_formats);
        } catch (const std::bad_alloc&) {
            clear_connection_data();
            return MD_ERR_NOMEM;
        }

        output_ = *output;
        output_.stable_id = stable_id_.c_str();
        output_.name = output_name_.c_str();
        caps_ = *caps;
        caps_.formats = formats_.empty() ? nullptr : formats_.data();
        caps_.features |= MD_FEATURE_EXPLICIT_SYNC;
        set_advertised_features(caps_.features);
        return MD_OK;
    }


/*
 * Starts a new connection over a pathname or @-prefixed abstract AF_UNIX
 * socket.  The display must be DISCONNECTED; identity strings and the output/caps
 * structs are copied into session-owned storage before the shared handshake
 * begins.
 */

    md_result_t begin_path_connection(const char* socket_path, const char* client_name,
                                      const char* client_version,
                                      const md_output_info_t* output,
                                      const md_consumer_caps_t* caps) {
        if (connection_state() != MD_CONNECTION_DISCONNECTED) {
            return MD_ERR_STATE;
        }
        const md_result_t configuration_result =
            configure_connection(socket_path, client_name, client_version, output, caps);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        const md_result_t connection_result = begin_connect();
        if (connection_result != MD_OK) {
            return connection_result;
        }
        output_id_ = 0U;
        return MD_OK;
    }


/*
 * Adopts an already-connected SOCK_SEQPACKET descriptor for broker handoff,
 * socket activation, or tests that cannot create pathname sockets.  The
 * descriptor must already be nonblocking and CLOEXEC; ownership transfers to the
 * display on success.
 */

    md_result_t begin_adopted_connection(const std::int32_t connected_fd,
                                         const char* client_name,
                                         const char* client_version,
                                         const md_output_info_t* output,
                                         const md_consumer_caps_t* caps) {
        if (connected_fd == mirage::kInvalidFd ||
            connection_state() != MD_CONNECTION_DISCONNECTED) {
            return MD_ERR_STATE;
        }
        const md_result_t configuration_result =
            configure_connection("", client_name, client_version, output, caps);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        const md_result_t connection_result = begin_connected_fd(connected_fd);
        if (connection_result != MD_OK) {
            return connection_result;
        }
        output_id_ = 0U;
        return MD_OK;
    }


/*
 * Blocking convenience wrapper around configure_connection + connect(), aimed
 * at command-line tools and tests rather than event-loop integrations.
 */

    md_result_t connect_path(const char* socket_path, const char* client_name,
                             const char* client_version, const md_output_info_t* output,
                             const md_consumer_caps_t* caps, const std::int32_t timeout_ms) {
        const md_result_t configuration_result =
            configure_connection(socket_path, client_name, client_version, output, caps);
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        output_id_ = 0U;
        return connect(timeout_ms);
    }


/*
 * Closes the session and abandons the active pool without a release callback:
 * this is teardown, not a broker-requested UNBIND, so the host must already be
 * done with all pool descriptors.
 */

    void close_display() noexcept {
        close();
        abandon_pool(true);
        output_id_ = 0U;
    }

    [[nodiscard]] std::uint64_t output_id() const noexcept {
        return output_id_;
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
 * Marks the in-flight UNBIND as deferred so the host render thread can destroy
 * GPU references after on_buffers_releasing returns.  Valid only from inside that
 * callback; the library keeps holding the pool descriptors until finish_unbind()
 * completes the sequence on the event thread.
 */

    md_result_t defer_unbind() {
        if (!in_unbind_callback_ || pending_unbind_generation_ == 0U) {
            return MD_ERR_STATE;
        }
        unbind_deferred_ = true;
        return MD_OK;
    }

    [[nodiscard]] std::uint64_t pending_unbind_generation() const noexcept {
        return unbind_deferred_ ? pending_unbind_generation_ : 0U;
    }

    md_result_t finish_unbind(const std::uint64_t generation) {
        if (!unbind_deferred_) {
            return MD_ERR_STATE;
        }
        return complete_unbind(generation);
    }


/*
 * Drains every currently readable packet and dispatches it to the borrowed
 * callbacks.  Returns the number of packets handled or a negative md_result_t.
 * A pending deferred unbind rejects further packets by design: the pool must be
 * released before the broker may send a new BIND_BUFFERS.
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
                return fail_session(md_map_io_error(receive_result), "session receive failed");
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
 * Reports changed output geometry to the broker, which may renegotiate and
 * start a fresh OUTPUT_CONFIG / BIND_BUFFERS cycle.
 */

    md_result_t update_output(const md_output_info_t* output) {
        if (!valid_output(output)) {
            return MD_ERR_INVALID;
        }
        std::array<std::uint8_t, 128U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_update_output(&writer, output) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_UPDATE_OUTPUT, 0U, payload.data(), writer.size);
    }


/*
 * Pointer message encoders share one shape: validate enum inputs, encode the
 * fixed wire payload into a stack buffer, then queue the message (coalescing
 * trailing pointer motion).  Coordinates are output physical pixels with a
 * top-left origin; timestamps are monotonic microseconds.
 */

    md_result_t send_pointer_enter(const float x, const float y, const std::uint64_t timestamp_us) {
        std::array<std::uint8_t, 32U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_pointer_enter(&writer, x, y, timestamp_us) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_POINTER_ENTER, 0U, payload.data(), writer.size);
    }

    md_result_t send_pointer_leave(const std::uint64_t timestamp_us) {
        std::array<std::uint8_t, 16U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_pointer_leave(&writer, timestamp_us) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_POINTER_LEAVE, 0U, payload.data(), writer.size);
    }

    md_result_t send_pointer_motion(const float x, const float y,
                                    const std::uint64_t timestamp_us,
                                    const std::uint32_t modifiers) {
        std::array<std::uint8_t, 32U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_pointer_motion(&writer, x, y, timestamp_us, modifiers) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_POINTER_MOTION, 0U, payload.data(), writer.size);
    }

    md_result_t send_pointer_button(const float x, const float y, const std::uint32_t button,
                                    const md_button_state_t state,
                                    const std::uint64_t timestamp_us,
                                    const std::uint32_t modifiers) {
        if (state != MD_BUTTON_RELEASED && state != MD_BUTTON_PRESSED) {
            return MD_ERR_INVALID;
        }
        std::array<std::uint8_t, 40U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_pointer_button(&writer, x, y, button, state, timestamp_us,
                                           modifiers) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_POINTER_BUTTON, 0U, payload.data(), writer.size);
    }

    md_result_t send_pointer_axis(const float x, const float y, const float delta_x,
                                  const float delta_y, const md_axis_source_t source,
                                  const std::uint64_t timestamp_us,
                                  const std::uint32_t modifiers) {
        if (source < MD_AXIS_WHEEL || source > MD_AXIS_CONTINUOUS) {
            return MD_ERR_INVALID;
        }
        std::array<std::uint8_t, 48U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_pointer_axis(&writer, x, y, delta_x, delta_y, source,
                                         timestamp_us, modifiers) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_POINTER_AXIS, 0U, payload.data(), writer.size);
    }

    md_result_t send_window_state(const std::uint32_t flags) {
        std::array<std::uint8_t, 8U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_u32(&writer, flags) != 0) {
            return MD_ERR_INVALID;
        }
        return queue_message(MD_OP_WINDOW_STATE, 0U, payload.data(), writer.size);
    }

protected:

/*
 * Role hooks for the shared ClientSession handshake: a display registers an
 * output, waits for OUTPUT_ACCEPTED, then sends CONSUMER_CAPS.
 */

    [[nodiscard]] std::uint16_t register_opcode() const noexcept override {
        return MD_OP_REGISTER_OUTPUT;
    }

    [[nodiscard]] std::uint16_t accepted_opcode() const noexcept override {
        return MD_OP_OUTPUT_ACCEPTED;
    }

    [[nodiscard]] std::uint16_t caps_opcode() const noexcept override {
        return MD_OP_CONSUMER_CAPS;
    }

    std::int32_t encode_registration(md_writer_t* const writer) override {
        return md_proto_encode_register_output(writer, &output_);
    }

    std::int32_t encode_caps(md_writer_t* const writer) override {
        return md_proto_encode_consumer_caps(writer, &caps_);
    }

    std::int32_t apply_accepted(const md_packet_t& packet) override {
        std::uint64_t accepted_output_id = 0U;
        if (md_proto_decode_output_accepted(packet.payload, packet.payload_size,
                                            &accepted_output_id) != 0 ||
            accepted_output_id == 0U) {
            return MD_ERR_PROTOCOL;
        }
        output_id_ = accepted_output_id;
        return MD_OK;
    }

    void on_ready() override {
        if (callbacks_.on_connected != nullptr) {
            callbacks_.on_connected(callbacks_.user_data, output_id_);
        }
    }

    void on_disconnected(const md_result_t reason, const char* const message) override {
        abandon_pool(true);
        if (callbacks_.on_disconnected != nullptr) {
            callbacks_.on_disconnected(callbacks_.user_data, reason, message);
        }
    }

private:

/*
 * Requires the documented NUL-terminated strings and nonzero geometry so a
 * malformed caller input can never reach the wire encoder.
 */

    static bool valid_output(const md_output_info_t* const output) {
        return output != nullptr && output->stable_id != nullptr && output->name != nullptr &&
               output->physical_width != 0U && output->physical_height != 0U &&
               output->logical_width != 0U && output->logical_height != 0U &&
               output->scale_120 != 0U && output->transform <= MD_TRANSFORM_FLIPPED_270;
    }

    static bool valid_caps(const md_consumer_caps_t* const caps) {
        if (caps == nullptr || caps->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
            (caps->format_count != 0U && caps->formats == nullptr)) {
            return false;
        }
        for (std::uint32_t index = 0U; index < caps->format_count; ++index) {
            if (caps->formats[index].plane_count == 0U ||
                caps->formats[index].plane_count > MIRAGE_DISPLAY_MAX_PLANES) {
                return false;
            }
        }
        return true;
    }

    void clear_connection_data() noexcept {
        clear_identity();
        stable_id_.clear();
        output_name_.clear();
        formats_.clear();
        std::memset(&output_, 0, sizeof(output_));
        std::memset(&caps_, 0, sizeof(caps_));
    }


/*
 * Releases the active pool: optionally notifies the host via
 * on_buffers_releasing, then closes every pool descriptor.  Used both for
 * broker-requested UNBIND and for connection teardown.
 */

    void release_pool(const bool notify) {
        if (!pool_active_) {
            return;
        }
        if (notify && callbacks_.on_buffers_releasing != nullptr) {
            callbacks_.on_buffers_releasing(callbacks_.user_data, &pool_);
        }
        md_close_pool(&pool_);
        pool_active_ = false;
    }


/*
 * Unconditional teardown of the active pool state.  During a deferred unbind
 * the release callback is skipped because finish_unbind() will complete the
 * broker-requested sequence instead.
 */

    void abandon_pool(const bool notify_if_needed) {
        const bool notify = notify_if_needed && pending_unbind_generation_ == 0U;
        release_pool(notify);
        in_unbind_callback_ = false;
        unbind_deferred_ = false;
        pending_unbind_generation_ = 0U;
    }

    std::int32_t fail_session(const md_result_t reason, const char* const message) {
        return disconnect(reason, message);
    }

    static bool coalescible(const std::uint16_t opcode) {
        return opcode == MD_OP_POINTER_MOTION || opcode == MD_OP_UPDATE_OUTPUT ||
               opcode == MD_OP_WINDOW_STATE;
    }

    md_result_t flush_messages() {
        const std::int32_t result = flush_outbox();
        if (result == MD_ERR_WOULD_BLOCK) {
            return MD_ERR_WOULD_BLOCK;
        }
        if (result < 0) {
            return static_cast<md_result_t>(
                fail_session(md_map_io_error(result), "outbox send failed"));
        }
        return MD_OK;
    }

    md_result_t queue_message(const std::uint16_t opcode, const std::uint16_t flags,
                              const std::uint8_t* const payload,
                              const std::size_t payload_size) {
        if (connection_state() != MD_CONNECTION_READY) {
            return MD_ERR_STATE;
        }
        const std::int32_t result =
            send_or_queue(opcode, flags, payload, payload_size, nullptr, 0U, coalescible(opcode));
        if (result == MD_OK || result == MD_ERR_WOULD_BLOCK || result == MD_ERR_NOMEM ||
            result == MD_ERR_INVALID) {
            return static_cast<md_result_t>(result);
        }
        return static_cast<md_result_t>(
            fail_session(md_map_io_error(result), "request send failed"));
    }


/*
 * Completes a broker-requested UNBIND: closes the pool descriptors without
 * re-notifying and sends UNBIND_DONE so the producer may retire this generation
 * and create the next one.
 */

    md_result_t complete_unbind(const std::uint64_t generation) {
        if (generation == 0U || !pool_active_ || pending_unbind_generation_ != generation) {
            return MD_ERR_STATE;
        }
        release_pool(false);
        in_unbind_callback_ = false;
        unbind_deferred_ = false;
        pending_unbind_generation_ = 0U;

        std::array<std::uint8_t, 8U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (md_proto_encode_u64(&writer, generation) != 0) {
            return MD_ERR_PROTOCOL;
        }
        return queue_message(MD_OP_UNBIND_DONE, 0U, payload.data(), writer.size);
    }

    /*
     * Rejecting a frame must consume both FDs: acquire is closed and release
     * is signaled so the producer never waits on an unreturned sync object.
     */
    md_result_t discard_frame_fds(md_packet_t* const packet) {
        if (packet->fd_count >= 1U && packet->fds[0] != mirage::kInvalidFd) {
            md_close_fds(packet->fds, 1U);
        }
        if (packet->fd_count >= 2U && packet->fds[1] != mirage::kInvalidFd) {
            const md_result_t signal_result = md_display_signal_release_syncobj_on_node(
                packet->fds[1], output_.drm_render_major, output_.drm_render_minor);
            packet->fds[1] = mirage::kInvalidFd;
            if (signal_result != MD_OK) {
                return signal_result;
            }
        }
        return MD_OK;
    }


/*
 * Dispatches one READY-state packet.  Malformed or stale packets fail the
 * session with MD_ERR_PROTOCOL; OPTIONAL-flagged unknown opcodes are ignored.
 * Every packet descriptor is resolved before return: moved into the pool or frame,
 * or consumed by discard_frame_fds.
 */

    md_result_t process_packet(md_packet_t* const packet) {
        if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
            packet->minor != selected_minor()) {
            return static_cast<md_result_t>(
                fail_session(MD_ERR_PROTOCOL, "wire version changed during session"));
        }
        if (packet->opcode == MD_OP_ERROR) {
            if (packet->fd_count != 0U) {
                return static_cast<md_result_t>(
                    fail_session(MD_ERR_PROTOCOL, "error has FDs"));
            }
            md_proto_error_t error;
            if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
                return static_cast<md_result_t>(
                    fail_session(MD_ERR_PROTOCOL, "malformed error packet"));
            }
            const md_result_t reason = error.fatal != 0U ? MD_ERR_PROTOCOL : MD_ERR_IO;
            const md_result_t result = error.fatal != 0U
                                           ? static_cast<md_result_t>(
                                                 fail_session(reason, error.message))
                                           : MD_OK;
            md_proto_error_clear(&error);
            return result;
        }

        if (pending_unbind_generation_ != 0U) {
            return static_cast<md_result_t>(fail_session(
                MD_ERR_PROTOCOL, "packet received before deferred unbind completed"));
        }

        switch (packet->opcode) {
            case MD_OP_BIND_BUFFERS: {
                if (pool_active_) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "pool rebound without unbind"));
                }
                md_buffer_pool_t pool;
                if (md_proto_decode_bind_buffers(packet->payload, packet->payload_size, &pool) != 0) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "malformed buffer pool"));
                }
                const std::size_t expected_fds =
                    static_cast<std::size_t>(pool.buffer_count) * pool.plane_count;
                if (packet->fd_count != expected_fds || pool.generation == 0U) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "buffer pool FD count mismatch"));
                }
                std::size_t index = 0U;
                for (std::uint32_t buffer = 0U; buffer < pool.buffer_count; ++buffer) {
                    for (std::uint32_t plane = 0U; plane < pool.plane_count; ++plane) {
                        pool.planes[buffer][plane].fd = packet->fds[index];
                        packet->fds[index] = mirage::kInvalidFd;
                        ++index;
                    }
                }
                packet->fd_count = 0U;
                pool_ = pool;
                pool_active_ = true;
                if (callbacks_.on_buffers_ready != nullptr) {
                    callbacks_.on_buffers_ready(callbacks_.user_data, &pool_);
                }
                return MD_OK;
            }
            case MD_OP_SET_CONFIG: {
                if (packet->fd_count != 0U) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "config has FDs"));
                }
                md_display_config_t config;
                if (md_proto_decode_config(packet->payload, packet->payload_size, &config) != 0) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "malformed display config"));
                }
                if (callbacks_.on_config != nullptr) {
                    callbacks_.on_config(callbacks_.user_data, &config);
                }
                return MD_OK;
            }

/*
 * A valid frame transfers both sync descriptors to the callback.  Stale
 * generations, out-of-range indices, and missing FDs are rejected through
 * discard_frame_fds so the producer never waits on an unreturned sync object.
 */

            case MD_OP_FRAME_READY: {
                if (packet->fd_count != 2U) {
                    const md_result_t discard_result = discard_frame_fds(packet);
                    if (discard_result != MD_OK) {
                        return static_cast<md_result_t>(
                            fail_session(discard_result, "failed to release rejected frame"));
                    }
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "frame FD count mismatch"));
                }
                md_frame_t frame;
                if (md_proto_decode_frame(packet->payload, packet->payload_size, &frame) != 0) {
                    const md_result_t discard_result = discard_frame_fds(packet);
                    if (discard_result != MD_OK) {
                        return static_cast<md_result_t>(
                            fail_session(discard_result, "failed to release malformed frame"));
                    }
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "malformed frame"));
                }
                if (!pool_active_ || frame.buffer_generation != pool_.generation) {
                    const md_result_t discard_result = discard_frame_fds(packet);
                    if (discard_result != MD_OK) {
                        return static_cast<md_result_t>(
                            fail_session(discard_result, "failed to release stale frame"));
                    }
                    return MD_OK;
                }
                if (frame.buffer_index >= pool_.buffer_count) {
                    const md_result_t discard_result = discard_frame_fds(packet);
                    if (discard_result != MD_OK) {
                        return static_cast<md_result_t>(
                            fail_session(discard_result, "failed to release invalid frame"));
                    }
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "frame buffer index out of range"));
                }
                frame.acquire_sync_fd = packet->fds[0];
                frame.release_syncobj_fd = packet->fds[1];
                packet->fds[0] = mirage::kInvalidFd;
                packet->fds[1] = mirage::kInvalidFd;
                packet->fd_count = 0U;
                if (callbacks_.on_frame != nullptr) {
                    callbacks_.on_frame(callbacks_.user_data, &frame);
                    return MD_OK;
                }
                md_close_fds(&frame.acquire_sync_fd, 1U);
                const md_result_t signal_result = md_display_signal_release_syncobj_on_node(
                    frame.release_syncobj_fd, output_.drm_render_major, output_.drm_render_minor);
                frame.release_syncobj_fd = mirage::kInvalidFd;
                if (signal_result != MD_OK) {
                    return static_cast<md_result_t>(
                        fail_session(signal_result, "failed to release unhandled frame"));
                }
                return MD_OK;
            }

/*
 * Broker-requested pool replacement.  The release callback runs synchronously
 * unless the adapter calls defer_unbind(); the deferred path completes later on
 * the event thread and sends UNBIND_DONE.
 */

            case MD_OP_UNBIND: {
                if (packet->fd_count != 0U) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "unbind has FDs"));
                }
                std::uint64_t generation = 0U;
                if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
                    !pool_active_ || generation != pool_.generation) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "invalid unbind generation"));
                }
                if (pending_unbind_generation_ != 0U) {
                    return static_cast<md_result_t>(
                        fail_session(MD_ERR_PROTOCOL, "overlapping unbind"));
                }
                pending_unbind_generation_ = generation;
                in_unbind_callback_ = true;
                unbind_deferred_ = false;
                if (callbacks_.on_buffers_releasing != nullptr) {
                    callbacks_.on_buffers_releasing(callbacks_.user_data, &pool_);
                }
                in_unbind_callback_ = false;
                if (unbind_deferred_) {
                    return MD_OK;
                }
                return complete_unbind(generation);
            }
            default:
                if ((packet->flags & MD_PACKET_OPTIONAL) != 0U) {
                    return MD_OK;
                }
                return static_cast<md_result_t>(
                    fail_session(MD_ERR_PROTOCOL, "unknown required opcode"));
        }
    }

    md_display_callbacks_t callbacks_;
    std::uint64_t output_id_;
    std::string stable_id_;
    std::string output_name_;
    md_output_info_t output_;
    md_consumer_caps_t caps_;
    std::vector<md_format_cap_t> formats_;
    bool pool_active_;
    bool in_unbind_callback_;
    bool unbind_deferred_;
    std::uint64_t pending_unbind_generation_;
    md_buffer_pool_t pool_;
};


/*
 * C ABI entry points below are thin wrappers over the C++ session object; null
 * handles map to documented errors instead of crashing.
 */

extern "C" md_display_t* md_display_new(const md_display_callbacks_t* const callbacks) {
    try {
        return new md_display(callbacks);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

extern "C" void md_display_free(md_display_t* const display) {
    delete display;
}

extern "C" md_result_t md_display_begin_connect(
    md_display_t* const display, const char* const socket_path, const char* const client_name,
    const char* const client_version, const md_output_info_t* const output,
    const md_consumer_caps_t* const caps) {
    return display == nullptr ? MD_ERR_STATE
                              : display->begin_path_connection(socket_path, client_name,
                                                               client_version, output, caps);
}

extern "C" md_result_t md_display_begin_connected_fd(
    md_display_t* const display, const std::int32_t connected_fd, const char* const client_name,
    const char* const client_version, const md_output_info_t* const output,
    const md_consumer_caps_t* const caps) {
    return display == nullptr ? MD_ERR_STATE
                              : display->begin_adopted_connection(connected_fd, client_name,
                                                                  client_version, output, caps);
}

extern "C" int32_t md_display_advance_handshake(md_display_t* const display) {
    return display == nullptr ? MD_ERR_STATE : display->advance_handshake();
}

extern "C" md_result_t md_display_connect(
    md_display_t* const display, const char* const socket_path, const char* const client_name,
    const char* const client_version, const md_output_info_t* const output,
    const md_consumer_caps_t* const caps, const std::int32_t timeout_ms) {
    return display == nullptr
               ? MD_ERR_STATE
               : display->connect_path(socket_path, client_name, client_version, output, caps,
                                       timeout_ms);
}

extern "C" void md_display_close(md_display_t* const display) {
    if (display != nullptr) {
        display->close_display();
    }
}

extern "C" std::int32_t md_display_get_fd(const md_display_t* const display) {
    return display == nullptr ? mirage::kInvalidFd : display->fd();
}

extern "C" md_connection_state_t
md_display_connection_state(const md_display_t* const display) {
    return display == nullptr ? MD_CONNECTION_DEAD : display->connection_state();
}

extern "C" md_handshake_state_t
md_display_handshake_state(const md_display_t* const display) {
    return display == nullptr ? MD_HANDSHAKE_IDLE : display->handshake_state();
}

extern "C" std::uint64_t md_display_output_id(const md_display_t* const display) {
    return display == nullptr ? 0U : display->output_id();
}

extern "C" std::int32_t md_display_dispatch(md_display_t* const display) {
    return display == nullptr ? MD_ERR_STATE : display->dispatch();
}

extern "C" std::uint8_t md_display_wants_writable(const md_display_t* const display) {
    return display != nullptr && display->wants_writable() ? UINT8_C(1) : UINT8_C(0);
}

extern "C" md_result_t md_display_handle_writable(md_display_t* const display) {
    return display == nullptr ? MD_ERR_STATE : display->handle_writable();
}

extern "C" md_result_t md_display_defer_unbind(md_display_t* const display) {
    return display == nullptr ? MD_ERR_INVALID : display->defer_unbind();
}

extern "C" std::uint64_t
md_display_pending_unbind_generation(const md_display_t* const display) {
    return display == nullptr ? 0U : display->pending_unbind_generation();
}

extern "C" md_result_t md_display_finish_unbind(md_display_t* const display,
                                                 const std::uint64_t generation) {
    return display == nullptr ? MD_ERR_INVALID : display->finish_unbind(generation);
}

extern "C" md_result_t md_display_update_output(md_display_t* const display,
                                                 const md_output_info_t* const output) {
    return display == nullptr ? MD_ERR_INVALID : display->update_output(output);
}

extern "C" md_result_t md_display_send_pointer_enter(md_display_t* const display,
                                                      const float x, const float y,
                                                      const std::uint64_t timestamp_us) {
    return display == nullptr ? MD_ERR_INVALID : display->send_pointer_enter(x, y, timestamp_us);
}

extern "C" md_result_t md_display_send_pointer_leave(md_display_t* const display,
                                                      const std::uint64_t timestamp_us) {
    return display == nullptr ? MD_ERR_INVALID : display->send_pointer_leave(timestamp_us);
}

extern "C" md_result_t md_display_send_pointer_motion(md_display_t* const display,
                                                       const float x, const float y,
                                                       const std::uint64_t timestamp_us,
                                                       const std::uint32_t modifiers) {
    return display == nullptr ? MD_ERR_INVALID
                              : display->send_pointer_motion(x, y, timestamp_us, modifiers);
}

extern "C" md_result_t md_display_send_pointer_button(
    md_display_t* const display, const float x, const float y, const std::uint32_t button,
    const md_button_state_t state, const std::uint64_t timestamp_us,
    const std::uint32_t modifiers) {
    return display == nullptr ? MD_ERR_INVALID
                              : display->send_pointer_button(x, y, button, state, timestamp_us,
                                                             modifiers);
}

extern "C" md_result_t md_display_send_pointer_axis(
    md_display_t* const display, const float x, const float y, const float delta_x,
    const float delta_y, const md_axis_source_t source, const std::uint64_t timestamp_us,
    const std::uint32_t modifiers) {
    return display == nullptr ? MD_ERR_INVALID
                              : display->send_pointer_axis(x, y, delta_x, delta_y, source,
                                                           timestamp_us, modifiers);
}

extern "C" md_result_t md_display_send_window_state(md_display_t* const display,
                                                     const std::uint32_t flags) {
    return display == nullptr ? MD_ERR_INVALID : display->send_window_state(flags);
}
