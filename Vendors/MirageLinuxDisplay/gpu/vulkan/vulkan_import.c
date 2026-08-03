#define _GNU_SOURCE

#include "mirage_display_vulkan.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MD_DRM_FORMAT(code0, code1, code2, code3) \
    ((uint32_t)(code0) | ((uint32_t)(code1) << 8) | ((uint32_t)(code2) << 16) | \
     ((uint32_t)(code3) << 24))

#define MD_DRM_FORMAT_XRGB8888 MD_DRM_FORMAT('X', 'R', '2', '4')
#define MD_DRM_FORMAT_ARGB8888 MD_DRM_FORMAT('A', 'R', '2', '4')
#define MD_DRM_FORMAT_XBGR8888 MD_DRM_FORMAT('X', 'B', '2', '4')
#define MD_DRM_FORMAT_ABGR8888 MD_DRM_FORMAT('A', 'B', '2', '4')
#define MD_DRM_FORMAT_NV12 MD_DRM_FORMAT('N', 'V', '1', '2')

struct md_vk_importer {
    md_vk_context_t context;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    md_vk_imported_pool_t pool;
    bool pool_active;
};

static void clear_pool(md_vk_imported_pool_t* pool) {
    memset(pool, 0, sizeof(*pool));
    for (size_t i = 0; i < MIRAGE_DISPLAY_MAX_BUFFERS; ++i) {
        pool->images[i] = VK_NULL_HANDLE;
        pool->memories[i] = VK_NULL_HANDLE;
        pool->views[i] = VK_NULL_HANDLE;
        pool->acquire_semaphores[i] = VK_NULL_HANDLE;
        pool->release_semaphores[i] = VK_NULL_HANDLE;
        for (size_t p = 0; p < MIRAGE_DISPLAY_MAX_PLANES; ++p) {
            pool->plane_memories[i][p] = VK_NULL_HANDLE;
        }
    }
    pool->ycbcr_conversion = VK_NULL_HANDLE;
}

int md_vk_fourcc_to_format(uint32_t fourcc, VkFormat* format, VkComponentMapping* mapping) {
    if (format == NULL || mapping == NULL) return MD_ERR_INVALID;
    mapping->r = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->g = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->b = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->a = VK_COMPONENT_SWIZZLE_IDENTITY;
    switch (fourcc) {
    case MD_DRM_FORMAT_XRGB8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case MD_DRM_FORMAT_ARGB8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        return MD_OK;
    case MD_DRM_FORMAT_XBGR8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case MD_DRM_FORMAT_ABGR8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        return MD_OK;
    case MD_DRM_FORMAT_NV12:
        *format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        return MD_OK;
    default:
        return MD_ERR_UNSUPPORTED;
    }
}

static bool format_is_disjoint(uint32_t fourcc) {
    return fourcc == MD_DRM_FORMAT_NV12;
}

int md_vk_query_format_caps(VkPhysicalDevice physical_device, uint32_t fourcc,
                            VkFormatFeatureFlags required_features,
                            md_format_cap_t* caps, uint32_t capacity,
                            uint32_t* out_count) {
    if (physical_device == VK_NULL_HANDLE || out_count == NULL ||
        (capacity > 0 && caps == NULL)) return MD_ERR_INVALID;
    VkFormat format;
    VkComponentMapping mapping;
    int rc = md_vk_fourcc_to_format(fourcc, &format, &mapping);
    if (rc != MD_OK) return rc;
    (void)mapping;

    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        .pNext = NULL,
        .drmFormatModifierCount = 0,
        .pDrmFormatModifierProperties = NULL,
    };
    VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);
    if (list.drmFormatModifierCount == 0) {
        *out_count = 0;
        return MD_OK;
    }
    VkDrmFormatModifierPropertiesEXT* modifiers =
        calloc(list.drmFormatModifierCount, sizeof(*modifiers));
    if (modifiers == NULL) return MD_ERR_NOMEM;
    list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);

    uint32_t written = 0;
    uint32_t available = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        const VkDrmFormatModifierPropertiesEXT* modifier = &modifiers[i];
        if ((modifier->drmFormatModifierTilingFeatures & required_features) !=
            required_features || modifier->drmFormatModifierPlaneCount == 0 ||
            modifier->drmFormatModifierPlaneCount > MIRAGE_DISPLAY_MAX_PLANES) {
            continue;
        }
        if (format_is_disjoint(fourcc) && modifier->drmFormatModifierPlaneCount != 2) {
            /* This importer binds one allocation for each NV12 format plane.
             * Modifiers exposing additional auxiliary memory planes require a
             * different binding model and must not be advertised here. */
            continue;
        }
        if (caps != NULL && written < capacity) {
            caps[written] = (md_format_cap_t) {
                .fourcc = fourcc,
                .plane_count = modifier->drmFormatModifierPlaneCount,
                .modifier = modifier->drmFormatModifier,
            };
            ++written;
        }
        ++available;
    }
    free(modifiers);
    *out_count = available;
    return caps != NULL && capacity < available ? MD_ERR_NOMEM : MD_OK;
}

