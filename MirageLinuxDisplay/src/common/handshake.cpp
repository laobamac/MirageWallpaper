#include "handshake.hpp"

#include "net.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Implementation of the shared ClientSession transport and handshake.
 *
 * Handshake packets are staged in a fixed buffer; the outbox holds any
 * post-registration messages so SCM_RIGHTS ownership is preserved while the
 * socket is not writable.
 */

namespace mirage {

ClientSession::ClientSession(const std::uint32_t role, const std::uint64_t advertised_features)
    : connection_state_(MD_CONNECTION_DISCONNECTED),
      handshake_state_(MD_HANDSHAKE_IDLE),
      selected_minor_(0U),
      negotiated_features_(0U),
      next_serial_(1U),
      disconnected_notified_(false),
      role_(role),
      advertised_features_(advertised_features),
      handshake_opcode_(0U),
      handshake_serial_(0U),
      handshake_size_(0U),
      handshake_payload_{} {}

ClientSession::~ClientSession() {
    close();
}

md_result_t ClientSession::set_identity(const char* const socket_path, const char* const client_name,
                                        const char* const client_version) {
    if (socket_path == nullptr || client_name == nullptr || client_version == nullptr) {
        return MD_ERR_INVALID;
    }

    try {
        std::string next_socket_path(socket_path);
        std::string next_client_name(client_name);
        std::string next_client_version(client_version);
        socket_path_ = std::move(next_socket_path);
        client_name_ = std::move(next_client_name);
        client_version_ = std::move(next_client_version);
    } catch (const std::bad_alloc&) {
        return MD_ERR_NOMEM;
    }
    return MD_OK;
}

void ClientSession::clear_identity() noexcept {
    socket_path_.clear();
    client_name_.clear();
    client_version_.clear();
}

md_result_t ClientSession::configure_connected_fd(const std::int32_t fd) const {
    const std::int32_t status_flags = ::fcntl(fd, F_GETFL);
    const std::int32_t descriptor_flags = ::fcntl(fd, F_GETFD);
    if (status_flags == mirage::kInvalidFd || descriptor_flags == mirage::kInvalidFd ||
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return MD_ERR_IO;
    }
    return MD_OK;
}

md_result_t ClientSession::activate_handshake() {
    connection_state_ = MD_CONNECTION_HANDSHAKING;
    handshake_state_ = MD_HANDSHAKE_HELLO_SEND;
    disconnected_notified_ = false;
    selected_minor_ = 0U;
    negotiated_features_ = 0U;
    const std::int32_t result = prepare_handshake(MD_OP_HELLO);
    return result == MD_OK ? MD_OK
                           : static_cast<md_result_t>(disconnect(
                                 static_cast<md_result_t>(result), "cannot encode hello"));
}


/*
 * Creates a nonblocking CLOEXEC SOCK_SEQPACKET socket and starts connecting to
 * the configured pathname or @-prefixed abstract address.  A completed connect
 * activates the handshake immediately; an in-progress one is finished by
 * advance_handshake().
 */
md_result_t ClientSession::begin_connect() {
    if (connection_state_ != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }

    UniqueFd connecting_fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!connecting_fd.valid()) {
        return MD_ERR_IO;
    }

    sockaddr_un address;
    socklen_t address_length = 0U;
    const md_result_t address_result =
        md_fill_unix_address(socket_path_.c_str(), &address, &address_length);
    if (address_result != MD_OK) {
        return address_result;
    }

    const std::int32_t connect_result =
        ::connect(connecting_fd.get(), static_cast<const sockaddr*>(static_cast<const void*>(&address)),
                  address_length);
    if (connect_result == 0) {
        const md_result_t configuration_result = configure_connected_fd(connecting_fd.get());
        if (configuration_result != MD_OK) {
            return configuration_result;
        }
        fd_.reset(connecting_fd.release());
        return activate_handshake();
    }

    const std::int32_t connect_error = errno;
    if (connect_error != EINPROGRESS && connect_error != EAGAIN && connect_error != EALREADY) {
        return static_cast<md_result_t>(disconnect(MD_ERR_IO, std::strerror(connect_error)));
    }

    fd_.reset(connecting_fd.release());
    connection_state_ = MD_CONNECTION_CONNECTING;
    handshake_state_ = MD_HANDSHAKE_CONNECTING;
    disconnected_notified_ = false;
    selected_minor_ = 0U;
    negotiated_features_ = 0U;
    return MD_OK;
}


