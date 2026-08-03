#define _GNU_SOURCE

#include "mirage_display.h"
#include "common/drm.h"
#include "sync_fanout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct md_sync_fanout {
    int drm_fd;
    uint32_t original_handle;
    uint32_t child_count;
    uint32_t* child_handles;
    bool* abandoned;
    int64_t started_ns;
    bool finished;
};

static int64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (int64_t)value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static int signal_syncobj_handles(int drm_fd, uint32_t* handles, uint32_t count) {
    if (drm_fd < 0 || handles == NULL || count == 0) return MD_ERR_INVALID;
    struct md_drm_syncobj_array signal = {
        .handles = (uint64_t)(uintptr_t)handles,
        .count_handles = count,
        .pad = 0,
    };
    return ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) == 0 ? MD_OK : MD_ERR_IO;
}

int md_sync_fanout_create_on_node(int original_syncobj_fd, uint32_t child_count,
                                  uint32_t drm_major, uint32_t drm_minor,
                                  int* child_fds, md_sync_fanout_t** out_fanout) {
    if (original_syncobj_fd < 0 || child_count < 2 || child_fds == NULL ||
        out_fanout == NULL) return MD_ERR_INVALID;
    *out_fanout = NULL;
    for (uint32_t i = 0; i < child_count; ++i) child_fds[i] = -1;

    md_sync_fanout_t* fanout = calloc(1, sizeof(*fanout));
    if (fanout == NULL) return MD_ERR_NOMEM;
    fanout->drm_fd = md_drm_open_render_node(drm_major, drm_minor);
    fanout->child_count = child_count;
    fanout->child_handles = calloc(child_count, sizeof(*fanout->child_handles));
    fanout->abandoned = calloc(child_count, sizeof(*fanout->abandoned));
    if (fanout->drm_fd < 0 || fanout->child_handles == NULL || fanout->abandoned == NULL) {
        md_sync_fanout_free(fanout);
        return MD_ERR_IO;
    }

    struct md_drm_syncobj_handle original = {
        .handle = 0,
        .flags = 0,
        .fd = original_syncobj_fd,
        .pad = 0,
    };
    if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &original) != 0) {
        md_sync_fanout_free(fanout);
        return MD_ERR_IO;
    }
    fanout->original_handle = original.handle;

    for (uint32_t i = 0; i < child_count; ++i) {
        struct md_drm_syncobj_create create = {.handle = 0, .flags = 0};
        if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
            for (uint32_t j = 0; j < child_count; ++j) {
                if (child_fds[j] >= 0) close(child_fds[j]);
            }
            md_sync_fanout_free(fanout);
            return MD_ERR_IO;
        }
        fanout->child_handles[i] = create.handle;
        struct md_drm_syncobj_handle export_handle = {
            .handle = create.handle,
            .flags = 0,
            .fd = -1,
            .pad = 0,
        };
        if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export_handle) != 0) {
            for (uint32_t j = 0; j < child_count; ++j) {
                if (child_fds[j] >= 0) close(child_fds[j]);
            }
            md_sync_fanout_free(fanout);
            return MD_ERR_IO;
        }
        child_fds[i] = export_handle.fd;
    }
    fanout->started_ns = monotonic_ns();
    *out_fanout = fanout;
    return MD_OK;
}

int md_sync_fanout_create(int original_syncobj_fd, uint32_t child_count,
                          int* child_fds, md_sync_fanout_t** out_fanout) {
    return md_sync_fanout_create_on_node(original_syncobj_fd, child_count, 0, 0,
                                         child_fds, out_fanout);
}

void md_sync_fanout_abandon(md_sync_fanout_t* fanout, uint32_t child_index) {
    if (fanout == NULL || fanout->finished || child_index >= fanout->child_count ||
        fanout->abandoned[child_index]) return;
    fanout->abandoned[child_index] = true;
    uint32_t handle = fanout->child_handles[child_index];
    if (handle != 0) (void)signal_syncobj_handles(fanout->drm_fd, &handle, 1);
}

