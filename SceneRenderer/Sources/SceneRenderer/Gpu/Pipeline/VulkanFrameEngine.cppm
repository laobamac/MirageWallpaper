module;

export module sr.vulkan_render;
import sr.types;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import sr.rgraph;

export import :vulkan_pass;
export import :shader_reflection_cache;
export import :resource;
export import :buffer_resolver;
export import :pass_common;
export import :copy_pass;
export import :custom_shader_pass;
export import :fin_pass;
export import :pre_pass;

export namespace sr
{

using ReDrawCB = std::function<void()>;
using MetalFrameCB = std::function<void(void* mtl_texture, uint32_t width, uint32_t height)>;

struct VulkanSurfaceInfo {
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurfaceOp;
    std::vector<std::string>                           instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };

    std::span<const std::uint8_t> uuid;
    TexTiling                     offscreen_tiling { TexTiling::OPTIMAL };
    VulkanSurfaceInfo             surface_info;

    uint16_t    width { 1920 };
    uint16_t    height { 1080 };
    std::string video_hwdec { "auto" };
    // MSAA samples for the screen RT only. 1 disables. Clamped down to
    // device's framebufferColorSampleCounts at init.
    uint32_t msaa_samples { 1 };
    ReDrawCB redraw_callback;
    MetalFrameCB metal_frame_callback;
};

std::unique_ptr<rg::RenderGraph> sceneToRenderGraph(Scene&);
std::unique_ptr<rg::RenderGraph> sceneToRenderGraph(Scene&, const RenderSceneSnapshot&);

namespace vulkan
{

class FinPass;

enum class RenderGraphResourceRetention
{
    KeepSceneTextures,
    ReleaseSceneTextures,
};

struct PreparedPassDiagnostic {
    bool                                      frame_pass { false };
    std::optional<rg::NodeID>                 graph_node;
    std::string                               pass_name;
    std::optional<rg::PassNode::Type>         pass_type;
    std::optional<RenderItemId>               render_item;
    PassInvalidationFlags                     invalidation_flags { PassInvalidationNone };
    std::optional<PipelineCacheKey>           pipeline_cache_key;
    bool                                      pipeline_cache_hit { false };
    uint64_t                                  pipeline_cache_observed_count { 0 };
    std::optional<RenderPassCacheKey>         render_pass_cache_key;
    bool                                      render_pass_cache_hit { false };
    uint64_t                                  render_pass_cache_observed_count { 0 };
    std::optional<FramebufferCacheKey>        framebuffer_cache_key;
    bool                                      framebuffer_cache_hit { false };
    uint64_t                                  framebuffer_cache_observed_count { 0 };
    std::vector<std::string>                  release_textures;
    std::vector<PassTextureRequestDiagnostic> texture_requests;
    bool                                      prepared { false };
};

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);

    void clearLastRenderGraph(
        RenderGraphResourceRetention retention = RenderGraphResourceRetention::KeepSceneTextures);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&);
    void refreshPreparedResources(Scene&);
    void refreshPreparedResources(Scene&, const RenderSceneSnapshot&);
    void invalidatePreparedRenderItems(std::span<const RenderItemId>, PassInvalidationFlags);
    void refreshPreparedRenderItems(Scene&, const RenderSceneSnapshot&,
                                    std::span<const RenderItemId>, PassInvalidationFlags);
    void refreshPreparedMaterial(Scene&, const RenderSceneSnapshot&, SceneMaterialId,
                                 PassInvalidationFlags);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&, SceneMaterialId);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&,
                                         std::span<const SceneMaterialId>);
    void refreshPreparedMesh(Scene&, const RenderSceneSnapshot&, SceneMeshId,
                             PassInvalidationFlags);
    std::vector<PreparedPassDiagnostic> preparedPassDiagnostics() const;
    // Free unreferenced MeshCache entries. Call when the scene set changes
    // (RenderSetScene); skip for swapchain-only rebuilds where the same
    // SceneMesh set survives.
    void evictUnusedMeshes();
    void UpdateCameraFillMode(Scene&, sr::FillMode);

    bool onSwapchainReady(unsigned width, unsigned height);

    ExSwapchain* exSwapchain() const;
    bool         inited() const;
    // Called on the render thread after compileRenderGraph; unlike inited(),
    // this includes successful render-program preparation.
    bool         readyToDraw() const;

    /* Tick all registered video-tex decoders. No-op when no scene
     * texture has been recognised as a VIDEO container. Invoked from
     * SceneWallpaper's per-frame RenderDraw handler. */
    void pumpVideoTextures(double dt_seconds);

    /* For every FontFace in scene.font_cache with non-empty DirtyRects,
     * coalesce to one AABB and vkCmdCopyBufferToImage into the face's
     * atlas VkImage. Skips faces whose VkImage hasn't been created yet
     * (CreateTex runs lazily on first material bind — those pixels reach
     * the GPU through the aliased Image::mip.data instead). Clears each
     * face's dirty_rects regardless. */
    void pumpFontAtlases(Scene& scene);

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace vulkan
} // namespace sr
