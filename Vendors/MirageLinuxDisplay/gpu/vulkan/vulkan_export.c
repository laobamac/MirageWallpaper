#define _GNU_SOURCE

#include "mirage_display_vulkan_export.h"
#include "mirage_display_vulkan.h"

#include "common/drm.h"
#include "common/util.h"
#include "vulkan_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct md_vk_export_slot {
    VkImage image;
    VkDeviceMemory memory;
    uint32_t release_handle;
    bool acquired;
    bool busy;
    bool foreign_owned;
} md_vk_export_slot_t;

struct md_vk_exporter {
    md_vk_export_context_t context;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
    int drm_fd;
    md_buffer_pool_t pool;
    md_vk_export_slot_t slots[MIRAGE_DISPLAY_MAX_BUFFERS];
    VkFormat format;
    uint32_t cursor;
    bool pool_active;
    VkCommandPool copy_command_pool;
    VkCommandBuffer copy_command_buffer;
    VkFence copy_fence;
    VkSemaphore copy_semaphore;
    bool copy_fence_pending;
};

static void destroy_release_handle(md_vk_exporter_t* exporter, uint32_t index) {
    md_vk_export_slot_t* slot = &exporter->slots[index];
    md_drm_destroy_syncobj(exporter->drm_fd, slot->release_handle);
    slot->release_handle = 0;
    slot->acquired = false;
    slot->busy = false;
}

static int poll_release(md_vk_exporter_t* exporter, uint32_t index) {
    md_vk_export_slot_t* slot = &exporter->slots[index];
    if (!slot->busy) return MD_OK;
    uint32_t handles[1] = {slot->release_handle};
    struct md_drm_syncobj_wait wait = {
        .handles = (uint64_t)(uintptr_t)handles,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = MD_DRM_SYNCOBJ_WAIT_ALL,
        .first_signaled = 0,
        .pad = 0,
        .deadline_nsec = 0,
    };
    if (ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0) {
        destroy_release_handle(exporter, index);
        return MD_OK;
    }
    if (errno == ETIME || errno == EBUSY) return MD_ERR_WOULD_BLOCK;
    return MD_ERR_IO;
}

md_vk_exporter_t* md_vk_exporter_new(const md_vk_export_context_t* context) {
    if (context == NULL || context->instance == VK_NULL_HANDLE ||
        context->physical_device == VK_NULL_HANDLE || context->device == VK_NULL_HANDLE ||
        context->queue == VK_NULL_HANDLE) {
        return NULL;
    }
    md_vk_exporter_t* exporter = calloc(1, sizeof(*exporter));
    if (exporter == NULL) return NULL;
    exporter->context = *context;
    exporter->drm_fd = context->drm_render_fd >= 0
                           ? fcntl(context->drm_render_fd, F_DUPFD_CLOEXEC, 0)
                           : md_drm_open_render_node(0, context->drm_render_minor);
    if (exporter->drm_fd < 0) {
        free(exporter);
        return NULL;
    }
    exporter->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(
        context->device, "vkGetMemoryFdKHR");
    exporter->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(
        context->device, "vkGetSemaphoreFdKHR");
    exporter->get_modifier_properties =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            context->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (exporter->get_memory_fd == NULL || exporter->get_semaphore_fd == NULL ||
        exporter->get_modifier_properties == NULL) {
        close(exporter->drm_fd);
        free(exporter);
        return NULL;
    }
    md_init_pool(&exporter->pool);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = context->queue_family_index,
    };
    if (vkCreateCommandPool(context->device, &pool_info, NULL,
                            &exporter->copy_command_pool) != VK_SUCCESS) {
        md_vk_exporter_free(exporter);
        return NULL;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = NULL,
        .commandPool = exporter->copy_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(context->device, &command_info,
                                 &exporter->copy_command_buffer) != VK_SUCCESS) {
        md_vk_exporter_free(exporter);
        return NULL;
    }
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
    };
    if (vkCreateFence(context->device, &fence_info, NULL,
                      &exporter->copy_fence) != VK_SUCCESS) {
        md_vk_exporter_free(exporter);
        return NULL;
    }
    VkExportSemaphoreCreateInfo export_semaphore = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext = NULL,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_semaphore,
        .flags = 0,
    };
    if (vkCreateSemaphore(context->device, &semaphore_info, NULL,
                          &exporter->copy_semaphore) != VK_SUCCESS) {
        md_vk_exporter_free(exporter);
        return NULL;
    }
    return exporter;
}

