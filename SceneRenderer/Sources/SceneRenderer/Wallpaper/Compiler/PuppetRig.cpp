module;

#include <rstd/macro.hpp>

module sr.pkg.puppet;
import eigen;
import sr.core;
import rstd.cppstd;

using namespace sr;
using namespace Eigen;

static double SampleBoneCurve(const std::vector<WPPuppet::BoneFrameCurve>&  curves,
                              unsigned                                      bone_index,
                              const WPPuppet::Animation::InterpolationInfo& info) {
    if (bone_index >= curves.size()) return 1.0;
    const auto& values = curves[bone_index].values;
    if (values.empty()) return 1.0;

    auto sample = [&](idx frame) {
        const auto i = std::min<usize>(static_cast<usize>(frame), values.size() - 1);
        return static_cast<double>(values[i]);
    };
    const double a = sample(info.frame_a);
    const double b = sample(info.frame_b);
    return a * (1.0 - info.t) + b * info.t;
}

static double LayerBoneBlend(const WPPuppet::Animation& anim, unsigned bone_index,
                             const WPPuppet::Animation::InterpolationInfo& info,
                             double                                        layer_blend) {
    // blend_curves are WE per-bone *opacity* envelopes, not pose blend
    // weights — the pose math must not see them. Pose blending is driven by
    // the layer's own blend amount only. See BoneFrameCurve's comment.
    double blend = layer_blend;
    blend *= SampleBoneCurve(anim.scalar_curves, bone_index, info);
    return std::max(0.0, blend);
}

// Per-bone opacity contribution of one animation layer, in 0..1.
//
// `layer_blend` fades the layer's envelope toward "fully opaque" so a layer
// that is dialled out cannot darken the sprite: blend 1 yields the raw curve,
// blend 0 yields 1.0. Layers then multiply together, which is exact for the
// single-layer case every puppet in the corpus actually uses and stays
// monotonic and in-range for stacks.
static double LayerBoneAlpha(const WPPuppet::Animation& anim, unsigned bone_index,
                             const WPPuppet::Animation::InterpolationInfo& info,
                             double                                        layer_blend) {
    const double curve = SampleBoneCurve(anim.blend_curves, bone_index, info);
    const double w     = std::clamp(layer_blend, 0.0, 1.0);
    return std::clamp(1.0 + w * (curve - 1.0), 0.0, 1.0);
}

static bool HasAuthoredTrack(const WPPuppet::BoneTrack& track) {
    constexpr float eps      = 1e-6f;
    auto            non_zero = [](const Eigen::Vector3f& v) {
        return v.cwiseAbs().maxCoeff() > eps;
    };
    auto non_default_scale = [](const Eigen::Vector3f& v) {
        const bool zero = v.cwiseAbs().maxCoeff() <= eps;
        const bool one  = (v - Eigen::Vector3f::Ones()).cwiseAbs().maxCoeff() <= eps;
        return ! zero && ! one;
    };
    for (const auto& frame : track.frames) {
        if (non_zero(frame.position) || non_zero(frame.angle) || non_default_scale(frame.scale))
            return true;
    }
    return false;
}

struct BindLinear {
    Quaterniond rotation;
    Vector3f    scale;
};

static Quaterniond ToQuaternion(Vector3f euler) {
    const std::array<Vector3d, 3> axis { Vector3d::UnitX(), Vector3d::UnitY(), Vector3d::UnitZ() };
    return AngleAxis<double>(euler.z(), axis[2]) * AngleAxis<double>(euler.y(), axis[1]) *
           AngleAxis<double>(euler.x(), axis[0]);
};

static BindLinear DecomposeBindLinear(const Matrix3f& linear) {
    Matrix3f rot = linear;
    Vector3f scale { rot.col(0).norm(), rot.col(1).norm(), rot.col(2).norm() };
    for (int i = 0; i < 3; ++i) {
        if (scale[i] > 0.000001f) {
            rot.col(i) /= scale[i];
        } else {
            rot.col(i).setZero();
            rot(i, i) = 1.0f;
            scale[i]  = 1.0f;
        }
    }
    if (rot.determinant() < 0.0f) {
        scale.x() = -scale.x();
        rot.col(0) *= -1.0f;
    }

    Quaterniond q { rot.cast<double>() };
    q.normalize();
    return { q, scale };
}

