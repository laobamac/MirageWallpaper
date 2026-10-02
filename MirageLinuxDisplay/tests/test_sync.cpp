#include "common/drm.hpp"
#include "mirage_display.h"
#include "sync_fanout.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/*
 * Unit tests for DRM syncobj fanout and sync_file bridging.
 */

static void assert_closed(int fd) {
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1);
    assert(errno == EBADF);
}

static int open_test_render_node(void) {
    for (int minor_number = 128; minor_number <= 255; ++minor_number) {
        char path[64];
        int written = snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor_number);
        if (written <= 0 || (size_t)written >= sizeof(path)) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

static void test_real_syncobj_fanout(void) {
    int drm_fd = open_test_render_node();
    if (drm_fd < 0) return;

    struct stat status;
    if (fstat(drm_fd, &status) != 0 || !S_ISCHR(status.st_mode)) {
        close(drm_fd);
        return;
    }
    struct md_drm_syncobj_create create = {.handle = 0, .flags = 0};
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
        close(drm_fd);
        return;
    }
    struct md_drm_syncobj_handle export_handle = {
        .handle = create.handle,
        .flags = 0,
        .fd = -1,
        .pad = 0,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export_handle) != 0) {
        struct md_drm_syncobj_destroy destroy = {.handle = create.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
        close(drm_fd);
        return;
    }

    int child_fds[2] = {-1, -1};
    md_sync_fanout_t* fanout = NULL;
    assert(md_sync_fanout_create_on_node(export_handle.fd, 2,
                                         (uint32_t)major(status.st_rdev),
                                         (uint32_t)minor(status.st_rdev),
                                         child_fds, &fanout) == MD_OK);
    assert(fanout != NULL);
    close(export_handle.fd);

    for (size_t i = 0; i < 2; ++i) {
        assert(child_fds[i] >= 0);
        assert(md_display_signal_release_syncobj_on_node(
                   child_fds[i], (uint32_t)major(status.st_rdev),
                   (uint32_t)minor(status.st_rdev)) == MD_OK);
    }
    assert(md_sync_fanout_poll(fanout) == 1);

    uint32_t handles[1] = {create.handle};
    struct md_drm_syncobj_wait wait = {
        .handles = (uint64_t)(uintptr_t)handles,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = MD_DRM_SYNCOBJ_WAIT_ALL,
        .first_signaled = 0,
        .pad = 0,
        .deadline_nsec = 0,
    };
    assert(ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0);

    md_sync_fanout_free(fanout);
    struct md_drm_syncobj_destroy destroy = {.handle = create.handle, .pad = 0};
    (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    close(drm_fd);
}

int main(void) {
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    int release_fd = pipe_fds[0];
    close(pipe_fds[1]);
    assert(md_display_signal_release_syncobj(release_fd) == MD_ERR_IO);
    assert_closed(release_fd);

    int release_pipe[2];
    int sync_pipe[2];
    assert(pipe2(release_pipe, O_CLOEXEC) == 0);
    assert(pipe2(sync_pipe, O_CLOEXEC) == 0);
    release_fd = release_pipe[0];
    int sync_fd = sync_pipe[0];
    close(release_pipe[1]);
    close(sync_pipe[1]);
    assert(md_display_release_after_sync_file(release_fd, sync_fd) == MD_ERR_IO);
    assert_closed(release_fd);
    assert_closed(sync_fd);
    test_real_syncobj_fanout();
    return 0;
}
