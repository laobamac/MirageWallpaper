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

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TexNode* in, TexNode* out) {
    builder.read(in);
    builder.write(out);

    desc.src = in->key();
    desc.dst = out->key();
}
void addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode* out) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            doCopy(builder, desc, in, out);
        });
}

void addCopyPass(RenderGraph& rgraph, const TexNode::Desc& in, const TexNode::Desc& out) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            auto* in_node  = builder.createTexNode(in);
            auto* out_node = builder.createTexNode(out, true);
            doCopy(builder, desc, in_node, out_node);
        });
}

TexNode* addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode::Desc* out_desc = nullptr) {
    TexNode* copy { nullptr };
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&copy, in, out_desc](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& pdesc) {
            auto desc = out_desc == nullptr ? in->genDesc() : *out_desc;
            if (out_desc == nullptr) {
                desc.key += "_" + std::to_string(in->version()) + "_copy";
                desc.name += "_" + std::to_string(in->version()) + "_copy";
            }
            copy = builder.createTexNode(desc, true);
            doCopy(builder, pdesc, in, copy);
        });
    return copy;
}

static TexNode::Desc createTexDesc(std::string path) {
    return TexNode::Desc { .name = path,
                           .key  = path,
                           .type = IsSpecTex(path) ? TexNode::TexType::Temp
                                                   : TexNode::TexType::Imported };
}
} // namespace sr::rg

static void TraverseNode(const std::function<void(SceneNode*)>& func, SceneNode* node,
                         const Set<i32>* skip_subtree_ids = nullptr) {
    if (skip_subtree_ids != nullptr && skip_subtree_ids->count(node->ID()) != 0) return;
    func(node);
    for (auto& child : node->GetChildren()) TraverseNode(func, child.as_ptr(), skip_subtree_ids);
}

static void CheckAndSetSprite(Scene& scene, vulkan::CustomShaderPass::Desc& desc,
                              std::span<const std::string> texs) {
    for (usize i = 0; i < texs.size(); i++) {
        auto& tex = texs[i];
        if (! tex.empty() && ! IsSpecTex(tex) && scene.textures.count(tex) != 0) {
            const auto& stex = scene.textures.at(tex);
            if (stex.isSprite) {
                desc.sprites_map[i] = stex.spriteAnim;
            }
        }
    }
}

struct DelayLinkInfo {
    rg::NodeID id;
    rg::NodeID link_id;
    i32        tex_index;
};

struct ExtraInfo {
    Map<size_t, rg::TexNode*>  id_link_map {};
    std::vector<DelayLinkInfo> link_info {};
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    Set<std::string>           depth_initialized_outputs {};
    rg::TexNode*               mip_framebuffer_snapshot { nullptr };
    // Result of Pass A; non-null during Pass B. Only layer IDs in this set
    // actually have downstream link consumers, so we skip id_link_map writes
    // for non-referenced layers.
    const Set<i32>* linked_ids { nullptr };
};

