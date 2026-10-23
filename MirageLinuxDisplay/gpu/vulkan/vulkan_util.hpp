#ifndef MIRAGE_DISPLAY_VULKAN_UTIL_HPP
#define MIRAGE_DISPLAY_VULKAN_UTIL_HPP

#include <cstdint>
#include <optional>

#include <vulkan/vulkan.h>

/*
 * Internal Vulkan helpers shared by the importer, blitter, and exporter.
 */

namespace mirage::vulkan {

/*
 * Returns the memory type that satisfies every required property first, falling
 * back to any compatible type when none does; returns nullopt only when type_bits
 * matches no memory type at all.
 */
[[nodiscard]] std::optional<uint32_t> choose_memory_type(
    VkPhysicalDevice physical_device, uint32_t type_bits,
    VkMemoryPropertyFlags required_properties);

/* Maps a negotiated memory plane to the Vulkan external-memory aspect bit. */
[[nodiscard]] VkImageAspectFlagBits memory_plane_aspect(uint32_t plane_index);

}  // namespace mirage::vulkan

#endif
