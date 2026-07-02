module;

#include <cstdlib>
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

namespace
{
bool RenderDiagEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SCENERENDERER_RENDER_DIAG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}
} // namespace

CustomShaderPass::CustomShaderPass(const Desc& desc) {
    m_desc.node              = desc.node;
    m_desc.submesh_index     = desc.submesh_index;
    m_desc.textures          = desc.textures;
    m_desc.output            = desc.output;
    m_desc.sprites_map       = desc.sprites_map;
    m_desc.clear_output      = desc.clear_output;
    m_desc.transparent_clear = desc.transparent_clear;
    m_desc.clear_depth       = desc.clear_depth;
    m_desc.preserve_output   = desc.preserve_output;
};
CustomShaderPass::~CustomShaderPass() {}

std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device, VkFormat format,
                                                VkAttachmentLoadOp    loadOp,
                                                VkImageLayout         finalLayout,
                                                VkSampleCountFlagBits samples, bool has_depth,
                                                VkAttachmentLoadOp depthLoadOp) {
    const bool has_resolve = (samples != VK_SAMPLE_COUNT_1_BIT);

    // attachment[0] is the color attachment. With MSAA it's the multisample
    // twin (never sampled, finalLayout=COLOR_ATTACHMENT_OPTIMAL); without
    // MSAA it's the resolved texture itself (finalLayout=SHADER_READ).
    VkAttachmentDescription color {
        .format         = format,
        .samples        = samples,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = has_resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finalLayout,
    };

    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
        color.initialLayout = has_resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // attachment[1] is the resolve target (single-sample). loadOp DONT_CARE
    // since we always overwrite via the implicit resolve at end-of-subpass.
    VkAttachmentDescription resolve {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = finalLayout,
    };

    VkAttachmentDescription depth {
        .format         = VK_FORMAT_D32_SFLOAT,
        .samples        = samples,
        .loadOp         = depthLoadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = depthLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD
                              ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference resolve_ref {
        .attachment = 1,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depth_ref {
        .attachment = has_resolve ? 2u : 1u,
        .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    std::vector<VkAttachmentDescription> attachments;
    attachments.reserve(3);
    attachments.push_back(color);
    if (has_resolve) attachments.push_back(resolve);
    if (has_depth) attachments.push_back(depth);

    VkSubpassDescription subpass {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &color_ref,
        .pResolveAttachments     = has_resolve ? &resolve_ref : nullptr,
        .pDepthStencilAttachment = has_depth ? &depth_ref : nullptr,
    };

    std::array<VkSubpassDependency, 2> dependencies {
        VkSubpassDependency {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask =
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
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
        },
        VkSubpassDependency {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        },
    };

    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = static_cast<uint32_t>(attachments.size()),
        .pAttachments    = attachments.data(),
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = static_cast<uint32_t>(dependencies.size()),
        .pDependencies   = dependencies.data(),
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
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
    const bool diag = RenderDiagEnabled();
    m_desc.vk_textures.resize(m_desc.textures.size());
    for (usize i = 0; i < m_desc.textures.size(); i++) {
        auto& tex_name = m_desc.textures[i];
        if (tex_name.empty()) continue;

        ImageSlotsRef img_slots;
        if (IsSpecTex(tex_name)) {
            if (scene.renderTargets.count(tex_name) == 0) continue;
            auto& rt  = scene.renderTargets.at(tex_name);
            auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            if (! opt.has_value()) continue;
            img_slots.slots = { opt.value() };
        } else {
            auto image = scene.imageParser->Parse(tex_name);
            if (image) {
                img_slots = device.tex_cache().CreateTex(*image);
            } else {
                rstd_error("parse tex \"{}\" failed", tex_name);
            }
        }
        m_desc.vk_textures[i] = img_slots;
    }
    bool out_force_clear { false };
    {
        auto& tex_name = m_desc.output;
        rstd_assert(IsSpecTex(tex_name));
        rstd_assert(scene.renderTargets.count(tex_name) > 0);
        auto& rt        = scene.renderTargets.at(tex_name);
        out_force_clear = rt.force_clear;
        if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_output = opt.value();
        } else
            return;

        m_desc.samples = ToVkSampleCount(rt.sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            // MSAA twin needs its own cache key — Query() resolves by name,
            // not by TextureKey content_hash.
            std::string twin_name = MsaaTwinName(tex_name, m_desc.samples);
            if (auto opt = device.tex_cache().Query(
                    twin_name, ToTexKeyMsaa(rt, m_desc.samples), /*persist*/ true);
                opt.has_value()) {
                m_desc.vk_output_msaa = opt.value();
            } else {
                rstd_error("MSAA twin Query failed for {}", tex_name);
                return;
            }
        }
    }

    SceneMesh& mesh = *(m_desc.node->Mesh());
    if (mesh.Submeshes().empty() || m_desc.submesh_index >= mesh.Submeshes().size()) {
        if (diag) {
            rstd_info("RenderDiag pass skipped: node=\"{}\" submesh={} submeshes={}",
                      m_desc.node != nullptr ? m_desc.node->Name() : std::string_view("<null>"),
                      m_desc.submesh_index,
                      mesh.Submeshes().size());
        }
        return;
    }
    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) {
        if (diag) {
            rstd_info("RenderDiag pass skipped: node=\"{}\" submesh={} bad material_slot={}/{}",
                      m_desc.node->Name(),
                      m_desc.submesh_index,
                      submesh.material_slot,
                      slots.size());
        }
        return;
    }
    SceneMaterial& material_ref         = *slots[submesh.material_slot];
    auto&          output_rt            = scene.renderTargets.at(m_desc.output);
    const bool     has_depth_attachment = output_rt.withDepth && UsesDepthAttachment(material_ref);
    m_desc.has_depth_attachment         = has_depth_attachment;
    VkAttachmentLoadOp depthLoadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    if (has_depth_attachment) {
        depthLoadOp = m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        m_desc.depth_load_op = depthLoadOp;

        auto depth_name = m_desc.output + "::depth";
        if (auto opt = device.tex_cache().Query(
                depth_name, ToDepthTexKey(output_rt), ! output_rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_depth = opt.value();
        } else {
            return;
        }
    }

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            ref;
    {
        SceneShader& shader = *(material_ref.customShader.shader);

        if (! GenReflect(shader.codes, spvs, ref)) {
            rstd_error("gen spv reflect failed, {}", shader.name);
            return;
        }
        if (diag) {
            rstd_info("RenderDiag pass prepare: node=\"{}\" material=\"{}\" shader=\"{}\" output=\"{}\" submesh={} vertex_arrays={} index_arrays={} inputs={} bindings={} blocks={}",
                      m_desc.node->Name(),
                      material_ref.name,
                      shader.name,
                      m_desc.output,
                      m_desc.submesh_index,
                      submesh.vertex_arrays.size(),
                      submesh.index_arrays.size(),
                      ref.input_location_map.size(),
                      ref.binding_map.size(),
                      ref.blocks.size());
        }
        for (const auto& blk : ref.blocks) CheckBlockOverlap(blk, shader.name);

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref.binding_map.size());

        /*
        rstd_info("----shader------");
        rstd_info("{}", shader.name);
        rstd_info("--inputs:");
        for (auto& i : ref.input_location_map) {
            rstd_info("{} {}", i.second, i.first);
        }
        rstd_info("--bindings:");
        */

        std::transform(
            ref.binding_map.begin(), ref.binding_map.end(), bindings.begin(), [](auto& item) {
                // rstd_info("{} {}", item.second.binding, item.first);
                return item.second;
            });

        m_desc.vk_tex_binding.clear();
        m_desc.vk_tex_binding.reserve(m_desc.vk_textures.size());

        for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
            i32 binding { -1 };
            if (i < WE_GLTEX_NAMES.size() && exists(ref.binding_map, WE_GLTEX_NAMES[i])) {
                binding = (i32)ref.binding_map.at(WE_GLTEX_NAMES[i]).binding;
            }
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    m_desc.draw_count = 0;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        m_desc.dyn_vertex = mesh.Dynamic();
        m_desc.vertex_bufs.clear();
        m_desc.vertex_dyn_bufs.clear();
        if (m_desc.dyn_vertex) m_desc.vertex_dyn_bufs.resize(submesh.vertex_arrays.size());

        auto& mc = device.mesh_cache();

        for (unsigned i = 0; i < submesh.vertex_arrays.size(); i++) {
            const auto& vertex    = submesh.vertex_arrays[i];
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref.input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                const bool attr_exists = exists(attrs_map, name);
                usize      offset      = attr_exists ? attrs_map[name].offset : 0;
                if (diag) {
                    if (attr_exists) {
                        rstd_info("RenderDiag attr: material=\"{}\" binding={} input=\"{}\" location={} offset={} stride={} format={}",
                                  material_ref.name,
                                  i,
                                  name,
                                  input.location,
                                  offset,
                                  bind_desc.stride,
                                  (int)input.format);
                    } else {
                        rstd_warn("RenderDiag missing attr: material=\"{}\" binding={} input=\"{}\" location={} stride={}",
                                  material_ref.name,
                                  i,
                                  name,
                                  input.location,
                                  bind_desc.stride);
                    }
                }

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
                };
                attr_descriptions.push_back(attr_desc);
            }
            if (! m_desc.dyn_vertex) {
                auto opt = mc.QueryOrUpload(
                    { &vertex, 0 }, { (const uint8_t*)vertex.Data(), vertex.CapacitySizeOf() });
                if (! opt) return;
                m_desc.vertex_bufs.push_back(std::move(*opt));
            } else {
                auto& buf = m_desc.vertex_dyn_bufs[i];
                if (! rr.dyn_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) return;
            }
            m_desc.draw_count += (u32)(vertex.DataSize() / vertex.OneSize());
        }

        if (! submesh.index_arrays.empty()) {
            auto& indice      = submesh.index_arrays[0];
            m_desc.draw_count = (u32)indice.DataCount();
            if (! m_desc.dyn_vertex) {
                auto opt = mc.QueryOrUpload(
                    { &indice, 0 }, { (const uint8_t*)indice.Data(), indice.CapacitySizeof() });
                if (! opt) return;
                m_desc.index_buf = std::move(*opt);
            } else {
                if (! rr.dyn_buf->allocateSubRef(indice.CapacitySizeof(), m_desc.index_dyn_buf))
                    return;
            }
        }
        if (diag) {
            rstd_info("RenderDiag draw setup: material=\"{}\" dyn={} primitive={} draw_count={} has_index={} bind_descs={} attr_descs={}",
                      material_ref.name,
                      m_desc.dyn_vertex,
                      (int)mesh.Primitive(),
                      m_desc.draw_count,
                      ! submesh.index_arrays.empty(),
                      bind_descriptions.size(),
                      attr_descriptions.size());
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend {};
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            bool alpha =
                ! (m_desc.node->Camera().empty() || sstart_with(m_desc.node->Camera(), "global"));

            if (alpha) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            auto blendmode = material_ref.blenmode;
            SetBlend(blendmode, color_blend);
            m_desc.blending = color_blend.blendEnable;

            SetAttachmentLoadOp(blendmode, loadOp);
            if (m_desc.preserve_output) loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (m_desc.clear_output) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (out_force_clear) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        }
        m_desc.color_load_op = loadOp;
        auto opt             = CreateRenderPass(device.handle(),
                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                loadOp,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                m_desc.samples,
                                                has_depth_attachment,
                                                depthLoadOp);
        if (! opt.has_value()) return;
        auto& pass = opt.value();

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        const bool has_index =
            m_desc.dyn_vertex ? (bool)m_desc.index_dyn_buf : (bool)m_desc.index_buf;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        switch (mesh.Primitive()) {
        case MeshPrimitive::POINT: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case MeshPrimitive::TRIANGLE:
            topology = has_index ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setColorBlendStates(spanone { color_blend })
            .setTopology(topology)
            .addInputBindingDescription(bind_descriptions)
            .addInputAttributeDescription(attr_descriptions)
            .setSampleCount(m_desc.samples);
        if (has_depth_attachment) SetDepthState(material_ref, pipeline.depth);
        SetCullMode(material_ref.cull_mode, pipeline.raster);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        if (! pipeline.create(device, pass, m_desc.pipeline)) {
            if (diag) rstd_warn("RenderDiag pipeline create failed: material=\"{}\"", material_ref.name);
            return;
        }
    }

    {
        const bool                 has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
        std::array<VkImageView, 3> views {
            has_msaa ? m_desc.vk_output_msaa.view : m_desc.vk_output.view,
            m_desc.vk_output.view,
            m_desc.vk_depth.view,
        };
        const uint32_t depth_view_index = has_msaa ? 2u : 1u;
        if (has_depth_attachment) views[depth_view_index] = m_desc.vk_depth.view;
        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = *m_desc.pipeline.pass,
            .attachmentCount = (has_msaa ? 2u : 1u) + (has_depth_attachment ? 1u : 0u),
            .pAttachments    = views.data(),
            .width           = m_desc.vk_output.extent.width,
            .height          = m_desc.vk_output.extent.height,
            .layers          = 1,
        };
        VVK_CHECK_VOID_RE(device.handle().CreateFramebuffer(info, m_desc.fb));
    }

    if (! ref.blocks.empty()) {
        auto& block = ref.blocks.front();
        rr.dyn_buf->allocateSubRef(
            block.size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment);
    }

    if (! ref.blocks.empty()) {
        std::function<void()> update_dyn_buf_op;
        if (m_desc.dyn_vertex) {
            auto& mesh        = *m_desc.node->Mesh();
            auto  smi         = m_desc.submesh_index;
            auto* dyn_buf     = rr.dyn_buf;
            auto& vertex_bufs = m_desc.vertex_dyn_bufs;
            auto& draw_count  = m_desc.draw_count;
            auto& index_buf   = m_desc.index_dyn_buf;
            update_dyn_buf_op = [&mesh, smi, &vertex_bufs, &draw_count, &index_buf, dyn_buf]() {
                if (mesh.Dirty().exchange(false)) {
                    if (smi >= mesh.Submeshes().size()) return;
                    const auto& sm = mesh.Submeshes()[smi];
                    for (usize i = 0; i < sm.vertex_arrays.size(); i++) {
                        const auto& vertex = sm.vertex_arrays[i];
                        auto&       buf    = vertex_bufs[i];
                        if (! dyn_buf->writeToBuf(buf,
                                                  { (uint8_t*)vertex.Data(), vertex.DataSizeOf() }))
                            return;
                    }
                    if (! sm.index_arrays.empty()) {
                        const auto& indice = sm.index_arrays[0];
                        draw_count         = (u32)indice.RenderDataCount();
                        auto& buf          = index_buf;
                        if (! dyn_buf->writeToBuf(buf,
                                                  { (uint8_t*)indice.Data(), indice.DataSizeOf() }))
                            return;
                    } else if (! sm.vertex_arrays.empty()) {
                        // No index buffer (e.g. POINT_LIST for GS-driven rope):
                        // draw_count comes from the first vertex array.
                        draw_count = (u32)sm.vertex_arrays[0].VertexCount();
                    }
                }
            };
        }

        auto  block  = ref.blocks.front();
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
        device.tex_cache().MarkShareReady(tex);
    }
    setPrepared();
    if (diag) {
        rstd_info("RenderDiag pass ready: material=\"{}\" output=\"{}\" extent={}x{} samples={} color_load={} depth={} draw_count={}",
                  material_ref.name,
                  m_desc.output,
                  m_desc.vk_output.extent.width,
                  m_desc.vk_output.extent.height,
                  (int)m_desc.samples,
                  (int)m_desc.color_load_op,
                  m_desc.has_depth_attachment,
                  m_desc.draw_count);
    }
}

