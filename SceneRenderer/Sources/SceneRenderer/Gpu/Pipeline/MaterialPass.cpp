module;

#include <rstd/macro.hpp>
#include "Utils/AutoDeletor.hpp"
#include "vvk/macros.hpp"

module sr.vulkan_render;
import sr.spec_texs;
import sr.core;
import rstd.log;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

using namespace sr::vulkan;

CustomShaderPass::CustomShaderPass(const Desc& desc) {
    m_desc.node                = desc.node;
    m_desc.draw_item           = desc.draw_item;
    m_desc.render_item         = desc.render_item;
    m_desc.submesh_index       = desc.submesh_index;
    m_desc.texture_bindings    = desc.texture_bindings;
    m_desc.output              = desc.output;
    m_desc.output_request      = desc.output_request;
    m_desc.output_msaa_request = desc.output_msaa_request;
    m_desc.depth_request       = desc.depth_request;
    m_desc.sprites_map         = desc.sprites_map;
    m_desc.clear_output        = desc.clear_output;
    m_desc.transparent_clear   = desc.transparent_clear;
    m_desc.clear_depth         = desc.clear_depth;
    m_desc.preserve_output     = desc.preserve_output;
};
CustomShaderPass::~CustomShaderPass() {}

namespace
{
std::optional<TextureRequest> TextureRequestFromScene(sr::Scene& scene, std::string_view name) {
    if (name.empty()) return std::nullopt;
    if (! sr::IsSpecTex(name)) return MakeImportedTextureRequest(name);
    auto it = scene.renderTargets.find(std::string(name));
    if (it == scene.renderTargets.end()) return std::nullopt;
    return MakeRenderTargetTextureRequest(name, it->second);
}

} // namespace

PassInvalidationFlags CustomShaderPass::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags = PassInvalidationNone;
    for (auto& binding : m_desc.texture_bindings) {
        if (binding.name.empty() || ! IsSpecTex(binding.name)) continue;
        if (SetTextureRequestIfChanged(binding.request,
                                       TextureRequestFromScene(scene, binding.name))) {
            flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }
    }

    if (! m_desc.output.empty() && IsSpecTex(m_desc.output)) {
        if (auto it = scene.renderTargets.find(m_desc.output); it != scene.renderTargets.end()) {
            auto& rt             = it->second;
            auto  output_request = MakeRenderTargetTextureRequest(m_desc.output, rt);
            if (SetTextureRequestIfChanged(m_desc.output_request, std::move(output_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            auto samples = TextureSampleCount(rt.sample_count);
            if (m_desc.samples != samples) {
                m_desc.samples = samples;
                flags |= PassInvalidationAll;
            }

            std::optional<TextureRequest> msaa_request;
            if (samples != VK_SAMPLE_COUNT_1_BIT) {
                auto twin_name = MsaaTwinName(m_desc.output, samples);
                msaa_request   = MakeMsaaTextureRequest(twin_name, rt, samples);
            }
            if (SetTextureRequestIfChanged(m_desc.output_msaa_request, std::move(msaa_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            bool has_depth_attachment = false;
            if (m_desc.node != nullptr && m_desc.node->Mesh() != nullptr) {
                auto& mesh = *m_desc.node->Mesh();
                if (m_desc.submesh_index < mesh.Submeshes().size()) {
                    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
                    const auto& slots   = mesh.MaterialSlots();
                    if (submesh.material_slot < slots.size() && slots[submesh.material_slot]) {
                        has_depth_attachment =
                            rt.withDepth && UsesDepthAttachment(*slots[submesh.material_slot]);
                    }
                }
            }
            if (m_desc.has_depth_attachment != has_depth_attachment) {
                m_desc.has_depth_attachment = has_depth_attachment;
                flags |= PassInvalidationAll;
            }

            std::optional<TextureRequest> depth_request;
            if (has_depth_attachment) {
                depth_request = MakeDepthTextureRequest(m_desc.output + "::depth", rt);
            }
            if (SetTextureRequestIfChanged(m_desc.depth_request, std::move(depth_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }
        }
    }
    return flags;
}

std::optional<sr::RenderItemId> CustomShaderPass::renderItemId() const {
    return m_desc.render_item;
}

std::optional<PipelineCacheKey> CustomShaderPass::pipelineCacheKey() const {
    return m_desc.pipeline_cache_key;
}

bool CustomShaderPass::pipelineCacheHit() const { return m_desc.pipeline_cache_hit; }

uint64_t CustomShaderPass::pipelineCacheObservedCount() const {
    return m_desc.pipeline_cache_observed_count;
}

std::optional<RenderPassCacheKey> CustomShaderPass::renderPassCacheKey() const {
    return m_desc.render_pass_cache_key;
}

bool CustomShaderPass::renderPassCacheHit() const { return m_desc.render_pass_cache_hit; }

uint64_t CustomShaderPass::renderPassCacheObservedCount() const {
    return m_desc.render_pass_cache_observed_count;
}

std::optional<FramebufferCacheKey> CustomShaderPass::framebufferCacheKey() const {
    return m_desc.framebuffer_cache_key;
}

bool CustomShaderPass::framebufferCacheHit() const { return m_desc.framebuffer_cache_hit; }

uint64_t CustomShaderPass::framebufferCacheObservedCount() const {
    return m_desc.framebuffer_cache_observed_count;
}

std::vector<PassTextureRequestDiagnostic> CustomShaderPass::textureRequestDiagnostics() const {
    std::vector<PassTextureRequestDiagnostic> out;
    out.reserve(m_desc.texture_bindings.size() + 3);
    for (std::size_t i = 0; i < m_desc.texture_bindings.size(); ++i) {
        const auto& binding = m_desc.texture_bindings[i];
        if (binding.name.empty() && ! binding.request.has_value()) continue;
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "sampled",
            .slot    = static_cast<uint32_t>(i),
            .name    = binding.name,
            .request = binding.request,
        });
    }
    if (! m_desc.output.empty() || m_desc.output_request.has_value()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "output",
            .name    = m_desc.output,
            .request = m_desc.output_request,
        });
    }
    if (m_desc.output_msaa_request.has_value()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "output-msaa",
            .name    = m_desc.output_msaa_request->name,
            .request = m_desc.output_msaa_request,
        });
    }
    if (m_desc.depth_request.has_value()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "depth",
            .name    = m_desc.depth_request->name,
            .request = m_desc.depth_request,
        });
    }
    return out;
}

