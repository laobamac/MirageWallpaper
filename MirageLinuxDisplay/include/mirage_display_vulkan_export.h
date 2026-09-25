#ifndef MIRAGE_DISPLAY_VULKAN_EXPORT_H
#define MIRAGE_DISPLAY_VULKAN_EXPORT_H

#include "mirage_display.h"

#include <vulkan/vulkan.h>

/*
 * Public C ABI for the Vulkan DMA-BUF exporter.
 *
 * The renderer side uses this helper to turn Vulkan images into protocol frames:
 * it exports a signaled binary semaphore as a sync_file, creates an unsignaled
 * binary DRM syncobj per frame for the consumer release, and recycles slots only
 * after consumers signal release.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Exporter DTOs are explicitly packed for the public C ABI. */
#pragma pack(push, 8)

typedef struct md_vk_exporter md_vk_exporter_t;

typedef struct md_vk_export_context {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;
    /* Optional already-open render node. It is duplicated when non-negative. */
    int32_t drm_render_fd;
    uint32_t drm_render_major;
    uint32_t drm_render_minor;
} md_vk_export_context_t;

typedef struct md_vk_export_pool_info {
    uint64_t generation;
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
} md_vk_export_pool_info_t;

/* Uses caller-owned Vulkan handles; the returned exporter is caller-owned. */
md_vk_exporter_t* md_vk_exporter_new(const md_vk_export_context_t* context);
void md_vk_exporter_free(md_vk_exporter_t* exporter);

/*
 * Enumerates exact DMA-BUF modifiers that the exporter can allocate for the
 * requested extent with its transfer source/destination usage. Passing
 * caps=NULL and capacity=0 queries the count. The caller owns the output array;
 * no returned data is retained, and this read-only query is thread-safe with
 * respect to unrelated Vulkan objects owned by the caller.
 */
md_result_t md_vk_query_export_format_caps(VkPhysicalDevice physical_device,
                                           uint32_t fourcc, uint32_t width,
                                           uint32_t height,
                                           md_format_cap_t* caps,
                                           uint32_t capacity,
                                           uint32_t* out_count);

/* Replaces the current pool after waiting for caller-owned device work to finish. */
md_result_t md_vk_exporter_create_pool(md_vk_exporter_t* exporter,
                                       const md_vk_export_pool_info_t* info);
void md_vk_exporter_release_pool(md_vk_exporter_t* exporter);

/* Borrowed descriptor and image views remain valid until pool replacement. */
const md_buffer_pool_t* md_vk_exporter_pool(const md_vk_exporter_t* exporter);
VkImage md_vk_exporter_image(const md_vk_exporter_t* exporter, uint32_t buffer_index);
VkFormat md_vk_exporter_format(const md_vk_exporter_t* exporter);

/*
 * Polls release syncobjs and returns a free slot. MD_ERR_WOULD_BLOCK means
 * every slot is still owned by a consumer. No slot is reused before release.
 */
md_result_t md_vk_exporter_acquire(md_vk_exporter_t* exporter,
                                   uint32_t* out_buffer_index);

/*
 * Exports a signaled binary Vulkan semaphore as a sync_file and creates a new
 * unsignaled binary DRM syncobj for the consumer release. Both returned FDs
 * are caller-owned and intended for md_producer_submit_frame().
 */
md_result_t md_vk_exporter_export_frame(md_vk_exporter_t* exporter,
                                        uint32_t buffer_index,
                                        VkSemaphore acquire_semaphore,
                                        int32_t* out_acquire_sync_fd,
                                        int32_t* out_release_syncobj_fd);

/*
 * GPU-copies a caller-owned RGBA image into an acquired export slot. The
 * source layout is restored before completion. The destination is acquired
 * from and released back to VK_QUEUE_FAMILY_FOREIGN_EXT as needed. Returned
 * descriptors have the same ownership contract as export_frame().
 */
md_result_t md_vk_exporter_copy_frame(md_vk_exporter_t* exporter,
                                      uint32_t buffer_index,
                                      VkImage source_image,
                                      VkImageLayout source_layout,
                                      uint32_t source_width,
                                      uint32_t source_height,
                                      int32_t* out_acquire_sync_fd,
                                      int32_t* out_release_syncobj_fd);

/*
 * Copies a caller-owned host-visible RGBA staging buffer directly into an
 * acquired export slot.  The buffer must contain tightly packed rows of
 * source_width*4 bytes and remain mapped until this call returns.  The
 * exporter owns the asynchronous GPU submission; returned FDs are consumed by
 * md_producer_submit_frame and must not be closed by the producer afterwards.
 */
md_result_t md_vk_exporter_copy_buffer_frame(md_vk_exporter_t* exporter,
                                             uint32_t buffer_index,
                                             VkBuffer source_buffer,
                                             uint32_t source_width,
                                             uint32_t source_height,
                                             int32_t* out_acquire_sync_fd,
                                             int32_t* out_release_syncobj_fd);

/* Rolls a slot back when frame submission failed after export_frame(). */
void md_vk_exporter_cancel_frame(md_vk_exporter_t* exporter, uint32_t buffer_index);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
