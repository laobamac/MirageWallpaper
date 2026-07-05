module;
#include <chrono>
#include <cstdlib>
#include <rstd/macro.hpp>

#include "vvk/macros.hpp"

#include <unistd.h>
#include <vulkan/vulkan.h>

module sr.vulkan_render;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;
import sr.vulkan;
import sr.utils;
import sr.scene;
import sr.spec_texs;
import sr.text;

import sr.rgraph;

using namespace sr::vulkan;

constexpr uint64_t vk_wait_time { 10u * 1000u * 1000000u };
constexpr uint32_t vk_command_num { 2 };

namespace
{
bool PerfDiagEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SCENERENDERER_PERF_DIAG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

using PerfClock = std::chrono::steady_clock;

double PerfMs(PerfClock::time_point begin, PerfClock::time_point end = PerfClock::now()) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
} // namespace

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME },
};
constexpr std::array base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    // Required by the Vulkan portability spec for any device that
    // reports VK_KHR_portability_subset (MoltenVK does).
    Extension { true, "VK_KHR_portability_subset" },
    // Lets SceneViewer export MoltenVK's underlying MTLTexture for a
    // GPU-only macOS display fallback. Optional so non-MoltenVK builds still run.
    Extension { false, "VK_EXT_metal_objects" },
    // Timeline semaphores are required by the 4b41483 render infrastructure
    // (upload/readback overlap). MoltenVK supports timelineSemaphore.
    Extension { true, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
    Extension { false, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
};

void AppendVideoDeviceExtensions(std::vector<Extension>& device_exts) {
    device_exts.push_back({ false, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_SHADER_OBJECT_EXTENSION_NAME });
}

bool RequiresVulkanVideoDeviceExtensions(std::string_view hwdec) {
#if defined(__APPLE__)
    return hwdec == "vulkan" || hwdec == "vaapi";
#else
    return hwdec != "none";
#endif
}

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();

    void drawFrame(Scene&);

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph();
    void clearTransientRenderGraphResources();
    void clearRenderGraphResources(bool clear_imported_textures);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void rebuildRenderPassScopes();
    void executeRenderPassScopes(RenderingResources&);
    void UpdateCameraFillMode(Scene&, sr::FillMode);

    bool initRes();
    void drawFrameSwapchain();
    void drawFrameOffscreen();
    void setRenderTargetSize(Scene&, rg::RenderGraph&);
    bool onSwapchainReady(unsigned width, unsigned height);

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;
    std::function<void(void*, uint32_t, uint32_t)> m_metal_frame_cb;

    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };

    vvk::CommandBuffers m_cmds;
    vvk::CommandBuffer  m_upload_cmd;
    vvk::CommandBuffer  m_render_cmd;

    bool m_with_surface { false };
    bool m_inited { false };
    bool m_pass_loaded { false };

    // MSAA sample count for the screen RT only. 1bit = disabled.
    // Resolved against device's framebufferColorSampleCounts in init().
    VkSampleCountFlagBits m_msaa_samples { VK_SAMPLE_COUNT_1_BIT };

    std::unique_ptr<ExSwapchain> m_ex_swapchain;
    RenderingResources           m_rendering_resources;

    // TODO(4b41483): upstream threads a RenderProgram / SceneResourceIndex
    // through compileRenderGraph and routes per-pass shader reflection +
    // buffer resolution through these caches. The port's render loop still
    // builds pipelines inline inside each VulkanPass; the reflection cache
    // is wired here only for lifecycle (create/clear) so it is ready to use
    // without disturbing the existing per-pass prepare() path.
    ShaderReflectionCache m_shader_reflection_cache;

    // for VUID-vkQueueSubmit-pSignalSemaphores-00067
    std::vector<vvk::Semaphore> m_sem_swap_finish_per_image;

    struct RenderPassScope {
        VulkanPass*                    single { nullptr };
        std::vector<CustomShaderPass*> shader_passes;
    };
    std::vector<VulkanPass*>     m_passes;
    std::vector<RenderPassScope> m_render_scopes;
};

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }

VkInstance VulkanRender::vkInstance() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_instance.inst();
}