MaterialTextureBindingRefresh
CustomShaderPass::refreshMaterialTextureBindings(const RenderSceneSnapshot& render_scene) {
    MaterialTextureBindingRefresh result;
    if (m_desc.node == nullptr || m_desc.node->Mesh() == nullptr) return result;

    auto& mesh = *m_desc.node->Mesh();
    if (m_desc.submesh_index >= mesh.Submeshes().size()) return result;
    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return result;

    const auto& textures = slots[submesh.material_slot]->textures;
    if (textures.size() != m_desc.texture_bindings.size()) {
        result.requires_graph_rebuild = true;
        return result;
    }

    for (usize i = 0; i < textures.size(); ++i) {
        const auto& next = textures[i];
        const auto& old  = m_desc.texture_bindings[i];
        if (! CanRefreshSceneMaterialTextureBinding(old.name, next, m_desc.output)) {
            result.requires_graph_rebuild = true;
            return result;
        }
    }

    for (usize i = 0; i < textures.size(); ++i) {
        const auto& next     = textures[i];
        auto&       old      = m_desc.texture_bindings[i];
        auto        next_dep = ClassifySceneMaterialTexture(next);
        if (old.name == next && ! IsLocalSceneMaterialTextureDependency(next_dep)) continue;

        TextureBindingRequest binding;
        if (! next.empty()) {
            binding.name    = next;
            binding.request = MakeImportedTextureRequest(next, render_scene.textureDescId(next));
        }

        if (! SameTextureBindingRequest(old, binding)) {
            old = std::move(binding);
            result.invalidation_flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }

        if (next.empty()) {
            m_desc.sprites_map.erase(i);
            continue;
        }

        auto  texture_id = render_scene.textureDescId(next);
        auto* texture    = texture_id.has_value() ? render_scene.textureDesc(*texture_id) : nullptr;
        if (texture != nullptr && texture->desc.isSprite) {
            m_desc.sprites_map[i] = texture->desc.spriteAnim;
        } else {
            m_desc.sprites_map.erase(i);
        }
    }

    return result;
}

