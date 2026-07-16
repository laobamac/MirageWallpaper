module;

#if defined(__linux__)
#include <string>
#endif

#include <rstd/macro.hpp>

export module sr.vulkan_render:resource;
import sr.core;
import rstd.log;
import rstd.cppstd;
import sr.types;
import sr.vulkan;
import sr.scene;

export import :resource_key;

export namespace sr::vulkan
{

class ShaderReflectionCache;

enum class TextureRequestKind
{
    Imported,
    RenderTarget,
    RenderTargetMsaa,
    DepthAttachment,
};

struct TextureRequest {
    TextureRequestKind                 kind { TextureRequestKind::Imported };
    std::string                        name;
    std::optional<RenderTextureDescId> imported_texture;
    std::optional<TextureKey>          cache_key;
    bool                               persist { false };
};

struct TextureBindingRequest {
    std::string                   name;
    std::optional<TextureRequest> request;

    bool empty() const { return name.empty(); }
};

inline bool SameTextureSample(const TextureSample& lhs, const TextureSample& rhs) {
    return lhs.wrapS == rhs.wrapS && lhs.wrapT == rhs.wrapT && lhs.magFilter == rhs.magFilter &&
           lhs.minFilter == rhs.minFilter;
}

inline bool SameTextureKey(const TextureKey& lhs, const TextureKey& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.usage == rhs.usage &&
           lhs.format == rhs.format && SameTextureSample(lhs.sample, rhs.sample) &&
           lhs.mipmap_level == rhs.mipmap_level && lhs.samples == rhs.samples;
}

inline bool SameRenderTextureDescId(const RenderTextureDescId& lhs,
                                    const RenderTextureDescId& rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

inline bool SameTextureRequest(const TextureRequest& lhs, const TextureRequest& rhs) {
    if (lhs.kind != rhs.kind || lhs.name != rhs.name || lhs.persist != rhs.persist) return false;
    if (lhs.imported_texture.has_value() != rhs.imported_texture.has_value()) return false;
    if (lhs.imported_texture.has_value() &&
        ! SameRenderTextureDescId(*lhs.imported_texture, *rhs.imported_texture))
        return false;
    if (lhs.cache_key.has_value() != rhs.cache_key.has_value()) return false;
    if (lhs.cache_key.has_value() && ! SameTextureKey(*lhs.cache_key, *rhs.cache_key)) return false;
    return true;
}

inline bool SameTextureRequest(const std::optional<TextureRequest>& lhs,
                               const std::optional<TextureRequest>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    return ! lhs.has_value() || SameTextureRequest(*lhs, *rhs);
}

inline bool SameTextureBindingRequest(const TextureBindingRequest& lhs,
                                      const TextureBindingRequest& rhs) {
    return lhs.name == rhs.name && SameTextureRequest(lhs.request, rhs.request);
}

inline void WriteTextureSampleIdentity(PipelineKeyWriter& writer, const TextureSample& sample) {
    WritePipelineScalar(writer, sample.wrapS);
    WritePipelineScalar(writer, sample.wrapT);
    WritePipelineScalar(writer, sample.magFilter);
    WritePipelineScalar(writer, sample.minFilter);
}

inline void WriteTextureKeyIdentity(PipelineKeyWriter& writer, const TextureKey& key) {
    writer.writeU32(static_cast<std::uint32_t>(key.width));
    writer.writeU32(static_cast<std::uint32_t>(key.height));
    WritePipelineScalar(writer, key.usage);
    WritePipelineScalar(writer, key.format);
    WriteTextureSampleIdentity(writer, key.sample);
    writer.writeU32(key.mipmap_level);
    WritePipelineScalar(writer, key.samples);
}

inline void WriteRenderTextureDescIdIdentity(PipelineKeyWriter&         writer,
                                             const RenderTextureDescId& id) {
    writer.writeU64(static_cast<std::uint64_t>(id.index));
    writer.writeU64(static_cast<std::uint64_t>(id.generation));
}

inline void WriteTextureRequestIdentity(PipelineKeyWriter& writer, const TextureRequest& request) {
    WritePipelineScalar(writer, request.kind);
    writer.writeString(request.name);
    writer.writeBool(request.imported_texture.has_value());
    if (request.imported_texture.has_value()) {
        WriteRenderTextureDescIdIdentity(writer, *request.imported_texture);
    }
    writer.writeBool(request.cache_key.has_value());
    if (request.cache_key.has_value()) WriteTextureKeyIdentity(writer, *request.cache_key);
    writer.writeBool(request.persist);
}

inline void WriteImageParametersIdentity(PipelineKeyWriter& writer, const ImageParameters& image) {
    writer.writeU64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.handle)));
    writer.writeU64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.view)));
    writer.writeU32(image.extent.width);
    writer.writeU32(image.extent.height);
    writer.writeU32(image.extent.depth);
    writer.writeU32(image.mipmap_level);
    writer.writeU64(image.generation);
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const TextureRequest& request, const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-v1");
    WriteTextureRequestIdentity(writer, request);
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(std::move(writer).finish());
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-image-v1");
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(std::move(writer).finish());
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const TextureRequest&  request,
                                                           const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(request, image),
    };
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(image),
    };
}

