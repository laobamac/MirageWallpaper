module;

#include <rstd/macro.hpp>

module sr.scene_uniform_updater;
import eigen;
import sr.spec_texs;
import sr.core;
import rstd.cppstd;
import sr.utils;
import sr.scene;

using namespace sr;
using namespace Eigen;

namespace
{

template<std::size_t N>
void AverageResample64(std::span<const float, 64> bins, std::array<float, N>& out) {
    static_assert(64 % N == 0);
    constexpr std::size_t ratio = 64 / N;
    for (std::size_t i = 0; i < N; ++i) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < ratio; ++k) {
            sum += std::max(0.0f, bins[i * ratio + k]);
        }
        out[i] = sum / static_cast<float>(ratio);
    }
}

float Smooth(float value) { return value * value * (3.0f - 2.0f * value); }

Vector2f ShakeOffset(float x, float roughness) {
    const float r    = std::clamp(roughness, 0.0f, 2.0f);
    const float over = std::clamp(r - 1.0f, 0.0f, 1.0f);
    const float grow = over * over;

    constexpr float kPi      = static_cast<float>(rstd::f64_::consts::PI);
    const float     beat_pos = std::max(0.0f, x) / (kPi * 0.5f);
    const int       beat     = static_cast<int>(std::floor(beat_pos));
    const float     local    = beat_pos - static_cast<float>(beat);
    const float     u        = Smooth(local);

    static constexpr std::array<std::array<float, 2>, 8> kDirections { {
        { -1.0f, 1.0f },
        { 1.0f, -1.0f },
        { -1.0f, 1.0f },
        { 1.0f, -1.0f },
        { 1.0f, 1.0f },
        { -1.0f, -1.0f },
        { 1.0f, 1.0f },
        { -1.0f, -1.0f },
    } };
    static constexpr std::array<float, 8>                kBaseFactors {
        0.8f, 1.0f, 0.45f, 0.6f, 0.8f, 1.0f, 0.45f, 0.6f,
    };
    static constexpr std::array<float, 8> kRoughFactors {
        6.0f, 8.0f, 1.0f, 1.0f, 6.0f, 8.0f, 1.0f, 1.0f,
    };

    auto sample = [&](int index) -> Vector2f {
        if ((index % 2) != 0) return Vector2f::Zero();
        const int   i      = (index / 2) % static_cast<int>(kDirections.size());
        const float factor = kBaseFactors[i] * (1.0f + (kRoughFactors[i] - 1.0f) * grow);
        return Vector2f { kDirections[i][0] * factor, kDirections[i][1] * factor };
    };

    const Vector2f a     = sample(beat);
    const Vector2f b     = sample(beat + 1);
    const Vector2f delta = b - a;
    Vector2f       curve { -delta.y(), delta.x() };
    if (curve.squaredNorm() > 0.0f) curve.normalize();
    const float bend = std::sin(local * kPi) * (0.09f + grow * 0.04f) * delta.norm();
    return a * (1.0f - u) + b * u + curve * bend;
}

} // namespace

void SceneUniformUpdater::FrameBegin() {
    /*
        using namespace std::chrono;
        auto nowTime = system_clock::to_time_t(system_clock::now());
        auto cTime   = std::localtime(&nowTime);
        m_dayTime =
            (((cTime->tm_hour * 60) + cTime->tm_min) * 60 + cTime->tm_sec) / (24.0f * 60.0f
       * 60.0f);
    */
    double new_time    = m_mouseDelayedTime + m_scene->frameTime;
    new_time           = new_time > m_parallax.delay ? m_parallax.delay : new_time;
    m_mouseDelayedTime = new_time;
    // Guard against parallax.delay == 0: scenes with cameraparallaxdelay=0
    // would otherwise produce 0/0 = NaN here, propagating through the MVP
    // and disappearing the wallpaper entirely (issue: gray-screen render).
    double t       = m_parallax.delay > 0.0 ? new_time / m_parallax.delay : 1.0;
    m_mousePosLast = m_mousePos;
    m_mousePos     = std::array { (float)algorism::lerp(t, m_mousePos[0], m_mousePosInput[0]),
                                  (float)algorism::lerp(t, m_mousePos[1], m_mousePosInput[1]) };
}

void SceneUniformUpdater::FrameEnd() {}

