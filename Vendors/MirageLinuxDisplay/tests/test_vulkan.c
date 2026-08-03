#include "mirage_display_vulkan.h"
#include "mirage_display_vulkan_blit.h"
#include "mirage_display_vulkan_export.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#define DRM_FORMAT(code0, code1, code2, code3) \
    ((uint32_t)(code0) | ((uint32_t)(code1) << 8) | ((uint32_t)(code2) << 16) | \
     ((uint32_t)(code3) << 24))

static void test_fourcc_mapping(void) {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping mapping;
    assert(md_vk_fourcc_to_format(DRM_FORMAT('X', 'R', '2', '4'), &format, &mapping) == MD_OK);
    assert(format == VK_FORMAT_B8G8R8A8_UNORM);
    assert(mapping.a == VK_COMPONENT_SWIZZLE_ONE);

    assert(md_vk_fourcc_to_format(DRM_FORMAT('A', 'B', '2', '4'), &format, &mapping) == MD_OK);
    assert(format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(mapping.a == VK_COMPONENT_SWIZZLE_IDENTITY);

    assert(md_vk_fourcc_to_format(DRM_FORMAT('N', 'V', '1', '2'), &format, &mapping) == MD_OK);
    assert(format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    assert(mapping.r == VK_COMPONENT_SWIZZLE_IDENTITY);
    assert(md_vk_fourcc_to_format(DRM_FORMAT('Y', 'U', 'Y', 'V'), &format, &mapping) ==
           MD_ERR_UNSUPPORTED);
    assert(md_vk_fourcc_to_format(0, NULL, &mapping) == MD_ERR_INVALID);
}

static void test_invalid_context(void) {
    md_vk_context_t context = {0};
    assert(md_vk_importer_new(NULL) == NULL);
    assert(md_vk_importer_new(&context) == NULL);

    md_vk_blit_context_t blit_context = {0};
    assert(md_vk_blitter_new(NULL) == NULL);
    assert(md_vk_blitter_new(&blit_context) == NULL);

    md_vk_export_context_t export_context = {0};
    assert(md_vk_exporter_new(NULL) == NULL);
    assert(md_vk_exporter_new(&export_context) == NULL);

    /* Exporter copy/export path invalid-parameter handling. All failure
     * paths must leave caller-owned output fds at -1. */
    int copy_acquire = 0;
    int copy_release = 0;
    assert(md_vk_exporter_copy_frame(NULL, 0, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
                                     &copy_acquire, &copy_release) == MD_ERR_INVALID);
    assert(copy_acquire == -1 && copy_release == -1);
    assert(md_vk_exporter_copy_frame(NULL, 0, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
                                     NULL, NULL) == MD_ERR_INVALID);

    int export_acquire = 0;
    int export_release = 0;
    assert(md_vk_exporter_export_frame(NULL, 0, VK_NULL_HANDLE,
                                       &export_acquire, &export_release) == MD_ERR_INVALID);
    assert(export_acquire == -1 && export_release == -1);

    uint32_t slot = 99;
    assert(md_vk_exporter_acquire(NULL, &slot) == MD_ERR_STATE);
    assert(md_vk_exporter_pool(NULL) == NULL);
    assert(md_vk_exporter_image(NULL, 0) == VK_NULL_HANDLE);
    assert(md_vk_exporter_format(NULL) == VK_FORMAT_UNDEFINED);
    md_vk_exporter_release_pool(NULL);
    md_vk_exporter_cancel_frame(NULL, 0);
    md_vk_export_pool_info_t pool_info = {
        .generation = 1,
        .buffer_count = 3,
        .width = 1920,
        .height = 1080,
        .fourcc = DRM_FORMAT('X', 'B', '2', '4'),
        .plane_count = 1,
        .modifier = 0,
    };
    assert(md_vk_exporter_create_pool(NULL, &pool_info) == MD_ERR_INVALID);

    uint32_t count = 99;
    assert(md_vk_query_format_caps(VK_NULL_HANDLE, DRM_FORMAT('X', 'R', '2', '4'),
                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
                                   NULL, 0, &count) == MD_ERR_INVALID);
}

static void test_invalid_barrier(void) {
    VkImageMemoryBarrier barrier;
    assert(md_vk_importer_acquire_barrier(NULL, 0, VK_ACCESS_SHADER_READ_BIT, &barrier) ==
           MD_ERR_INVALID);
    assert(md_vk_importer_release_barrier(NULL, 0, VK_ACCESS_SHADER_READ_BIT, &barrier) ==
           MD_ERR_INVALID);
}

static void test_real_device_format_queries(void) {
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = NULL,
        .pApplicationName = "mirage-display-test",
        .applicationVersion = 1,
        .pEngineName = NULL,
        .engineVersion = 0,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .pApplicationInfo = &application,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = NULL,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = NULL,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create_info, NULL, &instance) != VK_SUCCESS) return;

    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, NULL) != VK_SUCCESS ||
        device_count == 0) {
        vkDestroyInstance(instance, NULL);
        return;
    }
    VkPhysicalDevice* devices = calloc(device_count, sizeof(*devices));
    assert(devices != NULL);
    assert(vkEnumeratePhysicalDevices(instance, &device_count, devices) == VK_SUCCESS);

    const uint32_t formats[] = {
        DRM_FORMAT('X', 'R', '2', '4'),
        DRM_FORMAT('N', 'V', '1', '2'),
    };
    for (uint32_t device = 0; device < device_count; ++device) {
        for (size_t format_index = 0; format_index < sizeof(formats) / sizeof(formats[0]);
             ++format_index) {
            uint32_t count = 0;
            assert(md_vk_query_format_caps(devices[device], formats[format_index],
                                           VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
                                           NULL, 0, &count) == MD_OK);
            if (count == 0) continue;
            md_format_cap_t* caps = calloc(count, sizeof(*caps));
            assert(caps != NULL);
            uint32_t written = count;
            assert(md_vk_query_format_caps(devices[device], formats[format_index],
                                           VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
                                           caps, count, &written) == MD_OK);
            assert(written == count);
            for (uint32_t i = 0; i < written; ++i) {
                assert(caps[i].fourcc == formats[format_index]);
                assert(caps[i].plane_count > 0);
                assert(caps[i].plane_count <= MIRAGE_DISPLAY_MAX_PLANES);
            }
            free(caps);
        }
    }

    free(devices);
    vkDestroyInstance(instance, NULL);
}

int main(void) {
    test_fourcc_mapping();
    test_invalid_context();
    test_invalid_barrier();
    test_real_device_format_queries();
    assert(md_vk_result_string(VK_SUCCESS) != NULL);
    return 0;
}