VkPhysicalDevice VulkanRender::vkPhysicalDevice() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->gpu();
}

VkDevice VulkanRender::vkDevice() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->handle();
}

VkQueue VulkanRender::vkGraphicsQueue() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->graphics_queue().handle;
}

uint32_t VulkanRender::vkGraphicsQueueFamily() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return 0;
    return pImpl->m_device->graphics_queue().family_index;
}

void VulkanRender::deviceUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.deviceUUID, 16);
}

void VulkanRender::pumpVideoTextures(double dt_seconds) {
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    pImpl->m_device->tex_cache().PumpVideoTextures(dt_seconds);
}

void VulkanRender::pumpFontAtlases(Scene& scene) {
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    auto* fc = sr::text::SceneFontCache(scene);
    if (fc == nullptr) return;
    auto& tex = pImpl->m_device->tex_cache();
    for (auto* face : fc->Faces()) {
        if (face == nullptr) continue;
        auto rects = face->DirtyRects();
        if (rects.empty()) continue;
        // Coalesce all dirty rects into one AABB. Typical: ≤ a handful of
        // glyph slots per frame, so a single upload covering the union beats
        // submitting one copy per rect.
        std::uint32_t min_x = rects[0].x;
        std::uint32_t min_y = rects[0].y;
        std::uint32_t max_x = rects[0].x + rects[0].w;
        std::uint32_t max_y = rects[0].y + rects[0].h;
        for (auto& r : rects.subspan(1)) {
            if (r.x < min_x) min_x = r.x;
            if (r.y < min_y) min_y = r.y;
            const std::uint32_t rx2 = r.x + r.w;
            const std::uint32_t ry2 = r.y + r.h;
            if (rx2 > max_x) max_x = rx2;
            if (ry2 > max_y) max_y = ry2;
        }
        const auto fm     = face->Metrics();
        const auto pixels = face->AtlasPixels();
        (void)tex.UploadFontAtlasRegion(face->AtlasUrl(),
                                        pixels.data(),
                                        fm.atlas_w,
                                        min_x,
                                        min_y,
                                        max_x - min_x,
                                        max_y - min_y);
        // Clear regardless: if VkImage didn't exist yet, the pixels are
        // already in the CPU buffer that CreateTex aliases on its first
        // call. Re-uploading would just duplicate work.
        face->ClearDirtyRects();
    }
}

void VulkanRender::driverUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.driverUUID, 16);
}

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(std::move(info)); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::clearLastRenderGraph() { pImpl->clearLastRenderGraph(); };
void VulkanRender::clearTransientRenderGraphResources() {
    pImpl->clearTransientRenderGraphResources();
};
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
}
void VulkanRender::evictUnusedMeshes() {
    if (auto* d = pImpl->m_device.get()) d->mesh_cache().evictUnused();
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, sr::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};

bool VulkanRender::onSwapchainReady(unsigned width, unsigned height) {
    return pImpl->onSwapchainReady(width, height);
}