int md_sync_fanout_poll(md_sync_fanout_t* fanout) {
    if (fanout == NULL) return MD_ERR_INVALID;
    if (fanout->finished) return 1;
    int64_t now = monotonic_ns();
    struct md_drm_syncobj_wait wait = {
        .handles = (uint64_t)(uintptr_t)fanout->child_handles,
        .timeout_nsec = now,
        .count_handles = fanout->child_count,
        .flags = MD_DRM_SYNCOBJ_WAIT_ALL,
        .first_signaled = 0,
        .pad = 0,
        .deadline_nsec = 0,
    };
    bool completed = ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0;
    if (!completed && errno != ETIME && errno != EBUSY && errno != EAGAIN) {
        /* Do not leave the producer slot blocked when the render node
         * disappears or rejects a wait after a consumer restart. */
        if (signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1) == MD_OK) {
            fanout->finished = true;
        }
        return MD_ERR_IO;
    }
    /* A vanished consumer must not keep a producer slot blocked forever. */
    if (!completed && now - fanout->started_ns < INT64_C(5000000000)) return 0;
    if (signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1) != MD_OK) {
        return MD_ERR_IO;
    }
    fanout->finished = true;
    return 1;
}

void md_sync_fanout_free(md_sync_fanout_t* fanout) {
    if (fanout == NULL) return;
    if (fanout->drm_fd >= 0) {
        if (!fanout->finished && fanout->original_handle != 0) {
            /* Free is cancellation (broker shutdown/error), so release the
             * producer slot before destroying the imported handle. */
            (void)signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1);
        }
        for (uint32_t i = 0; i < fanout->child_count; ++i) {
            md_drm_destroy_syncobj(fanout->drm_fd,
                            fanout->child_handles != NULL ? fanout->child_handles[i] : 0);
        }
        md_drm_destroy_syncobj(fanout->drm_fd, fanout->original_handle);
        close(fanout->drm_fd);
    }
    free(fanout->child_handles);
    free(fanout->abandoned);
    free(fanout);
}

int md_display_signal_release_syncobj_on_node(int release_syncobj_fd,
                                               uint32_t drm_major,
                                               uint32_t drm_minor) {
    if (release_syncobj_fd < 0) return MD_ERR_INVALID;
    int drm_fd = md_drm_open_render_node(drm_major, drm_minor);
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        return MD_ERR_IO;
    }
    struct md_drm_syncobj_handle import = {
        .handle = 0,
        .flags = 0,
        .fd = release_syncobj_fd,
        .pad = 0,
    };
    int result = MD_ERR_IO;
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) == 0) {
        uint32_t handles[1] = {import.handle};
        struct md_drm_syncobj_array signal = {
            .handles = (uint64_t)(uintptr_t)handles,
            .count_handles = 1,
            .pad = 0,
        };
        if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) == 0) result = MD_OK;
        struct md_drm_syncobj_destroy destroy = {.handle = import.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    close(release_syncobj_fd);
    close(drm_fd);
    return result;
}

int md_display_signal_release_syncobj(int release_syncobj_fd) {
    return md_display_signal_release_syncobj_on_node(release_syncobj_fd, 0, 0);
}

int md_display_release_after_sync_file(int release_syncobj_fd, int sync_file_fd) {
    if (release_syncobj_fd < 0 || sync_file_fd < 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        if (sync_file_fd >= 0) close(sync_file_fd);
        return MD_ERR_INVALID;
    }
    int drm_fd = md_drm_open_render_node(0, 0);
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        close(sync_file_fd);
        return MD_ERR_IO;
    }

    int result = MD_ERR_IO;
    struct md_drm_syncobj_handle release = {
        .handle = 0,
        .flags = 0,
        .fd = release_syncobj_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &release) != 0) goto done;

    struct md_drm_syncobj_create source = {.handle = 0, .flags = 0};
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &source) != 0) goto destroy_release;

    struct md_drm_syncobj_handle sync_file = {
        .handle = source.handle,
        .flags = MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE,
        .fd = sync_file_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sync_file) == 0) {
        struct md_drm_syncobj_transfer transfer = {
            .src_handle = source.handle,
            .dst_handle = release.handle,
            .src_point = 0,
            .dst_point = 0,
            .flags = 0,
            .pad = 0,
        };
        if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) == 0) result = MD_OK;
    }
    {
        struct md_drm_syncobj_destroy destroy = {.handle = source.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
destroy_release:
    {
        struct md_drm_syncobj_destroy destroy = {.handle = release.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
done:
    close(release_syncobj_fd);
    close(sync_file_fd);
    close(drm_fd);
    return result;
}


