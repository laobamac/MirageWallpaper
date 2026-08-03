#define _GNU_SOURCE

#include "drm.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int md_drm_open_render_node(uint32_t drm_major, uint32_t drm_minor) {
    if (drm_major != 0 || drm_minor != 0) {
        char paths[2][64];
        size_t path_count = 0;
        int written = snprintf(paths[path_count], sizeof(paths[path_count]),
                               "/dev/char/%u:%u", drm_major, drm_minor);
        if (written > 0 && (size_t)written < sizeof(paths[0])) ++path_count;
        if (drm_minor >= 128u && drm_minor <= 255u && path_count < 2u) {
            written = snprintf(paths[path_count], sizeof(paths[path_count]),
                               "/dev/dri/renderD%u", drm_minor);
            if (written > 0 && (size_t)written < sizeof(paths[0])) ++path_count;
        }
        for (size_t i = 0; i < path_count; ++i) {
            int fd = open(paths[i], O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            struct stat status;
            if (fstat(fd, &status) == 0 && S_ISCHR(status.st_mode) &&
                (drm_major == 0 || major(status.st_rdev) == drm_major) &&
                (drm_minor == 0 || minor(status.st_rdev) == drm_minor)) {
                return fd;
            }
            close(fd);
        }
        return -1;
    }
    for (int minor_number = 128; minor_number <= 255; ++minor_number) {
        char path[64];
        int written = snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor_number);
        if (written <= 0 || (size_t)written >= sizeof(path)) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

void md_drm_destroy_syncobj(int drm_fd, uint32_t handle) {
    if (drm_fd < 0 || handle == 0) return;
    struct md_drm_syncobj_destroy destroy = {.handle = handle, .pad = 0};
    (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}