void md_vk_exporter_release_pool(md_vk_exporter_t* exporter) {
    if (exporter == NULL) return;
    if (exporter->pool_active) (void)vkDeviceWaitIdle(exporter->context.device);
    for (uint32_t b = 0; b < MIRAGE_DISPLAY_MAX_BUFFERS; ++b) {
        destroy_release_handle(exporter, b);
        exporter->slots[b].foreign_owned = false;
        for (uint32_t p = 0; p < MIRAGE_DISPLAY_MAX_PLANES; ++p) {
            if (exporter->pool.planes[b][p].fd >= 0) {
                close(exporter->pool.planes[b][p].fd);
                exporter->pool.planes[b][p].fd = -1;
            }
        }
        if (exporter->slots[b].image != VK_NULL_HANDLE) {
            vkDestroyImage(exporter->context.device, exporter->slots[b].image, NULL);
            exporter->slots[b].image = VK_NULL_HANDLE;
        }
        if (exporter->slots[b].memory != VK_NULL_HANDLE) {
            vkFreeMemory(exporter->context.device, exporter->slots[b].memory, NULL);
            exporter->slots[b].memory = VK_NULL_HANDLE;
        }
    }
    md_init_pool(&exporter->pool);
    exporter->format = VK_FORMAT_UNDEFINED;
    exporter->cursor = 0;
    exporter->pool_active = false;
    exporter->copy_fence_pending = false;
}

void md_vk_exporter_free(md_vk_exporter_t* exporter) {
    if (exporter == NULL) return;
    md_vk_exporter_release_pool(exporter);
    if (exporter->copy_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(exporter->context.device, exporter->copy_semaphore, NULL);
    }
    if (exporter->copy_fence != VK_NULL_HANDLE) {
        vkDestroyFence(exporter->context.device, exporter->copy_fence, NULL);
    }
    if (exporter->copy_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(exporter->context.device, exporter->copy_command_pool, NULL);
    }
    if (exporter->drm_fd >= 0) close(exporter->drm_fd);
    free(exporter);
}

