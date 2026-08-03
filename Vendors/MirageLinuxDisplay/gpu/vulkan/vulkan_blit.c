#include "mirage_display_vulkan_blit.h"
#include "vulkan_util.h"

#include <stdlib.h>
#include <string.h>

struct md_vk_blitter {
    md_vk_blit_context_t context;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    VkImage image;
    VkDeviceMemory memory;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    bool content_valid;
};

static void destroy_shadow(md_vk_blitter_t* blitter) {
    if (blitter->image != VK_NULL_HANDLE) {
        vkDestroyImage(blitter->context.device, blitter->image, NULL);
    }
    if (blitter->memory != VK_NULL_HANDLE) {
        vkFreeMemory(blitter->context.device, blitter->memory, NULL);
    }
    blitter->image = VK_NULL_HANDLE;
    blitter->memory = VK_NULL_HANDLE;
    blitter->width = 0;
    blitter->height = 0;
    blitter->format = VK_FORMAT_UNDEFINED;
    blitter->content_valid = false;
}

static int ensure_shadow(md_vk_blitter_t* blitter, uint32_t width, uint32_t height,
                         VkFormat format) {
    if (blitter->image != VK_NULL_HANDLE && blitter->width == width &&
        blitter->height == height && blitter->format == format) {
        return MD_OK;
    }
    if (vkDeviceWaitIdle(blitter->context.device) != VK_SUCCESS) return MD_ERR_IO;
    destroy_shadow(blitter);

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(blitter->context.device, &image_info, NULL, &blitter->image);
    if (result != VK_SUCCESS) return MD_ERR_UNSUPPORTED;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(blitter->context.device, blitter->image, &requirements);
    uint32_t memory_type = md_vk_choose_memory_type(blitter->context.physical_device,
                                              requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        destroy_shadow(blitter);
        return MD_ERR_UNSUPPORTED;
    }
    VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = NULL,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    result = vkAllocateMemory(blitter->context.device, &allocation_info, NULL, &blitter->memory);
    if (result != VK_SUCCESS) {
        destroy_shadow(blitter);
        return MD_ERR_IO;
    }
    result = vkBindImageMemory(blitter->context.device, blitter->image, blitter->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_shadow(blitter);
        return MD_ERR_IO;
    }
    blitter->width = width;
    blitter->height = height;
    blitter->format = format;
    return MD_OK;
}

md_vk_blitter_t* md_vk_blitter_new(const md_vk_blit_context_t* context) {
    if (context == NULL || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE || context->queue == VK_NULL_HANDLE) {
        return NULL;
    }
    md_vk_blitter_t* blitter = calloc(1, sizeof(*blitter));
    if (blitter == NULL) return NULL;
    blitter->context = *context;
    blitter->format = VK_FORMAT_UNDEFINED;

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = context->queue_family_index,
    };
    if (vkCreateCommandPool(context->device, &pool_info, NULL, &blitter->command_pool) !=
        VK_SUCCESS) {
        free(blitter);
        return NULL;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = NULL,
        .commandPool = blitter->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(context->device, &command_info, &blitter->command_buffer) !=
        VK_SUCCESS) {
        md_vk_blitter_free(blitter);
        return NULL;
    }
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
    };
    if (vkCreateFence(context->device, &fence_info, NULL, &blitter->fence) != VK_SUCCESS) {
        md_vk_blitter_free(blitter);
        return NULL;
    }
    return blitter;
}

void md_vk_blitter_free(md_vk_blitter_t* blitter) {
    if (blitter == NULL) return;
    if (blitter->context.device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(blitter->context.device);
        destroy_shadow(blitter);
        if (blitter->fence != VK_NULL_HANDLE) {
            vkDestroyFence(blitter->context.device, blitter->fence, NULL);
        }
        if (blitter->command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(blitter->context.device, blitter->command_pool, NULL);
        }
    }
    free(blitter);
}

int md_vk_blitter_blit(md_vk_blitter_t* blitter, const md_vk_imported_pool_t* pool,
                       uint32_t buffer_index, VkSemaphore acquire_semaphore,
                       VkSemaphore release_semaphore) {
    if (blitter == NULL || pool == NULL || buffer_index >= pool->buffer_count ||
        pool->images[buffer_index] == VK_NULL_HANDLE || pool->width == 0 || pool->height == 0 ||
        pool->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        return MD_ERR_INVALID;
    }
    int rc = ensure_shadow(blitter, pool->width, pool->height, pool->format);
    if (rc != MD_OK) return rc;

    if (vkResetCommandPool(blitter->context.device, blitter->command_pool, 0) != VK_SUCCESS ||
        vkResetFences(blitter->context.device, 1, &blitter->fence) != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = NULL,
    };
    if (vkBeginCommandBuffer(blitter->command_buffer, &begin_info) != VK_SUCCESS) return MD_ERR_IO;

    VkImageMemoryBarrier pre[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = blitter->context.queue_family_index,
            .image = pool->images[buffer_index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = blitter->content_valid ? VK_ACCESS_SHADER_READ_BIT : 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = blitter->content_valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = blitter->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
    };
    vkCmdPipelineBarrier(blitter->command_buffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                             (blitter->content_valid ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

    VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {pool->width, pool->height, 1},
    };
    vkCmdCopyImage(blitter->command_buffer, pool->images[buffer_index], VK_IMAGE_LAYOUT_GENERAL,
                   blitter->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier post[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = blitter->context.queue_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image = pool->images[buffer_index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = blitter->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
    };
    vkCmdPipelineBarrier(blitter->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 2, post);
    if (vkEndCommandBuffer(blitter->command_buffer) != VK_SUCCESS) return MD_ERR_IO;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = acquire_semaphore != VK_NULL_HANDLE ? 1u : 0u,
        .pWaitSemaphores = acquire_semaphore != VK_NULL_HANDLE ? &acquire_semaphore : NULL,
        .pWaitDstStageMask = acquire_semaphore != VK_NULL_HANDLE ? &wait_stage : NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &blitter->command_buffer,
        .signalSemaphoreCount = release_semaphore != VK_NULL_HANDLE ? 1u : 0u,
        .pSignalSemaphores = release_semaphore != VK_NULL_HANDLE ? &release_semaphore : NULL,
    };
    if (vkQueueSubmit(blitter->context.queue, 1, &submit, blitter->fence) != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    VkResult wait_result = vkWaitForFences(blitter->context.device, 1, &blitter->fence, VK_TRUE,
                                           UINT64_C(2000000000));
    if (wait_result != VK_SUCCESS) return wait_result == VK_TIMEOUT ? MD_ERR_WOULD_BLOCK : MD_ERR_IO;
    blitter->content_valid = true;
    return MD_OK;
}

VkImage md_vk_blitter_image(const md_vk_blitter_t* blitter) {
    return blitter != NULL ? blitter->image : VK_NULL_HANDLE;
}

VkImageLayout md_vk_blitter_layout(const md_vk_blitter_t* blitter) {
    (void)blitter;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkFormat md_vk_blitter_format(const md_vk_blitter_t* blitter) {
    return blitter != NULL ? blitter->format : VK_FORMAT_UNDEFINED;
}

uint32_t md_vk_blitter_width(const md_vk_blitter_t* blitter) {
    return blitter != NULL ? blitter->width : 0;
}

uint32_t md_vk_blitter_height(const md_vk_blitter_t* blitter) {
    return blitter != NULL ? blitter->height : 0;
}

bool md_vk_blitter_has_content(const md_vk_blitter_t* blitter) {
    return blitter != NULL && blitter->content_valid;
}