inline bool SetTextureRequestIfChanged(std::optional<TextureRequest>& target,
                                       std::optional<TextureRequest>  request) {
    if (SameTextureRequest(target, request)) return false;
    target = std::move(request);
    return true;
}

inline VkSampleCountFlagBits TextureSampleCount(unsigned sample_count) {
    switch (sample_count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline TextureKey RenderTargetTextureKey(sr::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = {},
        .format       = sr::TextureFormat::RGBA8,
        .sample       = rt.sample,
        .mipmap_level = rt.mipmap_level,
    };
}

inline TextureKey RenderTargetTextureKeyNoMip(sr::SceneRenderTarget rt) {
    return TextureKey {
        .width  = rt.width,
        .height = rt.height,
        .usage  = {},
        .format = sr::TextureFormat::RGBA8,
        .sample = rt.sample,
    };
}

inline TextureKey MsaaTextureKey(sr::SceneRenderTarget rt, VkSampleCountFlagBits samples) {
    auto key    = RenderTargetTextureKey(rt);
    key.samples = samples;
    return key;
}

inline TextureKey DepthTextureKey(sr::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = TexUsage::DEPTH,
        .format       = sr::TextureFormat::D32F,
        .sample       = rt.sample,
        .mipmap_level = 1,
        .samples      = TextureSampleCount(rt.sample_count),
    };
}

inline TextureRequest
MakeImportedTextureRequest(std::string_view                   name,
                           std::optional<RenderTextureDescId> texture = std::nullopt) {
    return TextureRequest { .kind             = TextureRequestKind::Imported,
                            .name             = std::string(name),
                            .imported_texture = texture };
}

inline TextureRequest MakeRenderTargetTextureRequest(std::string_view         name,
                                                     const SceneRenderTarget& rt) {
    return TextureRequest { .kind      = TextureRequestKind::RenderTarget,
                            .name      = std::string(name),
                            .cache_key = RenderTargetTextureKey(rt),
                            .persist   = ! rt.allowReuse };
}

inline TextureRequest MakeRenderTargetNoMipTextureRequest(std::string_view         name,
                                                          const SceneRenderTarget& rt) {
    return TextureRequest { .kind      = TextureRequestKind::RenderTarget,
                            .name      = std::string(name),
                            .cache_key = RenderTargetTextureKeyNoMip(rt),
                            .persist   = ! rt.allowReuse };
}

inline TextureRequest MakeMsaaTextureRequest(std::string_view name, const SceneRenderTarget& rt,
                                             VkSampleCountFlagBits samples) {
    return TextureRequest { .kind      = TextureRequestKind::RenderTargetMsaa,
                            .name      = std::string(name),
                            .cache_key = MsaaTextureKey(rt, samples),
                            .persist   = true };
}

