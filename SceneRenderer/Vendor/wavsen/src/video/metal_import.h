#pragma once

#ifndef VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_METAL_EXT
#endif
#include <vulkan/vulkan.h>

#include <cstdint>

namespace wavsen::video {

// One imported CVPixelBuffer plane: a VkImage aliasing a MTLTexture that
// itself wraps the plane's IOSurface. `hold` retains the CVMetalTexture +
// VkImage so the caller can release both after the GPU finishes.
struct MetalImportedPlane {
    VkImage image { VK_NULL_HANDLE };
    void*   hold { nullptr };
};

// Bind the Metal-import bridge to the VkDevice's underlying MTLDevice and
// create the shared CVMetalTextureCache. Idempotent per device. Returns
// false if the Metal objects can't be exported (non-MoltenVK device).
bool MetalImportInit(VkInstance instance, VkDevice device);
void MetalImportShutdown(VkDevice device);

// Import one plane of `cv_pixel_buffer` (a CVPixelBufferRef) as a VkImage of
// `fmt` at (w,h). plane 0 is R8_UNORM (Y), plane 1 is R8G8_UNORM (CbCr).
// On success `out->image` is a ready-to-sample VkImage in UNDEFINED layout
// and `out->hold` owns the backing refs. Returns false on any failure.
bool MetalImportPlane(VkDevice device, void* cv_pixel_buffer, std::uint32_t plane, VkFormat fmt,
                      std::uint32_t w, std::uint32_t h, MetalImportedPlane* out);

// Destroy the VkImage and release the retained Metal/CoreVideo refs.
void MetalImportReleasePlane(VkDevice device, MetalImportedPlane* p);

} // namespace wavsen::video