static VkImageAspectFlagBits plane_aspect(uint32_t plane) {
    switch (plane) {
    case 0: return VK_IMAGE_ASPECT_PLANE_0_BIT;
    case 1: return VK_IMAGE_ASPECT_PLANE_1_BIT;
    case 2: return VK_IMAGE_ASPECT_PLANE_2_BIT;
    default: return (VkImageAspectFlagBits)0;
    }
}

const char* md_vk_result_string(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_ERROR_UNRECOGNIZED";
    }
}

md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* context) {
    if (context == NULL || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE) return NULL;
    md_vk_importer_t* importer = calloc(1, sizeof(*importer));
    if (importer == NULL) return NULL;
    importer->context = *context;
    if (importer->context.image_usage == 0) importer->context.image_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    importer->get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            importer->context.device, "vkGetMemoryFdPropertiesKHR");
    importer->import_semaphore_fd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            importer->context.device, "vkImportSemaphoreFdKHR");
    clear_pool(&importer->pool);
    return importer;
}

static void destroy_pool_objects(md_vk_importer_t* importer) {
    VkDevice device = importer->context.device;
    for (uint32_t i = 0; i < importer->pool.buffer_count; ++i) {
        if (importer->pool.acquire_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.acquire_semaphores[i], NULL);
        }
        if (importer->pool.release_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.release_semaphores[i], NULL);
        }
        if (importer->pool.views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, importer->pool.views[i], NULL);
        }
        if (importer->pool.images[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, importer->pool.images[i], NULL);
        }
        for (uint32_t p = 0; p < importer->pool.plane_count; ++p) {
            if (importer->pool.plane_memories[i][p] != VK_NULL_HANDLE) {
                vkFreeMemory(device, importer->pool.plane_memories[i][p], NULL);
            }
        }
    }
    if (importer->pool.ycbcr_conversion != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(device, importer->pool.ycbcr_conversion, NULL);
    }
    clear_pool(&importer->pool);
}

void md_vk_importer_release_pool(md_vk_importer_t* importer) {
    if (importer == NULL || !importer->pool_active) return;
    destroy_pool_objects(importer);
    importer->pool_active = false;
}

void md_vk_importer_free(md_vk_importer_t* importer) {
    if (importer == NULL) return;
    md_vk_importer_release_pool(importer);
    free(importer);
}

const md_vk_imported_pool_t* md_vk_importer_pool(const md_vk_importer_t* importer) {
    return importer != NULL && importer->pool_active ? &importer->pool : NULL;
}

