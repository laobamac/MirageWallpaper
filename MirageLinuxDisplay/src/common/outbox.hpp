#ifndef MIRAGE_DISPLAY_COMMON_OUTBOX_HPP
#define MIRAGE_DISPLAY_COMMON_OUTBOX_HPP

#include "codec.hpp"
#include "util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/*
 * Session-owned FIFO of already encoded SCM_RIGHTS messages.
 *
 * std::deque keeps queued descriptor ownership stable across flushes while
 * std::vector stores the exact encoded bytes; send_or_queue transfers descriptor
 * ownership on every return path.
 */

inline constexpr std::size_t MD_OUTBOX_LIMIT = 64U;

/*
 * A session-owned FIFO of already encoded SCM_RIGHTS messages.  std::deque
 * keeps queued ownership stable while std::vector carries the exact encoded
 * byte count, eliminating the former flexible-array allocation protocol.
 */
class md_outbox_t final {
public:
    md_outbox_t() = default;
    ~md_outbox_t() = default;

    md_outbox_t(const md_outbox_t&) = delete;
    md_outbox_t& operator=(const md_outbox_t&) = delete;
    md_outbox_t(md_outbox_t&&) noexcept = default;
    md_outbox_t& operator=(md_outbox_t&&) noexcept = default;

    [[nodiscard]] bool has_pending() const noexcept;
    void clear() noexcept;

    /*
     * fds transfers ownership to the outbox on every return path.  A queue
     * admission failure therefore closes each supplied descriptor before the
     * caller can reuse it, matching the producer submission contract.
     */
    std::int32_t send_or_queue(std::int32_t fd, std::uint16_t minor,
                               std::uint16_t opcode, std::uint16_t flags,
                               std::uint32_t serial, const std::uint8_t* payload,
                               std::size_t payload_size, std::int32_t* fds,
                               std::size_t fd_count, bool coalesce_tail);

    std::int32_t flush(std::int32_t fd, std::uint16_t minor);

private:
    struct Message final {
        std::uint16_t opcode;
        std::uint16_t flags;
        std::uint32_t serial;
        std::vector<std::uint8_t> payload;
        std::array<mirage::UniqueFd, MD_WIRE_MAX_FDS> fds;
        std::size_t fd_count;

        Message(std::uint16_t message_opcode, std::uint16_t message_flags,
                std::uint32_t message_serial, const std::uint8_t* message_payload,
                std::size_t message_payload_size, std::int32_t* message_fds,
                std::size_t message_fd_count);
    };

    std::deque<Message> messages_;
};

/* Compatibility entry points for the broker's existing queue call sites. */
void md_outbox_init(md_outbox_t* outbox);
void md_outbox_clear(md_outbox_t* outbox);
std::int32_t md_outbox_send_or_queue(md_outbox_t* outbox, std::int32_t fd,
                                     std::uint16_t minor, std::uint16_t opcode,
                                     std::uint16_t flags, std::uint32_t serial,
                                     const std::uint8_t* payload,
                                     std::size_t payload_size, std::int32_t* fds,
                                     std::size_t fd_count, bool coalesce_tail);
std::int32_t md_outbox_flush(md_outbox_t* outbox, std::int32_t fd, std::uint16_t minor);

#endif
