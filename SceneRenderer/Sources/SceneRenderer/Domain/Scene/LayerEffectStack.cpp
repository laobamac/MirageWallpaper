module;

#include <rstd/macro.hpp>

module sr.scene;
import sr.spec_texs;
import sr.core;
import sr.types;
import rstd.cppstd;

using namespace sr;

SceneImageEffectLayer::SceneImageEffectLayer(SceneNode* node, float w, float h,
                                             std::string_view pingpong_a,
                                             std::string_view pingpong_b)
    : m_worldNode(node),
      m_pingpong_a(pingpong_a),
      m_pingpong_b(pingpong_b),
      m_final_mesh(std::make_unique<SceneMesh>()) {};

void SceneImageEffectLayer::ResolveEffect(const SceneMesh& default_mesh,
                                          std::string_view effect_cam) {
    if (m_resolved) return;
    std::string_view ppong_a = m_pingpong_a, ppong_b = m_pingpong_b;
    auto             swap_pp = [&ppong_a, &ppong_b]() {
        std::swap(ppong_a, ppong_b);
    };
    auto default_node = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    for (auto& eff : m_effects) {
        for (auto& cmd : eff->commands) {
            if (sstart_with(cmd.src, SR_EFFECT_PPONG_PREFIX_A)) cmd.src = ppong_a;

            if (sstart_with(cmd.dst, SR_EFFECT_PPONG_PREFIX_A)) cmd.dst = ppong_a;
        }
        for (auto it = eff->nodes.begin(); it != eff->nodes.end(); it++) {
            if (sstart_with(it->output, SR_EFFECT_PPONG_PREFIX_B) ||
                it->output == SpecTex_Default) {
                it->output  = ppong_b;
                last_output = &(*it);
            }

            rstd_assert(it->sceneNode->HasMaterial());

            auto& material = *(it->sceneNode->Mesh()->Material());
            {
                material.blenmode = BlendMode::Normal;
                it->sceneNode->SetCamera(effect_cam.data());
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }

            auto& texs = material.textures;
            std::replace_if(
                texs.begin(),
                texs.end(),
                [](auto& t) {
                    return sstart_with(t, SR_EFFECT_PPONG_PREFIX_A);
                },
                ppong_a);
        }
        swap_pp();
    }
    if (last_output != nullptr) {
        last_output->output  = m_final_target;
        auto& mesh           = *(last_output->sceneNode->Mesh());
        auto& material       = *mesh.Material();
        material.blenmode    = m_final_blend;
        material.depth_test  = m_final_depth_test;
        material.depth_write = m_final_depth_write;
        material.cull_mode   = m_final_cull_mode;
        if (fullscreen) {
            last_output->sceneNode->SetCamera(std::string(effect_cam));
            last_output->sceneNode->SetParentAnchor(nullptr);
            last_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else {
            const bool perspective = m_worldNode != nullptr && m_worldNode->Perspective();
            last_output->sceneNode->SetCamera(perspective ? "global_perspective" : "");
            last_output->sceneNode->SetPerspective(perspective);
            // Anchor to the layer's primary SceneNode so the composite quad
            // inherits the layer's world transform (including any container
            // parent chain) via ModelTrans. Identity local — no CopyTrans dance.
            last_output->sceneNode->SetParentAnchor(m_worldNode);
            mesh.ChangeMeshDataFrom(*m_final_mesh);
        }
        last_output->sceneNode->SetAlphaSource(m_worldNode);
    }
    m_resolved = true;
}
