#include "mirage_display_vulkan.h"

#include "common/util.hpp"
#include "vulkan_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

/*
 * Vulkan external-memory DMA-BUF importer (include/mirage_display_vulkan.h).
 *
 * Imports each plane as VkDeviceMemory through VK_KHR_external_memory_fd,
 * creates images/views and the optional YCbCr conversion, and exposes the
 * GENERAL-layout queue-family ownership barriers required by protocol v1.
 */

struct md_vk_importer {
    md_vk_context_t context;
    /* Resolved once from the device; NULL means the driver lacks the extension. */
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    md_vk_imported_pool_t pool;
    bool pool_active;
    /* Structured record of the most recent import failure (see the C header). */
    md_vk_import_error_t last_error;
};

namespace {

constexpr uint32_t make_drm_format(const char code0, const char code1, const char code2,
                                   const char code3) {
    return static_cast<uint32_t>(static_cast<unsigned char>(code0)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code1)) << 8U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code2)) << 16U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code3)) << 24U);
}

constexpr uint32_t kDrmFormatXrgb8888 = make_drm_format('X', 'R', '2', '4');
constexpr uint32_t kDrmFormatArgb8888 = make_drm_format('A', 'R', '2', '4');
constexpr uint32_t kDrmFormatXbgr8888 = make_drm_format('X', 'B', '2', '4');
constexpr uint32_t kDrmFormatAbgr8888 = make_drm_format('A', 'B', '2', '4');
constexpr uint32_t kDrmFormatNv12 = make_drm_format('N', 'V', '1', '2');

/*
 * Records the first failure for md_vk_importer_last_error().  Stage is
 * overwritten on every failure so the latest diagnosis always wins; the pool
 * fields are captured from the importer's current pool bookkeeping.  errno is
 * meaningful only for the fd-dup failure inside MEMORY_ALLOCATE (vk_result is
 * VK_SUCCESS there), so it is recorded for that case alone; candidate_count
 * belongs to MEMORY_ALLOCATE and is reset so earlier stages never leak a
 * stale count into later diagnostics.
 */
void record_error(md_vk_importer_t* const importer, const md_vk_import_stage_t stage,
                  const VkResult vk_result, const uint32_t buffer_index,
                  const uint32_t plane_index) {
    importer->last_error.stage = stage;
    importer->last_error.vk_result = vk_result;
    importer->last_error.sys_errno =
        stage == MD_VK_IMPORT_STAGE_MEMORY_ALLOCATE && vk_result == VK_SUCCESS ? errno : 0;
    importer->last_error.buffer_index = buffer_index;
    importer->last_error.plane_index = plane_index;
    importer->last_error.modifier = importer->pool.modifier;
    importer->last_error.fourcc = importer->pool.fourcc;
    importer->last_error.candidate_count = -1;
}

void clear_pool(md_vk_imported_pool_t* const pool) {
    md_vk_imported_pool_t cleared{};
    cleared.format = VK_FORMAT_UNDEFINED;
    for (uint32_t buffer_index = 0U; buffer_index < MIRAGE_DISPLAY_MAX_BUFFERS;
         ++buffer_index) {
        cleared.images[buffer_index] = VK_NULL_HANDLE;
        cleared.memories[buffer_index] = VK_NULL_HANDLE;
        cleared.views[buffer_index] = VK_NULL_HANDLE;
        cleared.acquire_semaphores[buffer_index] = VK_NULL_HANDLE;
        cleared.release_semaphores[buffer_index] = VK_NULL_HANDLE;
        for (uint32_t plane_index = 0U; plane_index < MIRAGE_DISPLAY_MAX_PLANES;
             ++plane_index) {
            cleared.plane_memories[buffer_index][plane_index] = VK_NULL_HANDLE;
        }
    }
    cleared.ycbcr_conversion = VK_NULL_HANDLE;
    *pool = cleared;
}

bool format_is_disjoint(const uint32_t fourcc) { return fourcc == kDrmFormatNv12; }

VkImageAspectFlagBits image_plane_aspect(const uint32_t plane_index) {
    switch (plane_index) {
    case 0U:
        return VK_IMAGE_ASPECT_PLANE_0_BIT;
    case 1U:
        return VK_IMAGE_ASPECT_PLANE_1_BIT;
    case 2U:
        return VK_IMAGE_ASPECT_PLANE_2_BIT;
    default:
        return VK_IMAGE_ASPECT_NONE;
    }
}

/*
 * Maps format features onto the image usage that exercises them.  Used only to
 * validate modifier/external combinations against the driver in
 * md_vk_query_format_caps; every mapping is additive so a feature set that maps
 * to nothing degrades to TRANSFER_SRC rather than rejecting the query.
 */