static std::span<uint8_t> MakeUniformUploadBytes(const sr::ShaderValue& value, size_t refl_size,
                                                 std::vector<sr::ShaderValue::value_type>& resized,
                                                 bool& compatible) {
    compatible                    = true;
    const size_t       value_size = value.size() * sizeof(sr::ShaderValue::value_type);
    std::span<uint8_t> value_u8 {
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())),
        value_size,
    };

    if (refl_size != value_size && refl_size % sizeof(sr::ShaderValue::value_type) == 0) {
        const size_t refl_count = refl_size / sizeof(sr::ShaderValue::value_type);
        resized.assign(refl_count, 0.0f);
        std::copy_n(value.data(), std::min(value.size(), refl_count), resized.begin());
        value_u8 = { reinterpret_cast<uint8_t*>(resized.data()), refl_size };
    } else if (refl_size != value_size) {
        compatible = false;
        value_u8   = value_u8.first(std::min(refl_size, value_u8.size()));
    }
    return value_u8;
}

static void UpdateUniform(StagingBuffer* buf, const StagingBufferRef& bufref,
                          const ShaderReflected::Block& block, std::string_view name,
                          const sr::ShaderValue& value) {
    using namespace sr;
    auto uni = block.member_map.find(name);
    if (uni == block.member_map.end()) {
        return;
    }

    const size_t                         offset    = uni->second.offset;
    const size_t                         refl_size = uni->second.size;
    bool                                 compatible {};
    std::vector<ShaderValue::value_type> resized;
    auto value_u8 = MakeUniformUploadBytes(value, refl_size, resized, compatible);
    if (! compatible) {
        rstd_warn("uniform \"{}\" size mismatch: reflected {} bytes, uploader {} bytes",
                  name,
                  refl_size,
                  value.size() * sizeof(ShaderValue::value_type));
    }
    buf->writeToBuf(bufref, value_u8, offset);
}

