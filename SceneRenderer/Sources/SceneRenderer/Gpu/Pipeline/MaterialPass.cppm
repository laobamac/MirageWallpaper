module;

export module sr.vulkan_render:custom_shader_pass;
import sr.core;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :vulkan_pass;
import :resource;
import :buffer_resolver;

export namespace sr::vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*      node { nullptr };
        SceneDrawItemId draw_item;
        RenderItemId    render_item;
        // Which submesh of node->Mesh() this pass renders. SceneToRenderGraph
        // emits one pass per (node, submesh).
        uint32_t                           submesh_index { 0 };
        std::vector<TextureBindingRequest> texture_bindings;
        std::string                        output;
        std::optional<TextureRequest>      output_request;
        std::optional<TextureRequest>      output_msaa_request;
        std::optional<TextureRequest>      depth_request;
        sprite_map_t                       sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;
        // MSAA twin (color attachment) when output RT has sample_count>1.
        // Empty handle means no MSAA; framebuffer attaches only vk_output.
        ImageParameters       vk_output_msaa;
        ImageParameters       vk_depth;
        VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };
        bool                  has_depth_attachment { false };

        // bufs
        DrawBufferRefs   draw_buffers;
        StagingBufferRef ubo_buf;

        // pipeline
        VkClearValue clear_value;
        // When non-null this pass tracks `scene.clearColor` per-frame
        // (re-syncing `clear_value.color` from the array each execute);
        // null means the pass owns a hard-coded clear (e.g. effect-layer
        // ppong RTs that always reset transparent).
        const std::array<float, 3>*            clear_value_src { nullptr };
        bool                                   blending { false };
        bool                                   clear_output { false };
        bool                                   transparent_clear { false };
        bool                                   clear_depth { false };
        bool                                   preserve_output { false };
        VkAttachmentLoadOp                     color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        VkAttachmentLoadOp                     depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        std::shared_ptr<vvk::Framebuffer>      fb;
        std::shared_ptr<PipelineResourceEntry> pipeline;
        std::optional<PipelineCacheKey>        pipeline_cache_key;
        std::optional<RenderPassCacheKey>      render_pass_cache_key;
        std::optional<FramebufferCacheKey>     framebuffer_cache_key;
        bool                                   pipeline_cache_hit { false };
        uint64_t                               pipeline_cache_observed_count { 0 };
        bool                                   render_pass_cache_hit { false };
        uint64_t                               render_pass_cache_observed_count { 0 };
        bool                                   framebuffer_cache_hit { false };
        uint64_t                               framebuffer_cache_observed_count { 0 };

        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    PassInvalidationFlags                     finalizeResourceRequests(Scene&) override;
    std::optional<RenderItemId>               renderItemId() const override;
    std::optional<PipelineCacheKey>           pipelineCacheKey() const override;
    bool                                      pipelineCacheHit() const override;
    uint64_t                                  pipelineCacheObservedCount() const override;
    std::optional<RenderPassCacheKey>         renderPassCacheKey() const override;
    bool                                      renderPassCacheHit() const override;
    uint64_t                                  renderPassCacheObservedCount() const override;
    std::optional<FramebufferCacheKey>        framebufferCacheKey() const override;
    bool                                      framebufferCacheHit() const override;
    uint64_t                                  framebufferCacheObservedCount() const override;
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    MaterialTextureBindingRefresh
         refreshMaterialTextureBindings(const RenderSceneSnapshot&) override;
    bool setTextureBinding(uint32_t index, TextureBindingRequest binding) override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    void prepareFrameData(RenderingResources&) override;
    bool supportsRenderScope() const override;
    bool canJoinRenderScopeAfter(const VulkanPass& previous) const override;
    void prepareRenderScopeDraw(RenderingResources&) override;
    void recordSampledImageBarriers(RenderingResources&);
    void beginRenderScope(RenderingResources&) override;
    void recordRenderScopeDraw(RenderingResources&) override;
    void endRenderScope(RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace sr::vulkan
