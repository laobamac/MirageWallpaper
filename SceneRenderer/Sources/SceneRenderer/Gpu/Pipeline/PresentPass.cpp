module;

#include <cstdlib>
#include <fstream>
#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#define SCENERENDERER_ENABLE_METAL_EXPORT 1
#else
#define SCENERENDERER_ENABLE_METAL_EXPORT 0
#endif
#include <vulkan/vulkan.h>
#include <rstd/macro.hpp>
#include "vk_mem_alloc.h"
#include "vvk/macros.hpp"

module sr.vulkan_render;
import rstd.log;
import rstd.cppstd;
import sr.core;
import sr.vulkan;
import sr.scene;

using namespace sr::vulkan;

namespace
{
using LiveFrameCallback = void (*)(const uint8_t* rgba, uint32_t width, uint32_t height,
                                   void* userdata);
using LiveMetalFrameCallback = void (*)(void* mtl_texture, uint32_t width, uint32_t height,
                                        void* userdata);

std::mutex         g_live_frame_mutex;
LiveFrameCallback g_live_frame_callback { nullptr };
void*             g_live_frame_userdata { nullptr };
std::mutex              g_live_metal_frame_mutex;
LiveMetalFrameCallback g_live_metal_frame_callback { nullptr };
void*                  g_live_metal_frame_userdata { nullptr };

bool LiveFrameRequested() {
    std::scoped_lock lock(g_live_frame_mutex);
    return g_live_frame_callback != nullptr;
}

void DispatchLiveFrame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    LiveFrameCallback cb { nullptr };
    void*             userdata { nullptr };
    {
        std::scoped_lock lock(g_live_frame_mutex);
        cb       = g_live_frame_callback;
        userdata = g_live_frame_userdata;
    }
    if (cb != nullptr) cb(rgba, width, height, userdata);
}

bool LiveMetalFrameRequested() {
    std::scoped_lock lock(g_live_metal_frame_mutex);
    return g_live_metal_frame_callback != nullptr;
}

void DispatchLiveMetalFrame(void* mtl_texture, uint32_t width, uint32_t height) {
    LiveMetalFrameCallback cb { nullptr };
    void*                  userdata { nullptr };
    {
        std::scoped_lock lock(g_live_metal_frame_mutex);
        cb       = g_live_metal_frame_callback;
        userdata = g_live_metal_frame_userdata;
    }
    if (cb != nullptr) cb(mtl_texture, width, height, userdata);
}

#if SCENERENDERER_ENABLE_METAL_EXPORT
void* ExportMetalTexture(const Device& device, const ImageParameters& image) {
    auto export_metal_objects = device.handle().Dispatch().vkExportMetalObjectsEXT;
    if (export_metal_objects == nullptr || image.handle == VK_NULL_HANDLE ||
        image.view == VK_NULL_HANDLE)
        return nullptr;

    VkExportMetalTextureInfoEXT texture_info {
        .sType      = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .pNext      = nullptr,
        .image      = image.handle,
        .imageView  = image.view,
        .bufferView = VK_NULL_HANDLE,
        .plane      = VK_IMAGE_ASPECT_COLOR_BIT,
        .mtlTexture = nullptr,
    };
    VkExportMetalObjectsInfoEXT export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &texture_info,
    };
    export_metal_objects(*device.handle(), &export_info);
    return reinterpret_cast<void*>(texture_info.mtlTexture);
}
#endif