static int allocate_slot(md_vk_exporter_t* exporter, const md_vk_export_pool_info_t* info,
                         uint32_t index) {
    uint64_t modifiers[1] = {info->modifier};
    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = modifiers,
    };
    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = exporter->format,
        .extent = {info->width, info->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(exporter->context.device, &image_info, NULL, &image) != VK_SUCCESS) {
        return MD_ERR_UNSUPPORTED;
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(exporter->context.device, image, &requirements);
    uint32_t memory_type = md_vk_choose_memory_type(
        exporter->context.physical_device, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        memory_type = md_vk_choose_memory_type(
            exporter->context.physical_device, requirements.memoryTypeBits, 0);
    }
    if (memory_type == UINT32_MAX) {
        vkDestroyImage(exporter->context.device, image, NULL);
        return MD_ERR_UNSUPPORTED;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = NULL,
        .image = image,
        .buffer = VK_NULL_HANDLE,
    };
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(exporter->context.device, &allocation, NULL, &memory) != VK_SUCCESS ||
        vkBindImageMemory(exporter->context.device, image, memory, 0) != VK_SUCCESS) {
        if (memory != VK_NULL_HANDLE) vkFreeMemory(exporter->context.device, memory, NULL);
        vkDestroyImage(exporter->context.device, image, NULL);
        return MD_ERR_NOMEM;
    }
    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = NULL,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int dma_buf_fd = -1;
    if (exporter->get_memory_fd(exporter->context.device, &fd_info, &dma_buf_fd) != VK_SUCCESS) {
        vkFreeMemory(exporter->context.device, memory, NULL);
        vkDestroyImage(exporter->context.device, image, NULL);
        return MD_ERR_IO;
    }

    VkImageDrmFormatModifierPropertiesEXT modifier_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        .pNext = NULL,
        .drmFormatModifier = 0,
    };
    if (exporter->get_modifier_properties(exporter->context.device, image,
                                           &modifier_properties) != VK_SUCCESS ||
        modifier_properties.drmFormatModifier != info->modifier) {
        close(dma_buf_fd);
        vkFreeMemory(exporter->context.device, memory, NULL);
        vkDestroyImage(exporter->context.device, image, NULL);
        return MD_ERR_UNSUPPORTED;
    }

    exporter->slots[index].image = image;
    exporter->slots[index].memory = memory;
    for (uint32_t plane = 0; plane < info->plane_count; ++plane) {
        int plane_fd = plane == 0 ? dma_buf_fd : fcntl(dma_buf_fd, F_DUPFD_CLOEXEC, 0);
        if (plane_fd < 0) {
            if (plane == 0) close(dma_buf_fd);
            return MD_ERR_IO;
        }
        VkImageSubresource subresource = {
            .aspectMask = md_vk_memory_plane_aspect(plane),
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(exporter->context.device, image, &subresource, &layout);
        if (layout.rowPitch > UINT32_MAX || layout.offset > UINT32_MAX) {
            close(plane_fd);
            return MD_ERR_UNSUPPORTED;
        }
        exporter->pool.planes[index][plane] = (md_plane_t) {
            .fd = plane_fd,
            .stride = (uint32_t)layout.rowPitch,
            .offset = (uint32_t)layout.offset,
            .size = layout.size != 0 ? layout.size : requirements.size,
        };
    }
    return MD_OK;
}

int md_vk_exporter_create_pool(md_vk_exporter_t* exporter,
                               const md_vk_export_pool_info_t* info) {
    if (exporter == NULL || info == NULL || info->generation == 0 ||
        info->buffer_count < 2 || info->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        info->width == 0 || info->height == 0 || info->plane_count == 0 ||
        info->plane_count > MIRAGE_DISPLAY_MAX_PLANES) return MD_ERR_INVALID;
    VkComponentMapping mapping;
    VkFormat format;
    int rc = md_vk_fourcc_to_format(info->fourcc, &format, &mapping);
    if (rc != MD_OK || format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        return MD_ERR_UNSUPPORTED;
    }
    (void)mapping;

    md_vk_exporter_release_pool(exporter);
    exporter->format = format;
    exporter->pool.generation = info->generation;
    exporter->pool.buffer_count = info->buffer_count;
    exporter->pool.width = info->width;
    exporter->pool.height = info->height;
    exporter->pool.fourcc = info->fourcc;
    exporter->pool.plane_count = info->plane_count;
    exporter->pool.modifier = info->modifier;
    for (uint32_t index = 0; index < info->buffer_count; ++index) {
        rc = allocate_slot(exporter, info, index);
        if (rc != MD_OK) {
            md_vk_exporter_release_pool(exporter);
            return rc;
        }
    }
    exporter->pool_active = true;
    return MD_OK;
}

const md_buffer_pool_t* md_vk_exporter_pool(const md_vk_exporter_t* exporter) {
    return exporter != NULL && exporter->pool_active ? &exporter->pool : NULL;
}

VkImage md_vk_exporter_image(const md_vk_exporter_t* exporter, uint32_t buffer_index) {
    if (exporter == NULL || !exporter->pool_active ||
        buffer_index >= exporter->pool.buffer_count) return VK_NULL_HANDLE;
    return exporter->slots[buffer_index].image;
}

VkFormat md_vk_exporter_format(const md_vk_exporter_t* exporter) {
    return exporter != NULL && exporter->pool_active ? exporter->format : VK_FORMAT_UNDEFINED;
}