static int import_plane_memory(md_vk_importer_t* importer, const md_buffer_pool_t* source,
                               uint32_t image_index, uint32_t plane_index,
                               bool disjoint) {
    VkDevice device = importer->context.device;
    const md_plane_t* plane = &source->planes[image_index][plane_index];
    int query_fd = plane->fd;
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = NULL,
        .memoryTypeBits = 0,
    };
    if (importer->get_memory_fd_properties == NULL) return MD_ERR_UNSUPPORTED;
    VkResult vk_result = importer->get_memory_fd_properties(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, query_fd, &fd_properties);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;

    VkImagePlaneMemoryRequirementsInfo plane_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
        .pNext = NULL,
        .planeAspect = disjoint ? plane_aspect(plane_index) : (VkImageAspectFlagBits)0,
    };
    if (disjoint && plane_info.planeAspect == 0) return MD_ERR_UNSUPPORTED;
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = disjoint ? &plane_info : NULL,
        .image = importer->pool.images[image_index],
    };
    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .pNext = NULL,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);
    uint32_t type_bits = requirements.memoryRequirements.memoryTypeBits &
                         fd_properties.memoryTypeBits;

    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(importer->context.physical_device, &memory_properties);
    uint32_t candidates[VK_MAX_MEMORY_TYPES];
    uint32_t candidate_count = 0;
    /* Prefer device-local memory, but retry every compatible type. PRIME and
     * some proprietary drivers expose the DMA-BUF only through a non-local
     * import type. */
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((type_bits & (UINT32_C(1) << i)) == 0) continue;
            const bool device_local =
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            if ((pass == 0 && !device_local) || (pass == 1 && device_local)) continue;
            candidates[candidate_count++] = i;
        }
    }
    if (candidate_count == 0) return MD_ERR_UNSUPPORTED;

    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = NULL,
        .image = importer->pool.images[image_index],
        .buffer = VK_NULL_HANDLE,
    };
    VkDeviceMemory* memory = &importer->pool.plane_memories[image_index][plane_index];
    VkResult last_result = VK_ERROR_UNKNOWN;
    bool allocated = false;
    for (uint32_t candidate = 0; candidate < candidate_count; ++candidate) {
        int imported_fd = fcntl(plane->fd, F_DUPFD_CLOEXEC, 0);
        if (imported_fd < 0) return MD_ERR_IO;
        VkImportMemoryFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = &dedicated_info,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = imported_fd,
        };
        VkMemoryAllocateInfo allocation_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = requirements.memoryRequirements.size,
            .memoryTypeIndex = candidates[candidate],
        };
        last_result = vkAllocateMemory(device, &allocation_info, NULL, memory);
        if (last_result == VK_SUCCESS) {
            allocated = true;
            break;
        }
        close(imported_fd);
    }
    if (!allocated) {
        return last_result == VK_ERROR_INVALID_EXTERNAL_HANDLE ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }
    if (plane_index == 0) importer->pool.memories[image_index] = *memory;

    VkBindImagePlaneMemoryInfo bind_plane_info = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
        .pNext = NULL,
        .planeAspect = disjoint ? plane_aspect(plane_index) : (VkImageAspectFlagBits)0,
    };
    VkBindImageMemoryInfo bind_info = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext = disjoint ? &bind_plane_info : NULL,
        .image = importer->pool.images[image_index],
        .memory = *memory,
        .memoryOffset = 0,
    };
    vk_result = vkBindImageMemory2(device, 1, &bind_info);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;
    return MD_OK;
}