VkImageUsageFlags features_to_usage(const VkFormatFeatureFlags features) {
    VkImageUsageFlags usage = 0U;
    if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if ((features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage == 0U) {
        usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    return usage;
}

/*
 * The caller is replacing or abandoning the complete pool, so all Vulkan
 * handles owned by that pool can be released in dependency order here.
 */
void destroy_pool_objects(md_vk_importer_t* const importer) {
    const VkDevice device = importer->context.device;
    for (uint32_t buffer_index = 0U; buffer_index < importer->pool.buffer_count;
         ++buffer_index) {
        if (importer->pool.acquire_semaphores[buffer_index] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.acquire_semaphores[buffer_index], nullptr);
        }
        if (importer->pool.release_semaphores[buffer_index] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.release_semaphores[buffer_index], nullptr);
        }
        if (importer->pool.views[buffer_index] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, importer->pool.views[buffer_index], nullptr);
        }
        if (importer->pool.images[buffer_index] != VK_NULL_HANDLE) {
            vkDestroyImage(device, importer->pool.images[buffer_index], nullptr);
        }
        for (uint32_t plane_index = 0U; plane_index < importer->pool.plane_count;
             ++plane_index) {
            if (importer->pool.plane_memories[buffer_index][plane_index] != VK_NULL_HANDLE) {
                vkFreeMemory(device,
                             importer->pool.plane_memories[buffer_index][plane_index], nullptr);
            }
        }
    }
    if (importer->pool.ycbcr_conversion != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(device, importer->pool.ycbcr_conversion, nullptr);
    }
    clear_pool(&importer->pool);
}


/*
 * Imports one DMA-BUF plane as VkDeviceMemory and binds it to the image.
 * Disjoint formats (NV12) allocate one memory object per plane; other formats
 * bind the single non-disjoint allocation to plane zero.
 */
md_result_t import_plane_memory(md_vk_importer_t* const importer,
                                const md_buffer_pool_t* const source,
                                const uint32_t image_index,
                                const uint32_t plane_index,
                                const bool disjoint) {
    const VkDevice device = importer->context.device;
    const md_plane_t& plane = source->planes[image_index][plane_index];

    VkMemoryFdPropertiesKHR fd_properties{};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (importer->get_memory_fd_properties == nullptr) {
        record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_PROPERTIES,
                     VK_ERROR_EXTENSION_NOT_PRESENT, image_index, plane_index);
        return MD_ERR_UNSUPPORTED;
    }
    const VkResult fd_result = importer->get_memory_fd_properties(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, plane.fd, &fd_properties);
    if (fd_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_PROPERTIES, fd_result,
                     image_index, plane_index);
        return fd_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }

    const VkImageAspectFlagBits aspect =
        disjoint ? image_plane_aspect(plane_index) : VK_IMAGE_ASPECT_NONE;
    if (disjoint && aspect == VK_IMAGE_ASPECT_NONE) {
        return MD_ERR_UNSUPPORTED;
    }

    VkImagePlaneMemoryRequirementsInfo plane_requirements{};
    plane_requirements.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
    plane_requirements.planeAspect = aspect;

    VkImageMemoryRequirementsInfo2 requirements_info{};
    requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    requirements_info.pNext = disjoint ? &plane_requirements : nullptr;
    requirements_info.image = importer->pool.images[image_index];

    VkMemoryDedicatedRequirements dedicated_requirements{};
    dedicated_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated_requirements;
    vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);

    const uint32_t compatible_type_bits =
        requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits;

    /* Enumerate every compatible memory type, preferring device-local while
     * keeping non-device-local candidates.  PRIME multi-GPU setups and some
     * proprietary drivers expose DMA-BUF only through non-local import types,
     * so each candidate must be tried individually instead of picking one. */
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(importer->context.physical_device, &memory_properties);
    std::array<uint32_t, VK_MAX_MEMORY_TYPES> candidates{};
    uint32_t candidate_count = 0U;
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        for (uint32_t index = 0U; index < memory_properties.memoryTypeCount; ++index) {
            if ((compatible_type_bits & (UINT32_C(1) << index)) == 0U) {
                continue;
            }
            const bool device_local =
                (memory_properties.memoryTypes[index].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U;
            if ((pass == 0U && !device_local) || (pass == 1U && device_local)) {
                continue;
            }
            candidates[candidate_count++] = index;
        }
    }
    if (candidate_count == 0U) {
        record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_TYPES, VK_SUCCESS,
                     image_index, plane_index);
        return MD_ERR_UNSUPPORTED;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info{};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = importer->pool.images[image_index];
    dedicated_info.buffer = VK_NULL_HANDLE;

    VkDeviceMemory& memory = importer->pool.plane_memories[image_index][plane_index];
    VkResult last_result = VK_ERROR_UNKNOWN;
    bool allocated = false;
    for (uint32_t candidate = 0U; candidate < candidate_count; ++candidate) {
        const int duplicated_descriptor = fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
        if (duplicated_descriptor < 0) {
            record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_ALLOCATE, VK_SUCCESS,
                         image_index, plane_index);
            importer->last_error.candidate_count = static_cast<int32_t>(candidate_count);
            return MD_ERR_IO;
        }
        mirage::UniqueFd imported_fd{static_cast<int32_t>(duplicated_descriptor)};

        VkImportMemoryFdInfoKHR import_info{};
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_info.pNext = &dedicated_info;
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        const int32_t transferred_descriptor = imported_fd.release();
        import_info.fd = transferred_descriptor;

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.pNext = &import_info;
        allocation_info.allocationSize = requirements.memoryRequirements.size;
        allocation_info.memoryTypeIndex = candidates[candidate];

        last_result = vkAllocateMemory(device, &allocation_info, nullptr, &memory);
        if (last_result == VK_SUCCESS) {
            allocated = true;
            break;
        }
        /* This memory type rejected the DMA-BUF: close the duplicated fd
         * created for this attempt and move on to the next candidate. */
        mirage::UniqueFd failed_transfer{transferred_descriptor};
    }
    if (!allocated) {
        record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_ALLOCATE, last_result,
                     image_index, plane_index);
        importer->last_error.candidate_count = static_cast<int32_t>(candidate_count);
        return last_result == VK_ERROR_INVALID_EXTERNAL_HANDLE ? MD_ERR_UNSUPPORTED
                                                                : MD_ERR_IO;
    }
    /* Vulkan owns the duplicate only after successful external-memory import. */
    if (plane_index == 0U) {
        importer->pool.memories[image_index] = memory;
    }

    VkBindImagePlaneMemoryInfo bind_plane_info{};
    bind_plane_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
    bind_plane_info.planeAspect = aspect;

    VkBindImageMemoryInfo bind_info{};
    bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bind_info.pNext = disjoint ? &bind_plane_info : nullptr;
    bind_info.image = importer->pool.images[image_index];
    bind_info.memory = memory;
    const VkResult bind_result = vkBindImageMemory2(device, 1U, &bind_info);
    if (bind_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_MEMORY_BIND, bind_result,
                     image_index, plane_index);
        return MD_ERR_IO;
    }
    return MD_OK;
}