int md_vk_exporter_acquire(md_vk_exporter_t* exporter, uint32_t* out_buffer_index) {
    if (exporter == NULL || out_buffer_index == NULL || !exporter->pool_active) {
        return MD_ERR_STATE;
    }
    int first_error = MD_OK;
    for (uint32_t offset = 0; offset < exporter->pool.buffer_count; ++offset) {
        uint32_t index = (exporter->cursor + offset) % exporter->pool.buffer_count;
        int rc = poll_release(exporter, index);
        if (rc == MD_OK && !exporter->slots[index].busy &&
            !exporter->slots[index].acquired) {
            exporter->cursor = (index + 1u) % exporter->pool.buffer_count;
            exporter->slots[index].acquired = true;
            *out_buffer_index = index;
            return MD_OK;
        }
        if (rc != MD_ERR_WOULD_BLOCK && first_error == MD_OK) first_error = rc;
    }
    return first_error != MD_OK ? first_error : MD_ERR_WOULD_BLOCK;
}

int md_vk_exporter_export_frame(md_vk_exporter_t* exporter, uint32_t buffer_index,
                                VkSemaphore acquire_semaphore, int* out_acquire_sync_fd,
                                int* out_release_syncobj_fd) {
    if (out_acquire_sync_fd != NULL) *out_acquire_sync_fd = -1;
    if (out_release_syncobj_fd != NULL) *out_release_syncobj_fd = -1;
    if (exporter == NULL || !exporter->pool_active || acquire_semaphore == VK_NULL_HANDLE ||
        out_acquire_sync_fd == NULL || out_release_syncobj_fd == NULL ||
        buffer_index >= exporter->pool.buffer_count || exporter->slots[buffer_index].busy ||
        !exporter->slots[buffer_index].acquired) {
        return MD_ERR_INVALID;
    }
    VkSemaphoreGetFdInfoKHR semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext = NULL,
        .semaphore = acquire_semaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int acquire_fd = -1;
    if (exporter->get_semaphore_fd(exporter->context.device, &semaphore_info, &acquire_fd) !=
        VK_SUCCESS) return MD_ERR_IO;

    struct md_drm_syncobj_create create = {.handle = 0, .flags = 0};
    if (ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
        close(acquire_fd);
        return MD_ERR_IO;
    }
    struct md_drm_syncobj_handle export_handle = {
        .handle = create.handle,
        .flags = 0,
        .fd = -1,
        .pad = 0,
    };
    if (ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export_handle) != 0) {
        struct md_drm_syncobj_destroy destroy = {.handle = create.handle, .pad = 0};
        (void)ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
        close(acquire_fd);
        return MD_ERR_IO;
    }
    exporter->slots[buffer_index].release_handle = create.handle;
    exporter->slots[buffer_index].acquired = false;
    exporter->slots[buffer_index].busy = true;
    *out_acquire_sync_fd = acquire_fd;
    *out_release_syncobj_fd = export_handle.fd;
    return MD_OK;
}