inline TextureRequest MakeDepthTextureRequest(std::string_view name, const SceneRenderTarget& rt) {
    return TextureRequest { .kind      = TextureRequestKind::DepthAttachment,
                            .name      = std::string(name),
                            .cache_key = DepthTextureKey(rt),
                            .persist   = ! rt.allowReuse };
}

inline std::optional<ImageParameters> QueryTextureRequest(const Device&         device,
                                                          const TextureRequest& request) {
    if (! request.cache_key.has_value()) return std::nullopt;
    return device.tex_cache().Query(request.name, *request.cache_key, request.persist);
}

inline std::optional<std::string>
ResolveImportedTextureName(const RenderSceneSnapshot& render_scene, const TextureRequest& request) {
    if (request.kind != TextureRequestKind::Imported) return std::nullopt;

    const RenderTextureDescRecord* record { nullptr };
    if (request.imported_texture.has_value()) {
        record = render_scene.textureDesc(*request.imported_texture);
    }
    if (record == nullptr) {
        if (auto id = render_scene.textureDescId(request.name)) {
            record = render_scene.textureDesc(*id);
        }
    }
    if (record == nullptr) return std::nullopt;
    if (! record->desc.url.empty()) return record->desc.url;
    return record->key;
}

class ImportedTextureProvider {
public:
    ImportedTextureProvider()          = default;
    virtual ~ImportedTextureProvider() = default;

    virtual std::string ResolveImportedTextureKey(const TextureRequest&) const = 0;
    virtual std::shared_ptr<Image> ParseImportedTexture(const TextureRequest&) const = 0;
};

class SnapshotImportedTextureProvider : public ImportedTextureProvider {
public:
    SnapshotImportedTextureProvider(const RenderSceneSnapshot& render_scene,
                                    IImageParser*              image_parser)
        : m_render_scene(&render_scene), m_image_parser(image_parser) {}

    std::string ResolveImportedTextureKey(const TextureRequest& request) const override {
        return ResolveImportedTextureName(*m_render_scene, request).value_or(request.name);
    }

    std::shared_ptr<Image> ParseImportedTexture(const TextureRequest& request) const override {
        if (m_image_parser == nullptr) return nullptr;
        return m_image_parser->Parse(ResolveImportedTextureKey(request));
    }

private:
    const RenderSceneSnapshot* m_render_scene { nullptr };
    IImageParser*              m_image_parser { nullptr };
};

class RenderResourceSystem {
public:
    explicit RenderResourceSystem(const Device& device): m_device(&device) {}
    RenderResourceSystem(ImportedTextureProvider* imported_textures, const Device& device)
        : m_imported_textures(imported_textures), m_device(&device) {}

    std::optional<ImageParameters> EnsureTexture(const TextureRequest& request) const {
        if (request.kind == TextureRequestKind::Imported) return std::nullopt;
        return QueryTextureRequest(*m_device, request);
    }

    std::optional<ImageSlotsRef> EnsureSampledTexture(const TextureRequest& request) const {
        if (request.kind != TextureRequestKind::Imported) {
            auto image = EnsureTexture(request);
            if (! image.has_value()) return std::nullopt;
            ImageSlotsRef slots;
            slots.slots = { *image };
            return slots;
        }

        if (m_imported_textures == nullptr) return std::nullopt;
        const auto imported_key = m_imported_textures->ResolveImportedTextureKey(request);
        if (auto cached = m_device->tex_cache().FindImported(imported_key)) {
            m_device->tex_cache().MarkVideoTextureActive(imported_key);
            return cached;
        }
        auto image = m_imported_textures->ParseImportedTexture(request);
        if (! image) {
            rstd_error("parse tex \"{}\" failed", request.name);
            return std::nullopt;
        }
        auto slots = m_device->tex_cache().CreateTex(*image);
        m_device->tex_cache().MarkVideoTextureActive(image->key);
        return slots;
    }

    void MarkShareReady(std::string_view key) const { m_device->tex_cache().MarkShareReady(key); }

private:
    ImportedTextureProvider* m_imported_textures { nullptr };
    const Device*            m_device { nullptr };
};

