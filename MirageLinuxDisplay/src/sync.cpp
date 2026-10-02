#include "mirage_display.h"

#include "common/drm.hpp"
#include "sync_fanout.h"

#include <bit>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <memory>
#include <new>
#include <unistd.h>

/*
 * DRM syncobj fanout and sync_file bridging used by the broker and the C
 * consumer helpers.
 *
 * The fanout turns one producer release syncobj into one child per consumer so
 * each display can release its own GPU work independently; md_sync_fanout_poll
 * tracks completion and md_sync_fanout_abandon drops unavailable consumers.
 */

/* The opaque C handle owns the DRM import, every child syncobj, and the
 * exported-child tracking arrays until md_sync_fanout_free consumes it. */
struct md_sync_fanout {
    int drm_fd;
    uint32_t original_handle;
    uint32_t child_count;
    std::unique_ptr<uint32_t[]> child_handles;
    std::unique_ptr<uint8_t[]> abandoned;
    int64_t started_ns;
    bool finished;
};

namespace {

/* DRM's syncobj ioctls encode a userspace pointer in a fixed uint64_t ABI
 * field. Mirage targets the 64-bit Linux DRM ABI, so bit_cast preserves the
 * exact address bits without introducing a business-data pointer conversion. */
static_assert(sizeof(void*) == sizeof(uint64_t));

/* The caller receives an error instead of a synthetic timestamp because wait
 * deadlines are part of the DRM synchronization protocol. */
int monotonic_ns(int64_t* const value) {
    timespec clock_value{};
    if (clock_gettime(CLOCK_MONOTONIC, &clock_value) != 0) {
        return MD_ERR_IO;
    }
    *value = static_cast<int64_t>(clock_value.tv_sec) * INT64_C(1000000000) +
             static_cast<int64_t>(clock_value.tv_nsec);
    return MD_OK;
}

int signal_syncobj_handles(const int drm_fd, const uint32_t* const handles,
                           const uint32_t count) {
    if (drm_fd < 0 || handles == nullptr || count == 0U) {
        return MD_ERR_INVALID;
    }
    md_drm_syncobj_array signal{
        std::bit_cast<uint64_t>(handles),
        count,
        0U,
    };
    return ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) == 0 ? MD_OK : MD_ERR_IO;
}

void close_child_fds(int* const child_fds, const uint32_t child_count) {
    for (uint32_t index = 0; index < child_count; ++index) {
        if (child_fds[index] >= 0) {
            /* These FDs have not escaped the create call, so closing them is
             * the only valid rollback after a partial DRM export failure. */
            close(child_fds[index]);
            child_fds[index] = -1;
        }
    }
}

}  // namespace


/*
 * Imports the producer's release syncobj on the given DRM render node and
 * creates child_count binary syncobjs, one per consumer.  The original FD is
 * borrowed; on MD_OK each child FD transfers to the caller and *out_fanout owns
 * the fanout until md_sync_fanout_free.
 */
