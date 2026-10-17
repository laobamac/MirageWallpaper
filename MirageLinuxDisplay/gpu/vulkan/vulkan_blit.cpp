#include "mirage_display_vulkan_blit.h"

#include "vulkan_util.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>

/*
 * Same-device relay/blit fallback (include/mirage_display_vulkan_blit.h).
 *
 * Copies an imported RGB image into a host-owned optimal image so Qt Quick
 * consumers can sample formats that cannot be sampled directly; semaphores
 * remain importer-owned.
 */

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

namespace {

/* The shadow image is private to this blitter and must outlive its memory. */
void destroy_shadow(md_vk_blitter_t* const blitter) {
    if (blitter->image != VK_NULL_HANDLE) {
        vkDestroyImage(blitter->context.device, blitter->image, nullptr);
    }
    if (blitter->memory != VK_NULL_HANDLE) {
        vkFreeMemory(blitter->context.device, blitter->memory, nullptr);
    }
    blitter->image = VK_NULL_HANDLE;
    blitter->memory = VK_NULL_HANDLE;
    blitter->width = 0U;
    blitter->height = 0U;
    blitter->format = VK_FORMAT_UNDEFINED;
    blitter->content_valid = false;
}


/*
 * Allocates or reallocates the host-owned optimal shadow image and its memory
 * so the blitter always has a sampler-compatible destination.
 */
md_result_t ensure_shadow(md_vk_blitter_t* const blitter, const uint32_t width,
                          const uint32_t height, const VkFormat format) {
    if (blitter->image != VK_NULL_HANDLE && blitter->width == width &&
        blitter->height == height && blitter->format == format) {
        return MD_OK;
    }
    if (vkDeviceWaitIdle(blitter->context.device) != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    destroy_shadow(blitter);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = VkExtent3D{width, height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult image_result =
        vkCreateImage(blitter->context.device, &image_info, nullptr, &blitter->image);
    if (image_result != VK_SUCCESS) {
        return MD_ERR_UNSUPPORTED;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(blitter->context.device, blitter->image, &requirements);
    const std::optional<uint32_t> memory_type = mirage::vulkan::choose_memory_type(
        blitter->context.physical_device, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type.has_value()) {
        destroy_shadow(blitter);
        return MD_ERR_UNSUPPORTED;
    }

    VkMemoryAllocateInfo allocation_info{};
    allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation_info.allocationSize = requirements.size;
    allocation_info.memoryTypeIndex = memory_type.value();
    const VkResult allocation_result = vkAllocateMemory(blitter->context.device,
                                                         &allocation_info, nullptr,
                                                         &blitter->memory);
    if (allocation_result != VK_SUCCESS) {
        destroy_shadow(blitter);
        return MD_ERR_IO;
    }
    const VkResult bind_result = vkBindImageMemory(blitter->context.device, blitter->image,
                                                   blitter->memory, 0U);
    if (bind_result != VK_SUCCESS) {
        destroy_shadow(blitter);
        return MD_ERR_IO;
    }

    blitter->width = width;
    blitter->height = height;
    blitter->format = format;
    return MD_OK;
}

void configure_color_range(VkImageSubresourceRange* const range) {
    range->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range->baseMipLevel = 0U;
    range->levelCount = 1U;
    range->baseArrayLayer = 0U;
    range->layerCount = 1U;
}

}  // namespace

extern "C" md_vk_blitter_t* md_vk_blitter_new(const md_vk_blit_context_t* const context) {
    if (context == nullptr || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE || context->queue == VK_NULL_HANDLE) {
        return nullptr;
    }

    std::unique_ptr<md_vk_blitter_t> blitter{new (std::nothrow) md_vk_blitter_t{}};
    if (!blitter) {
        return nullptr;
    }
    blitter->context = *context;
    blitter->format = VK_FORMAT_UNDEFINED;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context->queue_family_index;
    if (vkCreateCommandPool(context->device, &pool_info, nullptr, &blitter->command_pool) !=
        VK_SUCCESS) {
        return nullptr;
    }

    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = blitter->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1U;
    if (vkAllocateCommandBuffers(context->device, &command_info, &blitter->command_buffer) !=
        VK_SUCCESS) {
        vkDestroyCommandPool(context->device, blitter->command_pool, nullptr);
        return nullptr;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(context->device, &fence_info, nullptr, &blitter->fence) != VK_SUCCESS) {
        vkDestroyCommandPool(context->device, blitter->command_pool, nullptr);
        return nullptr;
    }
    return blitter.release();
}

extern "C" void md_vk_blitter_free(md_vk_blitter_t* const blitter) {
    if (blitter == nullptr) {
        return;
    }
    if (blitter->context.device != VK_NULL_HANDLE) {
        if (vkDeviceWaitIdle(blitter->context.device) != VK_SUCCESS) {
            /* The public destructor cannot report failure; destroy only this
             * object's handles because their owner is being retired. */
        }
        destroy_shadow(blitter);
        if (blitter->fence != VK_NULL_HANDLE) {
            vkDestroyFence(blitter->context.device, blitter->fence, nullptr);
        }
        if (blitter->command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(blitter->context.device, blitter->command_pool, nullptr);
        }
    }
    delete blitter;
}


/*
 * Copies one imported frame into the shadow image, waits the acquire
 * semaphore, and signals the release semaphore only after the imported image is
 * released back to VK_QUEUE_FAMILY_FOREIGN_EXT.  Returns after device work
 * completes; semaphores stay importer-owned.
 */
extern "C" md_result_t md_vk_blitter_blit(md_vk_blitter_t* const blitter,
                                           const md_vk_imported_pool_t* const pool,
                                           const uint32_t buffer_index,
                                           const VkSemaphore acquire_semaphore,
                                           const VkSemaphore release_semaphore) {
    if (blitter == nullptr || pool == nullptr || buffer_index >= pool->buffer_count ||
        pool->images[buffer_index] == VK_NULL_HANDLE || pool->width == 0U ||
        pool->height == 0U || pool->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        return MD_ERR_INVALID;
    }
    const md_result_t shadow_result =
        ensure_shadow(blitter, pool->width, pool->height, pool->format);
    if (shadow_result != MD_OK) {
        return shadow_result;
    }

    if (vkResetCommandPool(blitter->context.device, blitter->command_pool, 0U) != VK_SUCCESS ||
        vkResetFences(blitter->context.device, 1U, &blitter->fence) != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(blitter->command_buffer, &begin_info) != VK_SUCCESS) {
        return MD_ERR_IO;
    }

    std::array<VkImageMemoryBarrier, 2U> pre{};
    pre[0U].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[0U].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[0U].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre[0U].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre[0U].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    pre[0U].dstQueueFamilyIndex = blitter->context.queue_family_index;
    pre[0U].image = pool->images[buffer_index];
    configure_color_range(&pre[0U].subresourceRange);

    pre[1U].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[1U].srcAccessMask = blitter->content_valid
                                 ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
                                 : static_cast<VkAccessFlags>(0U);
    pre[1U].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1U].oldLayout = blitter->content_valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1U].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1U].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1U].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1U].image = blitter->image;
    configure_color_range(&pre[1U].subresourceRange);

    const VkPipelineStageFlags pre_source_stage =
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
        (blitter->content_valid
             ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
             : static_cast<VkPipelineStageFlags>(0U));
    vkCmdPipelineBarrier(blitter->command_buffer, pre_source_stage,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr,
                         static_cast<uint32_t>(pre.size()), pre.data());

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.mipLevel = 0U;
    copy.srcSubresource.baseArrayLayer = 0U;
    copy.srcSubresource.layerCount = 1U;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = VkExtent3D{pool->width, pool->height, 1U};
    vkCmdCopyImage(blitter->command_buffer, pool->images[buffer_index], VK_IMAGE_LAYOUT_GENERAL,
                   blitter->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);

    std::array<VkImageMemoryBarrier, 2U> post{};
    post[0U].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post[0U].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[0U].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[0U].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[0U].srcQueueFamilyIndex = blitter->context.queue_family_index;
    post[0U].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    post[0U].image = pool->images[buffer_index];
    configure_color_range(&post[0U].subresourceRange);

    post[1U].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post[1U].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post[1U].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[1U].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post[1U].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post[1U].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post[1U].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post[1U].image = blitter->image;
    configure_color_range(&post[1U].subresourceRange);

    vkCmdPipelineBarrier(blitter->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0U, 0U, nullptr, 0U, nullptr,
                         static_cast<uint32_t>(post.size()), post.data());
    if (vkEndCommandBuffer(blitter->command_buffer) != VK_SUCCESS) {
        return MD_ERR_IO;
    }

    const bool wait_for_acquire = acquire_semaphore != VK_NULL_HANDLE;
    const bool signal_release = release_semaphore != VK_NULL_HANDLE;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = wait_for_acquire ? 1U : 0U;
    submit.pWaitSemaphores = wait_for_acquire ? &acquire_semaphore : nullptr;
    submit.pWaitDstStageMask = wait_for_acquire ? &wait_stage : nullptr;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &blitter->command_buffer;
    submit.signalSemaphoreCount = signal_release ? 1U : 0U;
    submit.pSignalSemaphores = signal_release ? &release_semaphore : nullptr;
    if (vkQueueSubmit(blitter->context.queue, 1U, &submit, blitter->fence) != VK_SUCCESS) {
        return MD_ERR_IO;
    }

    const VkResult wait_result = vkWaitForFences(blitter->context.device, 1U, &blitter->fence,
                                                  VK_TRUE, UINT64_C(2000000000));
    if (wait_result != VK_SUCCESS) {
        return wait_result == VK_TIMEOUT ? MD_ERR_WOULD_BLOCK : MD_ERR_IO;
    }
    blitter->content_valid = true;
    return MD_OK;
}

extern "C" VkImage md_vk_blitter_image(const md_vk_blitter_t* const blitter) {
    return blitter != nullptr ? blitter->image : VK_NULL_HANDLE;
}

extern "C" VkImageLayout md_vk_blitter_layout(const md_vk_blitter_t*) {
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

extern "C" VkFormat md_vk_blitter_format(const md_vk_blitter_t* const blitter) {
    return blitter != nullptr ? blitter->format : VK_FORMAT_UNDEFINED;
}

extern "C" uint32_t md_vk_blitter_width(const md_vk_blitter_t* const blitter) {
    return blitter != nullptr ? blitter->width : 0U;
}

extern "C" uint32_t md_vk_blitter_height(const md_vk_blitter_t* const blitter) {
    return blitter != nullptr ? blitter->height : 0U;
}

extern "C" uint8_t md_vk_blitter_has_content(const md_vk_blitter_t* const blitter) {
    return blitter != nullptr && blitter->content_valid ? UINT8_C(1) : UINT8_C(0);
}