struct PipelineResourceEntry {
    PipelineParameters pipeline;
};

struct PipelineResourceResult {
    std::shared_ptr<PipelineResourceEntry> pipeline;
    PipelineCacheKey                       cache_key;
    RenderPassCacheKey                     render_pass_key;
    bool                                   cache_hit { false };
    uint64_t                               cache_observed_count { 0 };
    bool                                   render_pass_cache_hit { false };
    uint64_t                               render_pass_cache_observed_count { 0 };
};

struct FramebufferResourceResult {
    std::shared_ptr<vvk::Framebuffer> framebuffer;
    FramebufferCacheKey               cache_key;
    bool                              cache_hit { false };
    uint64_t                          cache_observed_count { 0 };
};

class PipelineCacheDiagnostics {
public:
    PipelineCacheProbe Record(PipelineCacheKey key) {
        auto& count = m_seen[key];
        bool  hit   = count > 0;
        ++count;
        return PipelineCacheProbe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = count,
        };
    }

private:
    std::unordered_map<PipelineCacheKey, uint64_t, CanonicalCacheKeyHash, PipelineCacheKeyEqual>
        m_seen;
};

class FramebufferCacheDiagnostics {
public:
    struct Probe {
        FramebufferCacheKey key;
        bool                hit { false };
        uint64_t            observed_count { 0 };
    };

    Probe Record(FramebufferCacheKey key) {
        auto& count = m_seen[key];
        bool  hit   = count > 0;
        ++count;
        return Probe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = count,
        };
    }

private:
    std::unordered_map<FramebufferCacheKey, uint64_t, CanonicalCacheKeyHash,
                       FramebufferCacheKeyEqual>
        m_seen;
};

class FramebufferResourceCache {
public:
    std::optional<FramebufferResourceResult> Ensure(const Device&                     device,
                                                    const FramebufferResourceRequest& request) {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return std::nullopt;
        }

        auto  desc = MakeFramebufferResourceDesc(request);
        auto  key  = MakeFramebufferCacheKey(desc);
        auto& slot = m_entries[key];
        if (auto existing = slot.framebuffer.lock()) {
            ++slot.observed_count;
            return FramebufferResourceResult {
                .framebuffer          = std::move(existing),
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = slot.observed_count,
            };
        }

        std::vector<VkImageView> attachment_views;
        attachment_views.reserve(desc.attachments.size());
        for (const auto& attachment : desc.attachments) attachment_views.push_back(attachment.view);

        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = desc.render_pass,
            .attachmentCount = static_cast<uint32_t>(attachment_views.size()),
            .pAttachments    = attachment_views.data(),
            .width           = desc.extent.width,
            .height          = desc.extent.height,
            .layers          = desc.layers,
        };
        vvk::Framebuffer framebuffer;
        if (device.handle().CreateFramebuffer(info, framebuffer) != VK_SUCCESS) {
            return std::nullopt;
        }
        auto shared      = std::make_shared<vvk::Framebuffer>(std::move(framebuffer));
        slot.framebuffer = shared;
        ++slot.observed_count;
        return FramebufferResourceResult {
            .framebuffer          = std::move(shared),
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = slot.observed_count,
        };
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second.framebuffer.expired()) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::weak_ptr<vvk::Framebuffer> framebuffer;
        uint64_t                        observed_count { 0 };
    };

    std::unordered_map<FramebufferCacheKey, Entry, CanonicalCacheKeyHash, FramebufferCacheKeyEqual>
        m_entries;
};

inline bool HasPipelineResources(const PipelineParameters& pipeline) {
    return static_cast<bool>(pipeline.handle) || static_cast<bool>(pipeline.layout) ||
           static_cast<bool>(pipeline.pass) || ! pipeline.descriptor_layouts.empty();
}

inline bool HasPipelineResources(const PipelineResourceEntry& entry) {
    return HasPipelineResources(entry.pipeline);
}

struct RenderPassResourceResult {
    std::shared_ptr<vvk::RenderPass> render_pass;
    RenderPassCacheKey               cache_key;
    bool                             cache_hit { false };
    uint64_t                         cache_observed_count { 0 };
};