/*
 * Adopts a caller-provided connected descriptor (broker handoff, socket
 * activation, tests).  Forces O_NONBLOCK and FD_CLOEXEC before ownership
 * transfers to the session.
 */
md_result_t ClientSession::begin_connected_fd(const std::int32_t connected_fd) {
    if (connected_fd == mirage::kInvalidFd ||
        connection_state_ != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    const md_result_t configuration_result = configure_connected_fd(connected_fd);
    if (configuration_result != MD_OK) {
        return configuration_result;
    }
    fd_.reset(connected_fd);
    return activate_handshake();
}

std::int32_t ClientSession::prepare_handshake(const std::uint16_t opcode) {
    md_writer_t writer;
    md_writer_init(&writer, handshake_payload_.data(), handshake_payload_.size());
    std::int32_t result = MD_ERR_INVALID;
    switch (opcode) {
        case MD_OP_HELLO:
            result = md_proto_encode_hello(&writer, role_, client_name_.c_str(),
                                           client_version_.c_str(), advertised_features_);
            break;
        case MD_OP_REGISTER_OUTPUT:
        case MD_OP_REGISTER_PRODUCER:
            result = encode_registration(&writer);
            break;
        case MD_OP_CONSUMER_CAPS:
            result = encode_caps(&writer);
            break;
        default:
            return MD_ERR_INVALID;
    }
    if (result != 0) {
        return result == -ENOMEM ? MD_ERR_NOMEM : MD_ERR_INVALID;
    }
    handshake_opcode_ = opcode;
    handshake_serial_ = next_serial();
    handshake_size_ = writer.size;
    return MD_OK;
}

std::int32_t ClientSession::send_handshake() {
    const std::uint16_t minor = handshake_opcode_ == MD_OP_HELLO ? 0U : selected_minor_;
    const std::int32_t result = md_codec_send(fd_.get(), minor, handshake_opcode_, 0U,
                                              handshake_serial_, handshake_payload_.data(),
                                              handshake_size_, nullptr, 0U);
    if (result == 1) {
        return MD_HANDSHAKE_NEED_WRITE;
    }
    if (result < 0) {
        return disconnect(md_map_io_error(result), "handshake send failed");
    }
    return MD_HANDSHAKE_PROGRESS;
}

std::int32_t ClientSession::receive_handshake(const std::uint16_t expected,
                                              md_packet_t* const packet) {
    const std::int32_t result = md_codec_recv(fd_.get(), packet);
    if (result == 0) {
        return MD_HANDSHAKE_NEED_READ;
    }
    if (result < 0) {
        return disconnect(md_map_io_error(result), "handshake receive failed");
    }
    if (packet->fd_count != 0U || packet->minor > MIRAGE_DISPLAY_PROTOCOL_MINOR) {
        md_packet_close_fds(packet);
        return disconnect(MD_ERR_PROTOCOL, "invalid handshake packet");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        const std::int32_t decode_result =
            md_proto_decode_error(packet->payload, packet->payload_size, &error);
        if (decode_result != 0) {
            return disconnect(MD_ERR_PROTOCOL, "malformed error packet");
        }
        const md_result_t reason = error.fatal != 0U ? MD_ERR_PROTOCOL : MD_ERR_IO;
        const std::int32_t disconnect_result = disconnect(reason, error.message);
        md_proto_error_clear(&error);
        return disconnect_result;
    }
    if (packet->opcode != expected) {
        return disconnect(MD_ERR_PROTOCOL, "unexpected handshake opcode");
    }
    return MD_HANDSHAKE_PROGRESS;
}


/*
 * Advances the nonblocking handshake state machine: CONNECTING -> HELLO_SEND
 * -> WELCOME_WAIT -> REGISTER_SEND -> ACCEPT_WAIT -> [CAPS_SEND] -> READY.
 * Returns an MD_HANDSHAKE_* progress value or a negative md_result_t.  The
 * welcome negotiation picks the minor version and intersects features; the
 * consumer-only caps step is skipped when caps_opcode() is zero.
 */
std::int32_t ClientSession::advance_handshake() {
    if (!fd_.valid()) {
        return MD_ERR_STATE;
    }

    switch (handshake_state_) {
        case MD_HANDSHAKE_CONNECTING: {
            std::int32_t socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                             &socket_error_size) != 0) {
                return disconnect(MD_ERR_IO, "getsockopt(SO_ERROR) failed");
            }
            if (socket_error == EINPROGRESS || socket_error == EALREADY) {
                return MD_HANDSHAKE_NEED_WRITE;
            }
            if (socket_error != 0) {
                return disconnect(MD_ERR_IO, std::strerror(socket_error));
            }
            const md_result_t configuration_result = configure_connected_fd(fd_.get());
            if (configuration_result != MD_OK) {
                return disconnect(configuration_result, "cannot configure socket");
            }
            return activate_handshake() == MD_OK
                       ? static_cast<std::int32_t>(MD_HANDSHAKE_PROGRESS)
                       : static_cast<std::int32_t>(MD_ERR_INVALID);
        }
        case MD_HANDSHAKE_HELLO_SEND: {
            const std::int32_t result = send_handshake();
            if (result == MD_HANDSHAKE_PROGRESS) {
                handshake_state_ = MD_HANDSHAKE_WELCOME_WAIT;
            }
            return result;
        }
        case MD_HANDSHAKE_WELCOME_WAIT: {
            md_packet_t packet;
            const std::int32_t receive_result = receive_handshake(MD_OP_WELCOME, &packet);
            if (receive_result != MD_HANDSHAKE_PROGRESS) {
                return receive_result;
            }
            md_proto_welcome_t welcome;
            const std::int32_t decode_result =
                md_proto_decode_welcome(packet.payload, packet.payload_size, &welcome);
            if (decode_result != 0 || welcome.selected_minor != MIRAGE_DISPLAY_PROTOCOL_MINOR ||
                (welcome.features & MD_FEATURE_EXPLICIT_SYNC) == 0U) {
                md_proto_welcome_clear(&welcome);
                return disconnect(MD_ERR_PROTOCOL, "unsupported welcome packet");
            }
            selected_minor_ = welcome.selected_minor;
            negotiated_features_ = advertised_features_ & welcome.features;
            md_proto_welcome_clear(&welcome);
            const std::int32_t preparation_result = prepare_handshake(register_opcode());
            if (preparation_result != MD_OK) {
                return disconnect(static_cast<md_result_t>(preparation_result),
                                  "cannot encode registration");
            }
            handshake_state_ = MD_HANDSHAKE_REGISTER_SEND;
            return MD_HANDSHAKE_PROGRESS;
        }
        case MD_HANDSHAKE_REGISTER_SEND: {
            const std::int32_t result = send_handshake();
            if (result == MD_HANDSHAKE_PROGRESS) {
                handshake_state_ = MD_HANDSHAKE_ACCEPT_WAIT;
            }
            return result;
        }
        case MD_HANDSHAKE_ACCEPT_WAIT: {
            md_packet_t packet;
            const std::int32_t receive_result = receive_handshake(accepted_opcode(), &packet);
            if (receive_result != MD_HANDSHAKE_PROGRESS) {
                return receive_result;
            }
            if (apply_accepted(packet) != MD_OK) {
                return disconnect(MD_ERR_PROTOCOL, "malformed accepted packet");
            }
            if (caps_opcode() != 0U) {
                const std::int32_t preparation_result = prepare_handshake(caps_opcode());
                if (preparation_result != MD_OK) {
                    return disconnect(static_cast<md_result_t>(preparation_result),
                                      "cannot encode caps");
                }
                handshake_state_ = MD_HANDSHAKE_CAPS_SEND;
                return MD_HANDSHAKE_PROGRESS;
            }
            handshake_state_ = MD_HANDSHAKE_READY;
            connection_state_ = MD_CONNECTION_READY;
            on_ready();
            return MD_HANDSHAKE_DONE;
        }
        case MD_HANDSHAKE_CAPS_SEND: {
            const std::int32_t result = send_handshake();
            if (result != MD_HANDSHAKE_PROGRESS) {
                return result;
            }
            handshake_state_ = MD_HANDSHAKE_READY;
            connection_state_ = MD_CONNECTION_READY;
            on_ready();
            return MD_HANDSHAKE_DONE;
        }
        case MD_HANDSHAKE_READY:
            return MD_HANDSHAKE_DONE;
        default:
            return MD_ERR_STATE;
    }
}


