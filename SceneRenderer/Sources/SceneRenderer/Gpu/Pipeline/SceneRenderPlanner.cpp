module;

#include <rstd/macro.hpp>
#include "FrameGraph/Pass.hpp"

module sr.vulkan_render;
import sr.spec_texs;
import sr.core;
import rstd.log;
import rstd.cppstd;
import eigen;
import sr.vulkan;
import sr.scene;

import sr.rgraph;

using namespace sr;
namespace sr::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TextureNodeRef in,
            TextureNodeRef out) {
    builder.read(in);
    builder.write(out);

    auto in_state  = builder.textureState(in);
    auto out_state = builder.textureState(out);
    rstd_assert(in_state.has_value() && out_state.has_value());
    if (! in_state.has_value() || ! out_state.has_value()) return;
    desc.src = in_state->desc.key;
    desc.dst = out_state->desc.key;
}
} // namespace sr::rg

static void TraverseNode(const std::function<void(SceneNode*)>& func, SceneNode* node,
                         const Set<const SceneNode*>* skip_subtrees = nullptr) {
    if (skip_subtrees != nullptr && skip_subtrees->count(static_cast<const SceneNode*>(node)) != 0)
        return;
    func(node);
    for (auto& child : node->GetChildren()) TraverseNode(func, child.as_ptr(), skip_subtrees);
}

static void CheckAndSetSprite(const RenderSceneSnapshot&      render_scene,
                              vulkan::CustomShaderPass::Desc& desc,
                              std::span<const std::string>    texs) {
    for (usize i = 0; i < texs.size(); i++) {
        auto& tex = texs[i];
        if (! tex.empty() && ! IsSpecTex(tex)) {
            if (auto tex_id = render_scene.textureDescId(tex)) {
                const auto* stex = render_scene.textureDesc(*tex_id);
                if (stex != nullptr && stex->desc.isSprite) {
                    desc.sprites_map[i] = stex->desc.spriteAnim;
                }
            }
        }
    }
}

struct ExtraInfo;

struct LinkTextureConsumer {
    rg::NodeID       pass_id;
    WallpaperLayerId source_layer;
    uint32_t         texture_index;
};

static rg::TextureDesc MakeTextureDesc(std::string_view key) {
    return rg::TextureDesc {
        .name = std::string(key),
        .key  = std::string(key),
        .kind = IsSpecTex(key) ? rg::TextureKind::Temp : rg::TextureKind::Imported,
    };
}

struct GraphTextureOutput {
    rg::TextureNodeRef            ref;
    vulkan::TextureBindingRequest binding;
    rg::TextureDesc               desc;
};

class GraphLinkFinalizer {
public:
    void setLinkedLayerIds(const Set<i32>* linked_ids) { m_linked_ids = linked_ids; }
    void recordSource(WallpaperLayerId source_layer, GraphTextureOutput output) {
        if (m_linked_ids == nullptr || m_linked_ids->count(source_layer.value) != 0) {
            m_source_outputs[source_layer.value] = std::move(output);
        }
    }
    void addConsumer(rg::NodeID pass_id, WallpaperLayerId source_layer, uint32_t texture_index) {
        m_consumers.push_back(LinkTextureConsumer {
            .pass_id       = pass_id,
            .source_layer  = source_layer,
            .texture_index = texture_index,
        });
    }
    void apply(ExtraInfo& extra);

private:
    const Set<i32>*                  m_linked_ids { nullptr };
    Map<i32, GraphTextureOutput>     m_source_outputs;
    std::vector<LinkTextureConsumer> m_consumers;
};

struct ExtraInfo {
    rg::RenderGraph*                  rgraph { nullptr };
    Scene*                            scene { nullptr };
    Set<std::string>                  depth_initialized_outputs {};
    std::optional<rg::TextureNodeRef> mip_framebuffer_snapshot;
    const RenderSceneSnapshot*        render_scene { nullptr };
    GraphLinkFinalizer                link_finalizer;
};

