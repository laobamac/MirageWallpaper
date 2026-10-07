#ifndef MIRAGE_DISPLAY_COMMON_DRM_HPP
#define MIRAGE_DISPLAY_COMMON_DRM_HPP

#include <cstdint>
#include <sys/ioctl.h>

/*
 * Minimal DRM syncobj ioctl ABI declarations.
 *
 * The ioctl structs use an explicit eight-byte wire layout because they cross
 * the kernel ABI; the ioctl numbers come from drm.h and are pinned here so the
 * core library does not depend on system headers for these operations.
 */

/* Kernel ioctl structs have an explicit eight-byte wire layout. */
#pragma pack(push, 8)
struct md_drm_syncobj_create {
    std::uint32_t handle;
    std::uint32_t flags;
};
struct md_drm_syncobj_destroy {
    std::uint32_t handle;
    std::uint32_t pad;
};
struct md_drm_syncobj_handle {
    std::uint32_t handle;
    std::uint32_t flags;
    std::int32_t fd;
    std::uint32_t pad;
};
struct md_drm_syncobj_array {
    std::uint64_t handles;
    std::uint32_t count_handles;
    std::uint32_t pad;
};
struct md_drm_syncobj_wait {
    std::uint64_t handles;
    std::int64_t timeout_nsec;
    std::uint32_t count_handles;
    std::uint32_t flags;
    std::uint32_t first_signaled;
    std::uint32_t pad;
    std::uint64_t deadline_nsec;
};
struct md_drm_syncobj_transfer {
    std::uint32_t src_handle;
    std::uint32_t dst_handle;
    std::uint64_t src_point;
    std::uint64_t dst_point;
    std::uint32_t flags;
    std::uint32_t pad;
};
#pragma pack(pop)

#define MD_DRM_IOCTL_SYNCOBJ_CREATE _IOWR('d', 0xbf, struct md_drm_syncobj_create)
#define MD_DRM_IOCTL_SYNCOBJ_DESTROY _IOWR('d', 0xc0, struct md_drm_syncobj_destroy)
#define MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD _IOWR('d', 0xc1, struct md_drm_syncobj_handle)
#define MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE _IOWR('d', 0xc2, struct md_drm_syncobj_handle)
#define MD_DRM_IOCTL_SYNCOBJ_WAIT _IOWR('d', 0xc3, struct md_drm_syncobj_wait)
#define MD_DRM_IOCTL_SYNCOBJ_SIGNAL _IOWR('d', 0xc5, struct md_drm_syncobj_array)
#define MD_DRM_IOCTL_SYNCOBJ_TRANSFER _IOWR('d', 0xcc, struct md_drm_syncobj_transfer)

inline constexpr std::uint32_t MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE =
    UINT32_C(1) << 0U;
inline constexpr std::uint32_t MD_DRM_SYNCOBJ_WAIT_ALL = UINT32_C(1) << 0U;

/* Opens the requested DRM render node, or the documented first render node for 0:0. */
std::int32_t md_drm_open_render_node(std::uint32_t drm_major, std::uint32_t drm_minor);

/* Destroys a nonzero handle. Kernel cleanup failures cannot be retried by this void API. */
void md_drm_destroy_syncobj(std::int32_t drm_fd, std::uint32_t handle);

#endif