sr::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    m_metal_frame_cb = std::move(info.metal_frame_callback);
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        rstd_error("too small swapchain image size: {}x{}", extent.width, extent.height);
    } else {
        rstd_info("set swapchain image size: {}x{}", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts { base_device_exts.begin(), base_device_exts.end() };
    const bool needs_vulkan_video = RequiresVulkanVideoDeviceExtensions(info.video_hwdec);
    if (needs_vulkan_video) {
        AppendVideoDeviceExtensions(device_exts);
    }

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        rstd_info("vulkan valid layer \"{}\" enabled", VALIDATION_LAYER_NAME);
    }

    const auto instance_api_version =
        needs_vulkan_video ? VK_API_VERSION_1_3 : SCENERENDERER_VULKAN_VERSION;
    if (! Instance::Create(m_instance, inst_exts, inst_layers, instance_api_version)) {
        rstd_error("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                rstd_error("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            rstd_error("init vulkan device failed");
            return false;
        }
        m_device->tex_cache().SetVideoDecodeOptions(TextureCache::VideoDecodeOptions {
            .hwdec = info.video_hwdec,
        });
    }

    {
        // Map requested integer to a bit; clamp down to highest supported bit
        // not exceeding the request, given device's framebufferColorSampleCounts.
        const uint32_t           requested = info.msaa_samples == 0 ? 1u : info.msaa_samples;
        const VkSampleCountFlags supported = m_device->limits().framebufferColorSampleCounts;
        VkSampleCountFlagBits    chosen    = VK_SAMPLE_COUNT_1_BIT;
        constexpr std::array<VkSampleCountFlagBits, 6> ladder { {
            VK_SAMPLE_COUNT_64_BIT,
            VK_SAMPLE_COUNT_32_BIT,
            VK_SAMPLE_COUNT_16_BIT,
            VK_SAMPLE_COUNT_8_BIT,
            VK_SAMPLE_COUNT_4_BIT,
            VK_SAMPLE_COUNT_2_BIT,
        } };
        for (auto bit : ladder) {
            if ((uint32_t)bit <= requested && (supported & bit)) {
                chosen = bit;
                break;
            }
        }
        m_msaa_samples = chosen;
        rstd_info("msaa requested={} actual={}", requested, (uint32_t)m_msaa_samples);
    }

    if (info.offscreen) {
        m_ex_swapchain = CreateLocalExSwapchain(*m_device,
                                                extent.width,
                                                extent.height,
                                                (info.offscreen_tiling == TexTiling::OPTIMAL
                                                     ? VK_IMAGE_TILING_OPTIMAL
                                                     : VK_IMAGE_TILING_LINEAR));
        m_with_surface = false;
    }

    if (! initRes()) return false;
    ;

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    m_finpass->setMetalFrameCallback(m_metal_frame_cb);
    if (m_with_surface) {
        // Surface mode: FinPass blits into the swapchain image, ending
        // it in PRESENT_SRC_KHR. Swapchain images use concurrent sharing if
        // graphics/present families differ, so no ownership transfer is needed.
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        m_finpass->setPresentQueueIndex(m_device->graphics_queue().family_index);
        m_finpass->setPresentFormat(m_device->swapchain().format());
        m_finpass->setPresentCanTransferSrc(
            (m_device->swapchain().usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
    } else {
        // Offscreen: ExSwapchain implementation chooses both. Bridge
        // returns (GENERAL, FOREIGN_EXT); local returns (GENERAL,
        // IGNORED). Translate IGNORED to graphics_family so FinPass's
        // release-barrier branch (`!= graphics_family`) skips cleanly.
        m_finpass->setPresentLayout(m_ex_swapchain->producerOutputLayout());
        uint32_t qf = m_ex_swapchain->releaseTargetQueueFamily();
        if (qf == VK_QUEUE_FAMILY_IGNORED) {
            qf = m_device->graphics_queue().family_index;
        }
        m_finpass->setPresentQueueIndex(qf);
        m_finpass->setPresentFormat(m_ex_swapchain->format());
    }

    m_dyn_buf = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmd = vvk::CommandBuffer(m_cmds[0], m_device->handle().Dispatch());
        m_render_cmd = vvk::CommandBuffer(m_cmds[1], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;
    if (m_device && m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res
        for (auto& p : m_passes) {
            p->destory(*m_device, m_rendering_resources);
        }
        m_render_scopes.clear();
        m_dyn_buf->destroy();
        m_device->mesh_cache().destroy();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command = m_render_cmd;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));

        const usize n_images = m_device->swapchain().images().size();
        m_sem_swap_finish_per_image.clear();
        m_sem_swap_finish_per_image.resize(n_images);
        for (auto& s : m_sem_swap_finish_per_image) {
            VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, s));
        }
    }

    // Offscreen-path signal semaphore for vkQueueSubmit. Surface mode
    // presents straight through the swapchain and never reaches this.
    if (! m_with_surface) {
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_export));
    }

    rr.dyn_buf = m_dyn_buf.get();
    rr.shader_reflection_cache = &m_shader_reflection_cache;
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}