void SceneUniformUpdater::MouseInput(double x, double y) {
    using namespace std::chrono;

    auto   now_time = steady_clock::now();
    double new_time = m_mouseDelayedTime -
                      duration_cast<duration<double>>(now_time - m_last_mouse_input_time).count();
    m_mouseDelayedTime = new_time < 0.0f ? 0.0f : new_time;

    m_mousePosInput[0] = (float)x;
    m_mousePosInput[1] = (float)y;

    m_last_mouse_input_time = now_time;
}

void SceneUniformUpdater::InitUniforms(SceneNode* pNode, const ExistsUniformOp& existsOp) {
    m_nodeUniformInfoMap[pNode] = SceneUniformInfo();
    auto& info                  = m_nodeUniformInfoMap[pNode];
    info.has_MI                 = existsOp(G_MI);
    info.has_M                  = existsOp(G_M);
    info.has_AM                 = existsOp(G_AM);
    info.has_MVP                = existsOp(G_MVP);
    info.has_MVPI               = existsOp(G_MVPI);
    info.has_EYEPOSITION        = existsOp(G_EYEPOSITION);
    info.has_EFFECTMODELMATRIX  = existsOp(G_EFFECTMODELMATRIX);
    info.has_EMVP               = existsOp(G_EMVP);
    info.has_EMVPI              = existsOp(G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE);
    info.has_LAYERMODELMATRIX   = existsOp(G_LAYERMODELMATRIX);
    info.has_ETVP               = existsOp(G_ETVP);
    info.has_ETVPI              = existsOp(G_ETVPI);

    info.has_VP = existsOp(G_VP);

    info.has_BONES               = existsOp(G_BONES);
    info.has_TIME                = existsOp(G_TIME);
    info.has_FRAMETIME           = existsOp(G_FRAMETIME);
    info.has_DAYTIME             = existsOp(G_DAYTIME);
    info.has_DAYTIME_LEGACY      = existsOp(G_DAYTIME_LEGACY);
    info.has_POINTERPOSITION     = existsOp(G_POINTERPOSITION);
    info.has_POINTERPOSITIONLAST = existsOp(G_POINTERPOSITIONLAST);
    info.has_PARALLAXPOSITION    = existsOp(G_PARALLAXPOSITION);
    info.has_TEXELSIZE           = existsOp(G_TEXELSIZE);
    info.has_TEXELSIZEHALF       = existsOp(G_TEXELSIZEHALF);
    info.has_SCREEN              = existsOp(G_SCREEN);
    info.has_LP                  = existsOp(G_LP);
    info.has_LCR                 = existsOp(G_LCR);
    info.has_USERALPHA           = existsOp(G_USERALPHA);
    info.has_COLOR4              = existsOp(G_COLOR4);
    info.has_COLOR               = existsOp(G_COLOR);
    info.has_ALPHA               = existsOp(G_ALPHA);
    info.has_BRIGHTNESS          = existsOp(G_BRIGHTNESS);
    info.has_audio_16_l          = existsOp(G_AUDIO_SPEC_16_L);
    info.has_audio_16_r          = existsOp(G_AUDIO_SPEC_16_R);
    info.has_audio_32_l          = existsOp(G_AUDIO_SPEC_32_L);
    info.has_audio_32_r          = existsOp(G_AUDIO_SPEC_32_R);
    info.has_audio_64_l          = existsOp(G_AUDIO_SPEC_64_L);
    info.has_audio_64_r          = existsOp(G_AUDIO_SPEC_64_R);

    std::accumulate(begin(info.texs), end(info.texs), 0, [&existsOp](unsigned index, auto& value) {
        value.has_resolution = existsOp(WE_GLTEX_RESOLUTION_NAMES[index]);
        value.has_mipmap     = existsOp(WE_GLTEX_MIPMAPINFO_NAMES[index]);
        return index + 1;
    });
}