static std::optional<vulkan::TextureRequest> BuildGraphTextureRequest(ExtraInfo&       extra,
                                                                      std::string_view key) {
    if (key.empty()) return std::nullopt;
    if (! IsSpecTex(key)) {
        std::optional<RenderTextureDescId> texture;
        if (extra.render_scene != nullptr) texture = extra.render_scene->textureDescId(key);
        return vulkan::MakeImportedTextureRequest(key, texture);
    }

    if (extra.render_scene != nullptr) {
        if (auto desc_id = extra.render_scene->renderTargetDescId(key)) {
            if (auto* desc = extra.render_scene->renderTargetDesc(*desc_id)) {
                return vulkan::MakeRenderTargetTextureRequest(key, desc->desc);
            }
        }
    }

    if (extra.scene != nullptr) {
        auto it = extra.scene->renderTargets.find(std::string(key));
        if (it != extra.scene->renderTargets.end()) {
            return vulkan::MakeRenderTargetTextureRequest(key, it->second);
        }
    }

    return std::nullopt;
}

static void FillCopyTextureRequests(ExtraInfo& extra, vulkan::CopyPass::Desc& desc) {
    desc.src_request = BuildGraphTextureRequest(extra, desc.src);
    desc.dst_request = BuildGraphTextureRequest(extra, desc.dst);
}

static GraphTextureOutput CaptureTextureOutput(ExtraInfo& extra, rg::TextureNodeRef ref) {
    auto state = extra.rgraph->textureState(ref);
    rstd_assert(state.has_value());
    if (! state.has_value()) return {};
    return GraphTextureOutput {
        .ref = ref,
        .binding =
            vulkan::TextureBindingRequest {
                .name    = state->desc.key,
                .request = BuildGraphTextureRequest(extra, state->desc.key),
            },
        .desc = state->desc,
    };
}

static void AddCopyPass(ExtraInfo& extra, rg::TextureDesc in, rg::TextureDesc out) {
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy",
        rg::PassNode::Type::Copy,
        [in = std::move(in), out = std::move(out), &extra](rg::RenderGraphBuilder& builder,
                                                           vulkan::CopyPass::Desc& desc) {
            auto in_node  = builder.createTexture(in);
            auto out_node = builder.createTexture(out, true);
            rg::doCopy(builder, desc, in_node, out_node);
            FillCopyTextureRequests(extra, desc);
        });
}

static rg::TextureNodeRef AddCopyPass(ExtraInfo& extra, rg::TextureNodeRef in,
                                      std::optional<rg::TextureDesc> out_desc = std::nullopt) {
    rg::TextureNodeRef copy {};
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy",
        rg::PassNode::Type::Copy,
        [&copy, in, out_desc = std::move(out_desc), &extra](rg::RenderGraphBuilder& builder,
                                                            vulkan::CopyPass::Desc& pdesc) {
            auto state = builder.textureState(in);
            rstd_assert(state.has_value());
            if (! state.has_value()) return;
            auto desc = out_desc.value_or(state->desc);
            if (! out_desc.has_value()) {
                desc.key += "_" + std::to_string(state->version) + "_copy";
                desc.name += "_" + std::to_string(state->version) + "_copy";
            }
            copy = builder.createTexture(desc, true);
            rg::doCopy(builder, pdesc, in, copy);
            FillCopyTextureRequests(extra, pdesc);
        });
    return copy;
}

void GraphLinkFinalizer::apply(ExtraInfo& extra) {
    for (auto& consumer : m_consumers) {
        auto output_it = m_source_outputs.find(consumer.source_layer.value);
        if (output_it == m_source_outputs.end()) {
            rstd_error("link tex {} not found", consumer.source_layer.value);
            continue;
        }

        auto* rgpass = extra.rgraph->getPass(consumer.pass_id);
        if (rgpass == nullptr) {
            rstd_error("link tex {} pass not found", consumer.source_layer.value);
            continue;
        }
        auto& pass = static_cast<vulkan::VulkanPass&>(*rgpass);

        GraphTextureOutput input = output_it->second;
        auto link_key = GenLinkTex(static_cast<std::ptrdiff_t>(consumer.source_layer.value));
        if (input.binding.name.compare(link_key) != 0) {
            auto copy_desc        = input.desc;
            copy_desc.key         = std::move(link_key);
            copy_desc.name        = copy_desc.key;
            input.ref             = AddCopyPass(extra, input.ref, copy_desc);
            input.binding.name    = copy_desc.key;
            input.desc            = std::move(copy_desc);
            input.binding.request = BuildGraphTextureRequest(extra, input.binding.name);
        }

        if (! extra.rgraph->readTexture(consumer.pass_id, input.ref)) {
            rstd_error("link tex {} read failed", consumer.source_layer.value);
            continue;
        }
        if (! pass.setTextureBinding(consumer.texture_index, std::move(input.binding))) {
            rstd_error("link tex {} binding failed", consumer.source_layer.value);
        }
    }
}

