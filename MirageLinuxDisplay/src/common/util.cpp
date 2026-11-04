#include "util.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <unistd.h>

/*
 * Implementation of UniqueFd and the descriptor-array helpers.
 *
 * close(2) errors are not retriable after ownership ends, so the destructor and
 * array closers only provide the POSIX best-effort guarantee.
 */

namespace mirage {

UniqueFd::UniqueFd(const std::int32_t fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() {
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

std::int32_t UniqueFd::get() const noexcept {
    return fd_;
}

bool UniqueFd::valid() const noexcept {
    return fd_ != kInvalidFd;
}

std::int32_t UniqueFd::release() noexcept {
    const std::int32_t released = fd_;
    fd_ = kInvalidFd;
    return released;
}

void UniqueFd::relinquish() noexcept {
    /* EGL/Vulkan now owns the descriptor after a successful import call. */
    fd_ = kInvalidFd;
}

void UniqueFd::reset(const std::int32_t fd) noexcept {
    if (valid()) {
        /* A descriptor is no longer usable after close(2), including EINTR. */
        const std::int32_t close_result = ::close(fd_);
        if (close_result != 0) {
            /* The destructor boundary cannot return the non-retriable error. */
        }
    }
    fd_ = fd;
}

}  // namespace mirage


void md_close_fds(std::int32_t* const fds, const std::size_t count) {
    if (fds == nullptr) {
        return;
    }

    for (std::size_t index = 0U; index < count; ++index) {
        if (fds[index] != mirage::kInvalidFd) {
            const std::int32_t close_result = ::close(fds[index]);
            if (close_result != 0) {
                /* Ownership ends even when close(2) reports an error. */
            }
        }
        fds[index] = mirage::kInvalidFd;
    }
}

md_result_t md_duplicate_fds(const std::int32_t* const source, const std::size_t count,
                             std::int32_t* const destination) {
    if (source == nullptr || destination == nullptr) {
        return MD_ERR_INVALID;
    }

    for (std::size_t index = 0U; index < count; ++index) {
        destination[index] = mirage::kInvalidFd;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const std::int32_t duplicated = ::fcntl(source[index], F_DUPFD_CLOEXEC, 0);
        if (duplicated == mirage::kInvalidFd) {
            md_close_fds(destination, count);
            return MD_ERR_IO;
        }
        destination[index] = duplicated;
    }
    return MD_OK;
}

void md_init_pool(md_buffer_pool_t* const pool) {
    std::memset(pool, 0, sizeof(*pool));
    for (std::size_t buffer_index = 0U; buffer_index < MIRAGE_DISPLAY_MAX_BUFFERS;
         ++buffer_index) {
        for (std::size_t plane_index = 0U; plane_index < MIRAGE_DISPLAY_MAX_PLANES;
             ++plane_index) {
            pool->planes[buffer_index][plane_index].fd = mirage::kInvalidFd;
        }
    }
}

void md_close_pool(md_buffer_pool_t* const pool) {
    if (pool == nullptr) {
        return;
    }

    /* Iterate the declared fixed protocol extent so malformed metadata cannot leak an FD. */
    for (std::size_t buffer_index = 0U; buffer_index < MIRAGE_DISPLAY_MAX_BUFFERS;
         ++buffer_index) {
        for (std::size_t plane_index = 0U; plane_index < MIRAGE_DISPLAY_MAX_PLANES;
             ++plane_index) {
            const std::int32_t fd = pool->planes[buffer_index][plane_index].fd;
            if (fd != mirage::kInvalidFd) {
                const std::int32_t close_result = ::close(fd);
                if (close_result != 0) {
                    /* Pool ownership ends after the first close attempt. */
                }
            }
        }
    }
    md_init_pool(pool);
}

md_result_t md_map_io_error(const std::int32_t error) {
    if (error == -ENOMEM) {
        return MD_ERR_NOMEM;
    }
    if (error == -EPROTO || error == -EMSGSIZE) {
        return MD_ERR_PROTOCOL;
    }
    if (error == -ECONNRESET || error == -EPIPE || error == -ENOTCONN) {
        return MD_ERR_DISCONNECTED;
    }
    return MD_ERR_IO;
}