/*
 * Creates one VkImage through the DRM-format-modifier explicit layout path,
 * binds its plane memories, and creates the image view (with a YCbCr conversion
 * for disjoint formats) plus the per-buffer acquire/release semaphores.
 */
md_result_t import_one_image(md_vk_importer_t* const importer,
                             const md_buffer_pool_t* const source,
                             const uint32_t image_index, const VkFormat format,
                             const VkComponentMapping mapping) {
    const VkDevice device = importer->context.device;
    const bool disjoint = format_is_disjoint(source->fourcc);

    std::array<VkSubresourceLayout, MIRAGE_DISPLAY_MAX_PLANES> layouts{};
    for (uint32_t plane_index = 0U; plane_index < source->plane_count; ++plane_index) {
        const md_plane_t& plane = source->planes[image_index][plane_index];
        /* The modifier extension consumes offset and rowPitch.  Its size is a
         * driver-derived subresource property, not producer bookkeeping. */
        layouts[plane_index].offset = plane.offset;
        layouts[plane_index].size = 0U;
        layouts[plane_index].rowPitch = plane.stride;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info{};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier_info.drmFormatModifier = source->modifier;
    modifier_info.drmFormatModifierPlaneCount = source->plane_count;
    modifier_info.pPlaneLayouts = layouts.data();

    VkExternalMemoryImageCreateInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.flags = disjoint ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_DISJOINT_BIT)
                                : static_cast<VkImageCreateFlags>(0U);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = VkExtent3D{source->width, source->height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = importer->context.image_usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkResult image_result =
        vkCreateImage(device, &image_info, nullptr, &importer->pool.images[image_index]);
    if (image_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_IMAGE_CREATE, image_result,
                     image_index, UINT32_MAX);
        return image_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED
                                                              : MD_ERR_IO;
    }

    const uint32_t allocation_count = disjoint ? source->plane_count : 1U;
    for (uint32_t plane_index = 0U; plane_index < allocation_count; ++plane_index) {
        const md_result_t import_result =
            import_plane_memory(importer, source, image_index, plane_index, disjoint);
        if (import_result != MD_OK) {
            return import_result;
        }
    }

    VkSamplerYcbcrConversionInfo conversion_info{};
    conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversion_info.conversion = importer->pool.ycbcr_conversion;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = disjoint ? &conversion_info : nullptr;
    view_info.image = importer->pool.images[image_index];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = mapping;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0U;
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.baseArrayLayer = 0U;
    view_info.subresourceRange.layerCount = 1U;
    const VkResult view_result =
        vkCreateImageView(device, &view_info, nullptr, &importer->pool.views[image_index]);
    if (view_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_VIEW, view_result,
                     image_index, UINT32_MAX);
        return MD_ERR_IO;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    const VkResult acquire_result = vkCreateSemaphore(
        device, &semaphore_info, nullptr, &importer->pool.acquire_semaphores[image_index]);
    if (acquire_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_SEMAPHORE, acquire_result,
                     image_index, UINT32_MAX);
        return MD_ERR_IO;
    }
    const VkResult release_result = vkCreateSemaphore(
        device, &semaphore_info, nullptr, &importer->pool.release_semaphores[image_index]);
    if (release_result != VK_SUCCESS) {
        record_error(importer, MD_VK_IMPORT_STAGE_SEMAPHORE, release_result,
                     image_index, UINT32_MAX);
    }
    return release_result == VK_SUCCESS ? MD_OK : MD_ERR_IO;
}