const char* EnvPath(const char* primary) {
    const char* value = std::getenv(primary);
    if (value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

bool EnvEnabled(const char* primary) {
    const char* value = EnvPath(primary);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool PresentTestEnabled() { return EnvEnabled("SCENERENDERER_TEST_PRESENT"); }
bool PresentDumpRequested() {
    return EnvPath("SCENERENDERER_DUMP_PRESENT") != nullptr;
}

void WritePpm(std::ofstream& out, const uint8_t* pixels, uint32_t width, uint32_t height,
              VkFormat format) {
    out << "P6\n" << width << " " << height << "\n255\n";
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM ||
                      format == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const auto* p = pixels + i * 4;
        const char  rgb[3] {
            static_cast<char>(bgra ? p[2] : p[0]),
            static_cast<char>(p[1]),
            static_cast<char>(bgra ? p[0] : p[2]),
        };
        out.write(rgb, sizeof(rgb));
    }
}
} // namespace

extern "C" void SceneRendererSetLiveFrameCallback(LiveFrameCallback cb, void* userdata) {
    std::scoped_lock lock(g_live_frame_mutex);
    g_live_frame_callback = cb;
    g_live_frame_userdata = userdata;
}

extern "C" void SceneRendererSetLiveMetalFrameCallback(LiveMetalFrameCallback cb, void* userdata) {
    std::scoped_lock lock(g_live_metal_frame_mutex);
    g_live_metal_frame_callback = cb;
    g_live_metal_frame_userdata = userdata;
}

FinPass::FinPass(const Desc& desc): m_desc(desc) {}
FinPass::~FinPass() {}

void FinPass::setPresent(ImageParameters img) { m_desc.vk_present = img; }
void FinPass::setPresentLayout(VkImageLayout layout) { m_desc.present_layout = layout; }
void FinPass::setPresentQueueIndex(uint32_t i) { m_desc.present_queue_index = i; }
void FinPass::setPresentFormat(VkFormat fmt) { m_desc.present_format = fmt; }
void FinPass::setPresentCanTransferSrc(bool can) { m_desc.present_can_transfer_src = can; }
void FinPass::setMetalFrameCallback(std::function<void(void*, uint32_t, uint32_t)> cb) {
    m_desc.metal_frame_callback = std::move(cb);
}
bool FinPass::setResultRequest(std::optional<TextureRequest> request) {
    return SetTextureRequestIfChanged(m_desc.result_request, std::move(request));
}

std::vector<PassTextureRequestDiagnostic> FinPass::textureRequestDiagnostics() const {
    return {
        PassTextureRequestDiagnostic {
            .role    = "frame-result",
            .name    = std::string(m_desc.result),
            .request = m_desc.result_request,
        },
    };
}

void FinPass::recordFrameDump(const Device& device, RenderingResources& rr) {
    const bool live_frame = LiveFrameRequested();
    if ((m_dump_done && ! live_frame) || m_dump_pending) return;
    const char* dump_path = EnvPath("SCENERENDERER_DUMP_FRAME");
    if ((dump_path == nullptr || dump_path[0] == '\0') && ! live_frame) return;

    const uint32_t width  = m_desc.vk_result.extent.width;
    const uint32_t height = m_desc.vk_result.extent.height;
    if (width == 0 || height == 0) return;

    const std::size_t dump_size = static_cast<std::size_t>(width) * height * 4;
    if (! m_dump_buffer || m_dump_size != dump_size) {
        VkBufferCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .size  = dump_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VmaAllocationCreateInfo vma_info {};
        vma_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        vvk::VmaBuffer buffer;
        if (auto res = vvk::CreateBuffer(device.vma_allocator(), ci, vma_info, buffer);
            res != VK_SUCCESS) {
            rstd_warn("SCENERENDERER_DUMP_FRAME: create readback buffer failed: {}", (int)res);
            m_dump_done = true;
            return;
        }
        m_dump_buffer = std::move(buffer);
        m_dump_size   = dump_size;
    }

    VkBufferImageCopy region {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    rr.command.CopyImageToBuffer(m_desc.vk_result.handle,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 *m_dump_buffer,
                                 region);
    m_dump_path    = dump_path != nullptr ? dump_path : "";
    m_dump_width   = width;
    m_dump_height  = height;
    m_dump_pending = true;
}

void FinPass::recordPresentDump(const Device& device, RenderingResources& rr) {
    if (m_present_dump_done || m_present_dump_pending) return;
    const char* dump_path = EnvPath("SCENERENDERER_DUMP_PRESENT");
    if (dump_path == nullptr) return;
    if (! m_desc.present_can_transfer_src) {
        if (! m_present_dump_warned) {
            rstd_warn("SCENERENDERER_DUMP_PRESENT: swapchain image lacks TRANSFER_SRC usage");
            m_present_dump_warned = true;
        }
        m_present_dump_done = true;
        return;
    }

    const uint32_t width  = m_desc.vk_present.extent.width;
    const uint32_t height = m_desc.vk_present.extent.height;
    if (width == 0 || height == 0) return;

    const std::size_t dump_size = static_cast<std::size_t>(width) * height * 4;
    if (! m_present_dump_buffer || m_present_dump_size != dump_size) {
        VkBufferCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .size  = dump_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VmaAllocationCreateInfo vma_info {};
        vma_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        vvk::VmaBuffer buffer;
        if (auto res = vvk::CreateBuffer(device.vma_allocator(), ci, vma_info, buffer);
            res != VK_SUCCESS) {
            rstd_warn("SCENERENDERER_DUMP_PRESENT: create readback buffer failed: {}", (int)res);
            m_present_dump_done = true;
            return;
        }
        m_present_dump_buffer = std::move(buffer);
        m_present_dump_size   = dump_size;
    }

    VkBufferImageCopy region {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    rr.command.CopyImageToBuffer(m_desc.vk_present.handle,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 *m_present_dump_buffer,
                                 region);
    m_present_dump_path    = dump_path;
    m_present_dump_width   = width;
    m_present_dump_height  = height;
    m_present_dump_pending = true;
}

void FinPass::finishFrameDump(const Device& device) {
#if SCENERENDERER_ENABLE_METAL_EXPORT
    if (m_desc.metal_frame_callback || LiveMetalFrameRequested()) {
        if (void* texture = ExportMetalTexture(device, m_desc.vk_result); texture != nullptr) {
            if (m_desc.metal_frame_callback) {
                m_desc.metal_frame_callback(texture,
                                            m_desc.vk_result.extent.width,
                                            m_desc.vk_result.extent.height);
            }
            if (LiveMetalFrameRequested()) {
                DispatchLiveMetalFrame(texture,
                                       m_desc.vk_result.extent.width,
                                       m_desc.vk_result.extent.height);
            }
        }
    }
#endif

    if (m_dump_pending && ! m_dump_done && m_dump_buffer) {
        void* mapped = nullptr;
        if (auto res = m_dump_buffer.MapMemory(&mapped); res != VK_SUCCESS || mapped == nullptr) {
            rstd_warn("SCENERENDERER_DUMP_FRAME: map readback buffer failed: {}", (int)res);
            m_dump_done    = true;
            m_dump_pending = false;
        } else {
            vmaInvalidateAllocation(device.vma_allocator(), m_dump_buffer.Allocation(), 0, m_dump_size);

            const auto* rgba = static_cast<const uint8_t*>(mapped);
            if (! m_dump_done && ! m_dump_path.empty()) {
                std::ofstream out(m_dump_path, std::ios::binary);
                if (! out) {
                    rstd_warn("SCENERENDERER_DUMP_FRAME: open output failed: {}", m_dump_path);
                } else {
                    WritePpm(out, rgba, m_dump_width, m_dump_height, VK_FORMAT_R8G8B8A8_UNORM);
                    rstd_info("SCENERENDERER_DUMP_FRAME: wrote {} ({}x{})",
                              m_dump_path,
                              m_dump_width,
                              m_dump_height);
                }
                m_dump_done = true;
            }
            if (LiveFrameRequested()) DispatchLiveFrame(rgba, m_dump_width, m_dump_height);

            m_dump_buffer.UnMapMemory();
            m_dump_pending = false;
        }
    }

    if (m_present_dump_pending && ! m_present_dump_done && m_present_dump_buffer) {
        void* mapped = nullptr;
        if (auto res = m_present_dump_buffer.MapMemory(&mapped);
            res != VK_SUCCESS || mapped == nullptr) {
            rstd_warn("SCENERENDERER_DUMP_PRESENT: map readback buffer failed: {}", (int)res);
            m_present_dump_done    = true;
            m_present_dump_pending = false;
        } else {
            vmaInvalidateAllocation(device.vma_allocator(),
                                    m_present_dump_buffer.Allocation(),
                                    0,
                                    m_present_dump_size);

            std::ofstream out(m_present_dump_path, std::ios::binary);
            if (! out) {
                rstd_warn("SCENERENDERER_DUMP_PRESENT: open output failed: {}", m_present_dump_path);
            } else {
                WritePpm(out,
                         static_cast<const uint8_t*>(mapped),
                         m_present_dump_width,
                         m_present_dump_height,
                         m_desc.present_format);
                rstd_info("SCENERENDERER_DUMP_PRESENT: wrote {} ({}x{})",
                          m_present_dump_path,
                          m_present_dump_width,
                          m_present_dump_height);
            }

            m_present_dump_buffer.UnMapMemory();
            m_present_dump_done    = true;
            m_present_dump_pending = false;
        }
    }
}

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& /*rr*/) {
    RenderResourceSystem resources(device);

    auto tex_name = std::string(m_desc.result);
    if (scene.renderTargets.count(tex_name) == 0) {
        rstd_error("FinPass: scene render target \"{}\" not found", tex_name);
        return;
    }
    auto& rt      = scene.renderTargets.at(tex_name);
    auto  request = m_desc.result_request.value_or(MakeRenderTargetTextureRequest(tex_name, rt));
    auto  opt     = resources.EnsureTexture(request);
    if (! opt.has_value()) {
        rstd_error("FinPass: TextureCache::Query(\"{}\") failed", tex_name);
        return;
    }
    m_desc.vk_result = opt.value();
    setPrepared();
}

void FinPass::execute(const Device& device, RenderingResources& rr) {
    auto&    cmd = rr.command;
    uint32_t gqf = device.graphics_queue().family_index;

    VkImageSubresourceRange sub {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_result.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = sub,
        };
        // srcStage = TRANSFER (not TOP_OF_PIPE) so the layout transition
        // observes the swapchain acquire-semaphore wait, which the submit
        // sets at the same TRANSFER stage. TOP_OF_PIPE would let the
        // transition race the presentation engine's still-in-flight read
        // (sync-validation WRITE_AFTER_READ).
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        // Result is always R8G8B8A8_UNORM (screen RT). We can copy when
        // present matches that and dimensions are identical; otherwise
        // fall back to blit which handles size/format mismatch.
        const bool can_copy = m_desc.vk_result.extent.width == m_desc.vk_present.extent.width &&
                              m_desc.vk_result.extent.height == m_desc.vk_present.extent.height &&
                              m_desc.present_format == VK_FORMAT_R8G8B8A8_UNORM;

        const bool present_test = PresentTestEnabled();
        if (! m_path_logged) {
            rstd_info("FinPass: {}", present_test ? "present-test" : (can_copy ? "copy" : "blit"));
            m_path_logged = true;
        }

        if (present_test) {
            VkClearColorValue color { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } };
            cmd.ClearColorImage(m_desc.vk_present.handle,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &color,
                                sr::spanone { sub });
        } else if (can_copy) {
            VkImageCopy region {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffset      = { 0, 0, 0 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffset      = { 0, 0, 0 },
                .extent = { m_desc.vk_result.extent.width, m_desc.vk_result.extent.height, 1 },
            };
            cmd.CopyImage(m_desc.vk_result.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          m_desc.vk_present.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          region);
        } else {
            VkImageBlit region {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets     = {
                    VkOffset3D { 0, 0, 0 },
                    VkOffset3D { (int32_t)m_desc.vk_result.extent.width,
                                 (int32_t)m_desc.vk_result.extent.height, 1 },
                },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets     = {
                    VkOffset3D { 0, 0, 0 },
                    VkOffset3D { (int32_t)m_desc.vk_present.extent.width,
                                 (int32_t)m_desc.vk_present.extent.height, 1 },
                },
            };
            cmd.BlitImage(m_desc.vk_result.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          m_desc.vk_present.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          region,
                          VK_FILTER_LINEAR);
        }
    }

    recordFrameDump(device, rr);

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_result.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    const bool dump_present = PresentDumpRequested();
    if (dump_present && m_desc.present_can_transfer_src) {
        {
            VkImageMemoryBarrier b {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext               = nullptr,
                .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = m_desc.vk_present.handle,
                .subresourceRange    = sub,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                b);
        }
        recordPresentDump(device, rr);
        {
            bool                 xfer = (m_desc.present_queue_index != gqf);
            VkImageMemoryBarrier b {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext               = nullptr,
                .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask       = 0,
                .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout           = m_desc.present_layout,
                .srcQueueFamilyIndex = xfer ? gqf : VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = xfer ? m_desc.present_queue_index : VK_QUEUE_FAMILY_IGNORED,
                .image               = m_desc.vk_present.handle,
                .subresourceRange    = sub,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                b);
        }
    } else {
        if (dump_present) recordPresentDump(device, rr);
        bool                 xfer = (m_desc.present_queue_index != gqf);
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = 0,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = m_desc.present_layout,
            .srcQueueFamilyIndex = xfer ? gqf : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = xfer ? m_desc.present_queue_index : VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }
}

void FinPass::destory(const Device&, RenderingResources&) {
    setPrepared(false);
    clearReleaseTexs();
    m_dump_buffer           = {};
    m_dump_pending          = false;
    m_present_dump_buffer   = {};
    m_present_dump_pending  = false;
    m_present_dump_warned   = false;
}
