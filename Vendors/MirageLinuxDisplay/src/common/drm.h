#ifndef MIRAGE_DISPLAY_COMMON_DRM_H
#define MIRAGE_DISPLAY_COMMON_DRM_H

#include <stdint.h>
#include <sys/ioctl.h>

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

/* Minimal DRM syncobj ABI, shared by the core and the GPU helpers so the
 * ioctl layouts and constants stay in one place. */
struct md_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};
struct md_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct md_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
};
struct md_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};
struct md_drm_syncobj_wait {
    uint64_t handles;
    int64_t timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
    uint64_t deadline_nsec;
};
struct md_drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
};

#define MD_DRM_IOCTL_SYNCOBJ_CREATE \
    _IOWR(DRM_IOCTL_BASE, 0xbf, struct md_drm_syncobj_create)
#define MD_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xc0, struct md_drm_syncobj_destroy)
#define MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD \
    _IOWR(DRM_IOCTL_BASE, 0xc1, struct md_drm_syncobj_handle)
#define MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0xc2, struct md_drm_syncobj_handle)
#define MD_DRM_IOCTL_SYNCOBJ_WAIT \
    _IOWR(DRM_IOCTL_BASE, 0xc3, struct md_drm_syncobj_wait)
#define MD_DRM_IOCTL_SYNCOBJ_SIGNAL \
    _IOWR(DRM_IOCTL_BASE, 0xc5, struct md_drm_syncobj_array)
#define MD_DRM_IOCTL_SYNCOBJ_TRANSFER \
    _IOWR(DRM_IOCTL_BASE, 0xcc, struct md_drm_syncobj_transfer)

#define MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE (UINT32_C(1) << 0)
#define MD_DRM_SYNCOBJ_WAIT_ALL (UINT32_C(1) << 0)

/* Opens the DRM render node matching the requested major/minor. Passing zero
 * for both selects the first available renderD node. Returns -1 on failure. */
int md_drm_open_render_node(uint32_t drm_major, uint32_t drm_minor);

/* Destroys a syncobj handle on the node; a no-op when handle is zero. */
void md_drm_destroy_syncobj(int drm_fd, uint32_t handle);

#endif