void VulkanRender::Impl::rebuildRenderPassScopes() {
    m_render_scopes.clear();
    std::vector<CustomShaderPass*> pending_shader_passes;

    auto flushShaderPasses = [&]() {
        if (pending_shader_passes.empty()) return;
        RenderPassScope scope;
        scope.shader_passes = std::move(pending_shader_passes);
        m_render_scopes.push_back(std::move(scope));
        pending_shader_passes.clear();
    };

    for (auto* pass : m_passes) {
        auto* shader_pass = dynamic_cast<CustomShaderPass*>(pass);
        if (shader_pass != nullptr && shader_pass->prepared()) {
            if (! pending_shader_passes.empty() &&
                shader_pass->canJoinRenderScopeAfter(*pending_shader_passes.back())) {
                pending_shader_passes.push_back(shader_pass);
            } else {
                flushShaderPasses();
                pending_shader_passes.push_back(shader_pass);
            }
            continue;
        }

        flushShaderPasses();
        m_render_scopes.push_back(RenderPassScope { .single = pass });
    }

    flushShaderPasses();
}

void VulkanRender::Impl::executeRenderPassScopes(RenderingResources& rr) {
    for (auto& scope : m_render_scopes) {
        if (scope.single != nullptr) {
            if (scope.single->prepared()) {
                scope.single->execute(*m_device, rr);
            }
            continue;
        }

        auto& shader_passes = scope.shader_passes;
        if (shader_passes.empty()) continue;
        if (shader_passes.size() == 1) {
            auto* pass = shader_passes.front();
            if (pass->prepared()) {
                pass->execute(*m_device, rr);
            }
            continue;
        }

        if (! std::all_of(shader_passes.begin(), shader_passes.end(), [](auto* pass) {
                return pass->prepared();
            })) {
            continue;
        }

        for (auto* pass : shader_passes) {
            pass->prepareRenderScopeDraw(rr);
        }
        shader_passes.front()->beginRenderScope(rr);
        for (auto* pass : shader_passes) {
            pass->recordRenderScopeDraw(rr);
        }
        shader_passes.front()->endRenderScope(rr);
    }
}

void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (! (m_inited && m_pass_loaded)) return;

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
    } else {
        drawFrameSwapchain();
    }

    if (m_redraw_cb) m_redraw_cb();
}

