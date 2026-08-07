#include "metal_import.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <mutex>
#include <unordered_map>

namespace wavsen::video {

namespace {

struct DeviceBridge {
    id<MTLDevice>         mtl { nil };
    CVMetalTextureCacheRef cache { nullptr };
};

std::mutex                                    g_mutex;
std::unordered_map<VkDevice, DeviceBridge>    g_bridges;

struct PlaneHold {
    CVMetalTextureRef tex { nullptr };
};

id<MTLDevice> ExportMtlDevice(VkInstance instance, VkDevice device) {
    auto get = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        vkGetInstanceProcAddr(instance, "vkExportMetalObjectsEXT"));
    if (! get) return nil;
    VkExportMetalDeviceInfoEXT dev_info {};
    dev_info.sType    = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
    dev_info.mtlDevice = nil;
    VkExportMetalObjectsInfoEXT objs {};
    objs.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
    objs.pNext = &dev_info;
    get(device, &objs);
    return dev_info.mtlDevice;
}

} // namespace

bool MetalImportInit(VkInstance instance, VkDevice device) {
    std::lock_guard lock(g_mutex);
    auto it = g_bridges.find(device);
    if (it != g_bridges.end() && it->second.cache != nullptr) return true;

    id<MTLDevice> mtl = ExportMtlDevice(instance, device);
    if (mtl == nil) return false;

    CVMetalTextureCacheRef cache = nullptr;
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, mtl, nullptr, &cache) != kCVReturnSuccess
        || cache == nullptr) {
        return false;
    }
    g_bridges[device] = DeviceBridge { mtl, cache };
    return true;
}

void MetalImportShutdown(VkDevice device) {
    std::lock_guard lock(g_mutex);
    auto it = g_bridges.find(device);
    if (it == g_bridges.end()) return;
    if (it->second.cache) {
        CVMetalTextureCacheFlush(it->second.cache, 0);
        CFRelease(it->second.cache);
    }
    g_bridges.erase(it);
}

bool MetalImportPlane(VkDevice device, void* cv_pixel_buffer, std::uint32_t plane, VkFormat fmt,
                      std::uint32_t w, std::uint32_t h, MetalImportedPlane* out) {
    if (! cv_pixel_buffer || ! out) return false;

    CVMetalTextureCacheRef cache = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_bridges.find(device);
        if (it == g_bridges.end() || it->second.cache == nullptr) return false;
        cache = it->second.cache;
    }

    auto pb = static_cast<CVPixelBufferRef>(cv_pixel_buffer);
    MTLPixelFormat mtl_fmt = (fmt == VK_FORMAT_R8G8_UNORM) ? MTLPixelFormatRG8Unorm
                                                           : MTLPixelFormatR8Unorm;

    CVMetalTextureRef cv_tex = nullptr;
    CVReturn cr = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, nullptr, mtl_fmt, w, h, plane, &cv_tex);
    if (cr != kCVReturnSuccess || cv_tex == nullptr) {
        if (cv_tex) CFRelease(cv_tex);
        return false;
    }
    id<MTLTexture> mtl_tex = CVMetalTextureGetTexture(cv_tex);
    if (mtl_tex == nil) {
        CFRelease(cv_tex);
        return false;
    }

    VkImportMetalTextureInfoEXT import {};
    import.sType      = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT;
    import.plane      = VK_IMAGE_ASPECT_COLOR_BIT;
    import.mtlTexture = (MTLTexture_id)mtl_tex;

    VkImageCreateInfo ci {};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext         = &import;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent        = { w, h, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &ci, nullptr, &image) != VK_SUCCESS || image == VK_NULL_HANDLE) {
        CFRelease(cv_tex);
        return false;
    }

    auto* hold = new PlaneHold { cv_tex };
    out->image = image;
    out->hold  = hold;
    return true;
}

void MetalImportReleasePlane(VkDevice device, MetalImportedPlane* p) {
    if (! p) return;
    if (p->image != VK_NULL_HANDLE) {
        vkDestroyImage(device, p->image, nullptr);
        p->image = VK_NULL_HANDLE;
    }
    if (p->hold) {
        auto* hold = static_cast<PlaneHold*>(p->hold);
        if (hold->tex) CFRelease(hold->tex);
        delete hold;
        p->hold = nullptr;
    }
}

} // namespace wavsen::video