void SceneUniformUpdater::UpdateUniforms(SceneNode* pNode, sprite_map_t& sprites,
                                         const UpdateUniformOp& updateOp) {
    if (! pNode->Mesh()) return;

    pNode->UpdateTrans();

    SceneCamera*     camera;
    std::string_view cam_name = pNode->Camera();
    if (! pNode->Camera().empty()) {
        camera = m_scene->cameras.at(cam_name.data()).get();
    } else if (pNode->Perspective()) {
        cam_name = "global_perspective";
        camera   = m_scene->cameras.at(cam_name.data()).get();
    } else
        camera = m_scene->activeCamera;

    if (! camera) return;

    auto* material = pNode->Mesh()->Material();
    if (! material) return;
    // auto& shadervs = material->customShader.updateValueList;
    // const auto& valueSet = material->customShader.valueSet;

    rstd_assert(exists(m_nodeUniformInfoMap, pNode));
    const auto& info = m_nodeUniformInfoMap[pNode];

    bool hasNodeData = exists(m_nodeDataMap, pNode);
    if (hasNodeData) {
        auto& nodeData = m_nodeDataMap.at(pNode);
        for (const auto& el : nodeData.renderTargets) {
            if (m_scene->renderTargets.count(el.second) == 0) continue;
            const auto& rt = m_scene->renderTargets[el.second];

            const auto& unifrom_tex = info.texs[el.first];

            if (unifrom_tex.has_resolution) {
                std::array<i32, 4> resolution_uint({ rt.width, rt.height, rt.width, rt.height });
                updateOp(WE_GLTEX_RESOLUTION_NAMES[el.first],
                         ShaderValue(array_cast<float>(resolution_uint)));
            }
            if (unifrom_tex.has_mipmap) {
                updateOp(WE_GLTEX_MIPMAPINFO_NAMES[el.first], (float)rt.mipmap_level);
            }
        }
        if (nodeData.puppet_layer && nodeData.puppet_layer->hasPuppet() && info.has_BONES) {
            auto data = nodeData.puppet_layer->genFrame(m_scene->elapsingTime);
            updateOp(G_BONES, std::span<const float> { data[0].data(), data.size() * 16 });
        }
    }

    bool reqMI    = info.has_MI;
    bool reqM     = info.has_M;
    bool reqAM    = info.has_AM;
    bool reqMVP   = info.has_MVP;
    bool reqMVPI  = info.has_MVPI;
    bool reqEMVP  = info.has_EMVP;
    bool reqEMVPI = info.has_EMVPI;
    bool reqEffectModel =
        info.has_EFFECTMODELMATRIX || reqEMVP || reqEMVPI || info.has_LAYERMODELMATRIX;
    bool reqETVP  = info.has_ETVP;
    bool reqETVPI = info.has_ETVPI;

    Matrix4d viewProTrans = camera->GetViewProjectionMatrix();
    if (m_cameraShake.enable && camera == m_scene->activeCamera && camera->AllowCameraShake() &&
        m_cameraShake.amplitude > 0.0f && m_cameraShake.speed > 0.0f) {
        const float base_extent =
            static_cast<float>(std::min(m_scene->ortho[0], m_scene->ortho[1]));
        const float scale = m_cameraShake.amplitude * base_extent * 0.01f;

        const float t      = static_cast<float>(m_scene->elapsingTime) * m_cameraShake.speed * 2.0f;
        auto        offset = ShakeOffset(t, m_cameraShake.roughness);
        Vector3d    shake {
            static_cast<double>(offset.x() * scale),
            static_cast<double>(offset.y() * scale),
            0.0,
        };
        viewProTrans = viewProTrans * Affine3d(Translation3d(shake)).matrix();
    }

    if (info.has_VP) {
        updateOp(G_VP, ShaderValue::fromMatrix(viewProTrans));
    }
    if (info.has_EYEPOSITION && hasNodeData && m_nodeDataMap.at(pNode).use_camera_eye_position) {
        const auto eye = camera->GetPosition().cast<float>();
        updateOp(G_EYEPOSITION, std::array<float, 3> { eye.x(), eye.y(), eye.z() });
    }
    if (reqM || reqMVP || reqMI || reqMVPI || reqEffectModel) {
        Matrix4d modelTrans = pNode->ModelTrans();
        if (hasNodeData && cam_name != "effect") {
            const auto& nodeData   = m_nodeDataMap.at(pNode);
            auto        cameraNode = camera->GetAttachedNode();
            const bool  layerLocalEffectSource =
                camera->HasImgEffect() && cameraNode.is_some() && *cameraNode == pNode;
            if (m_parallax.enable && ! layerLocalEffectSource) {
                auto*       parallaxNode = pNode;
                const auto* parallaxData = &nodeData;
                for (auto* parent = pNode->Parent(); parent != nullptr; parent = parent->Parent()) {
                    auto it = m_nodeDataMap.find(parent);
                    if (it == m_nodeDataMap.end()) continue;
                    if (! it->second.propagate_parallax_to_children) break;
                    parallaxNode = parent;
                    parallaxData = &it->second;
                }
                Matrix4d parallaxTrans = modelTrans;
                if (parallaxNode != pNode) {
                    parallaxNode->UpdateTrans();
                    parallaxTrans = parallaxNode->ModelTrans();
                }
                // World position, not local. Image-effect composite nodes
                // inherit transform via SetParentAnchor and keep identity
                // local trans — Translate() would return (0,0) and put the
                // parallax shift around canvas origin instead of the layer's
                // actual world position.
                Vector3f nodePos = parallaxTrans.block<3, 1>(0, 3).cast<float>();
                Vector2f depth(&parallaxData->propagatedParallaxDepth[0]);
                Vector2f ortho { (float)m_scene->ortho[0], (float)m_scene->ortho[1] };
                // flip mouse y axis
                Vector2f mouseVec =
                    Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - Vector2f(&m_mousePos[0]));
                mouseVec        = mouseVec.cwiseProduct(ortho) * m_parallax.mouseinfluence;
                Vector3f camPos = camera->GetPosition().cast<float>();
                Vector2f paraVec =
                    (nodePos.head<2>() - camPos.head<2>() + mouseVec).cwiseProduct(depth) *
                    m_parallax.amount;
                modelTrans =
                    Affine3d(Translation3d(Vector3d(paraVec.x(), paraVec.y(), 0.0f))).matrix() *
                    modelTrans;
            }
        }

        if (reqM) updateOp(G_M, ShaderValue::fromMatrix(modelTrans));
        if (reqAM) updateOp(G_AM, ShaderValue::fromMatrix(modelTrans));
        if (reqMI) updateOp(G_MI, ShaderValue::fromMatrix(modelTrans.inverse()));
        if (reqMVP) {
            Matrix4d mvpTrans = viewProTrans * modelTrans;
            updateOp(G_MVP, ShaderValue::fromMatrix(mvpTrans));
            if (reqMVPI) updateOp(G_MVPI, ShaderValue::fromMatrix(mvpTrans.inverse()));
        }
        if (reqEffectModel) {
            Matrix4d layerModel  = modelTrans;
            Matrix4d effectModel = modelTrans;
            if (hasNodeData && m_nodeDataMap.at(pNode).effect_projection_node != nullptr) {
                const auto& nodeData = m_nodeDataMap.at(pNode);
                auto*       source   = nodeData.effect_projection_node;
                source->UpdateTrans();
                layerModel  = source->ModelTrans();
                effectModel = layerModel;
                if (nodeData.effect_projection_size[0] > 0.0f &&
                    nodeData.effect_projection_size[1] > 0.0f) {
                    effectModel =
                        effectModel *
                        Affine3d(
                            Scaling(static_cast<double>(nodeData.effect_projection_size[0]) * 0.5,
                                    static_cast<double>(nodeData.effect_projection_size[1]) * 0.5,
                                    1.0))
                            .matrix();
                }
            }
            if (info.has_LAYERMODELMATRIX)
                updateOp(G_LAYERMODELMATRIX, ShaderValue::fromMatrix(layerModel));
            if (info.has_EFFECTMODELMATRIX)
                updateOp(G_EFFECTMODELMATRIX, ShaderValue::fromMatrix(effectModel));
            if (reqEMVP || reqEMVPI) {
                SceneCamera* effect_camera = m_scene->activeCamera ? m_scene->activeCamera : camera;
                const Matrix4d effect_mvp  = effect_camera->GetViewProjectionMatrix() * effectModel;
                if (reqEMVP) updateOp(G_EMVP, ShaderValue::fromMatrix(effect_mvp));
                if (reqEMVPI)
                    updateOp(G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE,
                             ShaderValue::fromMatrix(effect_mvp.inverse()));
            }
        }
        if (reqETVP || reqETVPI) {
            /*
            Vector3d nodePos = pNode->Translate().cast<double>();
            nodePos.z()      = 1.0f;
            Matrix4d etvpTrans =
                viewProTrans * modelTrans * Affine3d(Eigen::Scaling(nodePos)).matrix();
            if (reqETVPI) updateOp(G_ETVP, ShaderValue::fromMatrix(etvpTrans));
            if (reqETVPI) updateOp(G_ETVPI, ShaderValue::fromMatrix(etvpTrans.inverse()));
            */
        }
    }

    //	g_EffectTextureProjectionMatrix
    // shadervs.push_back({"g_EffectTextureProjectionMatrixInverse",
    // ShaderValue::ValueOf(Eigen::Matrix4f::Identity())});
    if (info.has_TIME) updateOp(G_TIME, (float)m_scene->elapsingTime);

    if (info.has_FRAMETIME) updateOp(G_FRAMETIME, (float)m_scene->frameTime);

    if (info.has_DAYTIME) updateOp(G_DAYTIME, (float)m_dayTime);
    if (info.has_DAYTIME_LEGACY) updateOp(G_DAYTIME_LEGACY, (float)m_dayTime);

    if (info.has_POINTERPOSITION) updateOp(G_POINTERPOSITION, m_mousePos);
    if (info.has_POINTERPOSITIONLAST) updateOp(G_POINTERPOSITIONLAST, m_mousePosLast);

    if (info.has_TEXELSIZE) updateOp(G_TEXELSIZE, m_texelSize);

    if (info.has_TEXELSIZEHALF)
        updateOp(G_TEXELSIZEHALF, std::array { m_texelSize[0] / 2.0f, m_texelSize[1] / 2.0f });

    if (info.has_SCREEN)
        updateOp(G_SCREEN,
                 std::array<float, 3> {
                     m_screen_size[0], m_screen_size[1], m_screen_size[0] / m_screen_size[1] });

    if (info.has_PARALLAXPOSITION) {
        Vector2f para { 0.5f, 0.5f };
        if (m_parallax.enable) {
            const Vector2f mouseCentered = Vector2f(&m_mousePos[0]) - Vector2f { 0.5f, 0.5f };
            para = Vector2f { 0.5f, 0.5f } +
                   (Scaling(1.0f, -1.0f) * mouseCentered) * m_parallax.mouseinfluence;
        }
        updateOp(G_PARALLAXPOSITION, std::array { para[0], para[1] });
    }

    const auto& anim_override = pNode->TexAnim();
    for (auto& [i, sp] : sprites) {
        // Script-driven override:
        //   current_frame >= 0  → pin to that frame
        //   playing == false    → freeze on current auto-advance frame
        //   else                → normal time-driven advance
        const SpriteFrame* fp = nullptr;
        if (anim_override.current_frame >= 0 && sp.numFrames() > 0) {
            const i32 idx = i32(anim_override.current_frame) % i32(sp.numFrames());
            fp            = &sp.GetFrame(idx);
        } else if (! anim_override.playing && sp.numFrames() > 0) {
            fp = &sp.GetCurFrame();
        } else {
            fp = &sp.GetAnimateFrame(m_scene->frameTime);
        }
        const auto& f      = *fp;
        auto        grot   = WE_GLTEX_ROTATION_NAMES[i];
        auto        gtrans = WE_GLTEX_TRANSLATION_NAMES[i];
        updateOp(grot, std::array { f.xAxis[0], f.xAxis[1], f.yAxis[0], f.yAxis[1] });
        updateOp(gtrans, std::array { f.x, f.y });
    }

    // WE's `g_LightsPosition[4]` is a vec3 array; std140 pads to vec4 stride
    // (16B) so the trailing scalar slot stays at 0.
    if (info.has_LP) {
        constexpr unsigned                kMaxLights = 4;
        std::array<float, kMaxLights * 4> lights_pos { 0 };
        std::array<float, kMaxLights * 4> lights_color_radius { 0 };
        std::array<float, 12>             lights_color_legacy { 0 };
        unsigned                          i = 0;
        for (auto& l : m_scene->lights) {
            if (i == kMaxLights) break;
            if (! l->runtimeVisible()) {
                i++;
                continue;
            }
            rstd_assert(l->node() != nullptr);
            const auto& trans              = l->node()->Translate();
            lights_pos[i * 4 + 0]          = trans.x();
            lights_pos[i * 4 + 1]          = trans.y();
            lights_pos[i * 4 + 2]          = trans.z();
            lights_pos[i * 4 + 3]          = 0.0f;
            const auto color               = l->color();
            lights_color_radius[i * 4 + 0] = color.x();
            lights_color_radius[i * 4 + 1] = color.y();
            lights_color_radius[i * 4 + 2] = color.z();
            lights_color_radius[i * 4 + 3] = l->radius();
            if (i < 3) {
                const auto& cp = l->premultipliedColor();
                std::copy(cp.begin(), cp.end(), lights_color_legacy.begin() + i * 4);
            }
            i++;
        }
        updateOp(G_LP, lights_pos);
        updateOp(G_LCP, lights_color_legacy);
        updateOp(G_LCR, lights_color_radius);
    }

    // Script-driven per-frame overrides. updateOp overlays the material's
    // baked constValue for this draw; we only push when the script has
    // actually written to avoid clobbering the bake.
    auto push_color4 = [&updateOp](const Eigen::Vector3f& color, float alpha) {
        updateOp(G_COLOR4, std::array<float, 4> { color.x(), color.y(), color.z(), alpha });
    };
    auto push_color = [&updateOp](const Eigen::Vector3f& color) {
        updateOp(G_COLOR, std::array<float, 3> { color.x(), color.y(), color.z() });
    };
    if (pNode->IsAlphaOverridden()) {
        const float eff_alpha = pNode->EffectiveAlpha();
        if (info.has_USERALPHA) {
            updateOp(G_USERALPHA, eff_alpha);
        }
        if (info.has_ALPHA) {
            updateOp(G_ALPHA, eff_alpha);
        }
        if (info.has_COLOR4) {
            if (! info.has_USERALPHA) {
                push_color4(pNode->IsColorOverridden() ? pNode->Color() : pNode->BaseColor(),
                            eff_alpha);
            } else if (pNode->IsColorOverridden()) {
                push_color4(pNode->Color(), pNode->BaseAlpha());
            }
        }
        if (info.has_COLOR && pNode->IsColorOverridden()) {
            push_color(pNode->Color());
        }
    } else if (pNode->IsColorOverridden()) {
        if (info.has_COLOR4) {
            push_color4(pNode->Color(), pNode->BaseAlpha());
        }
        if (info.has_COLOR) {
            push_color(pNode->Color());
        }
    }
    if (pNode->IsBrightnessOverridden() && info.has_BRIGHTNESS) {
        updateOp(G_BRIGHTNESS, pNode->Brightness());
    }

    // WE audio-bar shaders. std140 array stride is 16 bytes per element, so
    // pack each already-smoothed amplitude into .x and leave .yzw at zero.
    auto push_audio = [&](std::string_view name, std::span<const float> visual) {
        const std::size_t count = visual.size() * 4;
        rstd_assert(count <= m_audio_pack_scratch.size());
        std::span<float> packed(m_audio_pack_scratch.data(), count);
        std::fill(packed.begin(), packed.end(), 0.0f);
        for (std::size_t i = 0; i < visual.size(); ++i) packed[i * 4] = visual[i];
        updateOp(name, std::span<const float>(packed));
    };
    if (info.has_audio_16_l) push_audio(G_AUDIO_SPEC_16_L, m_audio_16_l);
    if (info.has_audio_16_r) push_audio(G_AUDIO_SPEC_16_R, m_audio_16_r);
    if (info.has_audio_32_l) push_audio(G_AUDIO_SPEC_32_L, m_audio_32_l);
    if (info.has_audio_32_r) push_audio(G_AUDIO_SPEC_32_R, m_audio_32_r);
    if (info.has_audio_64_l) push_audio(G_AUDIO_SPEC_64_L, m_audio_64_l);
    if (info.has_audio_64_r) push_audio(G_AUDIO_SPEC_64_R, m_audio_64_r);
}

void SceneUniformUpdater::SetNodeData(void* nodeAddr, const SceneUniformNodeData& data) {
    m_nodeDataMap[nodeAddr] = data;
}

void SceneUniformUpdater::CopyNodeData(void* src, void* dst) {
    auto it = m_nodeDataMap.find(src);
    if (it == m_nodeDataMap.end()) return;
    m_nodeDataMap[dst] = it->second;
}

void SceneUniformUpdater::SetTexelSize(float x, float y) { m_texelSize = { x, y }; }

void SceneUniformUpdater::SetAudioSpectrum(std::span<const float, 64> left,
                                           std::span<const float, 64> right) {
    AverageResample64(left, m_audio_16_l);
    AverageResample64(right, m_audio_16_r);
    AverageResample64(left, m_audio_32_l);
    AverageResample64(right, m_audio_32_r);
    AverageResample64(left, m_audio_64_l);
    AverageResample64(right, m_audio_64_r);
}