md_result_t import_semaphore(md_vk_importer_t* const importer, const uint32_t buffer_index,
                             const int32_t descriptor,
                             const VkExternalSemaphoreHandleTypeFlagBits handle_type,
                             const VkSemaphoreImportFlags flags, VkSemaphore* const out) {
    if (descriptor < 0) {
        return MD_ERR_INVALID;
    }
    mirage::UniqueFd owned_descriptor{descriptor};
    if (importer == nullptr || out == nullptr || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) {
        return MD_ERR_INVALID;
    }

    const VkSemaphore semaphore =
        handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
            ? importer->pool.acquire_semaphores[buffer_index]
            : importer->pool.release_semaphores[buffer_index];
    VkImportSemaphoreFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = semaphore;
    import_info.flags = flags;
    import_info.handleType = handle_type;
    if (importer->import_semaphore_fd == nullptr) {
        record_error(importer, MD_VK_IMPORT_STAGE_SYNC_IMPORT,
                     VK_ERROR_EXTENSION_NOT_PRESENT, buffer_index, UINT32_MAX);
        return MD_ERR_UNSUPPORTED;
    }
    const int32_t transferred_descriptor = owned_descriptor.release();
    import_info.fd = transferred_descriptor;
    const VkResult import_result =
        importer->import_semaphore_fd(importer->context.device, &import_info);
    if (import_result != VK_SUCCESS) {
        mirage::UniqueFd failed_transfer{transferred_descriptor};
        record_error(importer, MD_VK_IMPORT_STAGE_SYNC_IMPORT, import_result,
                     buffer_index, UINT32_MAX);
        return import_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }
    /* Vulkan has accepted the FD and now owns its lifetime. */
    *out = semaphore;
    return MD_OK;
}

md_result_t make_barrier(const md_vk_importer_t* const importer,
                         const uint32_t buffer_index, const VkAccessFlags source_access,
                         const VkAccessFlags destination_access,
                         const uint32_t source_family, const uint32_t destination_family,
                         VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr || out_barrier == nullptr || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) {
        return MD_ERR_INVALID;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = source_family;
    barrier.dstQueueFamilyIndex = destination_family;
    barrier.image = importer->pool.images[buffer_index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0U;
    barrier.subresourceRange.levelCount = 1U;
    barrier.subresourceRange.baseArrayLayer = 0U;
    barrier.subresourceRange.layerCount = 1U;
    *out_barrier = barrier;
    return MD_OK;
}

}  // namespace


/*
 * Maps the DRM fourcc codes supported by the first Vulkan backend revision
 * onto VkFormat plus a component swizzle that preserves the wire colors.
 */
extern "C" md_result_t md_vk_fourcc_to_format(const uint32_t fourcc, VkFormat* const format,
                                                VkComponentMapping* const mapping) {
    if (format == nullptr || mapping == nullptr) {
        return MD_ERR_INVALID;
    }
    mapping->r = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->g = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->b = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->a = VK_COMPONENT_SWIZZLE_IDENTITY;

    switch (fourcc) {
    case kDrmFormatXrgb8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case kDrmFormatArgb8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        return MD_OK;
    case kDrmFormatXbgr8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case kDrmFormatAbgr8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        return MD_OK;
    case kDrmFormatNv12:
        *format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        return MD_OK;
    default:
        return MD_ERR_UNSUPPORTED;
    }
}