void WPPuppet::prepared() {
    for (unsigned i = 0; i < bones.size(); i++) {
        auto& b = bones[i];
        rstd_assert(b.bind_parent < i || b.noBindParent());
        // vco bracket only applies to world-anchored puppets (MDLV21): each bone
        // is its own sprite root, and anim pivots around vertex_centroid_offset.
        // Chain LBS (MDLV22+) keeps strict parent.world_bind * local_bind.
        if (b.noBindParent()) {
            b.world_bind = b.local_bind;
            if (world_anchored_bones) {
                b.world_bind.pretranslate(b.vertex_centroid_offset);
            }
        } else {
            b.world_bind = bones[b.bind_parent].world_bind * b.local_bind;
        }
        b.inv_bind = b.world_bind.inverse();
    }
    for (auto& attachment : attachments) {
        attachment.bind_xform = attachment.local_xform;
        if (attachment.bone_index >= bones.size()) continue;

        std::vector<uint32_t> chain;
        uint32_t              bone_index = attachment.bone_index;
        while (bone_index != NO_PARENT && bone_index < bones.size()) {
            chain.push_back(bone_index);
            bone_index = bones[bone_index].file_parent;
        }

        Eigen::Affine3f bone_bind = Eigen::Affine3f::Identity();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            bone_bind = bone_bind * bones[*it].local_bind;
        }
        attachment.bind_xform = bone_bind * attachment.local_xform;
    }
    for (auto& anim : anims) {
        anim.frame_time = 1.0f / anim.fps;
        anim.max_time   = anim.length / anim.fps;
        for (auto& t : anim.bone_tracks) {
            for (auto& f : t.frames) {
                f.quaternion = ToQuaternion(f.angle);
            }
        }
    }

    m_final_affines.resize(bones.size());
    m_final_alphas.assign(bones.size(), 1.0f);
}

// A curve that never leaves 1.0 is indistinguishable from "no curve", so it
// must not switch the shader permutation on. Anything that dips below opaque
// (blink envelopes, fade-ins) does.
bool WPPuppet::hasAlphaCurves() const noexcept {
    constexpr float opaque_eps = 1e-4f;
    for (const auto& anim : anims) {
        for (const auto& curve : anim.blend_curves) {
            for (float v : curve.values) {
                if (v < 1.0f - opaque_eps) return true;
            }
        }
    }
    return false;
}

std::optional<std::size_t> WPPuppet::attachmentIndex(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        if (attachments[i].name == name) return i;
    }
    return std::nullopt;
}

std::optional<Eigen::Affine3f>
WPPuppet::attachmentBindTransform(std::size_t index) const noexcept {
    if (index >= attachments.size()) return std::nullopt;
    return attachments[index].bind_xform;
}