class RenderPassResourceCache {
public:
    std::optional<RenderPassResourceResult> Ensure(const Device&                 device,
                                                   const RenderPassResourceDesc& desc) {
        auto  key  = MakeRenderPassCacheKey(desc);
        auto& slot = m_entries[key];
        if (auto existing = slot.render_pass.lock()) {
            ++slot.observed_count;
            return RenderPassResourceResult {
                .render_pass          = std::move(existing),
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = slot.observed_count,
            };
        }

        auto created = CreateRenderPass(device, desc);
        if (! created.has_value()) return std::nullopt;
        auto shared      = std::make_shared<vvk::RenderPass>(std::move(*created));
        slot.render_pass = shared;
        ++slot.observed_count;
        return RenderPassResourceResult {
            .render_pass          = std::move(shared),
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = slot.observed_count,
        };
    }

    std::optional<RenderPassResourceResult> Ensure(const Device&                  device,
                                                   const PipelineResourceRequest& request) {
        return Ensure(device, MakeRenderPassResourceDesc(request));
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second.render_pass.expired()) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::weak_ptr<vvk::RenderPass> render_pass;
        uint64_t                       observed_count { 0 };
    };

    static std::optional<vvk::RenderPass> CreateRenderPass(const Device&                 device,
                                                           const RenderPassResourceDesc& desc) {
        const bool              has_resolve = desc.samples != VK_SAMPLE_COUNT_1_BIT;
        VkAttachmentDescription color {
            .format         = desc.color_format,
            .samples        = desc.samples,
            .loadOp         = desc.color_load_op,
            .storeOp        = desc.color_store_op,
            .stencilLoadOp  = desc.color_stencil_load_op,
            .stencilStoreOp = desc.color_stencil_store_op,
            .initialLayout  = desc.color_initial_layout,
            .finalLayout    = has_resolve ? desc.color_attachment_layout : desc.color_final_layout,
        };

        VkAttachmentDescription resolve {
            .format         = desc.color_format,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = desc.resolve_load_op,
            .storeOp        = desc.resolve_store_op,
            .stencilLoadOp  = desc.resolve_stencil_load_op,
            .stencilStoreOp = desc.resolve_stencil_store_op,
            .initialLayout  = desc.resolve_initial_layout,
            .finalLayout    = desc.resolve_final_layout,
        };

        VkAttachmentDescription depth {
            .format         = desc.depth_format,
            .samples        = desc.samples,
            .loadOp         = desc.depth_load_op,
            .storeOp        = desc.depth_store_op,
            .stencilLoadOp  = desc.depth_stencil_load_op,
            .stencilStoreOp = desc.depth_stencil_store_op,
            .initialLayout  = desc.depth_initial_layout,
            .finalLayout    = desc.depth_final_layout,
        };

        VkAttachmentReference color_ref {
            .attachment = 0,
            .layout     = desc.color_attachment_layout,
        };
        VkAttachmentReference resolve_ref {
            .attachment = 1,
            .layout     = desc.resolve_attachment_layout,
        };
        VkAttachmentReference depth_ref {
            .attachment = has_resolve ? 2u : 1u,
            .layout     = desc.depth_attachment_layout,
        };

        std::vector<VkAttachmentDescription> attachments;
        attachments.reserve(3);
        attachments.push_back(color);
        if (has_resolve) attachments.push_back(resolve);
        if (desc.has_depth_attachment) attachments.push_back(depth);

        VkSubpassDescription subpass {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &color_ref,
            .pResolveAttachments     = has_resolve ? &resolve_ref : nullptr,
            .pDepthStencilAttachment = desc.has_depth_attachment ? &depth_ref : nullptr,
        };

        VkSubpassDependency dependency {
            .srcSubpass   = VK_SUBPASS_EXTERNAL,
            .dstSubpass   = 0,
            .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        };

        VkRenderPassCreateInfo create {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = static_cast<uint32_t>(attachments.size()),
            .pAttachments    = attachments.data(),
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = 1,
            .pDependencies   = &dependency,
        };
        vvk::RenderPass pass;
        if (device.handle().CreateRenderPass(create, pass) != VK_SUCCESS) return std::nullopt;
        return pass;
    }

    std::unordered_map<RenderPassCacheKey, Entry, CanonicalCacheKeyHash, RenderPassCacheKeyEqual>
        m_entries;
};