/*
 * Enumerates DRM modifiers on the physical device that support every required
 * tiling feature, filling md_format_cap_t entries for the producer's capability
 * advertisement.  Passing caps=NULL and capacity=0 queries the count.
 */
extern "C" md_result_t md_vk_query_format_caps(
    const VkPhysicalDevice physical_device, const uint32_t fourcc,
    const VkFormatFeatureFlags required_features, md_format_cap_t* const caps,
    const uint32_t capacity, uint32_t* const out_count) {
    if (physical_device == VK_NULL_HANDLE || out_count == nullptr ||
        (capacity > 0U && caps == nullptr)) {
        return MD_ERR_INVALID;
    }

    VkFormat format{};
    VkComponentMapping mapping{};
    const md_result_t format_result = md_vk_fourcc_to_format(fourcc, &format, &mapping);
    if (format_result != MD_OK) {
        return format_result;
    }

    VkDrmFormatModifierPropertiesListEXT modifier_list{};
    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &modifier_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);
    if (modifier_list.drmFormatModifierCount == 0U) {
        *out_count = 0U;
        return MD_OK;
    }

    std::unique_ptr<VkDrmFormatModifierPropertiesEXT[]> modifiers{
        new (std::nothrow)
            VkDrmFormatModifierPropertiesEXT[modifier_list.drmFormatModifierCount]{}};
    if (!modifiers) {
        return MD_ERR_NOMEM;
    }
    modifier_list.pDrmFormatModifierProperties = modifiers.get();
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);

    uint32_t written = 0U;
    uint32_t available = 0U;
    const VkImageUsageFlags query_usage = features_to_usage(required_features);
    for (uint32_t modifier_index = 0U;
         modifier_index < modifier_list.drmFormatModifierCount; ++modifier_index) {
        const VkDrmFormatModifierPropertiesEXT& modifier = modifiers[modifier_index];
        if ((modifier.drmFormatModifierTilingFeatures & required_features) != required_features ||
            modifier.drmFormatModifierPlaneCount == 0U ||
            modifier.drmFormatModifierPlaneCount > MIRAGE_DISPLAY_MAX_PLANES) {
            continue;
        }
        if (format_is_disjoint(fourcc) && modifier.drmFormatModifierPlaneCount != 2U) {
            /* NV12 binds one allocation per image plane.  Modifiers with
             * auxiliary planes need a different protocol and are excluded. */
            continue;
        }
        /* Tiling features alone do not prove the modifier can be imported as
         * an external DMA-BUF (VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-
         * drmFormatModifier-02264).  Re-check the (format, modifier, usage)
         * triple with the external-memory capability query so consumers never
         * advertise a modifier their own driver cannot import. */
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{};
        modifier_info.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modifier_info.drmFormatModifier = modifier.drmFormatModifier;
        modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkPhysicalDeviceExternalImageFormatInfo external_info{};
        external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external_info.pNext = &modifier_info;
        external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageFormatInfo2 image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        image_info.pNext = &external_info;
        image_info.format = format;
        image_info.type = VK_IMAGE_TYPE_2D;
        image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = query_usage;
        image_info.flags = 0U;

        VkExternalImageFormatProperties external_properties{};
        external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 image_properties{};
        image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        image_properties.pNext = &external_properties;
        const VkResult image_props_result = vkGetPhysicalDeviceImageFormatProperties2(
            physical_device, &image_info, &image_properties);
        if (image_props_result != VK_SUCCESS ||
            (external_properties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0U) {
            continue;
        }

        if (caps != nullptr && written < capacity) {
            caps[written].fourcc = fourcc;
            caps[written].plane_count = modifier.drmFormatModifierPlaneCount;
            caps[written].modifier = modifier.drmFormatModifier;
            ++written;
        }
        ++available;
    }
    *out_count = available;
    return caps != nullptr && capacity < available ? MD_ERR_NOMEM : MD_OK;
}

extern "C" const char* md_vk_result_string(const VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    default:
        return "VK_ERROR_UNRECOGNIZED";
    }
}