bool CustomShaderPass::canJoinRenderScopeAfter(const CustomShaderPass& previous) const {
    if (! prepared() || ! previous.prepared()) return false;
    if (m_desc.clear_output || m_desc.clear_depth) return false;
    if (m_desc.color_load_op != VK_ATTACHMENT_LOAD_OP_LOAD) return false;
    if (m_desc.has_depth_attachment && m_desc.depth_load_op != VK_ATTACHMENT_LOAD_OP_LOAD)
        return false;

    const auto& prev = previous.m_desc;
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

void CustomShaderPass::prepareRenderScopeDraw(RenderingResources& rr) {
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
            .srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = img.handle,
            .subresourceRange = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
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
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
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
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
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
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
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

    if (m_desc.dyn_vertex) {
        auto gpu_buf = rr.dyn_buf->gpuBuf();
        for (usize i = 0; i < m_desc.vertex_dyn_bufs.size(); i++) {
            auto& buf = m_desc.vertex_dyn_bufs[i];
            cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
        }
        if (m_desc.index_dyn_buf) {
            cmd.BindIndexBuffer(gpu_buf, m_desc.index_dyn_buf.offset, VK_INDEX_TYPE_UINT32);
        }
    } else {
        for (usize i = 0; i < m_desc.vertex_bufs.size(); i++) {
            auto&        mref = m_desc.vertex_bufs[i];
            VkBuffer     vb   = mref.buffer();
            VkDeviceSize off  = mref.offset();
            cmd.BindVertexBuffers((u32)i, 1, &vb, &off);
        }
        if (m_desc.index_buf) {
            VkBuffer     ib  = m_desc.index_buf.buffer();
            VkDeviceSize off = m_desc.index_buf.offset();
            cmd.BindIndexBuffer(ib, off, VK_INDEX_TYPE_UINT32);
        }
    }

    const bool has_index = m_desc.dyn_vertex ? (bool)m_desc.index_dyn_buf : (bool)m_desc.index_buf;
    if (RenderDiagEnabled()) {
        static uint32_t logged_draws = 0;
        if (logged_draws < 80) {
            rstd_info("RenderDiag draw: output=\"{}\" draw_count={} has_index={} dyn={} ranges={}",
                      m_desc.output,
                      m_desc.draw_count,
                      has_index,
                      m_desc.dyn_vertex,
                      has_index && m_desc.submesh_index < m_desc.node->Mesh()->Submeshes().size()
                          ? m_desc.node->Mesh()->Submeshes()[m_desc.submesh_index].draw_ranges.size()
                          : 0);
            ++logged_draws;
        }
    }
    if (has_index) {
        const auto&                                    submeshes = m_desc.node->Mesh()->Submeshes();
        static const std::vector<SceneMesh::DrawRange> kEmpty;
        const auto& ranges = (m_desc.submesh_index < submeshes.size())
                                 ? submeshes[m_desc.submesh_index].draw_ranges
                                 : kEmpty;
        if (ranges.empty()) {
            cmd.DrawIndexed(m_desc.draw_count, 1, 0, 0, 0);
        } else {
            // Per-part drawing — preserves the file's z-order so later parts
            // overdraw earlier ones (eyelid over pupil during blink).
            for (const auto& r : ranges) {
                cmd.DrawIndexed(r.index_count, 1, r.first_index, 0, 0);
            }
        }
    } else {
        cmd.Draw(m_desc.draw_count, 1, 0, 0);
    }
}

void CustomShaderPass::endRenderScope(RenderingResources& rr) { rr.command.EndRenderPass(); }

void CustomShaderPass::execute(const Device&, RenderingResources& rr) {
    prepareRenderScopeDraw(rr);
    beginRenderScope(rr);
    recordRenderScopeDraw(rr);
    endRenderScope(rr);
}

void CustomShaderPass::destory(const Device&, RenderingResources& rr) {
    m_desc.update_op = {};
    for (auto& bufref : m_desc.vertex_dyn_bufs) rr.dyn_buf->unallocateSubRef(bufref);
    if (m_desc.index_dyn_buf) rr.dyn_buf->unallocateSubRef(m_desc.index_dyn_buf);
    rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
    // Static MeshBufferRef values dec their refcount via destructor when
    // m_desc.vertex_bufs / m_desc.index_buf go out of scope; clear here to
    // release the slot eagerly so a follow-up evictUnused can free.
    m_desc.vertex_bufs.clear();
    m_desc.index_buf = {};
}

void CustomShaderPass::setDescTex(u32 index, std::string_view tex_key) {
    rstd_assert(index < m_desc.textures.size());
    if (index >= m_desc.textures.size()) return;
    m_desc.textures[index] = tex_key;
}
