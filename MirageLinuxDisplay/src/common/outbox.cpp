#include "outbox.hpp"

#include <algorithm>
#include <cerrno>
#include <new>

/*
 * Implementation of the outbound message queue used when the socket is not
 * writable.
 */

md_outbox_t::Message::Message(const std::uint16_t message_opcode,
                              const std::uint16_t message_flags,
                              const std::uint32_t message_serial,
                              const std::uint8_t* const message_payload,
                              const std::size_t message_payload_size,
                              std::int32_t* const message_fds,
                              const std::size_t message_fd_count)
    : opcode(message_opcode),
      flags(message_flags),
      serial(message_serial),
      payload(message_payload_size),
      fd_count(message_fd_count) {
    if (message_payload_size != 0U) {
        std::copy_n(message_payload, message_payload_size, payload.data());
    }
    for (std::size_t index = 0U; index < fd_count; ++index) {
        fds[index].reset(message_fds[index]);
        message_fds[index] = mirage::kInvalidFd;
    }
}

bool md_outbox_t::has_pending() const noexcept {
    return !messages_.empty();
}

void md_outbox_t::clear() noexcept {
    messages_.clear();
}

std::int32_t md_outbox_t::send_or_queue(const std::int32_t fd, const std::uint16_t minor,
                                         const std::uint16_t opcode,
                                         const std::uint16_t flags,
                                         const std::uint32_t serial,
                                         const std::uint8_t* const payload,
                                         const std::size_t payload_size,
                                         std::int32_t* const fds,
                                         const std::size_t fd_count,
                                         const bool coalesce_tail) {
    if (fd == mirage::kInvalidFd || payload_size > MD_WIRE_MAX_PAYLOAD ||
        fd_count > MD_WIRE_MAX_FDS ||
        (payload_size != 0U && payload == nullptr) ||
        (fd_count != 0U && fds == nullptr)) {
        md_close_fds(fds, fd_count);
        return MD_ERR_INVALID;
    }

    if (messages_.empty()) {
        const std::int32_t result =
            md_codec_send(fd, minor, opcode, flags, serial, payload, payload_size, fds, fd_count);
        if (result == 0) {
            md_close_fds(fds, fd_count);
            return MD_OK;
        }
        if (result < 0) {
            md_close_fds(fds, fd_count);
            return result;
        }
    }

    if (coalesce_tail && fd_count == 0U && !messages_.empty()) {
        Message& tail = messages_.back();
        if (tail.opcode == opcode && tail.payload.size() == payload_size) {
            if (payload_size != 0U) {
                std::copy_n(payload, payload_size, tail.payload.data());
            }
            return MD_OK;
        }
    }

    if (messages_.size() >= MD_OUTBOX_LIMIT) {
        md_close_fds(fds, fd_count);
        return MD_ERR_WOULD_BLOCK;
    }

    try {
        messages_.emplace_back(opcode, flags, serial, payload, payload_size, fds, fd_count);
    } catch (const std::bad_alloc&) {
        md_close_fds(fds, fd_count);
        return MD_ERR_NOMEM;
    }
    return MD_OK;
}

std::int32_t md_outbox_t::flush(const std::int32_t fd, const std::uint16_t minor) {
    while (!messages_.empty()) {
        Message& message = messages_.front();
        std::array<std::int32_t, MD_WIRE_MAX_FDS> raw_fds{};
        for (std::size_t index = 0U; index < message.fd_count; ++index) {
            raw_fds[index] = message.fds[index].get();
        }
        const std::int32_t result = md_codec_send(
            fd, minor, message.opcode, message.flags, message.serial,
            message.payload.empty() ? nullptr : message.payload.data(), message.payload.size(),
            message.fd_count == 0U ? nullptr : raw_fds.data(), message.fd_count);
        if (result == 1) {
            return MD_ERR_WOULD_BLOCK;
        }
        if (result < 0) {
            return result;
        }
        /* Popping destroys UniqueFd instances and releases the sent SCM_RIGHTS FDs. */
        messages_.pop_front();
    }
    return MD_OK;
}

void md_outbox_init(md_outbox_t* const outbox) {
    if (outbox != nullptr) {
        outbox->clear();
    }
}

void md_outbox_clear(md_outbox_t* const outbox) {
    if (outbox != nullptr) {
        outbox->clear();
    }
}

std::int32_t md_outbox_send_or_queue(md_outbox_t* const outbox, const std::int32_t fd,
                                     const std::uint16_t minor, const std::uint16_t opcode,
                                     const std::uint16_t flags, const std::uint32_t serial,
                                     const std::uint8_t* const payload,
                                     const std::size_t payload_size,
                                     std::int32_t* const fds, const std::size_t fd_count,
                                     const bool coalesce_tail) {
    if (outbox == nullptr) {
        md_close_fds(fds, fd_count);
        return MD_ERR_INVALID;
    }
    return outbox->send_or_queue(fd, minor, opcode, flags, serial, payload, payload_size, fds,
                                 fd_count, coalesce_tail);
}

std::int32_t md_outbox_flush(md_outbox_t* const outbox, const std::int32_t fd,
                             const std::uint16_t minor) {
    return outbox == nullptr ? MD_ERR_INVALID : outbox->flush(fd, minor);
}