/*
 * Probes DMA-BUF import availability for a physical device / device pair.
 *
 * Two independent conditions gate the protocol's import path:
 *   1. the driver must expose the external-memory DMA-BUF extensions on the
 *      physical device (vkEnumerateDeviceExtensionProperties);
 *   2. those extensions must be enabled on the VkDevice. Vulkan offers no
 *      query for enabled device extensions, so the device is probed
 *      behaviorally: vkGetMemoryFdPropertiesKHR returns
 *      VK_ERROR_EXTENSION_NOT_PRESENT for
 *      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT unless
 *      VK_EXT_external_memory_dma_buf is enabled (VUID-00666), and
 *      VK_ERROR_INVALID_EXTERNAL_HANDLE for any other POSIX fd once it is.
 *      A /dev/null fd is therefore a safe probe that keeps the caller free
 *      of protocol buffers.
 *
 * Qt Quick scene graphs (e.g. plasmashell) create their VkDevice before a
 * plugin can register device extensions, so condition 2 commonly fails there;
 * the Qt-sanctioned workaround is the QT_VULKAN_DEVICE_EXTENSIONS environment
 * variable. The probe tells the caller which of the two conditions failed.
 */
extern "C" md_result_t md_vk_query_dma_buf_import_support(
    const VkPhysicalDevice physical_device, const VkDevice device,
    md_vk_dma_buf_import_state_t* const out_state, char* const missing_extensions,
    const size_t capacity) {
    if (out_state == nullptr) {
        return MD_ERR_INVALID;
    }
    *out_state = MD_VK_DMA_BUF_IMPORT_UNAVAILABLE;
    if (physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        return MD_ERR_INVALID;
    }

    // Extension names the import path (md_vk_importer_import_pool) relies on.
    // The list mirrors the renderer's requested device extensions, so a probe
    // failure always names the exact extension a fix must enable.
    static constexpr const char* kRequiredExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };

    uint32_t extension_count = 0U;
    if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                             nullptr) != VK_SUCCESS) {
        return MD_OK; /* out_state stays UNAVAILABLE */
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0U &&
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                             extensions.data()) != VK_SUCCESS) {
        return MD_OK; /* out_state stays UNAVAILABLE */
    }

    bool driver_ok = true;
    size_t written = 0U;
    for (const char* const required : kRequiredExtensions) {
        const bool found = std::any_of(
            extensions.begin(), extensions.end(),
            [required](const VkExtensionProperties& properties) {
                return std::strcmp(properties.extensionName, required) == 0;
            });
        if (found) {
            continue;
        }
        driver_ok = false;
        if (missing_extensions == nullptr || capacity == 0U) {
            continue;
        }
        const size_t length = std::strlen(required);
        if (written + length + 1U >= capacity) {
            break; /* Buffer full; remaining names are dropped. */
        }
        if (written > 0U) {
            missing_extensions[written++] = ',';
        }
        std::memcpy(missing_extensions + written, required, length);
        written += length;
    }
    if (missing_extensions != nullptr && capacity > 0U) {
        missing_extensions[written] = '\0';
    }
    if (!driver_ok) {
        *out_state = MD_VK_DMA_BUF_IMPORT_DRIVER_UNSUPPORTED;
        return MD_OK;
    }

    // Behaviorally probe whether the device enabled VK_EXT_external_memory_dma_buf.
    const auto get_memory_fd_properties = std::bit_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
    if (get_memory_fd_properties == nullptr) {
        return MD_OK; /* out_state stays UNAVAILABLE */
    }
    const int probe_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (probe_fd < 0) {
        return MD_OK; /* out_state stays UNAVAILABLE */
    }
    VkMemoryFdPropertiesKHR fd_properties{};
    const VkResult probe_result = get_memory_fd_properties(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, probe_fd, &fd_properties);
    close(probe_fd);
    if (probe_result == VK_ERROR_EXTENSION_NOT_PRESENT) {
        *out_state = MD_VK_DMA_BUF_IMPORT_DEVICE_NOT_ENABLED;
        return MD_OK;
    }
    // INVALID_EXTERNAL_HANDLE (or success) proves the DMA-BUF handle type is
    // active; /dev/null is simply not a DMA-BUF.
    *out_state = MD_VK_DMA_BUF_IMPORT_OK;
    return MD_OK;
}

extern "C" const char* md_vk_dma_buf_import_state_string(
    const md_vk_dma_buf_import_state_t state) {
    switch (state) {
    case MD_VK_DMA_BUF_IMPORT_OK:
        return "DMA-BUF import available";
    case MD_VK_DMA_BUF_IMPORT_DRIVER_UNSUPPORTED:
        return "driver lacks required extensions";
    case MD_VK_DMA_BUF_IMPORT_DEVICE_NOT_ENABLED:
        return "required extensions not enabled on the device";
    case MD_VK_DMA_BUF_IMPORT_UNAVAILABLE:
        return "Vulkan entry points not resolvable";
    default:
        return "unknown probe state";
    }
}