static int prepare_copy_commands(md_vk_exporter_t* exporter, uint32_t buffer_index,
                                 VkImage source_image, VkImageLayout source_layout) {
    if (exporter->copy_fence_pending) {
        if (vkWaitForFences(exporter->context.device, 1, &exporter->copy_fence,
                            VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            return MD_ERR_IO;
        }
        exporter->copy_fence_pending = false;
    }
    if (vkResetFences(exporter->context.device, 1, &exporter->copy_fence) != VK_SUCCESS ||
        vkResetCommandPool(exporter->context.device, exporter->copy_command_pool, 0) !=
            VK_SUCCESS) {
        return MD_ERR_IO;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = NULL,
    };
    if (vkBeginCommandBuffer(exporter->copy_command_buffer, &begin_info) != VK_SUCCESS) {
        return MD_ERR_IO;
    }

    md_vk_export_slot_t* slot = &exporter->slots[buffer_index];
    const VkAccessFlags source_access = source_layout == VK_IMAGE_LAYOUT_GENERAL
                                            ? VK_ACCESS_SHADER_WRITE_BIT
                                            : VK_ACCESS_SHADER_READ_BIT;
    VkImageMemoryBarrier before[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = source_access,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = source_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = slot->foreign_owned ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = slot->foreign_owned ? VK_QUEUE_FAMILY_FOREIGN_EXT
                                                       : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = slot->foreign_owned
                                       ? exporter->context.queue_family_index
                                       : VK_QUEUE_FAMILY_IGNORED,
            .image = slot->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
    };
    vkCmdPipelineBarrier(exporter->copy_command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, before);

    /* Copy only the negotiated pool region. The caller guarantees the
     * source is at least pool-sized (intermediate images may be
     * even-aligned while an output extent is odd), so the pool region is
     * always inside the source bounds. */
    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {exporter->pool.width, exporter->pool.height, 1},
    };
    vkCmdCopyImage(exporter->copy_command_buffer,
                   source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    VkImageMemoryBarrier after[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = source_access,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = source_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = exporter->context.queue_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image = slot->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
    };
    vkCmdPipelineBarrier(exporter->copy_command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 2, after);
    if (vkEndCommandBuffer(exporter->copy_command_buffer) != VK_SUCCESS) return MD_ERR_IO;
    return MD_OK;
}

int md_vk_exporter_copy_frame(md_vk_exporter_t* exporter, uint32_t buffer_index,
                              VkImage source_image, VkImageLayout source_layout,
                              uint32_t source_width, uint32_t source_height,
                              int* out_acquire_sync_fd, int* out_release_syncobj_fd) {
    if (out_acquire_sync_fd != NULL) *out_acquire_sync_fd = -1;
    if (out_release_syncobj_fd != NULL) *out_release_syncobj_fd = -1;
    if (exporter == NULL || !exporter->pool_active || source_image == VK_NULL_HANDLE ||
        source_width == 0 || source_height == 0 ||
        source_width < exporter->pool.width || source_height < exporter->pool.height ||
        out_acquire_sync_fd == NULL ||
        out_release_syncobj_fd == NULL || buffer_index >= exporter->pool.buffer_count ||
        exporter->slots[buffer_index].busy || !exporter->slots[buffer_index].acquired) {
        return MD_ERR_INVALID;
    }
    int rc = prepare_copy_commands(exporter, buffer_index, source_image, source_layout);
    if (rc != MD_OK) return rc;

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = NULL,
        .pWaitDstStageMask = NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &exporter->copy_command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &exporter->copy_semaphore,
    };
    if (vkQueueSubmit(exporter->context.queue, 1, &submit_info,
                      exporter->copy_fence) != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    exporter->copy_fence_pending = true;
    exporter->slots[buffer_index].foreign_owned = true;

    VkSemaphoreGetFdInfoKHR semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext = NULL,
        .semaphore = exporter->copy_semaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int acquire_fd = -1;
    if (exporter->get_semaphore_fd(exporter->context.device, &semaphore_info,
                                   &acquire_fd) != VK_SUCCESS) {
        md_vk_exporter_cancel_frame(exporter, buffer_index);
        return MD_ERR_IO;
    }

    struct md_drm_syncobj_create create = {.handle = 0, .flags = 0};
    if (ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
        close(acquire_fd);
        md_vk_exporter_cancel_frame(exporter, buffer_index);
        return MD_ERR_IO;
    }
    struct md_drm_syncobj_handle export_handle = {
        .handle = create.handle,
        .flags = 0,
        .fd = -1,
        .pad = 0,
    };
    if (ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,
              &export_handle) != 0) {
        struct md_drm_syncobj_destroy destroy = {.handle = create.handle, .pad = 0};
        (void)ioctl(exporter->drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
        close(acquire_fd);
        md_vk_exporter_cancel_frame(exporter, buffer_index);
        return MD_ERR_IO;
    }
    exporter->slots[buffer_index].release_handle = create.handle;
    exporter->slots[buffer_index].acquired = false;
    exporter->slots[buffer_index].busy = true;
    *out_acquire_sync_fd = acquire_fd;
    *out_release_syncobj_fd = export_handle.fd;
    return MD_OK;
}

void md_vk_exporter_cancel_frame(md_vk_exporter_t* exporter, uint32_t buffer_index) {
    if (exporter == NULL || buffer_index >= MIRAGE_DISPLAY_MAX_BUFFERS) return;
    destroy_release_handle(exporter, buffer_index);
}