/*
 * Blocking handshake for command-line tools and tests: drives begin_connect +
 * advance_handshake with poll(2), honoring timeout_ms (negative blocks
 * indefinitely).  Any failure closes the session and returns the error.
 */
md_result_t ClientSession::connect(const std::int32_t timeout_ms) {
    md_result_t result = begin_connect();
    if (result != MD_OK) {
        return result;
    }
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    for (;;) {
        const std::int32_t handshake_result = advance_handshake();
        if (handshake_result == MD_HANDSHAKE_DONE) {
            return MD_OK;
        }
        if (handshake_result < 0) {
            return static_cast<md_result_t>(handshake_result);
        }
        if (handshake_result == MD_HANDSHAKE_PROGRESS) {
            continue;
        }

        std::int32_t wait_ms = timeout_ms;
        if (timeout_ms >= 0) {
            const std::int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed >= timeout_ms) {
                close();
                return MD_ERR_IO;
            }
            wait_ms = timeout_ms - static_cast<std::int32_t>(elapsed);
        }
        pollfd descriptor;
        descriptor.fd = fd_.get();
        descriptor.events = handshake_result == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN;
        descriptor.revents = 0;
        std::int32_t poll_result = 0;
        do {
            poll_result = ::poll(&descriptor, 1U, wait_ms);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0) {
            close();
            return MD_ERR_IO;
        }
    }
}