extern "C" md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* const context) {
    if (context == nullptr || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE || context->image_usage == 0U) {
        return nullptr;
    }

    std::unique_ptr<md_vk_importer_t> importer{new (std::nothrow) md_vk_importer_t{}};
    if (!importer) {
        return nullptr;
    }
    importer->context = *context;
    importer->get_memory_fd_properties = std::bit_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(importer->context.device, "vkGetMemoryFdPropertiesKHR"));
    importer->import_semaphore_fd = std::bit_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(importer->context.device, "vkImportSemaphoreFdKHR"));
    clear_pool(&importer->pool);
    importer->pool_active = false;
    importer->last_error.stage = MD_VK_IMPORT_STAGE_NONE;
    importer->last_error.vk_result = VK_SUCCESS;
    importer->last_error.sys_errno = 0;
    importer->last_error.buffer_index = UINT32_MAX;
    importer->last_error.plane_index = UINT32_MAX;
    importer->last_error.modifier = 0U;
    importer->last_error.fourcc = 0U;
    importer->last_error.candidate_count = -1;
    return importer.release();
}

extern "C" void md_vk_importer_release_pool(md_vk_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return;
    }
    destroy_pool_objects(importer);
    importer->pool_active = false;
}

extern "C" void md_vk_importer_free(md_vk_importer_t* const importer) {
    if (importer == nullptr) {
        return;
    }
    md_vk_importer_release_pool(importer);
    delete importer;
}

extern "C" const md_vk_imported_pool_t* md_vk_importer_pool(
    const md_vk_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return nullptr;
    }
    return &importer->pool;
}

/*
 * Diagnostics: the record survives pool release (only a fresh failure
 * overwrites it), so the caller can still explain the last import error after
 * the failed pool has been cleaned up.
 */
extern "C" const md_vk_import_error_t* md_vk_importer_last_error(
    const md_vk_importer_t* const importer) {
    if (importer == nullptr || importer->last_error.stage == MD_VK_IMPORT_STAGE_NONE) {
        return nullptr;
    }
    return &importer->last_error;
}

extern "C" const char* md_vk_import_stage_string(const md_vk_import_stage_t stage) {
    switch (stage) {
    case MD_VK_IMPORT_STAGE_NONE:
        return "no error";
    case MD_VK_IMPORT_STAGE_POOL_INVALID:
        return "invalid buffer pool descriptor";
    case MD_VK_IMPORT_STAGE_FORMAT:
        return "unsupported DRM format";
    case MD_VK_IMPORT_STAGE_YCBCR:
        return "sampler YCbCr conversion creation failed";
    case MD_VK_IMPORT_STAGE_IMAGE_CREATE:
        return "image creation failed (modifier/layout not supported)";
    case MD_VK_IMPORT_STAGE_MEMORY_PROPERTIES:
        return "DMA-BUF memory FD properties query failed";
    case MD_VK_IMPORT_STAGE_MEMORY_TYPES:
        return "no memory type accepts this DMA-BUF (PRIME/cross-GPU?)";
    case MD_VK_IMPORT_STAGE_MEMORY_ALLOCATE:
        return "DMA-BUF memory import allocation failed";
    case MD_VK_IMPORT_STAGE_MEMORY_BIND:
        return "image memory bind failed";
    case MD_VK_IMPORT_STAGE_VIEW:
        return "image view creation failed";
    case MD_VK_IMPORT_STAGE_SEMAPHORE:
        return "semaphore creation failed";
    case MD_VK_IMPORT_STAGE_SYNC_IMPORT:
        return "frame sync FD import failed";
    default:
        return "unknown import failure";
    }
}

