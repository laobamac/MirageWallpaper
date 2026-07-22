module;

#include "FrameGraph/Pass.hpp"

export module sr.vulkan_render:vulkan_pass;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :resource;

export namespace sr
{

namespace vulkan
{

using PassInvalidationFlags = uint32_t;

enum class PassInvalidation : PassInvalidationFlags
{
    Resources   = 1u << 0u,
    Pipeline    = 1u << 1u,
    Framebuffer = 1u << 2u,
};

inline constexpr PassInvalidationFlags PassInvalidationNone { 0u };
inline constexpr PassInvalidationFlags PassInvalidationAll {
    static_cast<PassInvalidationFlags>(PassInvalidation::Resources) |
        static_cast<PassInvalidationFlags>(PassInvalidation::Pipeline) |
        static_cast<PassInvalidationFlags>(PassInvalidation::Framebuffer),
};

inline constexpr PassInvalidationFlags ToPassInvalidationFlags(PassInvalidation invalidation) {
    return static_cast<PassInvalidationFlags>(invalidation);
}

struct MaterialTextureBindingRefresh {
    PassInvalidationFlags invalidation_flags { PassInvalidationNone };
    bool                  requires_graph_rebuild { false };
};

struct PassTextureRequestDiagnostic {
    std::string                   role;
    uint32_t                      slot { 0 };
    std::string                   name;
    std::optional<TextureRequest> request;
};

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                                      = default;
    virtual ~VulkanPass()                                                             = default;
    virtual void                  prepare(Scene&, const Device&, RenderingResources&) = 0;
    virtual void                  execute(const Device&, RenderingResources&)         = 0;
    virtual void                  destory(const Device&, RenderingResources&)         = 0;
    virtual PassInvalidationFlags finalizeResourceRequests(Scene&) { return PassInvalidationNone; }
    virtual std::optional<RenderItemId>        renderItemId() const { return std::nullopt; }
    virtual std::optional<PipelineCacheKey>    pipelineCacheKey() const { return std::nullopt; }
    virtual bool                               pipelineCacheHit() const { return false; }
    virtual uint64_t                           pipelineCacheObservedCount() const { return 0; }
    virtual std::optional<RenderPassCacheKey>  renderPassCacheKey() const { return std::nullopt; }
    virtual bool                               renderPassCacheHit() const { return false; }
    virtual uint64_t                           renderPassCacheObservedCount() const { return 0; }
    virtual std::optional<FramebufferCacheKey> framebufferCacheKey() const { return std::nullopt; }
    virtual bool                               framebufferCacheHit() const { return false; }
    virtual uint64_t                           framebufferCacheObservedCount() const { return 0; }
    virtual std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const {
        return {};
    }
    virtual MaterialTextureBindingRefresh
    refreshMaterialTextureBindings(const RenderSceneSnapshot&) {
        return {};
    }
    virtual bool setTextureBinding(uint32_t, TextureBindingRequest) { return false; }
    // Update CPU-side per-frame data before the dynamic staging buffer is
    // copied to the GPU. Command-buffer work remains in execute()/the render
    // scope hooks below.
    virtual void prepareFrameData(RenderingResources&) {}
    virtual bool supportsRenderScope() const { return false; }
    virtual bool canJoinRenderScopeAfter(const VulkanPass&) const { return false; }
    virtual void prepareRenderScopeDraw(RenderingResources&) {}
    virtual void beginRenderScope(RenderingResources&) {}
    virtual void recordRenderScopeDraw(RenderingResources&) {}
    virtual void endRenderScope(RenderingResources&) {}

    void addReleaseTexs(std::span<const std::string_view> texs) {
        m_release_texs.clear();
        std::transform(texs.begin(), texs.end(), std::back_inserter(m_release_texs), [](auto& sv) {
            return std::string(sv);
        });
    }
    bool                         prepared() const { return m_prepared; }
    std::span<const std::string> releaseTexs() const { return m_release_texs; }
    void                         clearReleaseTexs() { m_release_texs.clear(); }
    void                         resetPrepared() { setPrepared(false); }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }

private:
    bool                     m_prepared { false };
    std::vector<std::string> m_release_texs;
};

} // namespace vulkan
} // namespace sr
