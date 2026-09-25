#include "drm.hpp"

#include "util.hpp"

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/*
 * Implementation of the DRM render-node opener and the syncobj destructor.
 */

std::int32_t md_drm_open_render_node(const std::uint32_t drm_major,
                                     const std::uint32_t drm_minor) {
    if (drm_major != 0U || drm_minor != 0U) {
        std::array<std::array<char, 64U>, 2U> paths{};
        std::size_t path_count = 0U;
        const std::int32_t char_device_length = std::snprintf(
            paths[path_count].data(), paths[path_count].size(), "/dev/char/%u:%u", drm_major,
            drm_minor);
        if (char_device_length > 0 &&
            static_cast<std::size_t>(char_device_length) < paths[path_count].size()) {
            ++path_count;
        }
        if (drm_minor >= 128U && drm_minor <= 255U && path_count < paths.size()) {
            const std::int32_t render_node_length = std::snprintf(
                paths[path_count].data(), paths[path_count].size(), "/dev/dri/renderD%u",
                drm_minor);
            if (render_node_length > 0 &&
                static_cast<std::size_t>(render_node_length) < paths[path_count].size()) {
                ++path_count;
            }
        }
        for (std::size_t index = 0U; index < path_count; ++index) {
            const std::int32_t fd = ::open(paths[index].data(), O_RDWR | O_CLOEXEC);
            if (fd == mirage::kInvalidFd) {
                continue;
            }
            struct stat status;
            const std::int32_t stat_result = ::fstat(fd, &status);
            if (stat_result == 0 && S_ISCHR(status.st_mode) &&
                (drm_major == 0U || major(status.st_rdev) == drm_major) &&
                (drm_minor == 0U || minor(status.st_rdev) == drm_minor)) {
                return fd;
            }
            const std::int32_t close_result = ::close(fd);
            if (close_result != 0) {
                /* This rejected candidate is never reused after close(2). */
            }
        }
        return mirage::kInvalidFd;
    }

    for (std::uint32_t minor = 128U; minor <= 255U; ++minor) {
        std::array<char, 64U> path{};
        const std::int32_t path_length =
            std::snprintf(path.data(), path.size(), "/dev/dri/renderD%u", minor);
        if (path_length <= 0 || static_cast<std::size_t>(path_length) >= path.size()) {
            continue;
        }
        const std::int32_t fd = ::open(path.data(), O_RDWR | O_CLOEXEC);
        if (fd != mirage::kInvalidFd) {
            return fd;
        }
    }
    return mirage::kInvalidFd;
}

void md_drm_destroy_syncobj(const std::int32_t drm_fd, const std::uint32_t handle) {
    if (drm_fd == mirage::kInvalidFd || handle == 0U) {
        return;
    }
    md_drm_syncobj_destroy destroy;
    destroy.handle = handle;
    destroy.pad = 0U;
    const std::int32_t ioctl_result = ::ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    if (ioctl_result != 0) {
        /* The API is a destructor hook; there is no live owner left to retry cleanup. */
    }
}