extern "C" md_result_t md_vk_importer_import_pool(md_vk_importer_t* const importer,
                                                    const md_buffer_pool_t* const pool) {
    if (importer == nullptr || pool == nullptr || importer->pool_active) {
        if (importer != nullptr) {
            record_error(importer, MD_VK_IMPORT_STAGE_POOL_INVALID, VK_SUCCESS,
                         UINT32_MAX, UINT32_MAX);
        }
        return MD_ERR_STATE;
    }
    /* Validation failures below happen before the pool fields are copied into
     * importer->pool, so snapshot the incoming descriptors explicitly. */
    const auto record_pool_error = [importer](const md_vk_import_stage_t stage,
                                              const md_buffer_pool_t* const source,
                                              const uint32_t buffer_index,
                                              const uint32_t plane_index) {
        record_error(importer, stage, VK_SUCCESS, buffer_index, plane_index);
        importer->last_error.fourcc = source->fourcc;
        importer->last_error.modifier = source->modifier;
    };
    if (pool->buffer_count < 2U || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count == 0U || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        pool->width == 0U || pool->height == 0U || pool->generation == 0U) {
        record_pool_error(MD_VK_IMPORT_STAGE_POOL_INVALID, pool, UINT32_MAX, UINT32_MAX);
        return MD_ERR_INVALID;
    }
    if (format_is_disjoint(pool->fourcc) && pool->plane_count != 2U) {
        record_pool_error(MD_VK_IMPORT_STAGE_POOL_INVALID, pool, UINT32_MAX, UINT32_MAX);
        return MD_ERR_UNSUPPORTED;
    }
    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        for (uint32_t plane_index = 0U; plane_index < pool->plane_count; ++plane_index) {
            const md_plane_t& plane = pool->planes[buffer_index][plane_index];
            if (plane.fd < 0 || plane.stride == 0U || plane.size == 0U) {
                record_pool_error(MD_VK_IMPORT_STAGE_POOL_INVALID, pool,
                                  buffer_index, plane_index);
                return MD_ERR_INVALID;
            }
        }
    }

    VkFormat format{};
    VkComponentMapping mapping{};
    const md_result_t format_result = md_vk_fourcc_to_format(pool->fourcc, &format, &mapping);
    if (format_result != MD_OK) {
        record_pool_error(MD_VK_IMPORT_STAGE_FORMAT, pool, UINT32_MAX, UINT32_MAX);
        return format_result;
    }

    clear_pool(&importer->pool);
    importer->pool.generation = pool->generation;
    importer->pool.buffer_count = pool->buffer_count;
    importer->pool.width = pool->width;
    importer->pool.height = pool->height;
    importer->pool.fourcc = pool->fourcc;
    importer->pool.plane_count = pool->plane_count;
    importer->pool.modifier = pool->modifier;
    importer->pool.format = format;

    if (format_is_disjoint(pool->fourcc)) {
        VkSamplerYcbcrConversionCreateInfo conversion_info{};
        conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        conversion_info.format = format;
        conversion_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        conversion_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        conversion_info.components = mapping;
        conversion_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversion_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversion_info.chromaFilter = VK_FILTER_LINEAR;
        conversion_info.forceExplicitReconstruction = VK_FALSE;
        const VkResult conversion_result = vkCreateSamplerYcbcrConversion(
            importer->context.device, &conversion_info, nullptr,
            &importer->pool.ycbcr_conversion);
        if (conversion_result != VK_SUCCESS) {
            record_error(importer, MD_VK_IMPORT_STAGE_YCBCR, conversion_result,
                         UINT32_MAX, UINT32_MAX);
            clear_pool(&importer->pool);
            return conversion_result == VK_ERROR_FORMAT_NOT_SUPPORTED ? MD_ERR_UNSUPPORTED
                                                                       : MD_ERR_IO;
        }
    }

    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        const md_result_t image_result =
            import_one_image(importer, pool, buffer_index, format, mapping);
        if (image_result != MD_OK) {
            destroy_pool_objects(importer);
            return image_result;
        }
    }
    importer->pool_active = true;
    /* A successful import supersedes any previous failure record. */
    importer->last_error.stage = MD_VK_IMPORT_STAGE_NONE;
    return MD_OK;
}


/*
 * Imports one frame acquire sync_file as a temporary binary semaphore that
 * the consumer waits in its first read submission.  Consumes the descriptor on
 * every path.
 */
extern "C" md_result_t md_vk_import_acquire_sync(md_vk_importer_t* const importer,
                                                   const uint32_t buffer_index,
                                                   const int32_t acquire_sync_fd,
                                                   VkSemaphore* const out_semaphore) {
    return import_semaphore(importer, buffer_index, acquire_sync_fd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                            VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, out_semaphore);
}

extern "C" md_result_t md_vk_import_release_syncobj(
    md_vk_importer_t* const importer, const uint32_t buffer_index,
    const int32_t release_syncobj_fd, VkSemaphore* const out_semaphore) {
    return import_semaphore(importer, buffer_index, release_syncobj_fd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, 0U,
                            out_semaphore);
}


/*
 * Builds the protocol-v1 GENERAL-layout queue-family ownership barrier the
 * consumer must record before the first read of a frame.
 */
extern "C" md_result_t md_vk_importer_acquire_barrier(
    const md_vk_importer_t* const importer, const uint32_t buffer_index,
    const VkAccessFlags destination_access, VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }
    return make_barrier(importer, buffer_index, 0U, destination_access,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, importer->context.queue_family_index,
                        out_barrier);
}

extern "C" md_result_t md_vk_importer_release_barrier(
    const md_vk_importer_t* const importer, const uint32_t buffer_index,
    const VkAccessFlags source_access, VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }
    return make_barrier(importer, buffer_index, source_access, 0U,
                        importer->context.queue_family_index,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, out_barrier);
}