void VulkanRender::Impl::drawFrameSwapchain() {
    static size_t resource_index = 0;

    const bool perf_diag = PerfDiagEnabled();
    auto       perf_begin = PerfClock::now();
    double     perf_acquire_ms {};
    double     perf_record_ms {};
    double     perf_submit_present_ms {};
    double     perf_wait_ms {};

    RenderingResources& rr = m_rendering_resources;
    resource_index         = (resource_index + 1) % 3;
    uint32_t image_index   = 0;
    {
        auto t = PerfClock::now();
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *rr.sem_swap_wait_image,
                                                                 {},
                                                                 &image_index));
        if (perf_diag) perf_acquire_ms = PerfMs(t);
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);

    {
        auto t = PerfClock::now();
        (void)rr.command.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        m_device->tex_cache().RecordPendingUploads(rr.command);
        m_dyn_buf->recordUpload(rr.command);
        executeRenderPassScopes(rr);
        (void)rr.command.End();
        if (perf_diag) perf_record_ms = PerfMs(t);
    }

    auto& sem_present_done = m_sem_swap_finish_per_image[image_index];

    // Swapchain image is only written via FinPass blit/copy (TRANSFER).
    // Waiting at COLOR_ATTACHMENT_OUTPUT lets the layout transition + transfer
    // race the presentation engine's read → sync-validation WRITE_AFTER_READ.
    VkPipelineStageFlags wait_dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo         sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = rr.sem_swap_wait_image.address(),
        .pWaitDstStageMask    = &wait_dst_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = sem_present_done.address(),
    };

    {
        auto t = PerfClock::now();
        VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));
        VkPresentInfoKHR present_info {
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext              = nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = sem_present_done.address(),
            .swapchainCount     = 1,
            .pSwapchains        = m_device->swapchain().handle().address(),
            .pImageIndices      = &image_index,
        };
        VVK_CHECK_VOID_RE(m_device->present_queue().handle.Present(present_info));
        if (perf_diag) perf_submit_present_ms = PerfMs(t);
    }

    {
        auto t = PerfClock::now();
        VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
        if (perf_diag) perf_wait_ms = PerfMs(t);
    }
    m_finpass->finishFrameDump(*m_device);
    m_device->tex_cache().ReleaseRecordedUploads();
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    if (perf_diag) {
        static uint64_t frame_count = 0;
        ++frame_count;
        if ((frame_count % 120u) == 0u) {
            rstd_info("PerfDiag vulkan surface frame={} total_us={} acquire_us={} record_us={} submit_present_us={} wait_us={}",
                      frame_count,
                      static_cast<std::uint64_t>(PerfMs(perf_begin) * 1000.0),
                      static_cast<std::uint64_t>(perf_acquire_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_record_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_submit_present_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_wait_ms * 1000.0));
        }
    }
}
void VulkanRender::Impl::drawFrameOffscreen() {
    if (! m_ex_swapchain) return;

    const bool perf_diag = PerfDiagEnabled();
    auto       perf_begin = PerfClock::now();
    double     perf_poll_acquire_ms {};
    double     perf_record_ms {};
    double     perf_submit_ms {};
    double     perf_wait_ms {};

    // Drain any pending bridge directive *before* committing to a slot.
    // Previous frame's GPU work has fenced at the tail of the last
    // drawFrameOffscreen, so the cmd pool is idle.
    RenderingResources& rr = m_rendering_resources;
    ImageParameters     image;
    {
        auto t = PerfClock::now();
        m_ex_swapchain->poll();

        // Skip until both the swapchain has slots and the scene has loaded
        // (FinPass.prepare runs from compileRenderGraph). FinPass itself is
        // format-agnostic now — vkCmdBlitImage handles cross-format channel
        // mapping, no rebuild needed on renegotiation.
        if (! m_ex_swapchain->ready() || ! m_finpass->prepared()) {
            return;
        }

        if (! m_ex_swapchain->acquireRenderTarget(image)) {
            return;
        }
        if (perf_diag) perf_poll_acquire_ms = PerfMs(t);
    }

    m_finpass->setPresent(image);

    {
        auto t = PerfClock::now();
        (void)rr.command.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        m_device->tex_cache().RecordPendingUploads(rr.command);
        m_dyn_buf->recordUpload(rr.command);

        executeRenderPassScopes(rr);

        (void)rr.command.End();
        if (perf_diag) perf_record_ms = PerfMs(t);
    }

    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = rr.sem_export.address(),
    };
    {
        auto t = PerfClock::now();
        VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));
        if (perf_diag) perf_submit_ms = PerfMs(t);
    }

    {
        auto t = PerfClock::now();
        VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
        if (perf_diag) perf_wait_ms = PerfMs(t);
    }
    m_finpass->finishFrameDump(*m_device);
    m_device->tex_cache().ReleaseRecordedUploads();
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    m_ex_swapchain->submitRendered(-1);

    if (perf_diag) {
        static uint64_t frame_count = 0;
        ++frame_count;
        if ((frame_count % 120u) == 0u) {
            rstd_info("PerfDiag vulkan offscreen frame={} total_us={} acquire_us={} record_us={} submit_us={} wait_us={}",
                      frame_count,
                      static_cast<std::uint64_t>(PerfMs(perf_begin) * 1000.0),
                      static_cast<std::uint64_t>(perf_poll_acquire_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_record_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_submit_ms * 1000.0),
                      static_cast<std::uint64_t>(perf_wait_ms * 1000.0));
        }
    }
}

bool VulkanRender::Impl::onSwapchainReady(unsigned width, unsigned height) {
    if (! m_inited || ! m_device) return false;
    auto& cur            = m_device->out_extent();
    bool  extent_changed = (width != cur.width) || (height != cur.height);
    if (! extent_changed) {
        // Format-only changes flow through ExSwapchain::format() and
        // are handled by drawFrameOffscreen's head check.
        return false;
    }
    // Drain GPU work for the previous extent's resources before tearing
    // down the texture cache + render passes inside compileRenderGraph
    // (which the caller will run next).
    VVK_CHECK(m_device->handle().WaitIdle());
    m_device->set_out_extent(VkExtent2D { width, height });
    return true;
}