static rg::TexNode* AddMipFramebufferCopy(ExtraInfo& extra, rg::RenderGraphBuilder& builder) {
    if (extra.mip_framebuffer_snapshot != nullptr) return extra.mip_framebuffer_snapshot;

    auto* source = builder.createTexNode(rg::TexNode::Desc { .name = SpecTex_Default.data(),
                                                             .key  = SpecTex_Default.data(),
                                                             .type = rg::TexNode::TexType::Temp });
    auto  copy_desc                = rg::TexNode::Desc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                                         .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                                         .type = rg::TexNode::TexType::Temp };
    extra.mip_framebuffer_snapshot = rg::addCopyPass(*extra.rgraph, source, &copy_desc);
    return extra.mip_framebuffer_snapshot;
}

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    auto loadEffect = [node, &rgraph, &scene, &extra](SceneImageEffectLayer* effs) {
        effs->ResolveEffect(scene.default_effect_mesh, "effect");

        for (usize i = 0; i < effs->EffectCount(); i++) {
            auto& eff     = effs->GetEffect(i);
            auto  cmdItor = eff->commands.begin();
            auto  cmdEnd  = eff->commands.end();
            int   nodePos = 0;
            for (auto& n : eff->nodes) {
                if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                    rg::addCopyPass(
                        rgraph, rg::createTexDesc(cmdItor->src), rg::createTexDesc(cmdItor->dst));
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
            imgeff = cam->GetImgEffect().get();
            output = imgeff->FirstTarget();
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
                pdesc.output        = std::string(pass_output);
                CheckAndSetSprite(scene, pdesc, material->textures);
                for (usize i = 0; i < material->textures.size(); i++) {
                    const auto&  url = material->textures[i];
                    rg::TexNode* input { nullptr };
                    if (url.empty()) {
                        pdesc.textures.emplace_back("");
                        continue;
                    } else if (IsSpecLinkTex(url)) {
                        auto id = ParseLinkTex(url);
                        extra.link_info.push_back(
                            DelayLinkInfo { .id = pass.ID(), .link_id = id, .tex_index = (i32)i });
                        pdesc.textures.emplace_back("");
                        continue;
                    } else {
                        rg::TexNode::Desc desc;
                        desc.key  = url;
                        desc.name = url;
                        desc.type = ! IsSpecTex(url) ? rg::TexNode::TexType::Imported
                                                     : rg::TexNode::TexType::Temp;
                        if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            input = AddMipFramebufferCopy(extra, builder);
                        } else {
                            input = builder.createTexNode(desc);
                        }
                        if (IsSpecTex(url) && ! sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            builder.markVirtualWrite(input);
                        }
                    }

                    if (url == pass_output) {
                        builder.markSelfWrite(input);
                        input = rg::addCopyPass(rgraph, input);
                    }
                    builder.read(input);
                    pdesc.textures.emplace_back(input->key());
                }

                rg::TexNode* output_node { nullptr };
                std::string  pass_output_s(pass_output);
                output_node =
                    builder.createTexNode(rg::TexNode::Desc { .name = pass_output_s,
                                                              .key  = pass_output_s,
                                                              .type = rg::TexNode::TexType::Temp },
                                          true);
                const auto& output_rt          = scene.renderTargets.at(pass_output_s);
                const bool  first_output_write = output_node->version() == 0;
                pdesc.transparent_clear = first_output_write && output_rt.clear_on_first_write;
                pdesc.clear_output =
                    (first_output_write && output_rt.bind.screen) || pdesc.transparent_clear;
                pdesc.preserve_output = output_node->version() > 0 && output_rt.preserve_on_write;
                const bool uses_depth =
                    output_rt.withDepth && vulkan::UsesDepthAttachment(*material);
                pdesc.clear_depth =
                    uses_depth && (pdesc.clear_output || output_rt.force_clear ||
                                   extra.depth_initialized_outputs.count(pass_output_s) == 0);
                if (uses_depth) {
                    extra.depth_initialized_outputs.insert(pass_output_s);
                } else if (pdesc.clear_output || output_rt.force_clear) {
                    extra.depth_initialized_outputs.erase(pass_output_s);
                }
                builder.write(output_node);
                auto record_link_source = [&](i32 id) {
                    if (extra.linked_ids == nullptr || extra.linked_ids->count(id) != 0) {
                        extra.id_link_map[(usize)id] = output_node;
                    }
                };
                if (pass_output == SpecTex_Default) {
                    record_link_source(imgId);
                } else if (IsSpecLinkTex(pass_output)) {
                    record_link_source((i32)ParseLinkTex(pass_output));
                }
            });
    }

    // load effect
    if (imgeff != nullptr) loadEffect(imgeff);
}

// Bottom-up collect: identify SceneNode subtrees whose every node is in
// `elidable_layer_ids` and not in `linked_ids` (i.e. nothing in the subtree
// needs to emit). Returns true when this subtree is fully skippable; only
// then is `node->ID()` added to `out_skip` so the emit walk can short-circuit
// at the root of the skippable subtree without descending. Does NOT mutate
// the scene tree — the tree topology is frozen after parse handoff (see the
// invariant on SceneNode in Scene.cppm).
static bool CollectEmitSkipSubtrees(SceneNode* node, Scene& scene, const Set<i32>& linked_ids,
                                    Set<i32>& out_skip) {
    bool all_children_skippable = true;
    for (auto& c : node->GetChildren()) {
        if (! CollectEmitSkipSubtrees(c.as_ptr(), scene, linked_ids, out_skip))
            all_children_skippable = false;
    }
    const i32  nid = node->ID();
    const bool self_skippable =
        scene.elidable_layer_ids.count(nid) != 0 && linked_ids.count(nid) == 0;
    if (self_skippable && all_children_skippable) {
        out_skip.insert(nid);
        return true;
    }
    return false;
}

