#include "vulkan_util.hpp"

/*
 * Implementation of the shared Vulkan instance/device helpers.
 */

namespace mirage::vulkan {

std::optional<uint32_t> choose_memory_type(
    const VkPhysicalDevice physical_device, const uint32_t type_bits,
    const VkMemoryPropertyFlags required_properties) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

    /* First pass: prefer a type that satisfies every required property. */
    for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        const uint32_t type_mask = UINT32_C(1) << index;
        const VkMemoryPropertyFlags properties_for_type =
            properties.memoryTypes[index].propertyFlags;
        if ((type_bits & type_mask) != 0U &&
            (properties_for_type & required_properties) == required_properties) {
            return index;
        }
    }
    /* Second pass: fall back to any compatible type when none satisfies the
     * required properties.  PRIME multi-GPU setups and some proprietary
     * drivers expose DMA-BUF only through non-DEVICE_LOCAL import types;
     * without this fallback external-memory import fails outright. */
    for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        const uint32_t type_mask = UINT32_C(1) << index;
        if ((type_bits & type_mask) != 0U) {
            return index;
        }
    }
    return std::nullopt;
}

VkImageAspectFlagBits memory_plane_aspect(const uint32_t plane_index) {
    switch (plane_index) {
    case 0U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    case 1U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
    case 2U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
    case 3U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    default:
        return VK_IMAGE_ASPECT_NONE;
    }
}

}  // namespace mirage::vulkan