void VulkanRender::Impl::setRenderTargetSize(Scene& scene, rg::RenderGraph& rg) {
    auto& ext = m_device->out_extent();
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = (i32)(rt.bind.scale * ext.width);
            rt.height = (i32)(rt.bind.scale * ext.height);
        }
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.renderTargets.find(rt.bind.name);
        if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
            rstd_error("unknonw render target bind: {}", rt.bind.name);
            continue;
        }
        rt.width  = (i32)(rt.bind.scale * bind_rt->second.width);
        rt.height = (i32)(rt.bind.scale * bind_rt->second.height);
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (! item.first.empty() && (rt.width * rt.height <= 4)) {
            rstd_error("wrong size for render target: {}", item.first);
        } else if (rt.has_mipmap) {
            rt.mipmap_level = std::max(3u,
                                       static_cast<unsigned>(
                                           std::floor(std::log2(std::min(rt.width, rt.height))))) -
                              2u;
        }
    }
    if (m_msaa_samples != VK_SAMPLE_COUNT_1_BIT) {
        auto it = scene.renderTargets.find(std::string(SpecTex_Default));
        if (it != scene.renderTargets.end()) {
            it->second.sample_count = (unsigned)m_msaa_samples;
        }
    }
    scene.shaderValueUpdater->SetScreenSize((i32)ext.width, (i32)ext.height);
}

void VulkanRender::Impl::UpdateCameraFillMode(sr::Scene& scene, sr::FillMode fillmode) {
    using namespace sr;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam    = *scene.cameras.at("global");
    auto&  gPerCam = *scene.cameras.at("global_perspective");
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");
    scene.CaptureCameraPathViewports();
}

void VulkanRender::Impl::clearRenderGraphResources(bool clear_imported_textures) {
    for (auto& p : m_passes) {
        p->destory(*m_device, m_rendering_resources);
    }
    m_render_scopes.clear();
    m_passes.clear();
    if (clear_imported_textures) {
        m_device->tex_cache().Clear();
    } else {
        m_device->tex_cache().ClearTransientGraphResources();
    }
    m_device->mesh_cache().onRenderGraphCleared();
    if (clear_imported_textures) {
        m_shader_reflection_cache.Clear();
    }

    m_dyn_buf->destroy();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::clearLastRenderGraph() { clearRenderGraphResources(true); }

void VulkanRender::Impl::clearTransientRenderGraphResources() {
    clearRenderGraphResources(false);
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    if (! m_inited) return;
    m_pass_loaded = false;

    auto nodes             = rg.topologicalOrder();
    auto node_release_texs = rg.getLastReadTexs(nodes);

    m_passes.clear();
    m_render_scopes.clear();
    m_passes.resize(nodes.size());

    std::transform(nodes.begin(),
                   nodes.end(),
                   node_release_texs.begin(),
                   m_passes.begin(),
                   [&rg](auto& id, auto& texs) {
                       auto* pass = rg.getPass(id);
                       rstd_assert(pass != nullptr);
                       VulkanPass* vpass = static_cast<VulkanPass*>(pass);
                       // rstd_info("----release tex");
                       for (auto& tex : texs) {
                           vpass->addReleaseTexs(spanone<const std::string_view> { tex->key() });
                           //    rstd_info("{}", tex->key().data());
                       }
                       return vpass;
                   });

    m_passes.insert(m_passes.begin(), m_prepass.get());
    m_passes.push_back(m_finpass.get());

    setRenderTargetSize(scene, rg);

    for (auto* p : m_passes) {
        if (! p->prepared()) {
            p->prepare(scene, *m_device, m_rendering_resources);
        }
    }
    rebuildRenderPassScopes();

    VVK_CHECK_VOID_RE(m_upload_cmd.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }));
    m_device->mesh_cache().recordPendingUploads(m_upload_cmd);
    VVK_CHECK_VOID_RE(m_upload_cmd.End());
    {
        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = m_upload_cmd.address(),
        };
        VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, {}));
        VVK_CHECK_VOID_RE(m_device->handle().WaitIdle());
    }
    m_pass_loaded = true;
};
