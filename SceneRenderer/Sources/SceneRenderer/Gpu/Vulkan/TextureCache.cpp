module;

#if defined(__linux__)
#include <string>
#include <memory>
#include <vector>
#endif

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#define SCENERENDERER_ENABLE_METAL_EXPORT 1
#else
#define SCENERENDERER_ENABLE_METAL_EXPORT 0
#endif
#include <vulkan/vulkan.h>
#include <rstd/macro.hpp>
#include "vk_mem_alloc.h"

#include "Utils/AutoDeletor.hpp"
#include "vvk/macros.hpp"

#include <cmath>
#include <limits>
#include <unordered_set>
#include <unistd.h>

module sr.vulkan;
import sr.core;
import rstd.log;
import rstd.cppstd;

import sr.types;
import sr.fs;
import wavsen.video;

using namespace sr;
using namespace sr::vulkan;

namespace sr
{
namespace vulkan
{
VkFormat ToVkType(TextureFormat tf) {
    switch (tf) {
    case TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    default: rstd_assert(false); return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkSamplerAddressMode ToVkType(sr::TextureWrap sam) {
    using namespace sr;
    switch (sam) {
    case TextureWrap::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::REPEAT:
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
VkFilter ToVkType(sr::TextureFilter sam) {
    using namespace sr;
    switch (sam) {
    case TextureFilter::LINEAR: return VK_FILTER_LINEAR;
    case TextureFilter::NEAREST:
    default: return VK_FILTER_NEAREST;
    }
}
} // namespace vulkan
} // namespace sr

namespace
{
VkSamplerCreateInfo GenSamplerInfo(TextureKey key) {
    auto& sam = key.sample;

    VkSamplerCreateInfo sampler_info { .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .pNext            = nullptr,
                                       .magFilter        = ToVkType(sam.magFilter),
                                       .minFilter        = (ToVkType(sam.minFilter)),
                                       .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                       .addressModeU     = (ToVkType(sam.wrapS)),
                                       .addressModeV     = (ToVkType(sam.wrapS)),
                                       .addressModeW     = (ToVkType(sam.wrapT)),
                                       .anisotropyEnable = (false),
                                       .maxAnisotropy    = (1.0f),
                                       .compareEnable    = (false),
                                       .compareOp        = VK_COMPARE_OP_NEVER,
                                       .minLod           = (0.0f),
                                       .maxLod           = (1.0f),
                                       .borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                       .unnormalizedCoordinates = (false) };
    return sampler_info;
}

std::optional<vvk::DeviceMemory> AllocateMemory(const vvk::Device& device, vvk::PhysicalDevice gpu,
                                                VkMemoryRequirements  reqs,
                                                VkMemoryPropertyFlags property,
                                                void*                 pNext = nullptr) {
    VkPhysicalDeviceMemoryProperties pros = gpu.GetMemoryProperties().memoryProperties;
    for (uint32_t i = 0; i < pros.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1 << i)) && (pros.memoryTypes[i].propertyFlags & property)) {
            VkMemoryAllocateInfo memory_allocate_info { .sType =
                                                            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .pNext           = pNext,
                                                        .allocationSize  = reqs.size,
                                                        .memoryTypeIndex = i };
            vvk::DeviceMemory    mem;
            VkResult             res = device.AllocateMemory(memory_allocate_info, mem);
            if (res == VK_SUCCESS) {
                return mem;
            } else {
                VVK_CHECK(res);
                return std::nullopt;
            }
        }
    }
    rstd_error("vulkan allocate memory failed, no memory match requires");
    return std::nullopt;
}

inline std::optional<VmaImageParameters>
CreateImage(const Device& device, VkExtent3D extent, u32 miplevel, VkFormat format,
            VkSamplerCreateInfo sampler_info, VkImageUsageFlags usage,
            VmaMemoryUsage        mem_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            VkSampleCountFlagBits samples   = VK_SAMPLE_COUNT_1_BIT) {
    VmaImageParameters image;
    do {
        // Multisample images can't have mipmaps; force levelCount=1 and
        // restrict usage to color attachment (no transfer/sampled needed
        // since the resolved sibling carries the readable copy).
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            miplevel = 1;
            if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
                usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
#if SCENERENDERER_ENABLE_METAL_EXPORT
        const bool metal_export =
            device.supportExt("VK_EXT_metal_objects") &&
            (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
        VkExportMetalObjectCreateInfoEXT metal_image_export {
            .sType            = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
            .pNext            = nullptr,
            .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
        };
#else
        const bool metal_export = false;
#endif
        VkImageCreateInfo info {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
#if SCENERENDERER_ENABLE_METAL_EXPORT
            .pNext                = metal_export ? &metal_image_export : nullptr,
#else
            .pNext                = nullptr,
#endif
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = format,
            .extent                = extent,
            .mipLevels             = miplevel,
            .arrayLayers           = 1,
            .samples               = samples,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = usage,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        image.extent = info.extent;
        VmaAllocationCreateInfo vma_info {};
        vma_info.usage = mem_usage;
        VVK_CHECK_ACT(break,
                      vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

        image.mipmap_level = miplevel;
        {
            const bool depth_usage = (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
#if SCENERENDERER_ENABLE_METAL_EXPORT
            VkExportMetalObjectCreateInfoEXT metal_view_export {
                .sType            = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
                .pNext            = nullptr,
                .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
            };
#endif
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
#if SCENERENDERER_ENABLE_METAL_EXPORT
                .pNext    = metal_export ? &metal_view_export : nullptr,
#else
                .pNext    = nullptr,
#endif
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask =
                            depth_usage ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = miplevel,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break, device.handle().CreateImageView(createinfo, image.view));
        }
        VVK_CHECK_ACT(break, device.handle().CreateSampler(sampler_info, image.sampler));
        return image;
    } while (false);
    /*
    if (result != vk::Result::eSuccess) {
        device.DestroyImageParameters(image);
    }
    */
    return std::nullopt;
}

void RecordQueuedImageUpload(vvk::CommandBuffer& cmd, const ImageParameters& image,
                             VkBuffer buffer, VkDeviceSize buffer_offset,
                             VkOffset3D image_offset, VkExtent3D image_extent,
                             VkImageLayout old_layout, VkImageLayout final_layout,
                             std::uint32_t mip_level) {
    if (image.handle == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || image_extent.width == 0 ||
        image_extent.height == 0)
        return;

    VkImageSubresourceRange range {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = mip_level,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    VkImageMemoryBarrier to_xfer {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask    = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0u
                                : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout        = old_layout,
        .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image            = image.handle,
        .subresourceRange = range,
    };
    cmd.PipelineBarrier(old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_TRANSFER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        to_xfer);

    VkBufferImageCopy region {
        .bufferOffset = buffer_offset,
        .imageSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = mip_level,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .imageOffset = image_offset,
        .imageExtent = image_extent,
    };
    cmd.CopyBufferToImage(buffer,
                          image.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          region);

    VkImageMemoryBarrier to_shader {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout        = final_layout,
        .image            = image.handle,
        .subresourceRange = range,
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        to_shader);
}

void RecordQueuedImageInitialization(vvk::CommandBuffer& cmd, const ImageParameters& image,
                                     VkImageLayout final_layout, VkImageAspectFlags aspect,
                                     bool clear_color) {
    if (image.handle == VK_NULL_HANDLE) return;

    const VkImageLayout intermediate =
        clear_color ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : final_layout;
    VkAccessFlags dst_access = 0;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (clear_color) {
        dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (final_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        dst_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    } else if (final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        dst_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dst_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else {
        dst_access = VK_ACCESS_SHADER_READ_BIT;
        dst_stage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    VkImageSubresourceRange range {
        .aspectMask     = aspect,
        .baseMipLevel   = 0,
        .levelCount     = image.mipmap_level,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    VkImageMemoryBarrier initialize {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask    = 0,
        .dstAccessMask    = dst_access,
        .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout        = intermediate,
        .image            = image.handle,
        .subresourceRange = range,
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        dst_stage,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        initialize);

    if (! clear_color) return;
    VkClearColorValue clear { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    cmd.ClearColorImage(image.handle, intermediate, &clear, range);
    VkImageMemoryBarrier ready {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout        = intermediate,
        .newLayout        = final_layout,
        .image            = image.handle,
        .subresourceRange = range,
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        ready);
}
} // namespace

std::size_t TextureKey::HashValue(const TextureKey& k) {
    std::size_t seed { 0 };
    utils::hash_combine(seed, k.width);
    utils::hash_combine(seed, k.height);
    utils::hash_combine(seed, (int)k.usage);
    utils::hash_combine(seed, (int)k.format);
    utils::hash_combine(seed, (int)k.mipmap_level);

    utils::hash_combine(seed, (int)k.sample.wrapS);
    utils::hash_combine(seed, (int)k.sample.wrapT);
    utils::hash_combine(seed, (int)k.sample.magFilter);
    utils::hash_combine(seed, (int)k.samples);
    return seed;
}

std::optional<VmaImageParameters>
TextureCache::CreateRenderTargetTex(uint32_t width, uint32_t height, VkFormat format) {
    VkSamplerCreateInfo sampler_info {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .magFilter               = VK_FILTER_NEAREST,
        .minFilter               = VK_FILTER_NEAREST,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable        = false,
        .maxAnisotropy           = 1.0f,
        .compareEnable           = false,
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = 0.0f,
        .maxLod                  = 1.0f,
        .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = false,
    };
    VkExtent3D ext { width, height, 1 };
    return CreateImage(m_device,
                       ext,
                       /*miplevel=*/1u,
                       format,
                       sampler_info,
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}

ImageSlotsRef TextureCache::CreateTex(Image& image) {
    if (exists(m_tex_map, image.key)) {
        return m_tex_map.at(image.key);
    }

    if (image.header.type == ImageType::VIDEO) {
        return CreateVideoTex(image);
    }

    ImageSlots img_slots;
    const auto pending_begin = m_pending_uploads.size();
    auto fail = [&]() -> ImageSlotsRef {
        m_pending_uploads.erase(m_pending_uploads.begin() + (std::ptrdiff_t)pending_begin,
                                m_pending_uploads.end());
        return {};
    };

    img_slots.slots.resize(image.slots.size());

    auto& sam = image.header.sample;

    for (usize i = 0; i < image.slots.size(); i++) {
        auto& image_paras   = img_slots.slots[i];
        auto& image_slot    = image.slots[i];
        auto  mipmap_levels = image_slot.mipmaps.size();

        // check data
        if (! image_slot) return fail();
        VkSamplerCreateInfo sampler_info {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .magFilter               = ToVkType(sam.magFilter),
            .minFilter               = (ToVkType(sam.minFilter)),
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU            = (ToVkType(sam.wrapS)),
            .addressModeV            = (ToVkType(sam.wrapS)),
            .addressModeW            = (ToVkType(sam.wrapT)),
            .anisotropyEnable        = (false),
            .maxAnisotropy           = (1.0f),
            .compareEnable           = (false),
            .compareOp               = VK_COMPARE_OP_NEVER,
            .minLod                  = (0.0f),
            .maxLod                  = (float)mipmap_levels,
            .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = (false),
        };
        VkFormat   format = ToVkType(image.header.format);
        VkExtent3D ext { (u32)image_slot.width, (u32)image_slot.height, 1 };

        if (auto opt = CreateImage(m_device,
                                   ext,
                                   (u32)mipmap_levels,
                                   format,
                                   sampler_info,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            opt.has_value()) {
            image_paras = std::move(opt.value());
            AssignImageGeneration(image_paras);
        } else {
            return fail();
        }

        for (usize j = 0; j < image_slot.mipmaps.size(); j++) {
            auto&               image_data = image_slot.mipmaps[j];
            if (image_data.size == 0 || image_data.data == nullptr || image_data.width <= 0 ||
                image_data.height <= 0) {
                return fail();
            }
            PendingImageUpload up {};
            if (! CreateStagingBuffer(
                    m_device.vma_allocator(), (u32)image_data.size, up.owned_stage)) {
                return fail();
            }
            {
                void* v_data;
                VVK_CHECK(up.owned_stage.handle.MapMemory(&v_data));
                std::memcpy(v_data, image_data.data.get(), (u32)image_data.size);
                up.owned_stage.handle.UnMapMemory();
            }
            up.image         = ToImageParameters(image_paras);
            up.buffer        = *up.owned_stage.handle;
            up.image_extent  = VkExtent3D { (u32)image_data.width, (u32)image_data.height, 1 };
            up.old_layout    = VK_IMAGE_LAYOUT_UNDEFINED;
            up.final_layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            up.mip_level     = (std::uint32_t)j;
            m_pending_uploads.push_back(std::move(up));
        }
    }
    m_tex_map[image.key] = std::move(img_slots);
    return m_tex_map[image.key];
}

std::optional<VmaImageParameters> TextureCache::CreateTex(TextureKey tex_key) {
    VmaImageParameters image_paras;
    do {
        VkSamplerCreateInfo sam_info = GenSamplerInfo(tex_key);
        VkFormat            format   = ToVkType(tex_key.format);
        VkExtent3D          ext { (u32)tex_key.width, (u32)tex_key.height, 1 };
        const bool          depth_usage = tex_key.usage == TexUsage::DEPTH;
        VkImageUsageFlags   usage =
            depth_usage ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        if (auto opt = CreateImage(m_device,
                                   ext,
                                   tex_key.mipmap_level,
                                   format,
                                   sam_info,
                                   usage,
                                   VMA_MEMORY_USAGE_GPU_ONLY,
                                   tex_key.samples);
            opt.has_value()) {
            image_paras = std::move(opt.value());
            AssignImageGeneration(image_paras);
        } else
            break;

        // Defer the transition into the first frame command buffer. All graph
        // resources are prepared before that buffer is recorded, so this
        // removes one queue submit + vkDeviceWaitIdle per render target while
        // preserving the exact layout expected by the first pass.
        const VkImageLayout initial_layout =
            depth_usage ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : tex_key.samples == VK_SAMPLE_COUNT_1_BIT
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m_pending_initializations.push_back(PendingImageInitialization {
            .image        = ToImageParameters(image_paras),
            .final_layout = initial_layout,
            .aspect       = depth_usage ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
        });
        return image_paras;
    } while (false);
    return std::nullopt;
}

/* ===========================================================================
 * Video-tex pipeline
 *
 * When TextureAssetDecoder detects an MP4 / WebM container inlined in a
 * .tex body (header.type == ImageType::VIDEO), it doesn't decompress
 * pixels — it stashes the pkg's IBinaryStream + byte range in
 * ImageData. CreateTex routes those Images here:
 *
 *   1. We allocate a stable RGBA8 VkImage at (w,h), cleared to black,
 *      and register it in m_tex_map under the Image's key so the rest
 *      of the renderer (descriptor sets, sprite-anim fallback, etc.)
 *      sees an ordinary single-slot texture.
 *   2. We wrap the pkg stream in a wavsen::video::IInputStream that
 *      clamps reads/seeks to [offset, offset+size). Lifetime: the
 *      shared_ptr<sr::fs::IBinaryStream> lands inside the adapter and
 *      stays alive as long as the VideoDecoder owns it.
 *   3. We spin up a wavsen::video::VideoDecoder via open_from_stream.
 *      When hwdec is enabled, a lazily-created wavsen Producer supplies
 *      the hwdevice; otherwise the decoder stays sw-only. The avformat
 *      probe is happy with ftyp/EBML at the head of the stream.
 *   4. Each render tick PumpVideoTextures advances PTS, pulls the
 *      correct frame view for the active decoder kind, and writes the
 *      stable RGBA8 VkImage through wavsen::video::YuvToRgba. Hardware
 *      decode uses SceneRenderer's own VkDevice via Producer::from_external
 *      so conversion and material sampling stay on the same device.
 *
 * ========================================================================= */

namespace
{

// Wraps an sr::fs::IBinaryStream sub-range as a wavsen avio source.
// Holds the underlying stream alive via shared_ptr so the .pkg file
// handle survives until the decoder closes.
class PkgRangedInputStream : public wavsen::video::IInputStream {
public:
    PkgRangedInputStream(std::shared_ptr<sr::fs::IBinaryStream> base, std::int64_t offset,
                         std::int64_t length)
        : m_base(std::move(base)), m_offset(offset), m_length(length) {}

    int read(std::uint8_t* buf, int size) override {
        if (! m_base || size <= 0) return 0;
        if (m_cursor >= m_length) return 0; /* EOF — avio shim maps to AVERROR_EOF */
        std::int64_t remain = m_length - m_cursor;
        int          take   = size < remain ? size : static_cast<int>(remain);
        m_base->SeekSet(static_cast<idx>(m_offset + m_cursor));
        auto got = m_base->Read(buf, static_cast<usize>(take));
        if (got == 0) return 0;
        m_cursor += static_cast<std::int64_t>(got);
        return static_cast<int>(got);
    }

    std::int64_t seek(std::int64_t offset, int whence) override {
        constexpr int AVSEEK_SIZE = 0x10000;
        if (whence == AVSEEK_SIZE) return m_length;
        std::int64_t next;
        switch (whence) {
        case 0: /* SEEK_SET */ next = offset; break;
        case 1: /* SEEK_CUR */ next = m_cursor + offset; break;
        case 2: /* SEEK_END */ next = m_length + offset; break;
        default: return -1;
        }
        if (next < 0 || next > m_length) return -1;
        m_cursor = next;
        return m_cursor;
    }

private:
    std::shared_ptr<sr::fs::IBinaryStream> m_base;
    std::int64_t                            m_offset { 0 };
    std::int64_t                            m_length { 0 };
    std::int64_t                            m_cursor { 0 };
};

wavsen::video::HwAccel ParseHwdec(std::string_view value) {
    if (value == "vulkan") return wavsen::video::HwAccel::Vulkan;
    if (value == "videotoolbox") return wavsen::video::HwAccel::VideoToolbox;
    if (value == "none") return wavsen::video::HwAccel::None;
    return wavsen::video::HwAccel::Auto;
}

const char* HwdecLabel(wavsen::video::HwAccel h) {
    switch (h) {
    case wavsen::video::HwAccel::Auto: return "auto";
    case wavsen::video::HwAccel::Vulkan: return "vulkan";
    case wavsen::video::HwAccel::VideoToolbox: return "videotoolbox";
    case wavsen::video::HwAccel::None: return "none";
    default: break;
    }
    return "?";
}

const char* FrameKindLabel(wavsen::video::FrameKind k) {
    switch (k) {
    case wavsen::video::FrameKind::Sw: return "sw";
    case wavsen::video::FrameKind::VulkanShared: return "vulkan-shared";
    case wavsen::video::FrameKind::VideoToolboxSw: return "videotoolbox-sw";
    default: break;
    }
    return "?";
}

bool NeedsSharedVulkanProducer(wavsen::video::HwAccel hwdec) {
#if defined(__APPLE__)
    return hwdec == wavsen::video::HwAccel::Vulkan;
#else
    return hwdec != wavsen::video::HwAccel::None;
#endif
}

std::vector<const char*> ExtensionPtrs(std::span<const std::string> names) {
    std::vector<const char*> out;
    out.reserve(names.size());
    for (const auto& name : names) out.push_back(name.c_str());
    return out;
}

std::vector<wavsen::video::QueueFamily> QueueFamiliesForFfmpeg(const Device& device) {
    auto                                    props = device.gpu().GetQueueFamilyProperties();
    std::vector<wavsen::video::QueueFamily> out;
    out.reserve(props.size());
    for (std::uint32_t i = 0; i < props.size(); ++i) {
        out.push_back(wavsen::video::QueueFamily {
            .index      = i,
            .flags      = props[i].queueFlags,
            .video_caps = 0,
        });
    }
    return out;
}

wavsen::video::Producer::ExternalDeviceInfo
MakeExternalProducerInfo(const Device& device, std::uint32_t width, std::uint32_t height) {
    return wavsen::video::Producer::ExternalDeviceInfo {
        .instance                    = device.instance_handle(),
        .physical_device             = *device.gpu(),
        .device                      = *device.handle(),
        .queue                       = *device.graphics_queue().handle,
        .queue_family_index          = device.graphics_queue().family_index,
        .queue_families              = QueueFamiliesForFfmpeg(device),
        .enabled_instance_extensions = ExtensionPtrs(device.enabled_instance_extensions()),
        .enabled_device_extensions   = ExtensionPtrs(device.enabled_device_extensions()),
        .api_version                 = device.instance_api_version(),
        .width                       = width,
        .height                      = height,
    };
}

void CloseSyncFd(int fd) {
    if (fd >= 0) ::close(fd);
}

std::uint8_t FloatToByte(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

bool ConvertNv12ToRgba(const wavsen::video::Nv12Frame& frame,
                       const wavsen::video::ColorMatrix& matrix,
                       std::vector<std::uint8_t>& rgba) {
    if (frame.width == 0 || frame.height == 0) return false;
    const std::size_t y_size = static_cast<std::size_t>(frame.width) * frame.height;
    const std::size_t expected = y_size + y_size / 2;
    if (frame.data.size() < expected) return false;

    rgba.resize(y_size * 4);
    const auto* y_plane  = frame.data.data();
    const auto* uv_plane = frame.data.data() + y_size;

    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint32_t uv_y = y / 2;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::uint32_t uv_x = x & ~1u;
            const std::size_t   yi   = static_cast<std::size_t>(y) * frame.width + x;
            const std::size_t   uvi  = static_cast<std::size_t>(uv_y) * frame.width + uv_x;

            const float yy = static_cast<float>(y_plane[yi]) / 255.0f + matrix.offset[0];
            const float cb = static_cast<float>(uv_plane[uvi]) / 255.0f + matrix.offset[1];
            const float cr = static_cast<float>(uv_plane[uvi + 1]) / 255.0f + matrix.offset[2];

            const float r = yy * matrix.m_r[0] + cb * matrix.m_r[1] + cr * matrix.m_r[2];
            const float g = yy * matrix.m_g[0] + cb * matrix.m_g[1] + cr * matrix.m_g[2];
            const float b = yy * matrix.m_b[0] + cb * matrix.m_b[1] + cr * matrix.m_b[2];

            const std::size_t oi = yi * 4;
            rgba[oi + 0]         = FloatToByte(r);
            rgba[oi + 1]         = FloatToByte(g);
            rgba[oi + 2]         = FloatToByte(b);
            rgba[oi + 3]         = 255;
        }
    }
    return true;
}

} // anonymous namespace

struct TextureCache::VideoRegistry {
    TextureCache::VideoDecodeOptions          options;
    std::unique_ptr<wavsen::video::Producer>  producer;
    std::unique_ptr<wavsen::video::YuvToRgba> yuv;
    std::uint32_t                             yuv_max_width { 0 };
    std::uint32_t                             yuv_max_height { 0 };

    struct Slot {
        std::string   key; /* matches m_tex_map */
        std::uint32_t width { 0 };
        std::uint32_t height { 0 };
        /* RGBA8 target lives in m_tex_map[key].slots[0] (transferred
         * during CreateVideoTex). Pump retrieves it via lookup so the
         * single owner stays in the cache. */
        VmaImageParameters                           image; /* moved into m_tex_map */
        std::unique_ptr<wavsen::video::VideoDecoder> decoder;
        wavsen::video::Nv12Frame                     nv12_scratch;
        std::vector<std::uint8_t>                    rgba_scratch;
        VmaBufferParameters                          upload_stage;
        std::size_t                                  upload_stage_size { 0 };
        double                                       pts_acc { 0.0 };
        double                                       last_pts { -1.0 };
        double                                       pts_origin {
            std::numeric_limits<double>::quiet_NaN()
        };
        double                                       frame_interval { 1.0 / 30.0 };
        uint64_t                                     active_epoch { 0 };
        bool                                         have_frame { false };
    };
    std::vector<std::unique_ptr<Slot>> slots;

    const wavsen::video::Producer* ensureProducer(const Device& device, std::uint32_t width,
                                                  std::uint32_t height) {
        if (producer) return producer.get();
        auto r =
            wavsen::video::Producer::from_external(MakeExternalProducerInfo(device, width, height));
        if (r.is_err()) {
            rstd_warn(
                "CreateVideoTex: shared-device producer unavailable; falling back to sw decode: {}",
                std::move(r).unwrap_err().message);
            return nullptr;
        }
        producer = std::move(r).unwrap();
        return producer.get();
    }

    wavsen::video::YuvToRgba* ensureYuv(const Device& device, std::uint32_t width,
                                        std::uint32_t height) {
        if (yuv && width <= yuv_max_width && height <= yuv_max_height) return yuv.get();
        auto next_w = std::max(width, yuv_max_width);
        auto next_h = std::max(height, yuv_max_height);
        auto r      = wavsen::video::YuvToRgba::create(device.instance_handle(),
                                                       *device.gpu(),
                                                       *device.handle(),
                                                       device.graphics_queue().family_index,
                                                       *device.graphics_queue().handle,
                                                       next_w,
                                                       next_h);
        if (r.is_err()) {
            rstd_error("CreateVideoTex: YuvToRgba create failed: {}",
                       std::move(r).unwrap_err().message);
            return nullptr;
        }
        yuv            = std::move(r).unwrap();
        yuv_max_width  = next_w;
        yuv_max_height = next_h;
        return yuv.get();
    }
};

ImageSlotsRef TextureCache::CreateVideoTex(Image& image) {
    if (image.slots.empty() || image.slots[0].mipmaps.empty()) return {};
    auto& mip = image.slots[0].mipmaps[0];
    if (! mip.videoStream || mip.videoSize <= 0 || mip.width <= 0 || mip.height <= 0) {
        rstd_error("CreateVideoTex: incomplete video-tex slot for {}", image.key);
        return {};
    }

    if (! m_video_registry) {
        m_video_registry          = std::make_unique<VideoRegistry>();
        m_video_registry->options = m_video_decode_options;
    }
    /* Pull the IBinaryStream back out of the opaque shared_ptr<void>
     * the parser stored. The cast is safe because TextureAssetDecoder is
     * the only writer and always populates with shared_ptr<IBinaryStream>
     * via the converting constructor. */
    std::shared_ptr<sr::fs::IBinaryStream> pkg_stream(
        mip.videoStream, static_cast<sr::fs::IBinaryStream*>(mip.videoStream.get()));

    auto slot = std::make_unique<VideoRegistry::Slot>();
    slot->key = image.key;
    /* NV12 chroma is 4:2:0 → both dimensions must be even. Keep the source
     * dimensions: video decode must not be used as a quality/performance
     * trade-off. */
    slot->width  = static_cast<std::uint32_t>(mip.width | (mip.width & 1));
    slot->height = static_cast<std::uint32_t>(mip.height | (mip.height & 1));
    if (slot->width != static_cast<std::uint32_t>(mip.width))
        slot->width = static_cast<std::uint32_t>(mip.width + 1);
    if (slot->height != static_cast<std::uint32_t>(mip.height))
        slot->height = static_cast<std::uint32_t>(mip.height + 1);

    /* 1) Allocate the stable RGBA8 target. */
    VkSamplerCreateInfo sampler_info {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .magFilter               = ToVkType(image.header.sample.magFilter),
        .minFilter               = ToVkType(image.header.sample.minFilter),
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = ToVkType(image.header.sample.wrapS),
        .addressModeV            = ToVkType(image.header.sample.wrapS),
        .addressModeW            = ToVkType(image.header.sample.wrapT),
        .anisotropyEnable        = false,
        .maxAnisotropy           = 1.0f,
        .compareEnable           = false,
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = 0.0f,
        .maxLod                  = 1.0f,
        .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = false,
    };
    VkExtent3D ext { slot->width, slot->height, 1 };
    auto       img_opt = CreateImage(m_device,
                                     ext,
                                     /*miplevel=*/1u,
                                     VK_FORMAT_R8G8B8A8_UNORM,
                                     sampler_info,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
    if (! img_opt) {
        rstd_error("CreateVideoTex: VkImage allocation failed for {}", image.key);
        return {};
    }
    slot->image = std::move(*img_opt);
    AssignImageGeneration(slot->image);

    /* 2) Initial layout + black clear is recorded with the first frame. This
     * keeps the video image valid before its decoder produces output without
     * forcing a standalone queue submit and full-device wait. */
    m_pending_initializations.push_back(PendingImageInitialization {
        .image        = ToImageParameters(slot->image),
        .final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .aspect       = VK_IMAGE_ASPECT_COLOR_BIT,
        .clear_color  = true,
    });

    /* 3) Open the decoder. The factory hands out a fresh
     * PkgRangedInputStream per trial; the captured shared_ptr keeps the
     * underlying pkg file handle alive. */
    auto factory = [pkg = pkg_stream,
                    off = static_cast<std::int64_t>(mip.videoOffset),
                    len = static_cast<std::int64_t>(
                        mip.videoSize)]() -> std::unique_ptr<wavsen::video::IInputStream> {
        return std::make_unique<PkgRangedInputStream>(pkg, off, len);
    };
    auto                    requested_hwdec = ParseHwdec(m_video_registry->options.hwdec);
    wavsen::video::OpenOpts opts {
        requested_hwdec,
        {},
    };
    const wavsen::video::Producer* producer = nullptr;
    if (NeedsSharedVulkanProducer(requested_hwdec)) {
        producer = m_video_registry->ensureProducer(m_device, slot->width, slot->height);
        if (! producer) opts.hwaccel = wavsen::video::HwAccel::None;
    }
    auto dec_r = wavsen::video::VideoDecoder::open_from_stream(std::move(factory),
                                                               slot->width,
                                                               slot->height,
                                                               /*loop=*/true,
                                                               producer,
                                                               opts);
    if (dec_r.is_err()) {
        rstd_error("CreateVideoTex: open_from_stream failed for {}: {}",
                   image.key,
                   dec_r.unwrap_err().message);
        return {};
    }
    slot->decoder = std::move(dec_r).unwrap();
    const double frame_interval = slot->decoder->frame_duration_seconds();
    if (std::isfinite(frame_interval) && frame_interval > 0.0 && frame_interval < 1.0) {
        slot->frame_interval = frame_interval;
    }
    rstd_info("CreateVideoTex: {} hwdec={} decoder kind={}",
              image.key,
              HwdecLabel(requested_hwdec),
              FrameKindLabel(slot->decoder->kind()));

    /* 4) Move VkImage ownership into m_tex_map[key] (the canonical
     * texture cache) and keep VideoRegistry::Slot referencing it by
     * key — Pump fetches via lookup. This avoids dual ownership of
     * VmaImageParameters (move-only) while keeping the same VkImage
     * handle stable across frames. */
    ImageSlots img_slots {};
    img_slots.slots.resize(1);
    img_slots.slots[0]   = std::move(slot->image);
    m_tex_map[image.key] = std::move(img_slots);
    m_video_registry->slots.push_back(std::move(slot));
    return m_tex_map[image.key];
}

void TextureCache::PumpVideoTextures(double dt_seconds) {
    if (! m_video_registry || m_video_registry->slots.empty()) return;

    auto advance_slot_pts = [](VideoRegistry::Slot& s, double raw_pts) {
        double next_pts = -1.0;
        if (std::isfinite(raw_pts) && raw_pts >= 0.0) {
            if (! std::isfinite(s.pts_origin)) s.pts_origin = raw_pts;
            const double normalized = raw_pts - s.pts_origin;
            if (std::isfinite(normalized) && normalized >= 0.0 &&
                (s.last_pts < 0.0 || normalized > s.last_pts)) {
                next_pts = normalized;
            }
        }
        if (next_pts < 0.0) {
            next_pts = (s.last_pts >= 0.0) ? (s.last_pts + s.frame_interval) : 0.0;
        }
        s.last_pts = next_pts;
    };

    for (auto& up : m_video_registry->slots) {
        auto& s = *up;
        if (s.active_epoch != m_video_activity_epoch) continue;
        s.pts_acc += dt_seconds;

        auto it = m_tex_map.find(s.key);
        if (it == m_tex_map.end() || it->second.slots.empty()) continue;
        ImageParameters ip = ToImageParameters(it->second.slots[0]);

        const auto                  fkind = s.decoder->kind();
        wavsen::video::VkFrameView  vkv {};
        const bool supported_frame_kind =
            fkind == wavsen::video::FrameKind::VulkanShared ||
            fkind == wavsen::video::FrameKind::Sw ||
            fkind == wavsen::video::FrameKind::VideoToolboxSw;
        if (! supported_frame_kind) {
            rstd_error("PumpVideoTextures[{}]: unsupported decoded frame kind {}",
                       s.key,
                       FrameKindLabel(fkind));
            continue;
        }

        /* Catch up to wall time without downloading every obsolete 4K frame.
         * For software/VideoToolbox frames we advance stale decoder output
         * cheaply, then transfer and convert only the newest frame that can
         * actually be presented this tick. */
        bool got_new = false;
        constexpr int kMaxAdvancePerTick = 12;
        for (int i = 0; i < kMaxAdvancePerTick; ++i) {
            if (s.last_pts >= 0.0 && s.last_pts > s.pts_acc) break;

            rstd::Result<wavsen::video::NextFrame, wavsen::video::Error> r =
                rstd::Ok(wavsen::video::NextFrame::Ok);
            double discarded_pts = -1.0;
            const bool discard = fkind != wavsen::video::FrameKind::VulkanShared &&
                                 s.last_pts >= 0.0 &&
                                 s.last_pts + s.frame_interval < s.pts_acc &&
                                 i + 1 < kMaxAdvancePerTick;
            if (discard) {
                r = s.decoder->discard_frame(discarded_pts);
            } else {
                switch (fkind) {
                case wavsen::video::FrameKind::VulkanShared:
                    r = s.decoder->next_vk_frame(vkv);
                    break;
                case wavsen::video::FrameKind::Sw: r = s.decoder->next_frame(s.nv12_scratch); break;
                case wavsen::video::FrameKind::VideoToolboxSw:
                    r = s.decoder->next_frame(s.nv12_scratch);
                    break;
                default: break;
                }
            }
            if (r.is_err()) {
                rstd_error("PumpVideoTextures[{}]: decode {}: {}",
                           s.key,
                           FrameKindLabel(fkind),
                           std::move(r).unwrap_err().message);
                break;
            }
            auto kind = r.unwrap();
            if (kind == wavsen::video::NextFrame::Eof) {
                s.pts_acc    = 0.0;
                s.last_pts   = -1.0;
                s.pts_origin = std::numeric_limits<double>::quiet_NaN();
                break;
            }
            const bool decoder_looped = kind == wavsen::video::NextFrame::Looped;
            if (decoder_looped) {
                s.pts_origin = std::numeric_limits<double>::quiet_NaN();
                s.last_pts   = -1.0;
            }
            double frame_pts = discarded_pts;
            if (! discard) {
                switch (fkind) {
                case wavsen::video::FrameKind::VulkanShared: frame_pts = vkv.pts_seconds; break;
                case wavsen::video::FrameKind::Sw: frame_pts = s.nv12_scratch.pts_seconds; break;
                case wavsen::video::FrameKind::VideoToolboxSw:
                    frame_pts = s.nv12_scratch.pts_seconds;
                    break;
                default: break;
                }
            }
            advance_slot_pts(s, frame_pts);
            if (! discard) got_new = true;
            if (decoder_looped) {
                s.pts_acc = std::max(s.last_pts, 0.0);
                break;
            }
            if (discard) continue;

            break; // one transfer/conversion/upload per presented frame
        }
        if (! got_new && s.have_frame) continue; /* nothing to upload */
        if (! got_new) continue;

        std::uint32_t cs_id = 0;
        std::uint32_t cr_id = 0;
        switch (fkind) {
        case wavsen::video::FrameKind::VulkanShared:
            cs_id = vkv.colorspace;
            cr_id = vkv.color_range;
            break;
        case wavsen::video::FrameKind::Sw:
        case wavsen::video::FrameKind::VideoToolboxSw:
            cs_id = s.nv12_scratch.colorspace;
            cr_id = s.nv12_scratch.color_range;
            break;
        default: break;
        }
        const auto color_matrix =
            wavsen::video::make_color_matrix(static_cast<wavsen::video::ColorSpace>(cs_id),
                                             static_cast<wavsen::video::ColorRange>(cr_id));

        auto upload_rgba = [&](const std::vector<std::uint8_t>& rgba) -> bool {
            const std::size_t bytes = static_cast<std::size_t>(s.width) * s.height * 4;
            if (rgba.size() < bytes) return false;

            if (! s.upload_stage.handle || s.upload_stage_size < bytes) {
                s.upload_stage      = {};
                s.upload_stage_size = 0;
                if (! CreateStagingBuffer(m_device.vma_allocator(), bytes, s.upload_stage))
                    return false;
                s.upload_stage_size = bytes;
            }
            {
                void* v = nullptr;
                VVK_CHECK(s.upload_stage.handle.MapMemory(&v));
                std::memcpy(v, rgba.data(), bytes);
                s.upload_stage.handle.UnMapMemory();
            }

            PendingImageUpload up {};
            up.image        = ip;
            up.buffer       = *s.upload_stage.handle;
            up.image_extent = VkExtent3D { s.width, s.height, 1 };
            m_pending_uploads.push_back(std::move(up));
            return true;
        };

        auto* yuv = m_video_registry->ensureYuv(m_device, s.width, s.height);
        if (! yuv) {
            if (fkind == wavsen::video::FrameKind::Sw ||
                fkind == wavsen::video::FrameKind::VideoToolboxSw) {
                if (! ConvertNv12ToRgba(s.nv12_scratch, color_matrix, s.rgba_scratch)) {
                    rstd_error("PumpVideoTextures[{}]: CPU NV12->RGBA conversion failed", s.key);
                    continue;
                }
                if (! upload_rgba(s.rgba_scratch)) {
                    rstd_error("PumpVideoTextures[{}]: CPU RGBA upload failed", s.key);
                    continue;
                }
                s.have_frame = true;
            }
            continue;
        }

        rstd::Result<int, wavsen::video::Error> cv = rstd::Ok(-1);
        switch (fkind) {
        case wavsen::video::FrameKind::VulkanShared: {
            wavsen::video::YuvToRgba::VkFrameImports im {};
            im.y_image           = vkv.img[0];
            im.uv_image          = vkv.plane_count > 1 ? vkv.img[1] : VK_NULL_HANDLE;
            im.y_sem             = vkv.sem[0];
            im.uv_sem            = vkv.plane_count > 1 ? vkv.sem[1] : vkv.sem[0];
            im.y_sem_val_in_out  = &vkv.sem_value[0];
            im.uv_sem_val_in_out = vkv.plane_count > 1 ? &vkv.sem_value[1] : &vkv.sem_value[0];
            im.y_layout_in_out   = &vkv.layout[0];
            im.uv_layout_in_out  = vkv.plane_count > 1 ? &vkv.layout[1] : &vkv.layout[0];
            im.y_qf_in_out       = &vkv.queue_family[0];
            im.uv_qf_in_out = vkv.plane_count > 1 ? &vkv.queue_family[1] : &vkv.queue_family[0];
            im.src_w        = vkv.width;
            im.src_h        = vkv.height;
            im.bit_depth    = vkv.bit_depth;
            cv              = yuv->convert_av_vk_frame(im,
                                                       ip.handle,
                                                       s.width,
                                                       s.height,
                                                       color_matrix,
                                                       wavsen::video::ConvertTarget::SampledLocal);
            break;
        }
        case wavsen::video::FrameKind::Sw:
        case wavsen::video::FrameKind::VideoToolboxSw:
            cv = yuv->convert_nv12(ip.handle,
                                   s.width,
                                   s.height,
                                   s.nv12_scratch.data.data(),
                                   s.nv12_scratch.data.size(),
                                   color_matrix,
                                   wavsen::video::ConvertTarget::SampledLocal);
            break;
        default: break;
        }
        if (cv.is_err()) {
            rstd_error("PumpVideoTextures[{}]: yuv conversion {}: {}",
                       s.key,
                       FrameKindLabel(fkind),
                       std::move(cv).unwrap_err().message);
            continue;
        }
        CloseSyncFd(std::move(cv).unwrap());
        s.have_frame = true;
    }
}

bool TextureCache::UploadFontAtlasRegion(const std::string& key, const std::uint8_t* atlas,
                                         std::uint32_t atlas_w, std::uint32_t x, std::uint32_t y,
                                         std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0) return true;
    auto it = m_tex_map.find(key);
    if (it == m_tex_map.end() || it->second.slots.empty()) return false;

    ImageParameters ip = ToImageParameters(it->second.slots[0]);

    // Tightly-packed staging buffer for the AABB. Allocating per-call keeps
    // this code path independent of the video-tex ring; atlas pumps are
    // small (a handful of glyphs per frame) so cost is negligible.
    const std::uint32_t bytes = w * h;
    VmaBufferParameters stage;
    if (! CreateStagingBuffer(m_device.vma_allocator(), bytes, stage)) return false;

    {
        void* v = nullptr;
        VVK_CHECK(stage.handle.MapMemory(&v));
        auto* dst = static_cast<std::uint8_t*>(v);
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(dst + row * w, atlas + (y + row) * atlas_w + x, w);
        }
        stage.handle.UnMapMemory();
    }

    PendingImageUpload up {};
    up.image         = ip;
    up.buffer        = *stage.handle;
    up.image_offset  = VkOffset3D { (int32_t)x, (int32_t)y, 0 };
    up.image_extent  = VkExtent3D { w, h, 1 };
    up.owned_stage   = std::move(stage);
    m_pending_uploads.push_back(std::move(up));
    return true;
}

TextureCache::TextureCache(const Device& device): m_device(device) {}

TextureCache::~TextureCache() {};

uint64_t TextureCache::nextImageGeneration() { return m_next_image_generation++; }

void TextureCache::AssignImageGeneration(VmaImageParameters& image) {
    image.generation = nextImageGeneration();
}

void TextureCache::SetVideoDecodeOptions(VideoDecodeOptions options) {
    m_video_decode_options = std::move(options);
    if (m_video_registry) {
        m_video_registry->options = m_video_decode_options;
        if (m_video_registry->slots.empty()) m_video_registry->producer.reset();
    }
}

void TextureCache::BeginVideoTextureActivity() {
    ++m_video_activity_epoch;
    // Keep zero as the sentinel for a slot that has never been referenced by
    // a compiled render graph.
    if (m_video_activity_epoch == 0) ++m_video_activity_epoch;
}

void TextureCache::MarkVideoTextureActive(std::string_view key) {
    if (! m_video_registry) return;
    for (auto& slot : m_video_registry->slots) {
        if (slot && slot->key == key) {
            slot->active_epoch = m_video_activity_epoch;
            return;
        }
    }
}

void TextureCache::Clear() {
    m_tex_map.clear();
    ClearTransientGraphResources();
    if (m_video_registry) m_video_registry->slots.clear();
    m_video_activity_epoch = 0;
    m_pending_initializations.clear();
    m_pending_uploads.clear();
    m_recorded_uploads.clear();
}

void TextureCache::ClearTransientGraphResources() {
    // Pending transitions store non-owning VkImage handles. Remove only those
    // belonging to transient render targets before their VmaImage owners are
    // destroyed; imported texture/video initializations remain valid across a
    // graph-only rebuild.
    std::unordered_set<VkImage> transient_images;
    transient_images.reserve(m_query_texs.size());
    for (const auto& query : m_query_texs) {
        if (query) transient_images.insert(ToImageParameters(query->image).handle);
    }
    std::erase_if(m_pending_initializations, [&](const auto& init) {
        return transient_images.contains(init.image.handle);
    });
    m_query_map.clear();
    m_query_texs.clear();
}

void TextureCache::RecordPendingUploads(vvk::CommandBuffer& cmd) {
    for (const auto& init : m_pending_initializations) {
        RecordQueuedImageInitialization(
            cmd, init.image, init.final_layout, init.aspect, init.clear_color);
    }
    m_pending_initializations.clear();

    if (m_pending_uploads.empty()) return;
    for (const auto& up : m_pending_uploads) {
        RecordQueuedImageUpload(cmd,
                                up.image,
                                up.buffer,
                                up.buffer_offset,
                                up.image_offset,
                                up.image_extent,
                                up.old_layout,
                                up.final_layout,
                                up.mip_level);
    }
    std::move(m_pending_uploads.begin(),
              m_pending_uploads.end(),
              std::back_inserter(m_recorded_uploads));
    m_pending_uploads.clear();
}

void TextureCache::ReleaseRecordedUploads() { m_recorded_uploads.clear(); }

std::optional<ImageSlotsRef> TextureCache::FindImported(std::string_view key) const {
    auto it = m_tex_map.find(std::string(key));
    if (it == m_tex_map.end()) return std::nullopt;
    return ImageSlotsRef(it->second);
}

std::optional<ImageParameters> TextureCache::Query(std::string_view key, TextureKey content_hash,
                                                   bool persist) {
    std::string query_key(key);
    TexHash      tex_hash = TextureKey::HashValue(content_hash);

    if (auto it = m_query_map.find(query_key); it != m_query_map.end()) {
        auto& query = *(it->second);

        if (query.content_hash != tex_hash) {
            query.query_keys.erase(query_key);
            query.share_ready = ! query.persist && query.query_keys.empty();
            m_query_map.erase(it);
        } else {
            query.share_ready = false;
            query.persist     = persist;

            return ToImageParameters(query.image);
        }
    }

    for (auto& query : m_query_texs) {
        if (! (query->share_ready)) continue;
        if (query->content_hash != tex_hash) continue;

        query->share_ready = false;
        query->persist     = persist;
        query->query_keys.insert(query_key);

        m_query_map[query_key] = &(*query);

        return ToImageParameters(query->image);
    }

    m_query_texs.emplace_back(std::make_unique<QueryTex>());
    auto& query            = *m_query_texs.back();
    m_query_map[query_key] = &query;

    query.index        = (idx)m_query_texs.size() - 1;
    query.content_hash = tex_hash;
    query.query_keys.insert(std::move(query_key));
    query.persist = persist;
    if (auto opt = CreateTex(content_hash); opt.has_value()) {
        query.image = std::move(opt.value());
        return ToImageParameters(query.image);
    }
    return std::nullopt;
}

void TextureCache::MarkShareReady(std::string_view key) {
    auto it = m_query_map.find(key);
    if (it != m_query_map.end()) {
        auto& query = it->second;
        if (query->persist) return;
        query->query_keys.erase(std::string(key));
        query->share_ready = query->query_keys.empty();
        m_query_map.erase(it);
    }
}

void TextureCache::RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const {
    VkImageMemoryBarrier barrier {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image.handle,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    /*
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        out_bar);
        */

    i32 mipWidth  = (i32)image.extent.width;
    i32 mipHeight = (i32)image.extent.height;

    for (unsigned i = 1; i < image.mipmap_level; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = i == 1 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = i == 1 ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkPipelineStageFlags src_stage =
            i == 1 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
        cmd.PipelineBarrier(
            src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, barrier);

        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask                 = 0;
        barrier.dstAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        VkImageBlit blit {
            .srcSubresource =
                VkImageSubresourceLayers {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = i - 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            .srcOffsets = { VkOffset3D { 0, 0, 0 }, VkOffset3D { mipWidth, mipHeight, 1 } },
            .dstOffsets = { VkOffset3D { 0, 0, 0 },
                            VkOffset3D { mipWidth > 1 ? mipWidth / 2 : 1,
                                         mipHeight > 1 ? mipHeight / 2 : 1,
                                         1 } },
        };
        blit.dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = blit.srcSubresource.aspectMask,
                .mipLevel       = blit.srcSubresource.mipLevel + 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },

        cmd.BlitImage(image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      blit,
                      VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = image.mipmap_level - 1;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        barrier);
}