// Sanity-check the reflected cbuffer: members in `block.member_map` must not
// overlap. An overlap means glslang packed two members at conflicting std140
// offsets — uploader writes will clobber neighbouring slots (the failure mode
// the EmitCBufferStd140 helper was added to prevent).
static void CheckBlockOverlap(const ShaderReflected::Block& block, std::string_view shader_name) {
    struct Span {
        std::size_t      off;
        std::size_t      end;
        std::string_view name;
    };
    std::vector<Span> spans;
    spans.reserve(block.member_map.size());
    for (const auto& [n, u] : block.member_map) {
        if (u.size == 0) continue;
        spans.push_back({ u.offset, u.offset + u.size, n });
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        return a.off < b.off;
    });
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].off < spans[i - 1].end) {
            rstd_warn("cbuffer overlap in \"{}\": \"{}\"[{}..{}) overlaps \"{}\"[{}..{})",
                      shader_name,
                      spans[i - 1].name,
                      spans[i - 1].off,
                      spans[i - 1].end,
                      spans[i].name,
                      spans[i].off,
                      spans[i].end);
        }
    }
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    RenderResourceSystem resources(rr.imported_texture_provider, device);

    m_desc.vk_textures.resize(m_desc.texture_bindings.size());
    for (usize i = 0; i < m_desc.texture_bindings.size(); i++) {
        auto& binding = m_desc.texture_bindings[i];
        if (binding.empty()) continue;

        ImageSlotsRef img_slots;
        auto request = binding.request.has_value() ? binding.request
                                                   : TextureRequestFromScene(scene, binding.name);
        if (! request.has_value()) continue;
        auto opt = resources.EnsureSampledTexture(*request);
        if (! opt.has_value()) continue;
        img_slots             = std::move(*opt);
        m_desc.vk_textures[i] = img_slots;
    }
    bool                          out_force_clear { false };
    std::optional<TextureRequest> output_attachment_request;
    std::optional<TextureRequest> msaa_attachment_request;
    std::optional<TextureRequest> depth_attachment_request;
    {
        auto& tex_name = m_desc.output;
        rstd_assert(IsSpecTex(tex_name));
        rstd_assert(scene.renderTargets.count(tex_name) > 0);
        auto& rt        = scene.renderTargets.at(tex_name);
        out_force_clear = rt.force_clear;
        auto request = m_desc.output_request.value_or(MakeRenderTargetTextureRequest(tex_name, rt));
        if (auto opt = resources.EnsureTexture(request); opt.has_value()) {
            m_desc.vk_output          = opt.value();
            output_attachment_request = request;
        } else
            return;

        m_desc.samples = TextureSampleCount(rt.sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            // MSAA twin needs its own cache key — Query() resolves by name,
            // not by TextureKey content_hash.
            std::string twin_name    = MsaaTwinName(tex_name, m_desc.samples);
            auto        msaa_request = m_desc.output_msaa_request.value_or(
                MakeMsaaTextureRequest(twin_name, rt, m_desc.samples));
            if (auto opt = resources.EnsureTexture(msaa_request); opt.has_value()) {
                m_desc.vk_output_msaa   = opt.value();
                msaa_attachment_request = msaa_request;
            } else {
                rstd_error("MSAA twin Query failed for {}", tex_name);
                return;
            }
        }
    }

    SceneMesh& mesh = *(m_desc.node->Mesh());
    if (mesh.Submeshes().empty() || m_desc.submesh_index >= mesh.Submeshes().size()) return;
    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return;
    SceneMaterial& material_ref         = *slots[submesh.material_slot];
    auto&          output_rt            = scene.renderTargets.at(m_desc.output);
    const bool     has_depth_attachment = output_rt.withDepth && UsesDepthAttachment(material_ref);
    m_desc.has_depth_attachment         = has_depth_attachment;
    VkAttachmentLoadOp depthLoadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    if (has_depth_attachment) {
        depthLoadOp = m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        m_desc.depth_load_op = depthLoadOp;

        auto depth_name = m_desc.output + "::depth";
        auto depth_request =
            m_desc.depth_request.value_or(MakeDepthTextureRequest(depth_name, output_rt));
        if (auto opt = resources.EnsureTexture(depth_request); opt.has_value()) {
            m_desc.vk_depth          = opt.value();
            depth_attachment_request = depth_request;
        } else {
            return;
        }
    }

    std::vector<Uni_ShaderSpv>    spvs;
    DescriptorSetInfo             descriptor_info;
    const CachedShaderReflection* shader_reflection { nullptr };
    const ShaderReflected*        ref { nullptr };
    {
        SceneShader& shader = *(material_ref.customShader.shader);

        if (rr.shader_reflection_cache != nullptr) {
            shader_reflection = rr.shader_reflection_cache->Query(shader);
        }
        if (shader_reflection == nullptr) {
            rstd_error("gen spv reflect failed, {}", shader.name);
            return;
        }
        ref  = &shader_reflection->reflected;
        spvs = CloneShaderSpvs(*shader_reflection);
        for (const auto& blk : ref->blocks) CheckBlockOverlap(blk, shader.name);

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref->binding_map.size());

        /*
        rstd_info("----shader------");
        rstd_info("{}", shader.name);
        rstd_info("--inputs:");
        for (auto& i : ref->input_location_map) {
            rstd_info("{} {}", i.second, i.first);
        }
        rstd_info("--bindings:");
        */

        std::transform(
            ref->binding_map.begin(), ref->binding_map.end(), bindings.begin(), [](auto& item) {
                // rstd_info("{} {}", item.second.binding, item.first);
                return item.second;
            });

        m_desc.vk_tex_binding.clear();
        m_desc.vk_tex_binding.reserve(m_desc.vk_textures.size());

        for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
            i32 binding { -1 };
            if (i < WE_GLTEX_NAMES.size() && exists(ref->binding_map, WE_GLTEX_NAMES[i])) {
                binding = (i32)ref->binding_map.at(WE_GLTEX_NAMES[i]).binding;
            }
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        RenderBufferResolver buffer_resolver(device, *rr.dyn_buf);
        DrawBufferRequest    buffer_request { .render_item   = m_desc.render_item,
                                              .mesh          = &mesh,
                                              .submesh_index = m_desc.submesh_index };
        auto                 draw_buffers = buffer_resolver.prepareDrawBuffers(buffer_request);
        if (! draw_buffers) return;
        m_desc.draw_buffers = std::move(*draw_buffers);

        for (unsigned i = 0; i < submesh.vertex_arrays.size(); i++) {
            const auto& vertex    = submesh.vertex_arrays[i];
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = vertex.InstanceRate() ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                    : VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref->input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                // An instanced particle has separate corner and instance
                // streams. Bind an input location only to the stream which
                // actually owns that named attribute; assigning a missing
                // input to offset zero would duplicate locations across
                // bindings and is invalid Vulkan vertex input.
                if (! exists(attrs_map, name)) continue;
                usize offset = attrs_map[name].offset;

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
                };
                attr_descriptions.push_back(attr_desc);
            }
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend {};
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            bool writes_alpha =
                ! (m_desc.node->Camera().empty() || sstart_with(m_desc.node->Camera(), "global"));

            if (writes_alpha) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            auto blendmode = material_ref.blenmode;
            SetBlend(blendmode, color_blend);
            SetAlphaBlendWritePolicy(color_blend, writes_alpha);
            m_desc.blending = color_blend.blendEnable;

            SetAttachmentLoadOp(blendmode, loadOp);
            if (m_desc.preserve_output) loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (m_desc.clear_output) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (out_force_clear) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        }
        m_desc.color_load_op                       = loadOp;
        constexpr VkFormat      color_format       = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline_state;
        pipeline_state.toDefault();
        pipeline_state.setSampleCount(m_desc.samples);
        if (has_depth_attachment) SetDepthState(material_ref, pipeline_state.depth);
        SetCullMode(material_ref.cull_mode, pipeline_state.raster);
        const bool          has_index = m_desc.draw_buffers.hasIndex();
        VkPrimitiveTopology topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        switch (mesh.Primitive()) {
        case MeshPrimitive::POINT: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case MeshPrimitive::TRIANGLE:
            topology = has_index ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
        PipelineResourceSystem pipeline_resources(
            device, &rr.pipeline_cache, &rr.render_pass_cache);
        PipelineResourceRequest pipeline_request {
            .descriptor_sets      = { descriptor_info },
            .vertex_bindings      = std::move(bind_descriptions),
            .vertex_attrs         = std::move(attr_descriptions),
            .shader_stages        = std::move(spvs),
            .color_blend          = color_blend,
            .depth                = pipeline_state.depth,
            .raster               = pipeline_state.raster,
            .multisample          = pipeline_state.multisample,
            .topology             = topology,
            .color_format         = color_format,
            .color_final_layout   = color_final_layout,
            .color_load_op        = loadOp,
            .depth_load_op        = depthLoadOp,
            .has_depth_attachment = has_depth_attachment,
        };
        m_desc.pipeline_cache_key.reset();
        m_desc.render_pass_cache_key.reset();
        m_desc.pipeline_cache_hit               = false;
        m_desc.pipeline_cache_observed_count    = 0;
        m_desc.render_pass_cache_hit            = false;
        m_desc.render_pass_cache_observed_count = 0;
        auto pipeline_result =
            pipeline_resources.CreateGraphicsPipeline(std::move(pipeline_request));
        if (! pipeline_result.has_value()) return;
        m_desc.pipeline                         = std::move(pipeline_result->pipeline);
        m_desc.pipeline_cache_key               = pipeline_result->cache_key;
        m_desc.render_pass_cache_key            = pipeline_result->render_pass_key;
        m_desc.pipeline_cache_hit               = pipeline_result->cache_hit;
        m_desc.pipeline_cache_observed_count    = pipeline_result->cache_observed_count;
        m_desc.render_pass_cache_hit            = pipeline_result->render_pass_cache_hit;
        m_desc.render_pass_cache_observed_count = pipeline_result->render_pass_cache_observed_count;
    }

    {
        const bool has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
        if (! output_attachment_request.has_value()) return;
        if (has_msaa && ! msaa_attachment_request.has_value()) return;
        if (has_depth_attachment && ! depth_attachment_request.has_value()) return;

        std::vector<FramebufferAttachmentDesc> attachments;
        attachments.reserve((has_msaa ? 2u : 1u) + (has_depth_attachment ? 1u : 0u));
        if (has_msaa) {
            attachments.push_back(
                MakeFramebufferAttachment(*msaa_attachment_request, m_desc.vk_output_msaa));
        }
        attachments.push_back(
            MakeFramebufferAttachment(*output_attachment_request, m_desc.vk_output));
        if (has_depth_attachment) {
            attachments.push_back(
                MakeFramebufferAttachment(*depth_attachment_request, m_desc.vk_depth));
        }

        m_desc.framebuffer_cache_key.reset();
        m_desc.framebuffer_cache_hit            = false;
        m_desc.framebuffer_cache_observed_count = 0;
        FramebufferResourceSystem framebuffer_resources(
            device, &rr.framebuffer_cache, &rr.framebuffer_cache_diagnostics);
        auto framebuffer = framebuffer_resources.CreateFramebuffer(FramebufferResourceRequest {
            .render_pass     = **m_desc.pipeline->pipeline.pass,
            .render_pass_key = m_desc.render_pass_cache_key.value_or(RenderPassCacheKey {}),
            .attachments     = std::move(attachments),
            .extent          = { m_desc.vk_output.extent.width, m_desc.vk_output.extent.height },
        });
        if (! framebuffer.has_value()) return;
        m_desc.fb                               = std::move(framebuffer->framebuffer);
        m_desc.framebuffer_cache_key            = framebuffer->cache_key;
        m_desc.framebuffer_cache_hit            = framebuffer->cache_hit;
        m_desc.framebuffer_cache_observed_count = framebuffer->cache_observed_count;
    }

    if (! ref->blocks.empty()) {
        auto& block = ref->blocks.front();
        rr.dyn_buf->allocateSubRef(
            block.size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment);
    }

    if (! ref->blocks.empty()) {
        std::function<void()> update_dyn_buf_op;
        if (m_desc.draw_buffers.dynamic) {
            auto& mesh         = *m_desc.node->Mesh();
            auto  smi          = m_desc.submesh_index;
            auto  render_item  = m_desc.render_item;
            auto* resolver_buf = rr.dyn_buf;
            auto* resolver_dev = &device;
            auto& draw_buffers = m_desc.draw_buffers;
            update_dyn_buf_op =
                [&mesh, smi, render_item, &draw_buffers, resolver_dev, resolver_buf]() {
                    RenderBufferResolver resolver(*resolver_dev, *resolver_buf);
                    DrawBufferRequest    request { .render_item   = render_item,
                                                   .mesh          = &mesh,
                                                   .submesh_index = smi };
                    (void)resolver.updateDynamicDrawBuffers(request, draw_buffers);
                };
        }

        auto  block  = ref->blocks.front();
        auto* buf    = rr.dyn_buf;
        auto* bufref = &m_desc.ubo_buf;

        auto* node           = m_desc.node;
        auto* shader_updater = scene.shaderValueUpdater.get();
        auto& sprites        = m_desc.sprites_map;
        auto& vk_textures    = m_desc.vk_textures;

        m_desc.update_op = [shader_updater,
                            block,
                            buf,
                            bufref,
                            node,
                            &sprites,
                            &vk_textures,
                            update_dyn_buf_op,
                            mat = &material_ref]() {
            // Re-push constValues when the host wrote a new user property since
            // the last frame. Same-thread mutation (RenderHandler runs on the
            // render loop), so no atomic needed.
            if (mat->customShader.dirty) {
                for (auto& v : mat->customShader.constValues) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(buf, *bufref, block, v.first, v.second);
                    }
                }
                mat->customShader.dirty = false;
            }
            auto update_unf_op = [&block, buf, bufref](std::string_view name,
                                                       sr::ShaderValue value) {
                UpdateUniform(buf, *bufref, block, name, value);
            };
            shader_updater->UpdateUniforms(node, sprites, update_unf_op);
            // update image slot for sprites
            {
                for (auto& [i, sp] : sprites) {
                    if (i >= vk_textures.size()) continue;
                    vk_textures.at(i).active = sp.GetCurFrame().imageId;
                }
            }
            if (update_dyn_buf_op) update_dyn_buf_op();
        };

        auto exists_unf_op = [&block](std::string_view name) {
            return exists(block.member_map, name);
        };
        shader_updater->InitUniforms(node, exists_unf_op);

        // memset uniform buf
        buf->fillBuf(*bufref, 0, bufref->size, 0);
        {
            auto&      default_values = material_ref.customShader.shader->default_uniforms;
            auto&      const_values   = material_ref.customShader.constValues;
            std::array values_array   = { &default_values, &const_values };
            for (auto& values : values_array) {
                for (auto& v : *values) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(buf, *bufref, block, v.first, v.second);
                    }
                }
            }
            // const_values was just fully written — clear any pending re-push
            // request from a prior RenderSetUserProperty.
            material_ref.customShader.dirty = false;
        }
        m_desc.update_op();
    }

    {
        if (out_force_clear || m_desc.transparent_clear) {
            // Some offscreen RTs need a transparent reset, not the scene's
            // opaque clear color.
            m_desc.clear_value     = VkClearValue { .color = { 0.0f, 0.0f, 0.0f, 0.0f } };
            m_desc.clear_value_src = nullptr;
        } else {
            auto& sc           = scene.clearColor;
            m_desc.clear_value = VkClearValue {
                .color = { sc[0], sc[1], sc[2], 1.0f },
            };
            // Track the live scene.clearColor: per-frame re-sync in
            // execute() picks up live edits (e.g. `schemecolor` user
            // property changes) without a render-graph rebuild.
            m_desc.clear_value_src = &scene.clearColor;
        }
    }
    for (auto& tex : releaseTexs()) {
        resources.MarkShareReady(tex);
    }
    setPrepared();
}

