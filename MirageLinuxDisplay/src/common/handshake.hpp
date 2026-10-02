#ifndef MIRAGE_DISPLAY_COMMON_HANDSHAKE_HPP
#define MIRAGE_DISPLAY_COMMON_HANDSHAKE_HPP

#include "codec.hpp"
#include "outbox.hpp"
#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

/*
 * Shared client transport and mirage-display-v1 handshake state machine used by
 * both the display and producer sessions.
 *
 * The two protocol roles share every handshake step except registration, so a
 * single base class with virtual role hooks replaces three near-identical
 * implementations.
 */

inline constexpr std::size_t MD_HANDSHAKE_BUFFER_SIZE = 4096U;
inline constexpr std::uint32_t MD_CLIENT_ROLE_DISPLAY = 1U;
inline constexpr std::uint32_t MD_CLIENT_ROLE_PRODUCER = 2U;

namespace mirage {

/*
 * Shared client transport and v1 handshake state.  Display and producer are
 * the only protocol roles, so virtual role hooks express their genuine shared
 * state machine without exposing a C callback table or untyped object pointer.
 */
class ClientSession {
public:
    explicit ClientSession(std::uint32_t role, std::uint64_t advertised_features);
    virtual ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    ClientSession(ClientSession&&) = delete;
    ClientSession& operator=(ClientSession&&) = delete;

    /* Copies required NUL-terminated connection identity strings. */
    md_result_t set_identity(const char* socket_path, const char* client_name,
                             const char* client_version);
    void clear_identity() noexcept;

    md_result_t begin_connect();
    md_result_t begin_connected_fd(std::int32_t connected_fd);
    std::int32_t advance_handshake();
    md_result_t connect(std::int32_t timeout_ms);
    void close() noexcept;

    [[nodiscard]] std::int32_t fd() const noexcept;
    [[nodiscard]] md_connection_state_t connection_state() const noexcept;
    [[nodiscard]] md_handshake_state_t handshake_state() const noexcept;
    [[nodiscard]] std::uint16_t selected_minor() const noexcept;
    [[nodiscard]] bool has_pending_output() const noexcept;

protected:
    virtual std::uint16_t register_opcode() const noexcept = 0;
    virtual std::uint16_t accepted_opcode() const noexcept = 0;
    virtual std::uint16_t caps_opcode() const noexcept = 0;
    virtual std::int32_t encode_registration(md_writer_t* writer) = 0;
    virtual std::int32_t encode_caps(md_writer_t* writer) = 0;
    virtual std::int32_t apply_accepted(const md_packet_t& packet) = 0;
    virtual void on_ready() = 0;
    virtual void on_disconnected(md_result_t reason, const char* message) = 0;

    void set_advertised_features(std::uint64_t features) noexcept;
    std::uint32_t next_serial() noexcept;
    std::int32_t flush_outbox();
    std::int32_t send_or_queue(std::uint16_t opcode, std::uint16_t flags,
                               const std::uint8_t* payload, std::size_t payload_size,
                               std::int32_t* fds, std::size_t fd_count,
                               bool coalesce_tail);
    std::int32_t disconnect(md_result_t reason, const char* message);

private:
    std::int32_t prepare_handshake(std::uint16_t opcode);
    md_result_t configure_connected_fd(std::int32_t fd) const;
    md_result_t activate_handshake();
    std::int32_t send_handshake();
    std::int32_t receive_handshake(std::uint16_t expected, md_packet_t* packet);

    UniqueFd fd_;
    md_connection_state_t connection_state_;
    md_handshake_state_t handshake_state_;
    std::uint16_t selected_minor_;
    std::uint64_t negotiated_features_;
    std::uint32_t next_serial_;
    bool disconnected_notified_;
    std::uint32_t role_;
    std::uint64_t advertised_features_;
    std::string socket_path_;
    std::string client_name_;
    std::string client_version_;
    std::uint16_t handshake_opcode_;
    std::uint32_t handshake_serial_;
    std::size_t handshake_size_;
    std::array<std::uint8_t, MD_HANDSHAKE_BUFFER_SIZE> handshake_payload_;
    md_outbox_t outbox_;
};

}  // namespace mirage

#endif
