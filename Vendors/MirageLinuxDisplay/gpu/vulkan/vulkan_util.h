#ifndef MIRAGE_DISPLAY_VULKAN_UTIL_H
#define MIRAGE_DISPLAY_VULKAN_UTIL_H

#include <stdint.h>
#include <vulkan/vulkan.h>

/* Chooses a memory type satisfying `required`, falling back to any matching
 * type when none does. Returns UINT32_MAX when the type bits match nothing. */
uint32_t md_vk_choose_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                                  VkMemoryPropertyFlags required);

/* Maps a memory plane index to its VK_IMAGE_ASPECT_MEMORY_PLANE_*_BIT_EXT bit. */
VkImageAspectFlagBits md_vk_memory_plane_aspect(uint32_t plane);

#endif