class PipelineResourceCache {
public:
    std::optional<PipelineResourceResult> Ensure(const Device&            device,
                                                 PipelineResourceRequest  request,
                                                 RenderPassResourceCache& render_pass_cache) {
        auto  desc = MakePipelineResourceDesc(request);
        auto  key  = MakePipelineCacheKey(desc);
        auto& slot = m_entries[key];
        if (auto existing = slot.pipeline.lock()) {
            ++slot.observed_count;
            return PipelineResourceResult {
                .pipeline                         = std::move(existing),
                .cache_key                        = key,
                .render_pass_key                  = slot.render_pass_key,
                .cache_hit                        = true,
                .cache_observed_count             = slot.observed_count,
                .render_pass_cache_hit            = true,
                .render_pass_cache_observed_count = slot.observed_count,
            };
        }

        auto render_pass = render_pass_cache.Ensure(device, desc.render_pass);
        if (! render_pass.has_value() || ! render_pass->render_pass) return std::nullopt;

        auto             entry = std::make_shared<PipelineResourceEntry>();
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        pipeline.depth       = desc.depth;
        pipeline.raster      = desc.raster;
        pipeline.multisample = desc.multisample;
        pipeline
            .setColorBlendStates(
                std::span<const VkPipelineColorBlendAttachmentState>(&desc.color_blend, 1))
            .setCreateInfoOptions(desc.create_flags, desc.subpass)
            .setColorBlendOptions(desc.color_blend_flags, desc.blend_constants)
            .setLogicOp(desc.logic_op_enable, desc.logic_op)
            .setTopology(desc.topology)
            .setPrimitiveRestartEnable(desc.primitive_restart_enable)
            .setViewportScissorCount(desc.viewport_count, desc.scissor_count)
            .setDynamicStates(desc.dynamic_states)
            .addInputBindingDescription(desc.vertex_bindings)
            .addInputAttributeDescription(desc.vertex_attrs)
            .addDescriptorSetInfo(desc.descriptor_sets);
        for (auto& spv : desc.shader_stages) {
            pipeline.addStage(std::make_unique<ShaderSpv>(std::move(spv)));
        }
        if (! pipeline.create(device, **render_pass->render_pass, entry->pipeline)) {
            return std::nullopt;
        }
        entry->pipeline.pass = render_pass->render_pass;
        slot.pipeline        = entry;
        slot.render_pass_key = render_pass->cache_key;
        ++slot.observed_count;
        return PipelineResourceResult {
            .pipeline                         = std::move(entry),
            .cache_key                        = key,
            .render_pass_key                  = render_pass->cache_key,
            .cache_hit                        = false,
            .cache_observed_count             = slot.observed_count,
            .render_pass_cache_hit            = render_pass->cache_hit,
            .render_pass_cache_observed_count = render_pass->cache_observed_count,
        };
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second.pipeline.expired()) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::weak_ptr<PipelineResourceEntry> pipeline;
        RenderPassCacheKey                   render_pass_key;
        uint64_t                             observed_count { 0 };
    };

    std::unordered_map<PipelineCacheKey, Entry, CanonicalCacheKeyHash, PipelineCacheKeyEqual>
        m_entries;
};

class PipelineRetireQueue {
public:
    void Retire(PipelineParameters&& pipeline) {
        if (! HasPipelineResources(pipeline)) return;
        m_retired.push_back(std::move(pipeline));
    }

    void Retire(std::shared_ptr<PipelineResourceEntry> pipeline) {
        if (! pipeline || ! HasPipelineResources(*pipeline)) return;
        m_pipeline_entries.push_back(std::move(pipeline));
    }