int md_sync_fanout_create_on_node(const int original_syncobj_fd, const uint32_t child_count,
                                  const uint32_t drm_major, const uint32_t drm_minor,
                                  int* const child_fds, md_sync_fanout_t** const out_fanout) {
    if (original_syncobj_fd < 0 || child_count < 2U || child_fds == nullptr ||
        out_fanout == nullptr) {
        return MD_ERR_INVALID;
    }

    *out_fanout = nullptr;
    for (uint32_t index = 0; index < child_count; ++index) {
        child_fds[index] = -1;
    }

    std::unique_ptr<md_sync_fanout> fanout{new (std::nothrow) md_sync_fanout{
        .drm_fd = -1,
        .original_handle = 0U,
        .child_count = child_count,
        .child_handles = {},
        .abandoned = {},
        .started_ns = 0,
        .finished = false,
    }};
    if (!fanout) {
        return MD_ERR_NOMEM;
    }

    fanout->child_handles.reset(new (std::nothrow) uint32_t[child_count]{});
    fanout->abandoned.reset(new (std::nothrow) uint8_t[child_count]{});
    if (!fanout->child_handles || !fanout->abandoned) {
        return MD_ERR_NOMEM;
    }

    fanout->drm_fd = md_drm_open_render_node(drm_major, drm_minor);
    if (fanout->drm_fd < 0) {
        return MD_ERR_IO;
    }

    md_drm_syncobj_handle original{
        0U,
        0U,
        original_syncobj_fd,
        0U,
    };
    if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &original) != 0) {
        close(fanout->drm_fd);
        return MD_ERR_IO;
    }
    fanout->original_handle = original.handle;

    for (uint32_t index = 0; index < child_count; ++index) {
        md_drm_syncobj_create create{
            0U,
            0U,
        };
        if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
            close_child_fds(child_fds, child_count);
            md_sync_fanout_free(fanout.release());
            return MD_ERR_IO;
        }
        fanout->child_handles[index] = create.handle;

        md_drm_syncobj_handle export_handle{
            create.handle,
            0U,
            -1,
            0U,
        };
        if (ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export_handle) != 0) {
            close_child_fds(child_fds, child_count);
            md_sync_fanout_free(fanout.release());
            return MD_ERR_IO;
        }
        child_fds[index] = export_handle.fd;
    }

    const int timestamp_result = monotonic_ns(&fanout->started_ns);
    if (timestamp_result != MD_OK) {
        close_child_fds(child_fds, child_count);
        md_sync_fanout_free(fanout.release());
        return timestamp_result;
    }

    *out_fanout = fanout.release();
    return MD_OK;
}

int md_sync_fanout_create(const int original_syncobj_fd, const uint32_t child_count,
                          int* const child_fds, md_sync_fanout_t** const out_fanout) {
    return md_sync_fanout_create_on_node(original_syncobj_fd, child_count, 0U, 0U, child_fds,
                                         out_fanout);
}


/*
 * Marks one consumer as gone and signals its child so the fanout never waits
 * on it; the fanout object itself remains owned by the caller.
 */
void md_sync_fanout_abandon(md_sync_fanout_t* const fanout, const uint32_t child_index) {
    if (fanout == nullptr || fanout->finished || child_index >= fanout->child_count ||
        fanout->abandoned[child_index] != 0U) {
        return;
    }

    fanout->abandoned[child_index] = 1U;
    const uint32_t handle = fanout->child_handles[child_index];
    if (handle != 0U) {
        const int signal_result = signal_syncobj_handles(fanout->drm_fd, &handle, 1U);
        if (signal_result != MD_OK) {
            /* The void C ABI cannot report abandonment failure. Poll/free will
             * still complete the original syncobj so a producer is not held. */
            fanout->finished = false;
        }
    }
}

int md_sync_fanout_poll(md_sync_fanout_t* const fanout) {
    if (fanout == nullptr) {
        return MD_ERR_INVALID;
    }
    if (fanout->finished) {
        return 1;
    }

    int64_t now{};
    const int timestamp_result = monotonic_ns(&now);
    if (timestamp_result != MD_OK) {
        return timestamp_result;
    }

    md_drm_syncobj_wait wait{
        std::bit_cast<uint64_t>(fanout->child_handles.get()),
        now,
        fanout->child_count,
        MD_DRM_SYNCOBJ_WAIT_ALL,
        0U,
        0U,
        0U,
    };
    const bool completed = ioctl(fanout->drm_fd, MD_DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0;
    if (!completed && errno != ETIME && errno != EBUSY && errno != EAGAIN) {
        /* A rejected wait means the broker cannot observe consumers anymore;
         * signaling the imported producer object prevents a permanent slot
         * leak even though this call still reports the node failure. */
        const int signal_result = signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1U);
        if (signal_result == MD_OK) {
            fanout->finished = true;
        }
        return MD_ERR_IO;
    }
    if (!completed && now - fanout->started_ns < INT64_C(5000000000)) {
        return 0;
    }

    const int signal_result = signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1U);
    if (signal_result != MD_OK) {
        return signal_result;
    }
    fanout->finished = true;
    return 1;
}