std::span<const Eigen::Affine3f> WPPuppet::genFrame(WPPuppetLayer& puppet_layer,
                                                    double         time) noexcept {
    puppet_layer.updateInterpolation(time);

    // TRS skinning is required: WE puppets animate scale (e.g. blink uses
    // frame.scale.y → ~0). A pure-translation g_Bones would shift the
    // whole sprite as a unit; intra-sprite compression needs non-identity
    // linear so vertices within the sprite get differential treatment.
    // Standard LBS: per-bone local affine = T(pos) · R(quat) · Diag(scale).
    // Chained through parent's anim transform, then M_skin = A_world · inv_bind.
    // WE anim convention: frame[0] is the replacement anchor pose for a bone.
    // MDLA blend curves decide which dense bone-track slots are active for each
    // animation layer; inactive bones keep bind pose instead of being diluted by
    // unrelated replacement layers.
    for (unsigned i = 0; i < m_final_affines.size(); i++) {
        const auto& bone   = bones[i];
        auto&       affine = m_final_affines[i];

        rstd_assert(bone.anim_parent < i || bone.noAnimParent());
        Affine3f parent = Affine3f::Identity();
        if (! bone.noAnimParent()) {
            parent = m_final_affines[bone.anim_parent];
            if (world_anchored_bones) {
                // MDLV21 child bind poses are already puppet-local; inherit
                // only the parent's animated delta to avoid double transforms.
                parent = parent * bones[bone.anim_parent].inv_bind.matrix();
            }
        }

        const WPPuppet::BoneFrame* replace_base_frame { nullptr };
        for (const auto& layer : puppet_layer.m_layers) {
            if (layer.anim == nullptr || ! layer.anim_layer.visible || layer.anim_layer.additive)
                continue;
            if (i >= layer.anim->bone_tracks.size()) continue;
            const auto& track = layer.anim->bone_tracks[i];
            if (! HasAuthoredTrack(track)) continue;
            const double blend =
                LayerBoneBlend(*layer.anim, i, layer.interp_info, layer.anim_layer.blend);
            if (blend <= 0.0) continue;
            replace_base_frame = std::addressof(track.frames[(usize)0]);
            break;
        }

        // Bind state. vco is a fixed render-time pivot offset for root sprite
        // bones (matches world_bind's pretranslate in prepared()) and is added
        // after layer deltas so the replacement anchor stays in puppet space.
        const BindLinear bind_linear = DecomposeBindLinear(bone.local_bind.linear());

        Vector3f trans { replace_base_frame != nullptr ? replace_base_frame->position
                                                       : bone.local_bind.translation() };
        Vector3f scale { replace_base_frame != nullptr ? replace_base_frame->scale
                                                       : bind_linear.scale };
        // quat absorbs the anchor rotation directly. Each layer multiplies in its
        // frame delta from frame[0], whose delta is identity.
        Quaterniond       quat { replace_base_frame != nullptr ? replace_base_frame->quaternion
                                                               : bind_linear.rotation };
        const Quaterniond ident { Quaterniond::Identity() };

        for (auto& layer : puppet_layer.m_layers) {
            auto& alayer = layer.anim_layer;
            if (layer.anim == nullptr || ! alayer.visible) continue;
            if (i >= layer.anim->bone_tracks.size()) continue;

            auto& info  = layer.interp_info;
            auto& track = layer.anim->bone_tracks[i];
            if (! HasAuthoredTrack(track)) continue;
            auto& frame_base = track.frames[(usize)0];
            auto& frame_a    = track.frames[(usize)info.frame_a];
            auto& frame_b    = track.frames[(usize)info.frame_b];

            double t     = info.t;
            double one_t = 1.0 - info.t;
            double blend = LayerBoneBlend(*layer.anim, i, info, alayer.blend);
            if (blend <= 0.0) continue;

            auto frame_a_quat_delta = frame_a.quaternion * frame_base.quaternion.conjugate();
            auto frame_b_quat_delta = frame_b.quaternion * frame_base.quaternion.conjugate();
            auto pos_a_delta        = frame_a.position - frame_base.position;
            auto pos_b_delta        = frame_b.position - frame_base.position;
            auto scale_a_delta      = frame_a.scale - frame_base.scale;
            auto scale_b_delta      = frame_b.scale - frame_base.scale;

            quat *= frame_a_quat_delta.slerp(t, frame_b_quat_delta).slerp(1.0 - blend, ident);
            if (alayer.additive) {
                trans += blend * (pos_a_delta * one_t + pos_b_delta * t);
                scale += blend * (scale_a_delta * one_t + scale_b_delta * t);
            } else {
                trans += blend * (pos_a_delta * one_t + pos_b_delta * t);
                scale += blend * (scale_a_delta * one_t + scale_b_delta * t);
            }
        }
        // Per-bone opacity. Deliberately independent of the pose loop above:
        // the envelope is a property of the curve, not of the track, and the
        // bones that matter most here (a lid held still while it fades) have
        // no authored motion at all. Flat per bone with no parent inheritance,
        // matching the shader's `g_BonesAlpha[a_BlendIndices.*]` lookup.
        double alpha = 1.0;
        for (const auto& layer : puppet_layer.m_layers) {
            if (layer.anim == nullptr || ! layer.anim_layer.visible) continue;
            alpha *= LayerBoneAlpha(*layer.anim, i, layer.interp_info, layer.anim_layer.blend);
        }
        m_final_alphas[i] = static_cast<float>(std::clamp(alpha, 0.0, 1.0));

        if (bone.noBindParent() && world_anchored_bones) {
            trans += bone.vertex_centroid_offset;
        }
        affine = Affine3f::Identity();
        affine.pretranslate(trans);
        affine.rotate(quat.cast<float>());
        affine.scale(scale);
        affine = parent * affine;
    }

    for (unsigned i = 0; i < m_final_affines.size(); i++) {
        m_final_affines[i] *= bones[i].inv_bind.matrix();
    }
    return m_final_affines;
}

static constexpr void genInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                           double& cur, u32 length, double frame_time,
                                           double max_time) {
    cur          = std::fmod(cur, max_time);
    double _rate = cur / frame_time;

    // `length` is the number of intervals; the track stores `length + 1`
    // frame samples (frame[0]..frame[length], where frame[length] closes
    // the loop). frame_b = frame_a + 1 is always in-range.
    info.frame_a = ((unsigned)_rate) % length;
    info.frame_b = info.frame_a + 1;
    info.t       = _rate - (double)info.frame_a;
}

static constexpr void genSingleInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                                 double& cur, u32 length, double frame_time,
                                                 double max_time) {
    if (length == 0 || frame_time <= 0.0) {
        cur          = 0.0;
        info.frame_a = 0;
        info.frame_b = 0;
        info.t       = 0.0;
        return;
    }

    cur          = std::clamp(cur, 0.0, max_time);
    double rate  = cur / frame_time;
    u32    frame = static_cast<u32>(rate);
    if (frame >= length) {
        info.frame_a = length - 1;
        info.frame_b = length;
        info.t       = 1.0;
        return;
    }

    info.frame_a = frame;
    info.frame_b = frame + 1;
    info.t       = rate - static_cast<double>(frame);
}

