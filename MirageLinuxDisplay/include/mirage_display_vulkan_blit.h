#ifndef MIRAGE_DISPLAY_VULKAN_BLIT_H
#define MIRAGE_DISPLAY_VULKAN_BLIT_H

#include "mirage_display_vulkan.h"

/*
 * Public C ABI for the Vulkan same-device relay/blit fallback.
 *
 * Copies one imported RGB image into a host-owned optimal image so consumers can
 * sample formats that cannot be sampled directly; the acquire semaphore is
 * waited and the release semaphore is signaled after the imported image returns
 * to VK_QUEUE_FAMILY_FOREIGN_EXT.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Blitter context has the explicit ABI layout shared by the Vulkan helpers. */
#pragma pack(push, 8)

typedef struct md_vk_blitter md_vk_blitter_t;

typedef struct md_vk_blit_context {
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;
} md_vk_blit_context_t;

/* Creates a caller-owned same-device DMA-BUF relay for Qt Quick consumers. */
md_vk_blitter_t* md_vk_blitter_new(const md_vk_blit_context_t* context);
void md_vk_blitter_free(md_vk_blitter_t* blitter);

/*
 * Copies one imported RGB image into a host-owned optimal image. The acquire
 * semaphore is waited, the release semaphore is signaled after the imported
 * image has been released back to VK_QUEUE_FAMILY_FOREIGN_EXT, and the call
 * waits for completion before returning. Semaphores remain importer-owned.
 */
md_result_t md_vk_blitter_blit(md_vk_blitter_t* blitter,
                               const md_vk_imported_pool_t* pool,
                               uint32_t buffer_index,
                               VkSemaphore acquire_semaphore,
                               VkSemaphore release_semaphore);

VkImage md_vk_blitter_image(const md_vk_blitter_t* blitter);
VkImageLayout md_vk_blitter_layout(const md_vk_blitter_t* blitter);
VkFormat md_vk_blitter_format(const md_vk_blitter_t* blitter);
uint32_t md_vk_blitter_width(const md_vk_blitter_t* blitter);
uint32_t md_vk_blitter_height(const md_vk_blitter_t* blitter);
uint8_t md_vk_blitter_has_content(const md_vk_blitter_t* blitter);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