static rg::TextureNodeRef AddMipFramebufferCopy(ExtraInfo& extra, rg::RenderGraphBuilder& builder) {
    if (extra.mip_framebuffer_snapshot) {
        return *extra.mip_framebuffer_snapshot;
    }

    auto source                    = builder.createTexture(MakeTextureDesc(SpecTex_Default));
    auto copy_desc                 = rg::TextureDesc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                                       .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                                       .kind = rg::TextureKind::Temp };
    auto snapshot                  = AddCopyPass(extra, source, copy_desc);
    extra.mip_framebuffer_snapshot = snapshot;
    return snapshot;
}

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    auto loadEffect = [node, &rgraph, &scene, &extra](SceneImageEffectLayer* effs) {
        effs->ResolveEffect(scene.default_effect_mesh, "effect");

        for (usize i = 0; i < effs->EffectCount(); i++) {
            auto& eff = effs->GetEffect(i);
            if (! eff || ! eff->runtime_visible) continue;
            auto cmdItor = eff->commands.begin();
            auto cmdEnd  = eff->commands.end();
            int  nodePos = 0;
            for (auto& n : eff->nodes) {
                if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                    AddCopyPass(
                        extra, MakeTextureDesc(cmdItor->src), MakeTextureDesc(cmdItor->dst));
                    cmdItor++;
                }
                auto& name = n.output;
                ToGraphPass(n.sceneNode.as_ptr(), name, node->ID(), extra);
                nodePos++;
            }
        }
    };

    if (node->Mesh() == nullptr) return;
    auto* mesh = node->Mesh();
    if (mesh->Submeshes().empty()) return;
    const auto& slots = mesh->MaterialSlots();

    SceneImageEffectLayer* imgeff = nullptr;
    if (! node->Camera().empty()) {
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            auto* effect = cam->GetImgEffect().get();
            // A layer with a final resolve (notably text with an optional,
            // currently-disabled effect) still needs its private target. If
            // it is rendered directly to _rt_default while retaining the
            // private ortho camera, the text's local pixel coordinates are
            // projected over the whole scene.
            if (effect->RequiresIntermediateTarget()) {
                imgeff = effect;
                output = imgeff->FirstTarget();
            }
        }
    }
    if (imgeff != nullptr) {
        for (auto& prefill : imgeff->PrefillNodes()) {
            std::string_view prefill_output =
                prefill.output.empty() ? output : std::string_view(prefill.output);
            ToGraphPass(prefill.sceneNode.as_ptr(), prefill_output, node->ID(), extra);
        }
    }

    for (uint32_t smi = 0; smi < mesh->Submeshes().size(); smi++) {
        const auto& submesh = mesh->Submeshes()[smi];
        if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) continue;
        SceneMaterial* material = slots[submesh.material_slot].get();
        std::string    passName = material->name;
        // Per-submesh output override (clipping-mask submeshes write into a
        // shared RT that the main puppet pass samples via g_Texture8).
        std::string_view pass_output =
            submesh.output_override.empty() ? output : std::string_view(submesh.output_override);

        rgraph.addPass<vulkan::CustomShaderPass>(
            passName,
            rg::PassNode::Type::CustomShader,
            [material, node, smi, pass_output, &output, &imgId, &rgraph, &scene, &extra](
                rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
                const auto& pass    = builder.workPassNode();
                pdesc.node          = node;
                pdesc.submesh_index = smi;
                if (auto node_id = scene.ResourceIndex().nodeId(*node)) {
                    if (auto draw_item = scene.ResourceIndex().drawItemFor(*node_id, smi)) {
                        pdesc.draw_item = *draw_item;
                        if (extra.render_scene != nullptr) {
                            if (auto render_item = extra.render_scene->renderItemFor(*draw_item)) {
                                pdesc.render_item = *render_item;
                            }
                        }
                    }
                }
                pdesc.output = std::string(pass_output);
                if (extra.render_scene != nullptr) {
                    CheckAndSetSprite(*extra.render_scene, pdesc, material->textures);
                }
                for (usize i = 0; i < material->textures.size(); i++) {
                    const auto&                       url = material->textures[i];
                    std::optional<rg::TextureNodeRef> input;
                    if (url.empty()) {
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    } else if (IsSpecLinkTex(url)) {
                        auto id = ParseLinkTex(url);
                        extra.link_finalizer.addConsumer(
                            pass.ID(),
                            WallpaperLayerId { .value = static_cast<i32>(id) },
                            static_cast<uint32_t>(i));
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    } else {
                        auto desc = MakeTextureDesc(url);
                        if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            input = AddMipFramebufferCopy(extra, builder);
                        } else {
                            input = builder.createTexture(desc);
                        }
                        if (IsSpecTex(url) && ! sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            builder.markVirtualWrite(*input);
                        }
                    }

                    if (url.compare(pass_output) == 0) {
                        builder.markSelfWrite(*input);
                        input = AddCopyPass(extra, *input);
                    }
                    builder.read(*input);
                    auto sampled_state = builder.textureState(*input);
                    rstd_assert(sampled_state.has_value());
                    if (! sampled_state.has_value()) {
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    }
                    auto sampled_key = sampled_state->desc.key;
                    pdesc.texture_bindings.emplace_back(vulkan::TextureBindingRequest {
                        .name    = sampled_key,
                        .request = BuildGraphTextureRequest(extra, sampled_key),
                    });
                }

                std::string pass_output_s(pass_output);
                auto output_node  = builder.createTexture(MakeTextureDesc(pass_output_s), true);
                auto output_state = builder.textureState(output_node);
                rstd_assert(output_state.has_value());
                if (! output_state.has_value()) return;
                const auto& output_rt          = scene.renderTargets.at(pass_output_s);
                const bool  first_output_write = output_state->version == 0;
                pdesc.output_request           = BuildGraphTextureRequest(extra, pass_output_s);
                pdesc.samples                  = vulkan::TextureSampleCount(output_rt.sample_count);
                if (pdesc.samples != VK_SAMPLE_COUNT_1_BIT) {
                    auto twin_name = vulkan::MsaaTwinName(pass_output_s, pdesc.samples);
                    pdesc.output_msaa_request =
                        vulkan::MakeMsaaTextureRequest(twin_name, output_rt, pdesc.samples);
                }
                pdesc.transparent_clear = first_output_write && output_rt.clear_on_first_write;
                pdesc.clear_output =
                    (first_output_write && output_rt.bind.screen) || pdesc.transparent_clear;
                pdesc.preserve_output = output_state->version > 0 && output_rt.preserve_on_write;
                const bool uses_depth =
                    output_rt.withDepth && vulkan::UsesDepthAttachment(*material);
                pdesc.has_depth_attachment = uses_depth;
                if (uses_depth) {
                    pdesc.depth_request =
                        vulkan::MakeDepthTextureRequest(pass_output_s + "::depth", output_rt);
                }
                pdesc.clear_depth =
                    uses_depth && (pdesc.clear_output || output_rt.force_clear ||
                                   extra.depth_initialized_outputs.count(pass_output_s) == 0);
                if (uses_depth) {
                    extra.depth_initialized_outputs.insert(pass_output_s);
                } else if (pdesc.clear_output || output_rt.force_clear) {
                    extra.depth_initialized_outputs.erase(pass_output_s);
                }
                builder.write(output_node);
                if (pass_output.compare(SpecTex_Default) == 0) {
                    extra.link_finalizer.recordSource(WallpaperLayerId { .value = imgId },
                                                      CaptureTextureOutput(extra, output_node));
                } else if (IsSpecLinkTex(pass_output)) {
                    extra.link_finalizer.recordSource(
                        WallpaperLayerId { .value = static_cast<i32>(ParseLinkTex(pass_output)) },
                        CaptureTextureOutput(extra, output_node));
                }
            });
    }

    if (imgeff != nullptr && imgeff->HasRenderEffects()) loadEffect(imgeff);
}