WPPuppet::Animation::InterpolationInfo
WPPuppet::Animation::getInterpolationInfo(double* cur_time) const {
    InterpolationInfo _info;
    auto&             _cur_time = *cur_time;

    if (mode == PlayMode::Loop) {
        genInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
    } else if (mode == PlayMode::Single) {
        genSingleInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
    } else if (mode == PlayMode::Mirror) {
        // Frames 0..length stored; mirror cycle is 0,1,..,length,length-1,..,0
        // (2*length intervals). Map any f in [0, 2*length] back into [0, length].
        const auto _get_frame = [this](auto f) -> idx {
            return f <= length ? f : (2 * length - f);
        };
        genInterpolationInfo(_info, _cur_time, (u32)length * 2, frame_time, max_time * 2.0f);
        _info.frame_a = _get_frame(_info.frame_a);
        _info.frame_b = _get_frame(_info.frame_b);
    }

    return _info;
}

void WPPuppetLayer::prepared(std::span<AnimationLayer> alayers) {
    m_layers.resize(alayers.size());

    const auto&           anims         = m_puppet->anims;
    const AnimationLayer* additive_base = nullptr;
    bool                  has_replace   = false;
    auto                  exists        = [&](const auto& layer) {
        return std::any_of(anims.begin(), anims.end(), [&](const auto& a) {
            return a.id == layer.id;
        });
    };
    for (const auto& layer : alayers) {
        if (! layer.visible || ! exists(layer)) continue;
        if (layer.additive) {
            if (! has_replace && additive_base == nullptr && layer.blend > 0.0) {
                additive_base = std::addressof(layer);
            }
            continue;
        }
        has_replace   = true;
        additive_base = nullptr;
    }

    std::transform(alayers.rbegin(),
                   alayers.rend(),
                   m_layers.rbegin(),
                   [additive_base, this](const auto& layer) {
                       const auto& anims     = m_puppet->anims;
                       auto        out_layer = layer;

                       auto it = std::find_if(anims.begin(), anims.end(), [&layer](auto& a) {
                           return layer.id == a.id;
                       });
                       bool ok = it != anims.end() && layer.visible;

                       if (ok && std::addressof(layer) == additive_base) {
                           // Additive-only stacks still need one absolute frame[0]
                           // pose; otherwise authored puppet pieces stay scattered.
                           out_layer.additive = false;
                       }

                       return Layer {
                           .anim_layer = out_layer,
                           .anim       = ok ? std::addressof(*it) : nullptr,
                       };
                   });
}

std::span<const Eigen::Affine3f> WPPuppetLayer::genFrame(double time) noexcept {
    return m_puppet->genFrame(*this, time);
}

std::span<const float> WPPuppetLayer::boneAlphas() const noexcept {
    if (! m_puppet) return {};
    return m_puppet->boneAlphas();
}

uint32_t WPPuppetLayer::boneIndex(std::string_view name) const noexcept {
    if (! m_puppet) return 0;
    for (uint32_t i = 0; i < m_puppet->bones.size(); ++i) {
        if (m_puppet->bones[i].name == name) return i + 1;
    }
    return 0;
}

std::optional<Eigen::Affine3f> WPPuppetLayer::boneTransform(uint32_t index, double time) noexcept {
    if (! m_puppet || index == 0) return std::nullopt;
    const uint32_t zero_based = index - 1;
    if (zero_based >= m_puppet->bones.size()) return std::nullopt;
    auto frame = genFrame(time);
    if (zero_based >= frame.size()) return std::nullopt;
    return frame[zero_based] * m_puppet->bones[zero_based].world_bind;
}

std::optional<Eigen::Affine3f> WPPuppetLayer::attachmentTransform(std::size_t index,
                                                                  double time) noexcept {
    if (! m_puppet || index >= m_puppet->attachments.size()) return std::nullopt;
    const auto& attachment = m_puppet->attachments[index];
    if (attachment.bone_index >= m_puppet->bones.size()) return std::nullopt;
    auto frame = genFrame(time);
    if (attachment.bone_index >= frame.size()) return std::nullopt;
    return frame[attachment.bone_index] * attachment.bind_xform;
}

void WPPuppetLayer::updateInterpolation(double elapsed) noexcept {
    double delta   = (m_last_elapsed < 0.0) ? 0.0 : (elapsed - m_last_elapsed);
    bool   advance = (m_last_elapsed < 0.0) || (delta > 0.0);
    if (advance) m_last_elapsed = elapsed;
    for (auto& layer : m_layers) {
        if (layer) {
            if (advance) layer.anim_layer.cur_time += delta * layer.anim_layer.rate;
            layer.interp_info = layer.anim->getInterpolationInfo(&(layer.anim_layer.cur_time));
        }
    }
}

WPPuppetLayer::WPPuppetLayer(std::shared_ptr<WPPuppet> pup): m_puppet(pup) {}
WPPuppetLayer::WPPuppetLayer()  = default;
WPPuppetLayer::~WPPuppetLayer() = default;