    void Retire(vvk::Framebuffer&& framebuffer) {
        if (! framebuffer) return;
        m_framebuffers.push_back(std::make_shared<vvk::Framebuffer>(std::move(framebuffer)));
    }

    void Retire(std::shared_ptr<vvk::Framebuffer> framebuffer) {
        if (! framebuffer || ! *framebuffer) return;
        m_framebuffers.push_back(std::move(framebuffer));
    }

    void ReleaseReady() {
        m_pipeline_entries.clear();
        m_retired.clear();
    }

    void ReleaseFramebuffersReady() { m_framebuffers.clear(); }

    void ReleaseAllReady() {
        ReleaseFramebuffersReady();
        ReleaseReady();
    }

    std::size_t pending() const {
        return m_pipeline_entries.size() + m_retired.size() + m_framebuffers.size();
    }

private:
    std::vector<std::shared_ptr<PipelineResourceEntry>> m_pipeline_entries;
    std::vector<PipelineParameters>                     m_retired;
    std::vector<std::shared_ptr<vvk::Framebuffer>>      m_framebuffers;
};

class FramebufferResourceSystem {
public:
    explicit FramebufferResourceSystem(const Device&                device,
                                       FramebufferResourceCache*    cache       = nullptr,
                                       FramebufferCacheDiagnostics* diagnostics = nullptr)
        : m_device(&device), m_cache(cache), m_diagnostics(diagnostics) {}

    std::optional<FramebufferResourceResult>
    CreateFramebuffer(const FramebufferResourceRequest& request) const {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return std::nullopt;
        }
        if (m_diagnostics != nullptr) m_diagnostics->Record(MakeFramebufferCacheKey(request));
        FramebufferResourceCache local_cache;
        auto&                    cache = m_cache != nullptr ? *m_cache : local_cache;
        return cache.Ensure(*m_device, request);
    }

private:
    const Device*                m_device { nullptr };
    FramebufferResourceCache*    m_cache { nullptr };
    FramebufferCacheDiagnostics* m_diagnostics { nullptr };
};

class PipelineResourceSystem {
public:
    explicit PipelineResourceSystem(const Device&            device,
                                    PipelineResourceCache*   pipeline_cache    = nullptr,
                                    RenderPassResourceCache* render_pass_cache = nullptr)
        : m_device(&device),
          m_pipeline_cache(pipeline_cache),
          m_render_pass_cache(render_pass_cache) {}

    std::optional<PipelineResourceResult>
    CreateGraphicsPipeline(PipelineResourceRequest request) const {
        PipelineResourceCache   local_pipeline_cache;
        RenderPassResourceCache local_render_pass_cache;
        auto&                   pipeline_cache =
            m_pipeline_cache != nullptr ? *m_pipeline_cache : local_pipeline_cache;
        auto& render_pass_cache =
            m_render_pass_cache != nullptr ? *m_render_pass_cache : local_render_pass_cache;
        return pipeline_cache.Ensure(*m_device, std::move(request), render_pass_cache);
    }

private:
    const Device*            m_device { nullptr };
    PipelineResourceCache*   m_pipeline_cache { nullptr };
    RenderPassResourceCache* m_render_pass_cache { nullptr };
};

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_export;
    vvk::Semaphore sem_upload;
    vvk::Fence     fence_frame;
    uint64_t       upload_timeline_value { 0 };
    uint64_t       pending_upload_value { 0 };

    // Static vertex/index buffers are owned by Device::mesh_cache() now;
    // only the per-rebuild dyn_buf lives here.
    StagingBuffer*              dyn_buf { nullptr };
    ShaderReflectionCache*      shader_reflection_cache { nullptr };
    ImportedTextureProvider*    imported_texture_provider { nullptr };
    PipelineResourceCache       pipeline_cache;
    RenderPassResourceCache     render_pass_cache;
    FramebufferResourceCache    framebuffer_cache;
    FramebufferCacheDiagnostics framebuffer_cache_diagnostics;
    PipelineRetireQueue         pipeline_retire_queue;
};

} // namespace sr::vulkan