// Bottom-up collect: identify SceneNode subtrees whose every node can be
// elided without losing a link source. Visibility-hidden ancestors also hide
// anonymous/generated descendants such as particle children, so the skip set
// is keyed by node pointer instead of WE layer id.
static bool CollectEmitSkipSubtrees(SceneNode* node, Scene& scene, const Set<i32>& linked_ids,
                                    Set<const SceneNode*>& out_skip,
                                    bool                   visibility_hidden_ancestor = false) {
    const i32  nid    = node->ID();
    const bool linked = nid >= 0 && linked_ids.count(nid) != 0;
    const bool visibility_hidden_self =
        nid >= 0 && scene.visibility_elidable_layer_ids.count(nid) != 0 && ! linked;
    const bool visibility_hidden = visibility_hidden_ancestor || visibility_hidden_self;

    bool all_children_skippable = true;
    for (auto& c : node->GetChildren()) {
        if (! CollectEmitSkipSubtrees(c.as_ptr(), scene, linked_ids, out_skip, visibility_hidden))
            all_children_skippable = false;
    }
    const bool self_skippable =
        ! linked && (visibility_hidden || (nid >= 0 && scene.elidable_layer_ids.count(nid) != 0));
    if (self_skippable && all_children_skippable) {
        out_skip.insert(node);
        return true;
    }
    return false;
}