static int import_one_image(md_vk_importer_t* importer, const md_buffer_pool_t* source,
                            uint32_t index, VkFormat format, VkComponentMapping mapping) {
    VkDevice device = importer->context.device;
    const bool disjoint = format_is_disjoint(source->fourcc);
    VkResult vk_result;

    VkSubresourceLayout layouts[MIRAGE_DISPLAY_MAX_PLANES];
    memset(layouts, 0, sizeof(layouts));
    for (uint32_t p = 0; p < source->plane_count; ++p) {
        layouts[p].offset = source->planes[index][p].offset;
        /* The modifier extension consumes offset/rowPitch; size is a
         * driver-computed subresource property and must not constrain an
         * imported allocation to the producer's bookkeeping value. */
        layouts[p].size = 0;
        layouts[p].rowPitch = source->planes[index][p].stride;
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifier = source->modifier,
        .drmFormatModifierPlaneCount = source->plane_count,
        .pPlaneLayouts = layouts,
    };
    VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .flags = disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {source->width, source->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = importer->context.image_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_result = vkCreateImage(device, &image_info, NULL, &importer->pool.images[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_UNSUPPORTED;

    const uint32_t allocation_count = disjoint ? source->plane_count : 1u;
    for (uint32_t p = 0; p < allocation_count; ++p) {
        int rc = import_plane_memory(importer, source, index, p, disjoint);
        if (rc != MD_OK) return rc;
    }

    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .pNext = NULL,
        .conversion = importer->pool.ycbcr_conversion,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = disjoint ? &conversion_info : NULL,
        .flags = 0,
        .image = importer->pool.images[index],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vk_result = vkCreateImageView(device, &view_info, NULL, &importer->pool.views[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
    };
    vk_result = vkCreateSemaphore(device, &semaphore_info, NULL,
                                  &importer->pool.acquire_semaphores[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;
    vk_result = vkCreateSemaphore(device, &semaphore_info, NULL,
                                  &importer->pool.release_semaphores[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;
    return MD_OK;
}

int md_vk_importer_import_pool(md_vk_importer_t* importer, const md_buffer_pool_t* pool) {
    if (importer == NULL || pool == NULL || importer->pool_active) return MD_ERR_STATE;
    if (pool->buffer_count < 2 || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count == 0 || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        pool->width == 0 || pool->height == 0 || pool->generation == 0) return MD_ERR_INVALID;
    if (format_is_disjoint(pool->fourcc) && pool->plane_count != 2) return MD_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < pool->buffer_count; ++i) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (pool->planes[i][p].fd < 0 || pool->planes[i][p].stride == 0 ||
                pool->planes[i][p].size == 0) return MD_ERR_INVALID;
        }
    }
    VkFormat format;
    VkComponentMapping mapping;
    int rc = md_vk_fourcc_to_format(pool->fourcc, &format, &mapping);
    if (rc != MD_OK) return rc;
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
        VkSamplerYcbcrConversionCreateInfo conversion_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            .pNext = NULL,
            .format = format,
            .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
            .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
            .components = mapping,
            .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .chromaFilter = VK_FILTER_LINEAR,
            .forceExplicitReconstruction = VK_FALSE,
        };
        VkResult result = vkCreateSamplerYcbcrConversion(importer->context.device,
                                                         &conversion_info, NULL,
                                                         &importer->pool.ycbcr_conversion);
        if (result != VK_SUCCESS) {
            clear_pool(&importer->pool);
            return result == VK_ERROR_FORMAT_NOT_SUPPORTED ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
        }
    }
    for (uint32_t i = 0; i < pool->buffer_count; ++i) {
        rc = import_one_image(importer, pool, i, format, mapping);
        if (rc != MD_OK) {
            destroy_pool_objects(importer);
            return rc;
        }
    }
    importer->pool_active = true;
    return MD_OK;
}

static int import_semaphore(md_vk_importer_t* importer, uint32_t buffer_index, int fd,
                            VkExternalSemaphoreHandleTypeFlagBits handle_type,
                            VkSemaphoreImportFlags flags, VkSemaphore* out) {
    if (importer == NULL || out == NULL || fd < 0 || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count || importer->import_semaphore_fd == NULL) {
        if (fd >= 0) close(fd);
        return MD_ERR_INVALID;
    }
    VkSemaphore semaphore = handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                ? importer->pool.acquire_semaphores[buffer_index]
                                : importer->pool.release_semaphores[buffer_index];
    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .pNext = NULL,
        .semaphore = semaphore,
        .flags = flags,
        .handleType = handle_type,
        .fd = fd,
    };
    VkResult result = importer->import_semaphore_fd(importer->context.device, &import_info);
    if (result != VK_SUCCESS) {
        close(fd);
        return result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }
    *out = semaphore;
    return MD_OK;
}

int md_vk_import_acquire_sync(md_vk_importer_t* importer, uint32_t buffer_index,
                              int acquire_sync_fd, VkSemaphore* out_semaphore) {
    return import_semaphore(importer, buffer_index, acquire_sync_fd,
                             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                             VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, out_semaphore);
}

int md_vk_import_release_syncobj(md_vk_importer_t* importer, uint32_t buffer_index,
                                 int release_syncobj_fd, VkSemaphore* out_semaphore) {
    return import_semaphore(importer, buffer_index, release_syncobj_fd,
                             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, 0, out_semaphore);
}

static int make_barrier(const md_vk_importer_t* importer, uint32_t buffer_index,
                        VkAccessFlags source_access, VkAccessFlags destination_access,
                        uint32_t source_family, uint32_t destination_family,
                        VkImageMemoryBarrier* out_barrier) {
    if (importer == NULL || out_barrier == NULL || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) return MD_ERR_INVALID;
    *out_barrier = (VkImageMemoryBarrier) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = source_access,
        .dstAccessMask = destination_access,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = source_family,
        .dstQueueFamilyIndex = destination_family,
        .image = importer->pool.images[buffer_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    return MD_OK;
}

int md_vk_importer_acquire_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags destination_access,
                                   VkImageMemoryBarrier* out_barrier) {
    return make_barrier(importer, buffer_index, 0, destination_access,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, importer != NULL
                            ? importer->context.queue_family_index : 0, out_barrier);
}

int md_vk_importer_release_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags source_access,
                                   VkImageMemoryBarrier* out_barrier) {
    return make_barrier(importer, buffer_index, source_access, 0,
                        importer != NULL ? importer->context.queue_family_index : 0,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, out_barrier);
}



