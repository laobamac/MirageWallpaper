#ifndef MIRAGE_DISPLAY_COMMON_UTIL_HPP
#define MIRAGE_DISPLAY_COMMON_UTIL_HPP

#include "mirage_display.h"

#include <cstddef>
#include <cstdint>

/*
 * Small internal ownership and helper primitives shared by every session:
 * UniqueFd, descriptor arrays, protocol pool lifecycle, and errno mapping.
 */

namespace mirage {

/*
 * Unix descriptors use a single documented invalid sentinel.  Keeping it in
 * one type prevents ownership code from spreading raw sentinel values across
 * the protocol implementation.
 */
inline constexpr std::int32_t kInvalidFd = -1;

/*
 * Owns one Unix file descriptor.  The owner transfers it with release(); all
 * other paths close it on scope exit, which is required for queued SCM_RIGHTS
 * messages and rejected protocol packets.  close(2) errors cannot be retried
 * safely after ownership has ended, so the destructor intentionally provides
 * only the POSIX best-effort release guarantee.
 */
class UniqueFd final {
public:
    explicit UniqueFd(const std::int32_t fd = kInvalidFd) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] std::int32_t get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::int32_t release() noexcept;
    /* A kernel API accepted ownership, so this wrapper must not close the FD. */
    void relinquish() noexcept;
    void reset(const std::int32_t fd = kInvalidFd) noexcept;

private:
    std::int32_t fd_;
};

}  // namespace mirage

/* Closes and resets each owned descriptor in the supplied fixed-length array. */
void md_close_fds(std::int32_t* fds, std::size_t count);

/*
 * Duplicates source descriptors with F_DUPFD_CLOEXEC.  destination receives
 * ownership only on MD_OK; on an error every duplicated descriptor is closed.
 */
md_result_t md_duplicate_fds(const std::int32_t* source, std::size_t count,
                             std::int32_t* destination);

/* Initializes a protocol pool and marks every possible plane descriptor invalid. */
void md_init_pool(md_buffer_pool_t* pool);

/* Closes all descriptors owned by a protocol pool before reinitializing it. */
void md_close_pool(md_buffer_pool_t* pool);

/* Maps known codec errno results onto the public C result enum. */
md_result_t md_map_io_error(std::int32_t error);

#endif
