#ifndef MIRAGE_DISPLAY_VULKAN_H
#define MIRAGE_DISPLAY_VULKAN_H

#include "mirage_display.h"

#include <vulkan/vulkan.h>

/*
 * Public C ABI for the Vulkan DMA-BUF import helper.
 *
 * Consumers with an existing Vulkan instance/device import protocol buffer pools
 * through external memory FDs / DRM modifiers, create images, views, and
 * optional YCbCr conversions, and import the per-frame acquire/release
 * synchronization as binary semaphores.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan DTOs have an explicit maximum alignment at the language boundary. */
#pragma pack(push, 8)

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

/* The stage inside the importer that failed, for diagnostics. */
typedef enum md_vk_import_stage {
    MD_VK_IMPORT_STAGE_NONE = 0,
    MD_VK_IMPORT_STAGE_POOL_INVALID,      /* md_buffer_pool_t validation failed */
    MD_VK_IMPORT_STAGE_FORMAT,            /* DRM fourcc has no Vulkan format */
    MD_VK_IMPORT_STAGE_YCBCR,             /* vkCreateSamplerYcbcrConversion failed */
    MD_VK_IMPORT_STAGE_IMAGE_CREATE,      /* vkCreateImage failed */
    MD_VK_IMPORT_STAGE_MEMORY_PROPERTIES, /* vkGetMemoryFdPropertiesKHR failed */
    MD_VK_IMPORT_STAGE_MEMORY_TYPES,      /* no memory type accepts the DMA-BUF */
    MD_VK_IMPORT_STAGE_MEMORY_ALLOCATE,   /* vkAllocateMemory failed on every candidate */
    MD_VK_IMPORT_STAGE_MEMORY_BIND,       /* vkBindImageMemory2 failed */
    MD_VK_IMPORT_STAGE_VIEW,              /* vkCreateImageView failed */
    MD_VK_IMPORT_STAGE_SEMAPHORE,         /* vkCreateSemaphore failed */
    MD_VK_IMPORT_STAGE_SYNC_IMPORT,       /* vkImportSemaphoreFdKHR failed */
} md_vk_import_stage_t;

/*
 * Structured record of the first failure inside md_vk_importer_import_pool
 * (and the acquire/release sync imports). Fields not applicable to the failing
 * stage are zeroed; buffer_index/plane_index are UINT32_MAX and candidate_count
 * is -1 when they do not apply.
 */
typedef struct md_vk_import_error {
    md_vk_import_stage_t stage;
    VkResult vk_result;      /* the failing VkResult; VK_SUCCESS for non-VkResult failures */
    int32_t sys_errno;       /* errno for fd-io failures; 0 otherwise */
    uint32_t buffer_index;   /* failing buffer; UINT32_MAX when not applicable */
    uint32_t plane_index;    /* failing plane; UINT32_MAX when not applicable */
    uint64_t modifier;       /* negotiated DRM modifier at failure time */
    uint32_t fourcc;         /* negotiated DRM fourcc at failure time */
    int32_t candidate_count; /* memory types tried for MEMORY_ALLOCATE; -1 otherwise */
} md_vk_import_error_t;

/* Creates a caller-owned importer using an already-created Vulkan instance/device. */
md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* context);
void md_vk_importer_free(md_vk_importer_t* importer);

/* The caller must ensure no queue submission still references the old pool. */
md_result_t md_vk_importer_import_pool(md_vk_importer_t* importer,
                                       const md_buffer_pool_t* pool);
void md_vk_importer_release_pool(md_vk_importer_t* importer);
const md_vk_imported_pool_t* md_vk_importer_pool(const md_vk_importer_t* importer);

/*
 * Returns the structured record of the most recent import failure, or NULL when
 * no import has failed since the importer was created or the last successful
 * import.  The record is overwritten by every subsequent failed import.
 */
const md_vk_import_error_t* md_vk_importer_last_error(const md_vk_importer_t* importer);
/* Human-readable English phrase for a stage; never returns NULL. */
const char* md_vk_import_stage_string(md_vk_import_stage_t stage);

/*
 * Imports and consumes one frame synchronization FD. The acquire import is
 * temporary and is waited by the next submit. The release import is an opaque
 * binary semaphore that the consumer must signal from the final read submit.
 * On every return path ownership of fd is consumed and it must not be closed
 * by the caller.
 */
md_result_t md_vk_import_acquire_sync(md_vk_importer_t* importer, uint32_t buffer_index,
                                      int32_t acquire_sync_fd,
                                      VkSemaphore* out_semaphore);
md_result_t md_vk_import_release_syncobj(md_vk_importer_t* importer,
                                         uint32_t buffer_index,
                                         int32_t release_syncobj_fd,
                                         VkSemaphore* out_semaphore);

/* GENERAL-layout queue-family ownership barriers required by protocol v1. */
md_result_t md_vk_importer_acquire_barrier(const md_vk_importer_t* importer,
                                           uint32_t buffer_index,
                                           VkAccessFlags destination_access,
                                           VkImageMemoryBarrier* out_barrier);
md_result_t md_vk_importer_release_barrier(const md_vk_importer_t* importer,
                                           uint32_t buffer_index,
                                           VkAccessFlags source_access,
                                           VkImageMemoryBarrier* out_barrier);

/* DRM fourcc mapping supported by the first Vulkan backend revision. */
md_result_t md_vk_fourcc_to_format(uint32_t fourcc, VkFormat* format,
                                   VkComponentMapping* mapping);
/*
 * Enumerates DRM modifiers that support all required tiling features. Passing
 * caps=NULL and capacity=0 queries the count. Each returned plane_count is the
 * modifier memory-plane count required by the explicit layout import.
 */
md_result_t md_vk_query_format_caps(VkPhysicalDevice physical_device, uint32_t fourcc,
                                    VkFormatFeatureFlags required_features,
                                    md_format_cap_t* caps, uint32_t capacity,
                                    uint32_t* out_count);

/* Result of probing whether a device can import the protocol's DMA-BUFs. */
typedef enum md_vk_dma_buf_import_state {
    MD_VK_DMA_BUF_IMPORT_OK = 0,             /* device can import protocol DMA-BUFs */
    MD_VK_DMA_BUF_IMPORT_DRIVER_UNSUPPORTED, /* driver lacks required extensions */
    MD_VK_DMA_BUF_IMPORT_DEVICE_NOT_ENABLED, /* extensions exist but are not enabled on the device */
    MD_VK_DMA_BUF_IMPORT_UNAVAILABLE,        /* Vulkan entry points are not resolvable */
} md_vk_dma_buf_import_state_t;

/*
 * Probes whether the device can import the DMA-BUFs carried by the display
 * protocol. On MD_VK_DMA_BUF_IMPORT_DRIVER_UNSUPPORTED, missing_extensions
 * receives a comma-separated list of required extensions the driver does not
 * expose (pass NULL/0 to skip). out_state is always written. Returns MD_OK on
 * success, MD_ERR_INVALID for NULL handles.
 */
md_result_t md_vk_query_dma_buf_import_support(VkPhysicalDevice physical_device,
                                               VkDevice device,
                                               md_vk_dma_buf_import_state_t* out_state,
                                               char* missing_extensions, size_t capacity);
/* Human-readable English phrase for a probe state; never returns NULL. */
const char* md_vk_dma_buf_import_state_string(md_vk_dma_buf_import_state_t state);

const char* md_vk_result_string(VkResult result);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