/*
 * Best-effort close: sends GOODBYE when READY, then releases the descriptor
 * and resets all session state.  Closing is not retriable, so send failures are
 * deliberately ignored.
 */
void ClientSession::close() noexcept {
    if (fd_.valid()) {
        std::array<std::uint8_t, 4U> payload{};
        md_writer_t writer;
        md_writer_init(&writer, payload.data(), payload.size());
        if (connection_state_ == MD_CONNECTION_READY && md_proto_encode_u32(&writer, 0U) == 0) {
            const std::int32_t goodbye_result =
                md_codec_send(fd_.get(), selected_minor_, MD_OP_GOODBYE, 0U, next_serial(),
                              payload.data(), writer.size, nullptr, 0U);
            if (goodbye_result != 0) {
                /* Closing a session is best effort; the descriptor is released below regardless. */
            }
        }
    }
    fd_.reset();
    outbox_.clear();
    connection_state_ = MD_CONNECTION_DISCONNECTED;
    handshake_state_ = MD_HANDSHAKE_IDLE;
    selected_minor_ = 0U;
    negotiated_features_ = 0U;
    disconnected_notified_ = false;
}

std::int32_t ClientSession::fd() const noexcept {
    return fd_.get();
}

md_connection_state_t ClientSession::connection_state() const noexcept {
    return connection_state_;
}

md_handshake_state_t ClientSession::handshake_state() const noexcept {
    return handshake_state_;
}

std::uint16_t ClientSession::selected_minor() const noexcept {
    return selected_minor_;
}

bool ClientSession::has_pending_output() const noexcept {
    return outbox_.has_pending();
}

void ClientSession::set_advertised_features(const std::uint64_t features) noexcept {
    advertised_features_ = features;
}

std::uint32_t ClientSession::next_serial() noexcept {
    const std::uint32_t serial = next_serial_;
    ++next_serial_;
    return serial;
}

std::int32_t ClientSession::flush_outbox() {
    return outbox_.flush(fd_.get(), selected_minor_);
}

std::int32_t ClientSession::send_or_queue(const std::uint16_t opcode, const std::uint16_t flags,
                                          const std::uint8_t* const payload,
                                          const std::size_t payload_size,
                                          std::int32_t* const fds,
                                          const std::size_t fd_count,
                                          const bool coalesce_tail) {
    return outbox_.send_or_queue(fd_.get(), selected_minor_, opcode, flags, next_serial(), payload,
                                 payload_size, fds, fd_count, coalesce_tail);
}

std::int32_t ClientSession::disconnect(const md_result_t reason, const char* const message) {
    fd_.reset();
    outbox_.clear();
    connection_state_ = MD_CONNECTION_DEAD;
    handshake_state_ = MD_HANDSHAKE_IDLE;
    selected_minor_ = 0U;
    negotiated_features_ = 0U;
    if (!disconnected_notified_) {
        disconnected_notified_ = true;
        on_disconnected(reason, message);
    }
    return reason;
}

}  // namespace mirage
