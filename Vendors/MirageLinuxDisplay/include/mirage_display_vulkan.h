#ifndef MIRAGE_DISPLAY_VULKAN_H
#define MIRAGE_DISPLAY_VULKAN_H

#include "mirage_display.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct md_vk_importer md_vk_importer_t;

typedef struct md_vk_context {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family_index;
    VkImageUsageFlags image_usage;
} md_vk_context_t;

typedef struct md_vk_imported_pool {
    uint64_t generation;
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint64_t modifier;
    VkFormat format;
    VkImage images[MIRAGE_DISPLAY_MAX_BUFFERS];
    /* Plane zero aliases plane_memories[][0] for source compatibility. */
    VkDeviceMemory memories[MIRAGE_DISPLAY_MAX_BUFFERS];
    VkDeviceMemory plane_memories[MIRAGE_DISPLAY_MAX_BUFFERS][MIRAGE_DISPLAY_MAX_PLANES];
    VkImageView views[MIRAGE_DISPLAY_MAX_BUFFERS];
    VkSemaphore acquire_semaphores[MIRAGE_DISPLAY_MAX_BUFFERS];
    VkSemaphore release_semaphores[MIRAGE_DISPLAY_MAX_BUFFERS];
    /* Non-null for formats such as NV12 whose image views require conversion. */
    VkSamplerYcbcrConversion ycbcr_conversion;
} md_vk_imported_pool_t;

/* Creates an importer using an already-created Vulkan instance/device. */
md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* context);
void md_vk_importer_free(md_vk_importer_t* importer);

/* The caller must ensure no queue submission still references the old pool. */
int md_vk_importer_import_pool(md_vk_importer_t* importer, const md_buffer_pool_t* pool);
void md_vk_importer_release_pool(md_vk_importer_t* importer);
const md_vk_imported_pool_t* md_vk_importer_pool(const md_vk_importer_t* importer);

/*
 * Imports and consumes one frame synchronization FD. The acquire import is
 * temporary and is waited by the next submit. The release import is an opaque
 * binary semaphore that the consumer must signal from the final read submit.
 * On every return path ownership of fd is consumed and it must not be closed
 * by the caller.
 */
int md_vk_import_acquire_sync(md_vk_importer_t* importer, uint32_t buffer_index,
                              int acquire_sync_fd, VkSemaphore* out_semaphore);
int md_vk_import_release_syncobj(md_vk_importer_t* importer, uint32_t buffer_index,
                                 int release_syncobj_fd, VkSemaphore* out_semaphore);

/* GENERAL-layout queue-family ownership barriers required by protocol v1. */
int md_vk_importer_acquire_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags destination_access,
                                   VkImageMemoryBarrier* out_barrier);
int md_vk_importer_release_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags source_access,
                                   VkImageMemoryBarrier* out_barrier);

/* DRM fourcc mapping supported by the first Vulkan backend revision. */
int md_vk_fourcc_to_format(uint32_t fourcc, VkFormat* format, VkComponentMapping* mapping);
/*
 * Enumerates DRM modifiers that support all required tiling features. Passing
 * caps=NULL and capacity=0 queries the count. Each returned plane_count is the
 * modifier memory-plane count required by the explicit layout import.
 */
int md_vk_query_format_caps(VkPhysicalDevice physical_device, uint32_t fourcc,
                            VkFormatFeatureFlags required_features,
                            md_format_cap_t* caps, uint32_t capacity,
                            uint32_t* out_count);
const char* md_vk_result_string(VkResult result);

#ifdef __cplusplus
}
#endif

#endif