static bool ShouldSkipNoRuntimeEffect(SceneNode* node, Scene& scene) {
    if (node == nullptr || node->Camera().empty()) return false;
    auto camera_it = scene.cameras.find(node->Camera());
    if (camera_it == scene.cameras.end() || ! camera_it->second->HasImgEffect()) return false;
    const auto& effect_layer = camera_it->second->GetImgEffect();
    return effect_layer && effect_layer->SkipWhenNoRuntimeEffect() &&
           effect_layer->EffectCount() > 0 && ! effect_layer->HasRuntimeVisibleEffect();
}

std::unique_ptr<rg::RenderGraph> sr::sceneToRenderGraph(Scene&                     scene,
                                                         const RenderSceneSnapshot& render_scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo extra { .rgraph = rgraph.get(), .scene = &scene, .render_scene = &render_scene };

    // The snapshot owns link-consumer discovery; graph build only consumes the
    // resulting source ids.
    const auto& linked_ids = render_scene.LinkedLayerIds();
    extra.link_finalizer.setLinkedLayerIds(&linked_ids);

    // Skip subtrees the parser tagged as elidable (user-hidden, or no-effect
    // identity passthrough layers) when nothing in the subtree links anything.
    // Most corpora have ~25x more elidable layers than link-referenced ones;
    // the skip set lets the emit walk short-circuit without mutating the tree.
    Set<const SceneNode*> emit_skip_subtrees;
    CollectEmitSkipSubtrees(scene.sceneGraph.as_ptr(), scene, linked_ids, emit_skip_subtrees);

    // Pass B: emit passes. For elidable layers with a link consumer, route
    // into a private `_rt_link_<id>` RT instead of `_rt_default`; elidable
    // layers without a link consumer fall through and emit nothing.
    TraverseNode(
        [&extra, &scene, &linked_ids](SceneNode* node) {
            const i32  nid      = node->ID();
            const bool elidable = scene.elidable_layer_ids.count(nid) != 0;
            const bool linked   = linked_ids.count(nid) != 0;
            if (! linked && ShouldSkipNoRuntimeEffect(node, scene)) return;
            if (elidable) {
                if (! linked) return;
                auto* link_source =
                    extra.render_scene->linkSource(WallpaperLayerId { .value = nid });
                if (link_source == nullptr) {
                    rstd_error("link render target for layer {} not found in snapshot", nid);
                    return;
                }
                std::string link_key = link_source->render_target_key;
                if (! node->Camera().empty()) {
                    auto cit = scene.cameras.find(node->Camera());
                    if (cit != scene.cameras.end() && cit->second->HasImgEffect()) {
                        cit->second->GetImgEffect()->SetFinalTarget(link_key);
                        cit->second->GetImgEffect()->SetFinalLocal(true);
                    }
                }
                ToGraphPass(node, link_key, nid, extra);
            } else {
                ToGraphPass(node, SpecTex_Default, nid, extra);
            }
        },
        scene.sceneGraph.as_ptr(),
        &emit_skip_subtrees);

    // Emit global post-process passes after the main scene-graph traversal.
    // Each step is either a CustomShaderPass (built on the synthetic node's
    // mesh+material) or a CopyPass (RT-to-RT blit).
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                std::string_view target =
                    sp->output.empty() ? SpecTex_Default : std::string_view(sp->output);
                ToGraphPass(sp->node.as_ptr(), target, sp->node->ID(), extra);
            } else if (auto* cp = std::get_if<ScenePostProcessCopy>(&step)) {
                AddCopyPass(extra, MakeTextureDesc(cp->src), MakeTextureDesc(cp->dst));
            }
        }
    }

    extra.link_finalizer.apply(extra);

    scene.RebuildResourceIndex();
    return rgraph;
}

std::unique_ptr<rg::RenderGraph> sr::sceneToRenderGraph(Scene& scene) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    return sceneToRenderGraph(scene, render_scene);
}