bool CustomShaderPass::supportsRenderScope() const { return prepared(); }

bool CustomShaderPass::canJoinRenderScopeAfter(const VulkanPass& previous) const {
    const auto* prev_pass = dynamic_cast<const CustomShaderPass*>(&previous);
    if (prev_pass == nullptr) return false;
    if (! prepared() || ! prev_pass->prepared()) return false;
    if (m_desc.clear_output || m_desc.clear_depth) return false;
    if (m_desc.color_load_op != VK_ATTACHMENT_LOAD_OP_LOAD) return false;
    if (m_desc.has_depth_attachment && m_desc.depth_load_op != VK_ATTACHMENT_LOAD_OP_LOAD)
        return false;

    const auto& prev = prev_pass->m_desc;
    if (m_desc.output != prev.output) return false;
    if (m_desc.samples != prev.samples) return false;
    if (m_desc.has_depth_attachment != prev.has_depth_attachment) return false;
    if (m_desc.vk_output.handle != prev.vk_output.handle ||
        m_desc.vk_output.view != prev.vk_output.view ||
        m_desc.vk_output.extent.width != prev.vk_output.extent.width ||
        m_desc.vk_output.extent.height != prev.vk_output.extent.height) {
        return false;
    }
    if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT &&
        (m_desc.vk_output_msaa.handle != prev.vk_output_msaa.handle ||
         m_desc.vk_output_msaa.view != prev.vk_output_msaa.view)) {
        return false;
    }
    if (m_desc.has_depth_attachment && (m_desc.vk_depth.handle != prev.vk_depth.handle ||
                                        m_desc.vk_depth.view != prev.vk_depth.view)) {
        return false;
    }
    return true;
}

