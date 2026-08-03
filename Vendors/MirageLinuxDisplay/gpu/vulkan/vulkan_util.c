#include "vulkan_util.h"

uint32_t md_vk_choose_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                                  VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) != 0) return i;
    }
    return UINT32_MAX;
}

VkImageAspectFlagBits md_vk_memory_plane_aspect(uint32_t plane) {
    switch (plane) {
    case 0: return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    case 1: return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
    case 2: return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
    case 3: return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    default: return (VkImageAspectFlagBits)0;
    }
}