// Walk the SceneNode subtree (plus its imgeff's effect nodes) and collect every
// WE layer id referenced as `_rt_link_<id>` by any material's texture slot.
static void CollectLinkedIds(SceneNode* node, Scene& scene, Set<i32>& out) {
    if (node == nullptr) return;
    auto inspect_material = [&](const SceneMaterial& mat) {
        for (auto& t : mat.textures) {
            if (IsSpecLinkTex(t)) out.insert((i32)ParseLinkTex(t));
        }
    };
    if (node->HasMaterial()) inspect_material(*node->Mesh()->Material());
    if (! node->Camera().empty()) {
        auto it = scene.cameras.find(node->Camera());
        if (it != scene.cameras.end() && it->second->HasImgEffect()) {
            auto& eff_layer = it->second->GetImgEffect();
            for (usize i = 0; i < eff_layer->EffectCount(); i++) {
                auto& eff = eff_layer->GetEffect(i);
                for (auto& n : eff->nodes) {
                    if (n.sceneNode->HasMaterial())
                        inspect_material(*n.sceneNode->Mesh()->Material());
                }
            }
        }
    }
    for (auto& c : node->GetChildren()) CollectLinkedIds(c.as_ptr(), scene, out);
}

std::unique_ptr<rg::RenderGraph> sr::sceneToRenderGraph(Scene& scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(), .scene = &scene };

    // Pass A: walk the scene tree (and post-process step nodes) once, collecting
    // every WE layer id that any material binds via `_rt_link_<id>`. This is the
    // delay-resolve step replacing a JSON pre-scan.
    Set<i32> linked_ids;
    CollectLinkedIds(scene.sceneGraph.as_ptr(), scene, linked_ids);
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                CollectLinkedIds(sp->node.as_ptr(), scene, linked_ids);
            }
        }
    }
    extra.linked_ids = &linked_ids;

    // Skip subtrees the parser tagged as elidable (user-hidden, or no-effect
    // identity passthrough layers) when nothing in the subtree links anything.
    // Most corpora have ~25x more elidable layers than link-referenced ones;
    // the skip set lets the emit walk short-circuit without mutating the tree.
    Set<i32> emit_skip_subtree_ids;
    CollectEmitSkipSubtrees(scene.sceneGraph.as_ptr(), scene, linked_ids, emit_skip_subtree_ids);

    // Pass B: emit passes. For elidable layers with a link consumer, route
    // into a private `_rt_link_<id>` RT instead of `_rt_default`; elidable
    // layers without a link consumer fall through and emit nothing.
    TraverseNode(
        [&extra, &scene, &linked_ids](SceneNode* node) {
            const i32  nid      = node->ID();
            const bool elidable = scene.elidable_layer_ids.count(nid) != 0;
            if (elidable) {
                if (linked_ids.count(nid) == 0) return;
                std::string link_key = GenLinkTex((idx)nid);
                if (! node->Camera().empty()) {
                    auto cit = scene.cameras.find(node->Camera());
                    if (cit != scene.cameras.end() && cit->second->HasImgEffect()) {
                        cit->second->GetImgEffect()->SetFinalTarget(link_key);
                    }
                }
                if (scene.renderTargets.count(link_key) == 0) {
                    auto sz                       = node->Size();
                    scene.renderTargets[link_key] = {
                        .width      = sz.x() > 0 ? (i32)sz.x() : scene.ortho[0],
                        .height     = sz.y() > 0 ? (i32)sz.y() : scene.ortho[1],
                        .allowReuse = false,
                    };
                }
                ToGraphPass(node, link_key, nid, extra);
            } else {
                ToGraphPass(node, SpecTex_Default, nid, extra);
            }
        },
        scene.sceneGraph.as_ptr(),
        &emit_skip_subtree_ids);

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
                rg::addCopyPass(*rgraph, rg::createTexDesc(cp->src), rg::createTexDesc(cp->dst));
            }
        }
    }

    for (auto& info : extra.link_info) {
        if (! exists(extra.id_link_map, info.link_id)) {
            rstd_error("link tex {} not found", info.link_id);
            continue;
        }
        rgraph->afterBuild(
            info.id, [&rgraph, &extra, &info](rg::RenderGraphBuilder& builder, rg::Pass& rgpass) {
                auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);

                auto* link_tex_node = extra.id_link_map.at(info.link_id);
                auto  link_key      = GenLinkTex((idx)info.link_id);
                if (link_tex_node->key() == link_key) {
                    builder.read(link_tex_node);
                    pass.setDescTex((u32)info.tex_index, link_tex_node->key());
                    return true;
                }
                auto copy_desc = link_tex_node->genDesc();
                copy_desc.key  = std::move(link_key);
                copy_desc.name = copy_desc.key;

                auto new_in = rg::addCopyPass(*rgraph, link_tex_node, &copy_desc);
                builder.read(new_in);
                pass.setDescTex((u32)info.tex_index, new_in->key());
                return true;
            });
    }

    return rgraph;
}
