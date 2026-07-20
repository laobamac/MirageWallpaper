module;

#if defined(__linux__)
#include <string>
#endif

#include <rstd/macro.hpp>

module sr.scene;
import sr.spec_texs;
import sr.core;
import sr.types;
import rstd.cppstd;

using namespace sr;

namespace
{

void ChangeMeshToUnitQuad(SceneMesh& target) {
    SceneMesh mesh;
    // clang-format off
    const std::array pos = {
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
    };
    const std::array tex_coord = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
    };
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), 4);
    vertex.SetVertex(WE_IN_POSITION, pos);
    vertex.SetVertex(WE_IN_TEXCOORD, tex_coord);
    mesh.AddVertexArray(std::move(vertex));
    target.ChangeMeshDataFrom(mesh);
}

} // namespace

SceneImageEffectLayer::SceneImageEffectLayer(SceneNode* node, float w, float h,
                                             std::string_view pingpong_a,
                                             std::string_view pingpong_b)
    : m_worldNode(node),
      m_width(w),
      m_height(h),
      m_pingpong_a(pingpong_a),
      m_pingpong_b(pingpong_b),
      m_final_mesh(std::make_unique<SceneMesh>()) {};

void SceneImageEffectLayer::ResolveEffect(const SceneMesh& default_mesh,
                                          std::string_view effect_cam) {
    if (m_resolved) return;
    m_resolved_effects.clear();
    std::string_view ppong_a = m_pingpong_a, ppong_b = m_pingpong_b;
    auto             swap_pp = [&ppong_a, &ppong_b]() {
        std::swap(ppong_a, ppong_b);
    };
    auto default_node = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    auto                  resolve_effect = [&](SceneImageEffect& eff) {
        for (auto& cmd : eff.commands) {
            auto state_it = m_command_resolve_state
                                .try_emplace(&cmd,
                                             EffectCommandResolveState {
                                                 .src = cmd.src,
                                                 .dst = cmd.dst,
                                             })
                                .first;
            cmd.src       = state_it->second.src;
            cmd.dst       = state_it->second.dst;
            if (sstart_with(cmd.src, SR_EFFECT_PPONG_PREFIX_A)) cmd.src = ppong_a;

            if (sstart_with(cmd.dst, SR_EFFECT_PPONG_PREFIX_A)) cmd.dst = ppong_a;
        }
        for (auto it = eff.nodes.begin(); it != eff.nodes.end(); it++) {
            rstd_assert(it->sceneNode->HasMaterial());
            auto& material            = *(it->sceneNode->Mesh()->Material());
            auto [state_it, inserted] = m_node_resolve_state.try_emplace(
                &(*it), EffectNodeResolveState { .output = it->output });
            auto& state = state_it->second;
            if (inserted) {
                for (usize i = 0; i < material.textures.size(); ++i) {
                    if (sstart_with(material.textures[i], SR_EFFECT_PPONG_PREFIX_A))
                        state.pingpong_input_slots.push_back(i);
                }
            }
            it->output = state.output;
            for (usize slot : state.pingpong_input_slots) {
                if (slot < material.textures.size()) material.textures[slot] = ppong_a;
            }
            
            if (sstart_with(it->output, SR_EFFECT_PPONG_PREFIX_B) ||
                it->output == SpecTex_Default) {
                it->output  = ppong_b;
                last_output = &(*it);
            }

            {
                material.blenmode = BlendMode::Normal;
                it->sceneNode->SetCamera(effect_cam.data());
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }
        }
        m_resolved_effects.push_back(&eff);
        swap_pp();
    };
    for (auto& eff : m_effects) {
        if (eff && eff->runtime_visible) resolve_effect(*eff);
    }
    if (m_final_resolve_effect) resolve_effect(*m_final_resolve_effect);
    if (last_output != nullptr) {
        last_output->output  = m_final_target;
        auto& mesh           = *(last_output->sceneNode->Mesh());
        auto& material       = *mesh.Material();
        material.blenmode    = m_final_blend;
        material.depth_test  = m_final_depth_test;
        material.depth_write = m_final_depth_write;
        material.cull_mode   = m_final_cull_mode;
        if (m_final_local) {
            last_output->sceneNode->SetCamera(std::string(effect_cam));
            last_output->sceneNode->SetParentAnchor(nullptr);
            last_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else if (fullscreen) {
            last_output->sceneNode->SetCamera(std::string(effect_cam));
            last_output->sceneNode->SetParentAnchor(nullptr);
            last_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else {
            const bool perspective = m_worldNode != nullptr && m_worldNode->Perspective();
            last_output->sceneNode->SetCamera(m_final_camera.empty()
                                                  ? (perspective ? "global_perspective" : "")
                                                  : m_final_camera);
            last_output->sceneNode->SetPerspective(perspective);
            // Anchor to the layer's primary SceneNode so the composite quad
            // inherits the layer's world transform (including any container
            // parent chain) via ModelTrans. Identity local — no CopyTrans dance.
            last_output->sceneNode->SetParentAnchor(m_worldNode);
            if (last_output->uses_unit_final_quad) {
                last_output->sceneNode->SetTranslate({ -m_width * 0.5f, -m_height * 0.5f, 0.0f });
                last_output->sceneNode->SetScale({ m_width, m_height, 1.0f });
                ChangeMeshToUnitQuad(mesh);

            } else {
                mesh.ChangeMeshDataFrom(*m_final_mesh);
            }
            for (const auto& [name, value] : last_output->final_quad_shader_values) {
                material.SetShaderValue(name, value.base);
                if (value.curve && ! value.curve->Empty()) {
                    material.customShader.valueAnimations[name] = value;
                } else {
                    material.customShader.valueAnimations.erase(name);
                }
            }
        }
        last_output->sceneNode->SetAlphaSource(m_worldNode);
    }
    m_resolved = true;
}