void CustomShaderPass::prepareFrameData(RenderingResources&) {
    if (m_desc.update_op) m_desc.update_op();

    // Re-sync clear_value from the live scene.clearColor when this pass
    // tracks the scene clear (i.e. not a per-layer transparent reset).
    // Cheap: a 3-float copy per pass per frame.
    if (m_desc.clear_value_src) {
        const auto& sc                      = *m_desc.clear_value_src;
        m_desc.clear_value.color.float32[0] = sc[0];
        m_desc.clear_value.color.float32[1] = sc[1];
        m_desc.clear_value.color.float32[2] = sc[2];
        m_desc.clear_value.color.float32[3] = 1.0f;
    }
}

void CustomShaderPass::prepareRenderScopeDraw(RenderingResources& rr) {
    recordSampledImageBarriers(rr);
}

void CustomShaderPass::recordSampledImageBarriers(RenderingResources& rr) {
    auto&                   cmd = rr.command;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,
    };
    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto&                img = slot.getActive();
        VkImageMemoryBarrier imb {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = img.handle,
            .subresourceRange = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
}

void CustomShaderPass::beginRenderScope(RenderingResources& rr) {
    auto&          cmd         = rr.command;
    auto&          outext      = m_desc.vk_output.extent;
    const bool     has_msaa    = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
    const uint32_t clear_count = (has_msaa ? 2u : 1u) + (m_desc.has_depth_attachment ? 1u : 0u);
    std::array<VkClearValue, 3> clears {};
    clears[0] = m_desc.clear_value;
    if (m_desc.has_depth_attachment) {
        const uint32_t depth_index       = has_msaa ? 2u : 1u;
        clears[depth_index].depthStencil = { 1.0f, 0 };
    }
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = **m_desc.pipeline->pipeline.pass,
        .framebuffer = **m_desc.fb,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = clear_count,
        .pClearValues    = clears.data(),
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void CustomShaderPass::recordRenderScopeDraw(RenderingResources& rr) {
    auto& cmd    = rr.command;
    auto& outext = m_desc.vk_output.extent;
    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto&                 img = slot.getActive();
        VkDescriptorImageInfo desc_img { img.sampler,
                                         img.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet  wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = (uint32_t)binding,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(
            VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline->pipeline.layout, 0, wset);
    }

    if (m_desc.ubo_buf) {
        VkDescriptorBufferInfo desc_buf {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &desc_buf,
        };
        cmd.PushDescriptorSetKHR(
            VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline->pipeline.layout, 0, wset);
    }

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline->pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };

    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    auto& draw_buffers = m_desc.draw_buffers;
    {
        const auto gpu_buf = rr.dyn_buf->gpuBuf();
        for (usize i = 0; i < draw_buffers.dynamic_vertices.size(); i++) {
            if (draw_buffers.dynamic_vertices[i]) {
                auto& buf = draw_buffers.dynamic_vertices[i];
                cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
                continue;
            }
            if (i >= draw_buffers.static_vertices.size() || ! draw_buffers.static_vertices[i])
                continue;
            auto&        mref = draw_buffers.static_vertices[i];
            VkBuffer     vb   = mref.buffer();
            VkDeviceSize off  = mref.offset();
            cmd.BindVertexBuffers((u32)i, 1, &vb, &off);
        }
    }
    if (draw_buffers.dynamic_index) {
        auto gpu_buf = rr.dyn_buf->gpuBuf();
        cmd.BindIndexBuffer(gpu_buf, draw_buffers.dynamic_index.offset, VK_INDEX_TYPE_UINT32);
    } else if (draw_buffers.static_index) {
        VkBuffer     ib  = draw_buffers.static_index.buffer();
        VkDeviceSize off = draw_buffers.static_index.offset();
        cmd.BindIndexBuffer(ib, off, VK_INDEX_TYPE_UINT32);
    }

    const bool has_index = draw_buffers.hasIndex();
    if (has_index) {
        const auto&                                    submeshes = m_desc.node->Mesh()->Submeshes();
        static const std::vector<SceneMesh::DrawRange> kEmpty;
        const auto& ranges = (m_desc.submesh_index < submeshes.size())
                                 ? submeshes[m_desc.submesh_index].draw_ranges
                                 : kEmpty;
        if (ranges.empty()) {
            cmd.DrawIndexed(draw_buffers.draw_count, draw_buffers.instance_count, 0, 0, 0);
        } else {
            // Per-part drawing — preserves the file's z-order so later parts
            // overdraw earlier ones (eyelid over pupil during blink).
            for (const auto& r : ranges) {
                cmd.DrawIndexed(r.index_count,
                                draw_buffers.instance_count,
                                r.first_index,
                                0,
                                0);
            }
        }
    } else {
        cmd.Draw(draw_buffers.draw_count, draw_buffers.instance_count, 0, 0);
    }
}