void md_sync_fanout_free(md_sync_fanout_t* const fanout) {
    if (fanout == nullptr) {
        return;
    }

    if (fanout->drm_fd >= 0) {
        if (!fanout->finished && fanout->original_handle != 0U) {
            /* Free represents cancellation. The interface has no error return,
             * so teardown still destroys the imported object after attempting
             * the required producer release signal. */
            const int signal_result =
                signal_syncobj_handles(fanout->drm_fd, &fanout->original_handle, 1U);
            if (signal_result == MD_OK) {
                fanout->finished = true;
            }
        }
        for (uint32_t index = 0; index < fanout->child_count; ++index) {
            md_drm_destroy_syncobj(fanout->drm_fd, fanout->child_handles[index]);
        }
        md_drm_destroy_syncobj(fanout->drm_fd, fanout->original_handle);
        close(fanout->drm_fd);
    }
    delete fanout;
}


/*
 * CPU fallback that signals a release syncobj without GPU work, used when a
 * frame is rejected, stale, or unhandled.  Consumes release_syncobj_fd on every
 * path.
 */
md_result_t md_display_signal_release_syncobj_on_node(const int release_syncobj_fd,
                                                       const uint32_t drm_major,
                                                       const uint32_t drm_minor) {
    if (release_syncobj_fd < 0) {
        return MD_ERR_INVALID;
    }

    const int drm_fd = md_drm_open_render_node(drm_major, drm_minor);
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        return MD_ERR_IO;
    }

    md_drm_syncobj_handle imported{
        0U,
        0U,
        release_syncobj_fd,
        0U,
    };
    md_result_t result = MD_ERR_IO;
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imported) == 0) {
        const int signal_result = signal_syncobj_handles(drm_fd, &imported.handle, 1U);
        if (signal_result == MD_OK) {
            result = MD_OK;
        }
        md_drm_destroy_syncobj(drm_fd, imported.handle);
    }
    close(release_syncobj_fd);
    close(drm_fd);
    return result;
}

md_result_t md_display_signal_release_syncobj(const int32_t release_syncobj_fd) {
    return md_display_signal_release_syncobj_on_node(release_syncobj_fd, 0U, 0U);
}


/*
 * Bridges a completed sync_file into a release syncobj so a host without
 * explicit-sync plumbing can still release a slot exactly once.  Consumes both
 * descriptors on every path.
 */
md_result_t md_display_release_after_sync_file(const int32_t release_syncobj_fd,
                                               const int32_t sync_file_fd) {
    if (release_syncobj_fd < 0 || sync_file_fd < 0) {
        if (release_syncobj_fd >= 0) {
            close(release_syncobj_fd);
        }
        if (sync_file_fd >= 0) {
            close(sync_file_fd);
        }
        return MD_ERR_INVALID;
    }

    const int drm_fd = md_drm_open_render_node(0U, 0U);
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        close(sync_file_fd);
        return MD_ERR_IO;
    }

    md_result_t result = MD_ERR_IO;
    md_drm_syncobj_handle release{
        0U,
        0U,
        release_syncobj_fd,
        0U,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &release) == 0) {
        md_drm_syncobj_create source{
            0U,
            0U,
        };
        if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &source) == 0) {
            md_drm_syncobj_handle sync_file{
                source.handle,
                MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE,
                sync_file_fd,
                0U,
            };
            if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sync_file) == 0) {
                md_drm_syncobj_transfer transfer{
                    source.handle,
                    release.handle,
                    0U,
                    0U,
                    0U,
                    0U,
                };
                if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) == 0) {
                    result = MD_OK;
                }
            }
            md_drm_destroy_syncobj(drm_fd, source.handle);
        }
        md_drm_destroy_syncobj(drm_fd, release.handle);
    }
    close(release_syncobj_fd);
    close(sync_file_fd);
    close(drm_fd);
    return result;
}