void CustomShaderPass::endRenderScope(RenderingResources& rr) { rr.command.EndRenderPass(); }

void CustomShaderPass::execute(const Device&, RenderingResources& rr) {
    prepareRenderScopeDraw(rr);
    beginRenderScope(rr);
    recordRenderScopeDraw(rr);
    endRenderScope(rr);
}

void CustomShaderPass::destory(const Device& device, RenderingResources& rr) {
    m_desc.update_op = {};
    rr.pipeline_retire_queue.Retire(std::move(m_desc.fb));
    m_desc.fb.reset();
    rr.pipeline_retire_queue.Retire(std::move(m_desc.pipeline));
    m_desc.pipeline.reset();
    m_desc.pipeline_cache_key.reset();
    m_desc.render_pass_cache_key.reset();
    m_desc.framebuffer_cache_key.reset();
    m_desc.pipeline_cache_hit               = false;
    m_desc.pipeline_cache_observed_count    = 0;
    m_desc.render_pass_cache_hit            = false;
    m_desc.render_pass_cache_observed_count = 0;
    m_desc.framebuffer_cache_hit            = false;
    m_desc.framebuffer_cache_observed_count = 0;
    RenderBufferResolver resolver(device, *rr.dyn_buf);
    resolver.releaseDynamicDrawBuffers(m_desc.draw_buffers);
    if (m_desc.ubo_buf) rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
    m_desc.ubo_buf      = {};
    m_desc.draw_buffers = {};
}

bool CustomShaderPass::setTextureBinding(uint32_t index, TextureBindingRequest binding) {
    if (index >= m_desc.texture_bindings.size()) return false;
    m_desc.texture_bindings[index] = std::move(binding);
    return true;
}
