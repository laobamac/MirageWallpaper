module;

#include <algorithm>
#include <cmath>
#include <sstream>

#include <rstd/macro.hpp>

#include "Utils/StringUtil.h"
#include "Utils/Sha.hpp"

module sr.pkg.parse;
import eigen;
import nlohmann.json;
import sr.spec_texs;
import sr.core;
import sr.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import sr.utils;
import sr.scene;
import sr.text;
import sr.script;

import sr.scene_uniform_updater;

using namespace sr;
using namespace Eigen;

std::string getAddr(void* p) { return std::to_string(reinterpret_cast<intptr_t>(p)); }

// ParseContext, SceneObjectVar, ProcessOpts and the stage entry points
// (ExpandObjects / AdjustAutoOrthoProjection / BuildContext /
// ProcessObjects / FinalizeScene) are exported from the
// :scene_stages partition; their definitions live near the bottom of
// this file.

namespace
{
// Detect the WE audio-bar fanout pattern: scripts that bind a layer's
// `visible` field, call engine.registerAudioBuffers(N), and then create
// N-1 sibling layers in init() via thisScene.createLayer(...). sr doesn't
// have a runtime model parser, so we pre-spawn the N-1 SceneNode clones at
// parse time (sharing the template's mesh + shader-value record) and hand
// them to the script through FieldScript::clone_queue.
//
// Returns N (resolution) when the source matches the pattern, otherwise 0.
unsigned DetectAudioFanoutCount(std::string_view src) {
    auto pos = src.find("registerAudioBuffers");
    if (pos == std::string_view::npos) return 0;
    if (src.find("createLayer") == std::string_view::npos) return 0;
    pos += std::string_view("registerAudioBuffers").size();
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
    if (pos >= src.size() || src[pos] != '(') return 0;
    ++pos;
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;

    auto is_digit = [](char c) {
        return c >= '0' && c <= '9';
    };
    auto is_ident = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '$';
    };
    auto read_num = [&](usize p) -> unsigned {
        unsigned n = 0;
        while (p < src.size() && is_digit(src[p])) n = n * 10 + unsigned(src[p++] - '0');
        return n;
    };

    // Numeric literal: registerAudioBuffers(64).
    if (pos < src.size() && is_digit(src[pos])) return read_num(pos);

    // Variable: registerAudioBuffers(audioBuffer) with `var audioBuffer = 64`
    // earlier (the common WE audio-bar template). Resolve the first
    // `<ident> = <number>` assignment to that name. We don't run JS, so this
    // only handles a literal-initialized count (always 16/32/64 in practice).
    if (pos >= src.size() || ! is_ident(src[pos]) || is_digit(src[pos])) return 0;
    usize e = pos;
    while (e < src.size() && is_ident(src[e])) ++e;
    std::string_view name = src.substr(pos, e - pos);
    for (usize p = 0; (p = src.find(name, p)) != std::string_view::npos; p += name.size()) {
        const bool  lb = (p == 0) || ! is_ident(src[p - 1]);
        const usize a  = p + name.size();
        const bool  rb = (a >= src.size()) || ! is_ident(src[a]);
        if (! lb || ! rb) continue;
        usize q = a;
        while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) ++q;
        if (q >= src.size() || src[q] != '=') continue;
        ++q;
        while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) ++q;
        if (q < src.size() && is_digit(src[q])) return read_num(q);
    }
    return 0;
}

std::shared_ptr<WPPuppetLayer> MakePuppetLayer(std::shared_ptr<WPPuppet>                puppet,
                                               std::span<WPPuppetLayer::AnimationLayer> layers) {
    if (! puppet) return nullptr;
    auto out = std::make_shared<WPPuppetLayer>(std::move(puppet));
    out->prepared(layers);
    return out;
}

void RegisterPuppetLayer(ParseContext& context, SceneNode* node,
                         std::shared_ptr<WPPuppetLayer> layer) {
    if (! node || ! layer) return;
    context.puppet_layers->by_node[node] = std::move(layer);
}

std::shared_ptr<WPPuppetLayer> LookupPuppetLayer(const std::shared_ptr<PuppetLayerRegistry>& layers,
                                                 SceneNode*                                  node) {
    if (! layers || ! node) return nullptr;
    auto it = layers->by_node.find(node);
    if (it != layers->by_node.end()) return it->second;
    auto fallback_it = layers->fallback_by_node.find(node);
    return fallback_it == layers->fallback_by_node.end() ? nullptr : fallback_it->second;
}

SceneNode* RootOf(SceneNode* node) {
    if (! node) return nullptr;
    while (node->Parent()) node = node->Parent();
    return node;
}

void CollectLinkedSourceIdsFromJsonValue(const nlohmann::json& value, Set<std::int32_t>& out) {
    if (value.is_string()) {
        const auto s = value.get<std::string>();
        if (auto id = ParseImageLayerCompositeId(s)) out.insert(static_cast<std::int32_t>(*id));
        if (IsSpecLinkTex(s)) out.insert(static_cast<std::int32_t>(ParseLinkTex(s)));
        return;
    }
    if (value.is_array()) {
        for (const auto& el : value) CollectLinkedSourceIdsFromJsonValue(el, out);
        return;
    }
    if (! value.is_object()) return;
    for (const auto& el : value.items()) {
        if (el.key() == "dependencies" && el.value().is_array()) {
            for (const auto& dep : el.value()) {
                if (dep.is_number_integer()) out.insert(dep.get<std::int32_t>());
            }
        }
        CollectLinkedSourceIdsFromJsonValue(el.value(), out);
    }
}

Set<std::int32_t> CollectLinkedSourceIdsFromJson(const nlohmann::json& json) {
    Set<std::int32_t> out;
    if (json.contains("objects")) CollectLinkedSourceIdsFromJsonValue(json.at("objects"), out);
    return out;
}

void MarkHiddenLinkSource(ParseContext& context, std::int32_t id) {
    if (context.hidden_link_source_ids.count(id) != 0) context.scene->elidable_layer_ids.insert(id);
}

std::array<float, 2> Texture0UvScale(const SceneMaterial& material, bool nopadding = false) {
    if (nopadding) return { 1.0f, 1.0f };
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[0]);
    if (it == material.customShader.constValues.end()) return { 1.0f, 1.0f };
    const auto& r = it->second;
    if (r.size() < 4 || r[0] == 0.0f || r[1] == 0.0f) return { 1.0f, 1.0f };
    return { r[2] / r[0], r[3] / r[1] };
}

float ParticleTextureRatio(const SceneMaterial& material) {
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[0]);
    if (it == material.customShader.constValues.end()) return 1.0f;
    const auto& r = it->second;
    if (r.size() < 2 || r[0] == 0.0f) return 1.0f;
    return r[1] / r[0];
}

std::shared_ptr<WPPuppetLayer>
FindPuppetLayerWithBone(const std::shared_ptr<PuppetLayerRegistry>& layers, SceneNode* node,
                        std::string_view name, uint32_t& index) {
    if (! layers || ! node) return nullptr;
    if (auto it = layers->by_node.find(node); it != layers->by_node.end()) {
        index = it->second ? it->second->boneIndex(name) : 0;
        if (index != 0) return it->second;
    }
    for (auto& child : node->GetChildren()) {
        if (auto hit = FindPuppetLayerWithBone(layers, child.as_ptr(), name, index)) return hit;
    }
    return nullptr;
}

std::vector<sr::SceneNode*> SpawnLayerClones(ParseContext& context, SceneNode* tmpl,
                                              unsigned count) {
    std::vector<sr::SceneNode*> out;
    if (! tmpl || count == 0) return out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        auto clone = rstd::sync::Arc<SceneNode>::make(
            tmpl->Translate(), tmpl->Scale(), tmpl->Rotation(), tmpl->Name());
        clone->SetSize(tmpl->Size());
        clone->SetPerspective(tmpl->Perspective());
        if (! tmpl->Camera().empty()) clone->SetCamera(tmpl->Camera());
        clone->AddMesh(tmpl->MeshShared());
        clone->ID() = -((i32)i + 1); // negative IDs reserved for clones
        context.shader_updater->CopyNodeData(tmpl, clone.as_ptr());
        if (auto layer = LookupPuppetLayer(context.puppet_layers, tmpl))
            RegisterPuppetLayer(context, clone.as_ptr(), std::move(layer));
        out.push_back(clone.as_ptr());
        // Defer attachment to FinalizeScene so the clones land at the
        // template's z-position (right after it), not at the root front.
        context.layer_clones[tmpl->ID()].push_back(std::move(clone));
    }
    return out;
}

script::ScriptScene& EnsureScriptScene(ParseContext& context) {
    if (! context.script_scene) {
        context.script_scene = std::make_unique<script::ScriptScene>();
        auto layers          = context.puppet_layers;
        context.script_scene->runtime().SetBoneResolvers(
            [layers](SceneNode* node, std::string_view name) -> uint32_t {
                auto     layer = LookupPuppetLayer(layers, node);
                uint32_t index = layer ? layer->boneIndex(name) : 0;
                if (index != 0) return index;

                if (auto fallback = FindPuppetLayerWithBone(layers, RootOf(node), name, index)) {
                    layers->fallback_by_node[node] = std::move(fallback);
                    return index;
                }
                return 0;
            },
            [layers](SceneNode* node,
                     uint32_t   index,
                     double     time) -> std::optional<script::BoneTranslation> {
                auto layer = LookupPuppetLayer(layers, node);
                if (! layer) return std::nullopt;
                auto bone = layer->boneTransform(index, time);
                if (! bone) return std::nullopt;

                node->UpdateTrans();
                Eigen::Affine3f world = Eigen::Affine3f::Identity();
                world.matrix()        = node->ModelTrans().cast<float>();
                auto t                = (world * *bone).translation();
                return script::BoneTranslation { t.x(), t.y(), t.z() };
            });
        if (context.user_properties != nullptr) {
            for (const auto& [key, prop] : *context.user_properties) {
                context.script_scene->runtime().SetUserProperty(key, prop);
            }
        }
    }
    return *context.script_scene;
}

std::optional<float> ScriptValueAsFloat(const script::ScriptValue& value) {
    if (auto* p = std::get_if<script::ScalarValue>(&value)) return static_cast<float>(p->v);
    if (auto* p = std::get_if<script::BoolValue>(&value)) return p->v ? 1.0f : 0.0f;
    if (auto* p = std::get_if<script::Vec2Value>(&value)) return static_cast<float>(p->x);
    if (auto* p = std::get_if<script::Vec3Value>(&value)) return static_cast<float>(p->x);
    return std::nullopt;
}

std::optional<Vector3f> ScriptValueAsVec3(const script::ScriptValue& value,
                                          const Vector3f&            current) {
    Vector3f next = current;
    if (auto* p = std::get_if<script::Vec3Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x),
                          static_cast<float>(p->y),
                          static_cast<float>(p->z) };
    } else if (auto* p = std::get_if<script::Vec2Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x), static_cast<float>(p->y), current.z() };
    } else if (auto* p = std::get_if<script::ScalarValue>(&value)) {
        next.x() = static_cast<float>(p->v);
    } else
        return std::nullopt;
    return next;
}

bool IsFractionSliderProperty(const ParseContext& context, const nlohmann::json& binding) {
    if (! context.user_properties || ! binding.is_object() || ! binding.contains("user") ||
        ! binding.at("user").is_string())
        return false;
    const auto key = binding.at("user").get<std::string>();
    auto       it  = context.user_properties->find(key);
    if (it == context.user_properties->end() || ! it->second.is_object()) return false;
    const auto& prop = it->second;
    return prop.value("type", std::string {}) == "slider" && prop.value("fraction", false);
}

nlohmann::json ScriptPropertiesForField(const ParseContext& context, std::string_view field,
                                        const wpscene::ScriptBinding& binding) {
    nlohmann::json props = binding.properties;
    if (field != "scale" || binding.source.find("/10000") == std::string::npos ||
        ! props.is_object())
        return props;

    for (auto& item : props.items()) {
        if (IsFractionSliderProperty(context, item.value())) {
            item.value()["__scriptValueScale"] = 50.0;
        }
    }
    return props;
}

SceneAnimationKey ToSceneAnimationKey(const wpscene::AnimKeyframe& key) {
    return {
        .frame         = key.frame,
        .value         = key.value,
        .front_enabled = key.front.enabled,
        .front_x       = key.front.x,
        .front_y       = key.front.y,
        .back_enabled  = key.back.enabled,
        .back_x        = key.back.x,
        .back_y        = key.back.y,
    };
}

std::vector<SceneAnimationKey>
ToSceneAnimationAxis(const std::vector<wpscene::AnimKeyframe>& keys) {
    std::vector<SceneAnimationKey> out;
    out.reserve(keys.size());
    for (const auto& key : keys) out.push_back(ToSceneAnimationKey(key));
    std::ranges::sort(out, {}, &SceneAnimationKey::frame);
    return out;
}

SceneAnimationCurve ToSceneAnimationCurve(const wpscene::AnimCurve& curve) {
    SceneAnimationCurve out;
    out.c0       = ToSceneAnimationAxis(curve.c0);
    out.c1       = ToSceneAnimationAxis(curve.c1);
    out.c2       = ToSceneAnimationAxis(curve.c2);
    out.fps      = curve.options.fps;
    out.length   = curve.options.length;
    out.mode     = curve.options.mode;
    out.wraploop = curve.options.wraploop;
    out.relative = curve.relative;
    return out;
}

void AssignCurve(SceneAnimationCurve& dst, const wpscene::FieldBindings& bindings,
                 std::string_view field) {
    auto it = bindings.animations.find(std::string(field));
    if (it != bindings.animations.end()) dst = ToSceneAnimationCurve(it->second);
}

std::optional<SceneCameraLookAtKey> ParseLookAtKey(const nlohmann::json& json) {
    if (! json.is_object()) return std::nullopt;
    SceneCameraLookAtKey key;
    std::array<float, 3> eye {};
    std::array<float, 3> center {};
    std::array<float, 3> up {};
    if (! sr::GetJsonValue(json, "eye", eye, false)) return std::nullopt;
    if (! sr::GetJsonValue(json, "center", center, false)) return std::nullopt;
    if (! sr::GetJsonValue(json, "up", up, false)) return std::nullopt;
    sr::GetJsonValue(json, "timestamp", key.frame, false);
    key.eye    = Vector3f(eye.data());
    key.center = Vector3f(center.data());
    key.up     = Vector3f(up.data());
    return key;
}

std::optional<SceneCameraLookAtTrack> ParseLookAtTrack(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("transforms") || ! json.at("transforms").is_array())
        return std::nullopt;

    SceneCameraLookAtTrack track;
    sr::GetJsonValue(json, "duration", track.duration, false);
    for (const auto& raw_key : json.at("transforms")) {
        auto key = ParseLookAtKey(raw_key);
        if (key) track.keys.push_back(*key);
    }
    if (track.keys.empty()) return std::nullopt;

    std::ranges::sort(track.keys, {}, &SceneCameraLookAtKey::frame);
    if (track.duration <= 0.0f) track.duration = track.keys.back().frame;
    if (track.duration <= 0.0f) track.duration = 1.0f;
    return track;
}

void LoadRootCameraPaths(ParseContext& context, const wpscene::SceneMetadata& sc) {
    if (sc.general.isOrtho || sc.camera.paths.empty() || context.vfs == nullptr) return;

    auto it = context.scene->cameras.find("global_perspective");
    if (it == context.scene->cameras.end()) return;

    auto path               = std::make_shared<SceneCameraPath>();
    path->camera_name       = "global_perspective";
    path->camera            = it->second;
    path->node              = context.global_perspective_camera_node.is_some()
                                  ? (*context.global_perspective_camera_node).as_ptr()
                                  : nullptr;
    path->default_translate = path->node ? path->node->Translate() : Vector3f::Zero();
    path->default_rotation  = path->node ? path->node->Rotation() : Vector3f::Zero();
    path->default_width     = path->camera->Width();
    path->default_height    = path->camera->Height();
    path->default_fov       = path->camera->Fov();
    path->fov_base          = static_cast<float>(path->camera->Fov());
    path->perspective       = true;
    path->enabled           = true;
    path->default_lookat    = true;
    path->default_eye       = Vector3f(sc.camera.eye.data());
    path->default_center    = Vector3f(sc.camera.center.data());
    path->default_up        = Vector3f(sc.camera.up.data());

    for (const auto& rel : sc.camera.paths) {
        auto file = context.vfs->Open("/assets/" + rel);
        if (! file) continue;
        auto json = nlohmann::json::parse(file->ReadAllStr(), nullptr, false);
        if (json.is_discarded() || ! json.contains("paths") || ! json.at("paths").is_array())
            continue;
        for (const auto& raw_track : json.at("paths")) {
            auto track = ParseLookAtTrack(raw_track);
            if (track) path->lookat_tracks.push_back(std::move(*track));
        }
    }

    if (! path->lookat_tracks.empty()) context.scene->camera_paths.push_back(std::move(path));
}
} // namespace

// Walks `fb.scripts` for one parsed object's field bindings and, for the
// supported fields, creates a FieldScript + closure-based Actuator. Text
// bindings are wired by ParseTextObj's own call site (with the layouter
// closure); side-effect-only bindings (`visible`) get the script without an
// actuator so update() still drives scene mutations.
void WireFieldScripts(ParseContext& context, const rstd::sync::Arc<SceneNode>& node_sp,
                      const wpscene::FieldBindings&                   fb,
                      std::function<void(const script::ScriptValue&)> origin_apply = {},
                      std::function<void(const script::ScriptValue&)> scale_apply  = {}) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::NodeTransformTarget tgt = script::NodeTransformTarget::Translate;
        script::FieldKind           kind;
        bool                        has_actuator = true;
        bool                        is_alpha     = false;
        if (field == "origin") {
            tgt  = script::NodeTransformTarget::Translate;
            kind = script::FieldKind::Vec3;
        } else if (field == "scale") {
            tgt  = script::NodeTransformTarget::Scale;
            kind = script::FieldKind::Vec3;
        } else if (field == "angles") {
            tgt  = script::NodeTransformTarget::Rotation;
            kind = script::FieldKind::Vec3;
        } else if (field == "visible") {
            // Side-effect-only script bound to visibility. update() may
            // drive other layers via createLayer + property writes; we
            // don't write a return value back to the node.
            kind         = script::FieldKind::Bool;
            has_actuator = false;
        } else if (field == "alpha") {
            kind     = script::FieldKind::Scalar;
            is_alpha = true;
        } else {
            // text/color/rate/intensity/... are wired elsewhere or not yet supported.
            continue;
        }
        std::string                  sha = utils::genSha1(std::span<const char>(sb.source));
        std::vector<sr::SceneNode*> clones;
        if (unsigned n = DetectAudioFanoutCount(sb.source); n > 1) {
            clones = SpawnLayerClones(context, node, n - 1);
        }
        auto  props = ScriptPropertiesForField(context, field, sb);
        auto* fs    = rt.MakeFieldScript(
            sb.source, sha, kind, props, sb.initial_value, node, std::move(clones));
        if (! fs) continue;
        if (! has_actuator) continue;
        if (is_alpha)
            ss.AddActuator({ fs, script::MakeNodeAlphaApply(node_sp.clone()) });
        else if (field == "origin" && origin_apply)
            ss.AddActuator({ fs, origin_apply });
        else if (field == "scale" && scale_apply)
            ss.AddActuator({ fs, scale_apply });
        else
            ss.AddActuator({ fs, script::MakeNodeTransformApply(node_sp.clone(), tgt) });
    }
}

void WireCameraShakeScripts(ParseContext& context, const wpscene::FieldBindings& fb) {
    if (fb.scripts.empty()) return;

    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Scalar;
        if (field == "camerashake") {
            kind = script::FieldKind::Bool;
        } else if (field != "camerashakeamplitude" && field != "camerashakespeed" &&
                   field != "camerashakeroughness") {
            continue;
        }

        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, sb.initial_value);
        if (! fs) continue;

        auto* updater = context.shader_updater;
        ss.AddActuator({ fs, [updater, field](const script::ScriptValue& value) {
                            if (! updater) return;
                            auto scalar = ScriptValueAsFloat(value);
                            if (! scalar) return;
                            if (field == "camerashake")
                                updater->SetCameraShakeEnabled(*scalar >= 0.5f);
                            else if (field == "camerashakeamplitude")
                                updater->SetCameraShakeAmplitude(*scalar);
                            else if (field == "camerashakespeed")
                                updater->SetCameraShakeSpeed(*scalar);
                            else if (field == "camerashakeroughness")
                                updater->SetCameraShakeRoughness(*scalar);
                        } });
    }
}

void WireCameraFieldScripts(ParseContext& context, const rstd::sync::Arc<SceneNode>& node_sp,
                            std::shared_ptr<SceneCamera>     camera,
                            std::shared_ptr<SceneCameraPath> camera_path,
                            const wpscene::FieldBindings& fb, const Vector3f& translate_bias,
                            const Vector3f& rotation_bias) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Vec3;
        if (field == "visible") {
            kind = script::FieldKind::Bool;
        } else if (field != "origin" && field != "angles") {
            continue;
        }

        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto* fs = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, sb.initial_value, node);
        if (! fs) continue;

        if (field == "origin") {
            ss.AddActuator(
                { fs,
                  [node, camera, camera_path, translate_bias](const script::ScriptValue& value) {
                      Vector3f current = camera_path ? camera_path->origin_base
                                                     : node->Translate() - translate_bias;
                      auto     next    = ScriptValueAsVec3(value, current);
                      if (next) {
                          if (camera_path) camera_path->origin_base = *next;
                          node->SetTranslate(translate_bias + *next);
                          if (camera) camera->Update();
                      }
                  } });
        } else if (field == "angles") {
            ss.AddActuator(
                { fs, [node, camera, camera_path, rotation_bias](const script::ScriptValue& value) {
                     constexpr float kRadToDeg = 180.0f / rstd::f32_::consts::PI;
                     constexpr float kDegToRad = rstd::f32_::consts::PI / 180.0f;
                     Vector3f        current   = camera_path ? camera_path->rotation_base
                                                             : node->Rotation() - rotation_bias;
                     current *= kRadToDeg;
                     auto next = ScriptValueAsVec3(value, current);
                     if (next) {
                         if (camera_path) camera_path->rotation_base = *next * kDegToRad;
                         node->SetRotation(rotation_bias + *next * kDegToRad);
                         if (camera) camera->Update();
                     }
                 } });
        }
    }
}

// SceneObjectVar is exported from :scene_stages.

namespace
{
// mapRate < 1.0
void GenCardMesh(SceneMesh& mesh, const std::array<uint16_t, 2> size,
                 const std::array<float, 2> mapRate = { 1.0f, 1.0f }) {
    float left   = -(size[0] / 2.0f);
    float right  = size[0] / 2.0f;
    float bottom = -(size[1] / 2.0f);
    float top    = size[1] / 2.0f;
    float z      = 0.0f;

    float tw = mapRate[0], th = mapRate[1];

    // clang-format off
	const std::array pos = {
		left,  top, z,
		left, bottom, z,
		right,  top, z,
		right, bottom, z,
	};
	const std::array texCoord = {
		0.0f, 0.0f,
		0.0f, th,
		tw, 0.0f,
		tw, th,
	};
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), 4);
    vertex.SetVertex(WE_IN_POSITION, pos);
    vertex.SetVertex(WE_IN_TEXCOORD, texCoord);
    mesh.AddVertexArray(std::move(vertex));
}

void SetParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                     bool thick_format, bool geometry_shader) {
    (void)particle;
    std::vector<VertexAttrSpec> specs {
        VAttr::Position,
        VAttr::TexCoordVec4,
        VAttr::Color,
    };
    if (thick_format) specs.push_back(VAttr::TexCoordVec4C1);
    if (geometry_shader) {
        mesh.SetPrimitive(MeshPrimitive::POINT);
        mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), count));
    } else {
        specs.push_back(VAttr::TexCoordC2);
        mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), count * 4));
        mesh.AddIndexArray(SceneIndexArray(count * 6));
    }
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

bool IsLayerCompositeShader(std::string_view shader) {
    return shader == "genericimage" || shader == "genericimage2" || shader == "genericimage3" ||
           shader == "genericimage4" || shader == "passthrough";
}

bool PlatformSupportsGeometryShaders() {
    // Metal has no geometry-shader stage; MoltenVK can't lower them.
    return false;
}

void SetRopeParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                         bool thick_format, bool geometry_shader) {
    (void)particle;
    std::vector<VertexAttrSpec> specs {
        VAttr::PositionVec4,
        VAttr::TexCoordVec4,
        VAttr::TexCoordVec4C1,
    };
    if (thick_format) {
        specs.push_back(VAttr::TexCoordVec4C2);
        specs.push_back(VAttr::TexCoordVec4C3);
        if (! geometry_shader) {
            specs.push_back({ WE_IN_TEXCOORDC4, VertexType::FLOAT2 });
        }
    } else {
        specs.push_back(VAttr::TexCoordVec3C2);
        if (! geometry_shader) {
            specs.push_back({ WE_IN_TEXCOORDC3, VertexType::FLOAT2 });
        }
    }
    specs.push_back(VAttr::Color);
    if (geometry_shader) {
        mesh.SetPrimitive(MeshPrimitive::POINT);
        mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), count));
    } else {
        mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), count * 4));
        mesh.AddIndexArray(SceneIndexArray(count * 6));
    }
    mesh.GetVertexArray(0).SetOption(WE_PRENDER_ROPE, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

struct ParticleRenderDesc {
    bool rope { false };
    bool trail { false };
    bool geometry_shader { false };
};

ParticleRenderDesc DescribeParticleRender(const wpscene::ParticleRender& render) {
    ParticleRenderDesc desc;
    desc.rope            = render.name == "rope";
    desc.trail           = send_with(render.name, "trail");
    desc.geometry_shader =
        PlatformSupportsGeometryShaders() && (desc.rope || render.name == "sprite" || desc.trail);
    return desc;
}

ParticleAnimationMode ToAnimMode(const std::string& str) {
    if (str == "randomframe")
        return ParticleAnimationMode::RANDOMONE;
    else if (str == "sequence")
        return ParticleAnimationMode::SEQUENCE;
    else {
        return ParticleAnimationMode::SEQUENCE;
    }
}

void LoadControlPoint(ParticleSubSystem& pSys, const wpscene::Particle& wp) {
    std::span<ParticleControlpoint> pcs = pSys.Controlpoints();
    usize                           s   = std::min(pcs.size(), wp.controlpoints.size());
    for (usize i = 0; i < s; i++) {
        pcs[i].base_offset =
            Eigen::Vector3d { array_cast<double>(wp.controlpoints[i].offset).data() };
        pcs[i].offset = pcs[i].base_offset;
        pcs[i].link_mouse =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     std::shared_ptr<wpscene::ParticleInstanceoverride> over_state) {
    for (const auto& ini : wp.initializers) {
        pSys.AddInitializer(ParticleProgramCompiler::genParticleInitOp(ini));
    }
    if (over_state->enabled) pSys.AddInitializer(ParticleProgramCompiler::genOverrideInitOp(over_state));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  std::shared_ptr<wpscene::ParticleInstanceoverride> over_state) {
    for (const auto& op : wp.operators) {
        pSys.AddOperator(ParticleProgramCompiler::genParticleOperatorOp(op, over_state));
    }
}
void LoadEmitter(ParticleSubSystem& pSys, const wpscene::Particle& wp, float count,
                 bool render_rope, i32 cp_start_index = 0) {
    // Sort was used by the rope generator to keep adjacent-particle pairs
    // packed at the front of m_particles[]. With per-particle trail history
    // each slot is independent, so sort is unnecessary and would shuffle the
    // slot<->trail mapping mid-frame.
    (void)render_rope;
    bool sort = false;
    for (const auto& em : wp.emitters) {
        auto newEm = em;
        newEm.rate *= count;
        // controlpointstartindex on the parent's child entry biases the
        // child emitter's controlpoint index. Without this, a child JSON
        // authored as `controlpoint: 0` always samples cps[0] even when WE
        // wired it through `cps[cp_start_index]`.
        if (newEm.controlpoint >= 0) newEm.controlpoint += cp_start_index;
        pSys.AddEmitter(ParticleProgramCompiler::genParticleEmittOp(newEm, sort));
    }
}

ParticleSubSystem::SpawnType ParseSpawnType(std::string_view str) {
    using ST = ParticleSubSystem::SpawnType;
    ST type { ST::STATIC };
    if (str == "eventfollow") {
        type = ST::EVENT_FOLLOW;
    } else if (str == "eventspawn") {
        type = ST::EVENT_SPAWN;
    } else if (str == "eventdeath") {
        type = ST::EVENT_DEATH;
    }
    return type;
};

BlendMode ParseBlendMode(std::string_view str) {
    BlendMode bm;
    if (str == "translucent") {
        bm = BlendMode::Translucent;
    } else if (str == "additive") {
        bm = BlendMode::Additive;
    } else if (str == "normal") {
        bm = BlendMode::Normal;
    } else if (str == "disabled") {
        bm = BlendMode::Disable;
    } else {
        bm = BlendMode::Normal;
        rstd_error("unknown blending: {}", str);
    }
    return bm;
}

bool ParseEnabled(std::string_view str) { return str == "enabled"; }

CullMode ParseCullMode(std::string_view str) {
    if (str == "back" || str == "normal") return CullMode::Back;
    if (str == "front") return CullMode::Front;
    if (str == "nocull" || str == "none" || str.empty()) return CullMode::None;
    rstd_error("unknown cullmode: {}", str);
    return CullMode::None;
}

void ParseSpecTexName(std::string& name, const wpscene::Material& wpmat, const WPShaderInfo& sinfo,
                      const Scene& scene) {
    if (IsSpecTex(name)) {
        if (name == WE_FULL_FRAME_BUFFER) {
            name = SpecTex_Default;
            if (wpmat.shader == "genericimage2" && ! exists(sinfo.combos, "BLENDMODE")) name = "";
            /*
            if(wpmat.shader == "genericparticle") {
                name = "_rt_ParticleRefract";
            }
            */
        } else if (auto wpid = ParseImageLayerCompositeId(name)) {
            rstd_info("link tex \"{}\"", name);
            name = GenLinkTex(*wpid);
        } else if (sstart_with(name, WE_MIP_MAPPED_FRAME_BUFFER)) {
        } else if (sstart_with(name, WE_SHADOW_ATLAS_PREFIX)) {
            name.clear();
        } else if (sstart_with(name, SR_BLOOM_MIP_PREFIX)) {
        } else if (sstart_with(name, WE_REFLECTION_PREFIX)) {
            name.clear();
        } else if (sstart_with(name, SR_EFFECT_PPONG_PREFIX)) {
        } else if (sstart_with(name, WE_HALF_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_QUARTER_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_FULL_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_EIGHT_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_VOLUMETRICS_PREFIX) ||
                   sstart_with(name, WE_QUARTER_FORCE_RG_PREFIX) ||
                   sstart_with(name, WE_BLOOM_PREFIX) ||
                   sstart_with(name, WE_QUARTER_FRAME_BUFFER_PREFIX) ||
                   sstart_with(name, WE_EIGHTH_FRAME_BUFFER_PREFIX)) {
            name.clear();
        } else if (scene.renderTargets.count(name) > 0) {
            // an effect-local fbo registered with a non-conventional name
            // (e.g. WE DOF's `_rt__coc_<addr>`) — already a valid RT.
        } else {
            rstd_warn("ignoring unsupported special tex \"{}\"", name);
            name.clear();
        }
    }
}

bool LoadMaterial(fs::VFS& vfs, const wpscene::Material& wpmat, Scene* pScene, SceneNode* pNode,
                  SceneMaterial* pMaterial, SceneUniformNodeData* pSvData,
                  WPShaderInfo* pWPShaderInfo = nullptr, bool enable_geometry_shader = false,
                  bool* out_geometry_shader = nullptr) {
    (void)pNode;
    if (out_geometry_shader) *out_geometry_shader = false;

    auto& svData   = *pSvData;
    auto& material = *pMaterial;

    std::unique_ptr<WPShaderInfo> upWPShaderInfo(nullptr);
    if (pWPShaderInfo == nullptr) {
        upWPShaderInfo = std::make_unique<WPShaderInfo>();
        pWPShaderInfo  = upWPShaderInfo.get();
    }

    SceneMaterialCustomShader materialShader;

    auto& shader = materialShader.shader;
    shader       = std::make_shared<SceneShader>();
    shader->name = wpmat.shader;

    std::string shaderPath("/assets/shaders/" + wpmat.shader);

    std::vector<WPShaderUnit> sd_units;
    sd_units.push_back({
        .stage           = ShaderType::VERTEX,
        .src             = fs::GetFileContent(vfs, shaderPath + ".vert"),
        .preprocess_info = {},
    });
    if (enable_geometry_shader && PlatformSupportsGeometryShaders()) {
        std::string geom_path = shaderPath + ".geom";
        if (vfs.Contains(geom_path)) {
            sd_units.push_back({
                .stage           = ShaderType::GEOMETRY,
                .src             = fs::GetFileContent(vfs, geom_path),
                .preprocess_info = {},
            });
            pWPShaderInfo->combos["GS_ENABLED"] = "1";
            if (out_geometry_shader) *out_geometry_shader = true;
        }
    }
    sd_units.push_back({
        .stage           = ShaderType::FRAGMENT,
        .src             = fs::GetFileContent(vfs, shaderPath + ".frag"),
        .preprocess_info = {},
    });

    std::vector<WPShaderTexInfo>                 texinfos;
    std::unordered_map<std::string, ImageHeader> texHeaders;
    for (const auto& el : wpmat.textures) {
        if (el.empty()) {
            texinfos.push_back({ false });
        } else if (! IsSpecTex(el)) {
            const auto& texh = pScene->imageParser->ParseHeader(el);
            texHeaders[el]   = texh;
            if (texh.extraHeader.count("compo1") == 0) {
                texinfos.push_back({ false });
                continue;
            }
            texinfos.push_back({ true,
                                 {
                                     (bool)texh.extraHeader.at("compo1").val,
                                     (bool)texh.extraHeader.at("compo2").val,
                                     (bool)texh.extraHeader.at("compo3").val,
                                 } });
        } else
            texinfos.push_back({ true });
    }

    for (auto& unit : sd_units) {
        unit.src = MaterialProgramCompiler::PreShaderSrc(vfs, unit.src, pWPShaderInfo, texinfos);
    }

    shader->default_uniforms = pWPShaderInfo->svs;

    for (const auto& el : wpmat.combos) {
        pWPShaderInfo->combos[el.first] = std::to_string(el.second);
    }

    auto textures = wpmat.textures;
    if (pWPShaderInfo->defTexs.size() > 0) {
        for (auto& t : pWPShaderInfo->defTexs) {
            if (textures.size() > t.first) {
                if (! textures.at(t.first).empty()) continue;
            } else {
                textures.resize(t.first + 1);
            }
            textures[t.first] = t.second;
        }
    }

    for (usize i = 0; i < textures.size(); i++) {
        std::string name                   = textures.at(i);
        bool        unsupported_reflection = sstart_with(name, WE_REFLECTION_PREFIX);
        ParseSpecTexName(name, wpmat, *pWPShaderInfo, *pScene);
        if (unsupported_reflection && name.empty()) {
            pWPShaderInfo->combos["reflection"] = "0";
            pWPShaderInfo->combos["REFLECTION"] = "0";
        }
        material.textures.push_back(name);
        material.defines.push_back("g_Texture" + std::to_string(i));
        if (name.empty()) {
            continue;
        }

        std::array<i32, 4> resolution {};
        if (IsSpecTex(name)) {
            if (IsSpecLinkTex(name)) {
                svData.renderTargets.push_back({ i, name });
            } else if (pScene->renderTargets.count(name) == 0) {
                rstd_error("{} not found in render targes", name);
            } else {
                svData.renderTargets.push_back({ i, name });
                const auto& rt = pScene->renderTargets.at(name);
                resolution     = { rt.width, rt.height, rt.width, rt.height };
            }
        } else {
            const ImageHeader& texh = texHeaders.count(name) == 0
                                          ? pScene->imageParser->ParseHeader(name)
                                          : texHeaders.at(name);
            if (i == 0) {
                if (texh.format == TextureFormat::R8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_R8";
                else if (texh.format == TextureFormat::RG8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_RG88";
            }
            if (texh.mipmap_larger) {
                resolution = { texh.width, texh.height, texh.mapWidth, texh.mapHeight };
            } else {
                resolution = { texh.mapWidth, texh.mapHeight, texh.mapWidth, texh.mapHeight };
            }

            if (pScene->textures.count(name) == 0) {
                SceneTexture stex;
                stex.sample = texh.sample;
                stex.url    = name;
                if (texh.isSprite) {
                    stex.isSprite   = texh.isSprite;
                    stex.spriteAnim = texh.spriteAnim;
                }
                pScene->textures[name] = stex;
            }
            if ((pScene->textures.at(name)).isSprite) {
                material.hasSprite = true;
                const auto& f1     = texh.spriteAnim.GetCurFrame();
                if (wpmat.shader == "genericparticle" || wpmat.shader == "genericropeparticle") {
                    pWPShaderInfo->combos["SPRITESHEET"] = "1";
                    pWPShaderInfo->combos["THICKFORMAT"] = "1";
                    if (algorism::IsPowOfTwo((u32)texh.width) &&
                        algorism::IsPowOfTwo((u32)texh.height)) {
                        pWPShaderInfo->combos["SPRITESHEETBLENDNPOT"] = "1";
                        resolution[2] = resolution[0] - resolution[0] % (int)f1.width;
                        resolution[3] = resolution[1] - resolution[1] % (int)f1.height;
                    }
                    materialShader.constValues["g_RenderVar1"] = std::array {
                        f1.xAxis[0], f1.yAxis[1], (float)(texh.spriteAnim.numFrames()), f1.rate
                    };
                }
            }
        }
        if (! resolution.empty()) {
            const std::string gResolution = WE_GLTEX_RESOLUTION_NAMES[i];

            materialShader.constValues[gResolution] = array_cast<float>(resolution);
        }
    }
    if (exists(pWPShaderInfo->combos, "LIGHTING")) {
        // pWPShaderInfo->combos["PRELIGHTING"] =
        // pWPShaderInfo->combos.at("LIGHTING");
    }

    if (! MaterialProgramCompiler::CompileToSpv(
            pScene->scene_id, sd_units, shader->codes, vfs, pWPShaderInfo, texinfos)) {
        return false;
    }

    material.blenmode    = ParseBlendMode(wpmat.blending);
    material.depth_test  = ParseEnabled(wpmat.depthtest);
    material.depth_write = ParseEnabled(wpmat.depthwrite);
    material.cull_mode   = ParseCullMode(wpmat.cullmode);

    // FS is always the last unit (VS may be followed by optional GS, then FS).
    const auto& fs_active = sd_units.back().preprocess_info.active_tex_slots;
    for (unsigned i = 0; i < material.textures.size(); i++) {
        if (! exists(fs_active, i)) material.textures[i].clear();
    }

    for (const auto& el : pWPShaderInfo->baseConstSvs) {
        materialShader.constValues[el.first] = el.second;
    }
    material.customShader = materialShader;
    material.name         = wpmat.shader;

    // u_* user-variable uniforms: stage records into pWPShaderInfo so the
    // caller can register them into `Scene::shader_user_var_index` AFTER
    // moving `material` into a shared_ptr. Registering here would store a
    // stack-local pointer, freed once `AddMaterial(std::move(material))`
    // runs — a use-after-free as soon as ApplyUserPropertyToShaders fires.
    // Default values still seed constValues here; the values get carried
    // along by the move into the shared_ptr.
    for (const auto& var : pWPShaderInfo->scalar_uniforms) {
        if (! var.is_user || var.material.empty()) continue;
        pWPShaderInfo->user_var_staging.push_back({ var.material, var.name, var.default_value });
        if (! var.default_value.is_null()) {
            ShaderValue sv;
            const auto& v = var.default_value;
            if (v.is_string()) {
                std::vector<float> tmp;
                sr::GetJsonValue(v, tmp);
                sv = std::span<const float>(tmp);
            } else if (v.is_number()) {
                sv.setSize(1);
                sr::GetJsonValue(v, sv[0]);
            }
            if (sv.size() > 0) material.customShader.constValues[var.name] = sv;
        }
    }

    return true;
}

std::string ResolveShaderMaterialKey(const WPShaderInfo& info, const std::string& material_key) {
    if (auto it = info.alias.find(material_key); it != info.alias.end()) return it->second;

    for (const auto& el : info.alias) {
        if (el.second.size() > 2 && el.second.substr(2) == material_key) return el.second;
    }
    return {};
}

bool IsShaderPositionUniform(const WPShaderInfo& info, const std::string& glname) {
    for (const auto& var : info.scalar_uniforms) {
        if (var.name == glname) return var.position;
    }
    return false;
}

bool UsesEffectQuadPositionSpace(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/spin") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == 1;
}

// Register a (material, shader-info, wpmat) triple into the scene-wide user
// variable index. Must be called AFTER the SceneMaterial has been moved into
// a shared_ptr (e.g. `mesh->AddMaterial(std::move(local))`) and `stable_mat`
// points to `mesh->Material()` / `m_materials.back().get()`. Wires up:
//   (1) Direct-route u_* whose shader annotation's `material` field is the
//       wallpaper-level project.json key (the legacy convention).
//   (2) Instance-bound effect-internal keys from
//       `wpmat.constantshadervalues_user`, mapped through `info.alias` to
//       the GLSL uniform name.
//   (3) Legacy material `usershadervalues` bindings: project key to shader
//       material key.
void RegisterShaderUserVarIndex(Scene* pScene, SceneMaterial* stable_mat,
                                const wpscene::Material& wpmat, const WPShaderInfo& info) {
    if (! pScene || ! stable_mat) return;
    for (const auto& rec : info.user_var_staging) {
        pScene->shader_user_var_index[rec.material].push_back({ stable_mat, rec.name });
    }
    for (const auto& [effect_key, wallpaper_key] : wpmat.constantshadervalues_user) {
        // Resolve effect-internal key → GLSL uniform name via alias.
        // LoadConstvalue's fallback search (alias entry whose value, after
        // dropping the leading "u_", matches the key) is honored here too.
        std::string glname = ResolveShaderMaterialKey(info, effect_key);
        if (glname.empty()) {
            rstd_warn("user binding '{}' → no shader uniform with material='{}'",
                      wallpaper_key,
                      effect_key);
            continue;
        }
        pScene->shader_user_var_index[wallpaper_key].push_back({ stable_mat, glname });
    }
    for (const auto& [wallpaper_key, material_key] : wpmat.user_shader_values) {
        std::string glname = ResolveShaderMaterialKey(info, material_key);
        if (glname.empty()) {
            rstd_warn("user shader value '{}' -> no shader uniform with material='{}'",
                      wallpaper_key,
                      material_key);
            continue;
        }
        pScene->shader_user_var_index[wallpaper_key].push_back({ stable_mat, glname });
    }
}

void LoadAlignment(SceneNode& node, std::string_view align, Vector2f size) {
    Vector3f trans = node.Translate();
    size *= 0.5f;
    size.y() *= 1.0f;

    auto contains = [&](std::string_view s) {
        return align.find(s) != std::string::npos;
    };

    // topleft top center ...
    if (contains("top")) trans.y() -= size.y();
    if (contains("left")) trans.x() += size.x();
    if (contains("right")) trans.x() -= size.x();
    if (contains("bottom")) trans.y() += size.y();

    node.SetTranslate(trans);
}

// Apply effect-pass `bind` overrides onto wpmat.textures by index, using
// fboMap to resolve effect-local FBO names to actual scene RT keys.
void ApplyTextureBinds(wpscene::Material&                                  wpmat,
                       std::span<const wpscene::MaterialPassBindItem>      binds,
                       const std::unordered_map<std::string, std::string>& fboMap) {
    for (const auto& el : binds) {
        if (fboMap.count(el.name) == 0) {
            rstd_error("fbo {} not found", el.name);
            continue;
        }
        if (wpmat.textures.size() <= (usize)el.index) wpmat.textures.resize((usize)el.index + 1);
        wpmat.textures[(usize)el.index] = fboMap.at(el.name);
    }
}

bool IsSystemMediaTextureBinding(const nlohmann::json& binding) {
    if (! binding.is_object()) return false;
    auto type = binding.find("type");
    auto name = binding.find("name");
    if (type == binding.end() || name == binding.end()) return false;
    if (! type->is_string() || ! name->is_string()) return false;
    if (type->get<std::string>() != "system") return false;
    const auto value = name->get<std::string>();
    return value == "$mediaThumbnail" || value == "$mediaPreviousThumbnail";
}

std::string ResolveSceneTextureProperty(const ParseContext& context, std::string_view key) {
    if (! context.user_properties) return {};
    auto it = context.user_properties->find(std::string(key));
    if (it == context.user_properties->end()) return {};

    const auto& prop = it->second;
    if (prop.is_string()) {
        auto value = prop.get<std::string>();
        return value.empty() ? std::string {} : value;
    }
    if (! prop.is_object()) return {};

    std::string type;
    if (prop.contains("type") && prop.at("type").is_string())
        type = prop.at("type").get<std::string>();
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return {};
    if (! prop.contains("value") || ! prop.at("value").is_string()) return {};

    const auto value = prop.at("value").get<std::string>();
    return value.empty() ? std::string {} : value;
}

std::string ResolveUserTextureProperty(const ParseContext& context, const nlohmann::json& binding) {
    if (! binding.is_string()) return {};
    return ResolveSceneTextureProperty(context, binding.get<std::string>());
}

std::string ResolveMaterialTextureSlot(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    std::string fallback;
    if (slot < material.textures.size()) fallback = material.textures[slot];
    if (slot >= material.usertextures.size()) return fallback;

    if (auto prop = ResolveUserTextureProperty(context, material.usertextures[slot]);
        ! prop.empty())
        return prop;
    return fallback;
}

std::string ResolveLinkedImageFallback(const ParseContext& context, std::string_view texture) {
    std::optional<std::uint32_t> linked_id = ParseImageLayerCompositeId(texture);
    if (! linked_id && IsSpecLinkTex(texture)) linked_id = ParseLinkTex(texture);
    if (! linked_id) return {};

    auto it = context.image_texture_fallbacks.find(static_cast<std::int32_t>(*linked_id));
    if (it == context.image_texture_fallbacks.end()) return {};
    return it->second;
}

std::string ResolveSystemMediaFallback(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    if (slot >= material.textures.size()) return {};
    return ResolveLinkedImageFallback(context, material.textures[slot]);
}

void ApplyUserTextureBindings(ParseContext& context, wpscene::Material& material) {
    for (usize i = 0; i < material.usertextures.size(); ++i) {
        const auto& binding = material.usertextures[i];
        if (binding.is_null()) continue;

        std::string resolved = ResolveUserTextureProperty(context, binding);
        if (resolved.empty() && IsSystemMediaTextureBinding(binding)) {
            resolved = ResolveSystemMediaFallback(context, material, i);
        }
        if (resolved.empty()) continue;

        if (material.textures.size() <= i) material.textures.resize(i + 1);
        material.textures[i] = std::move(resolved);
    }
}

void IndexImageTextureFallbacks(ParseContext& context, std::span<SceneObjectVar> scene_objs) {
    context.image_texture_fallbacks.clear();
    for (const auto& obj : scene_objs) {
        if (const auto* image = std::get_if<wpscene::ImageObject>(&obj)) {
            auto texture = ResolveMaterialTextureSlot(context, image->material, 0);
            if (! texture.empty()) context.image_texture_fallbacks[image->id] = std::move(texture);
        }
    }
}

void LoadConstvalue(SceneMaterial& material, const wpscene::Material& wpmat,
                    const WPShaderInfo& info) {
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name  = cs.first;
        const std::vector<float>& value = cs.second;
        std::string               glname;
        if (info.alias.count(name) != 0) {
            glname = info.alias.at(name);
        } else {
            for (const auto& el : info.alias) {
                if (el.second.substr(2) == name) {
                    glname = el.second;
                    break;
                }
            }
        }
        if (glname.empty()) {
            rstd_error("ShaderValue: {} not found in glsl", name);
        } else {
            std::vector<float> const_value = value;
            if (UsesEffectQuadPositionSpace(wpmat) && IsShaderPositionUniform(info, glname) &&
                const_value.size() >= 2) {
                const_value[0] = const_value[0] * 2.0f - 1.0f;
                const_value[1] = const_value[1] * 2.0f - 1.0f;
            }
            material.customShader.constValues[glname] = const_value;
        }
    }
}

// parse

void ParseCamera(ParseContext& context, wpscene::SceneMetadata& sc) {
    auto& scene   = *context.scene;
    auto& general = sc.general;
    // effect camera
    scene.cameras["effect"]    = std::make_shared<SceneCamera>(2, 2, -1.0f, 1.0f);
    context.effect_camera_node = rstd::Some(rstd::sync::Arc<SceneNode>::make()); // at 0,0,0
    scene.cameras.at("effect")->AttatchNode((*context.effect_camera_node).as_ptr());
    scene.sceneGraph->AppendChild((*context.effect_camera_node).clone());

    // global camera
    scene.cameras["global"] = std::make_shared<SceneCamera>((context.ortho_w / (i32)general.zoom),
                                                            (context.ortho_h / (i32)general.zoom),
                                                            -5000.0f,
                                                            5000.0f);
    scene.activeCamera      = scene.cameras.at("global").get();
    Vector3f cori { (float)context.ortho_w / 2.0f, (float)context.ortho_h / 2.0f, 0 },
        cscale { 1.0f, 1.0f, 1.0f }, cangle(Vector3f::Zero());

    context.global_camera_node = rstd::Some(rstd::sync::Arc<SceneNode>::make(cori, cscale, cangle));
    scene.activeCamera->AttatchNode((*context.global_camera_node).as_ptr());
    scene.sceneGraph->AppendChild((*context.global_camera_node).clone());

    scene.cameras["global_perspective"] =
        std::make_shared<SceneCamera>((float)context.ortho_w / (float)context.ortho_h,
                                      general.nearz,
                                      general.farz,
                                      algorism::CalculatePersperctiveFov(1000.0f, context.ortho_h));

    Vector3f cperori = cori;
    cperori[2]       = 1000.0f;
    context.global_perspective_camera_node =
        rstd::Some(rstd::sync::Arc<SceneNode>::make(cperori, cscale, cangle));
    scene.cameras["global_perspective"]->AttatchNode(
        (*context.global_perspective_camera_node).as_ptr());
    scene.sceneGraph->AppendChild((*context.global_perspective_camera_node).clone());

    // Perspective scene (orthogonalprojection==null). The content is authored
    // in WE world units around the origin and viewed by an explicit eye/center
    // camera, not the 2D pixel-space placement above. Drive global_perspective
    // from scene.camera + general.fov and make it the active camera so every
    // layer (and its composite) renders under the same world-space view.
    if (! general.isOrtho) {
        auto&    per = *scene.cameras.at("global_perspective");
        Vector3d eye { sc.camera.eye[0], sc.camera.eye[1], sc.camera.eye[2] };
        Vector3d center { sc.camera.center[0], sc.camera.center[1], sc.camera.center[2] };
        Vector3d up { sc.camera.up[0], sc.camera.up[1], sc.camera.up[2] };
        per.SetLookAt(eye, center, up);
        per.SetFov(general.perspectiveoverridefov > 0.0f ? general.perspectiveoverridefov
                                                         : general.fov);
        per.SetAspect((double)context.ortho_w / (double)context.ortho_h);
        scene.activeCamera = scene.cameras.at("global_perspective").get();
        LoadRootCameraPaths(context, sc);
    }
}

void ParseCameraObj(ParseContext& context, wpscene::CameraObject& cam) {
    auto& scene           = *context.scene;
    bool  use_perspective = false;
    auto  per_it          = scene.cameras.find("global_perspective");
    if (per_it != scene.cameras.end() && scene.activeCamera == per_it->second.get())
        use_perspective = true;

    std::string camera_name = use_perspective ? "global_perspective" : "global";
    auto        it          = scene.cameras.find(camera_name);
    if (it == scene.cameras.end()) return;

    auto       camera = it->second;
    SceneNode* default_node =
        use_perspective
            ? (context.global_perspective_camera_node.is_some()
                   ? (*context.global_perspective_camera_node).as_ptr()
                   : nullptr)
            : (context.global_camera_node.is_some() ? (*context.global_camera_node).as_ptr()
                                                    : nullptr);
    if (default_node == nullptr) {
        auto attached = camera->GetAttachedNode();
        if (attached.is_some()) default_node = attached.unwrap();
    }
    if (default_node == nullptr) return;

    double   default_width     = camera->Width();
    double   default_height    = camera->Height();
    double   default_fov       = camera->Fov();
    Vector3f default_translate = default_node->Translate();
    Vector3f default_rotation  = default_node->Rotation();
    Vector3f origin { cam.origin[0], cam.origin[1], cam.origin[2] };
    Vector3f angles { cam.angles[0], cam.angles[1], cam.angles[2] };
    Vector3f path_translate_bias = use_perspective ? Vector3f::Zero() : default_translate;
    Vector3f path_rotation_bias  = use_perspective ? Vector3f::Zero() : default_rotation;

    auto node = rstd::sync::Arc<SceneNode>::make(
        path_translate_bias + origin, Vector3f::Ones(), path_rotation_bias + angles, cam.name);
    node->ID() = cam.id;
    if (! cam.visible) node->SetVisible(false);
    if (! cam.visible_user_key.empty()) node->SetVisibleUserKey(cam.visible_user_key);

    if (cam.visible) camera->AttatchNode(node.as_ptr());
    if (use_perspective) {
        camera->SetAllowCameraShake(false);
        if (cam.fov > 0.0f) camera->SetFov(cam.fov);
        camera->SetAspect((double)context.ortho_w / (double)context.ortho_h);
        scene.activeCamera = camera.get();
    }

    auto path                 = std::make_shared<SceneCameraPath>();
    path->camera_name         = camera_name;
    path->camera              = camera;
    path->node                = node.as_ptr();
    path->default_translate   = default_translate;
    path->default_rotation    = default_rotation;
    path->path_translate_bias = path_translate_bias;
    path->path_rotation_bias  = path_rotation_bias;
    path->default_width       = default_width;
    path->default_height      = default_height;
    path->default_fov         = default_fov;
    path->origin_base         = origin;
    path->rotation_base       = angles;
    path->zoom_base           = cam.zoom;
    path->fov_base            = cam.fov;
    path->perspective         = use_perspective;
    path->enabled             = cam.visible;
    AssignCurve(path->origin_curve, cam.field_bindings, "origin");
    AssignCurve(path->rotation_curve, cam.field_bindings, "angles");
    AssignCurve(path->zoom_curve, cam.field_bindings, "zoom");
    AssignCurve(path->fov_curve, cam.field_bindings, "fov");
    scene.camera_paths.push_back(path);
    if (! cam.visible_user_key.empty())
        scene.camera_path_user_index[cam.visible_user_key].push_back(path);

    WireCameraFieldScripts(
        context, node, camera, path, cam.field_bindings, path_translate_bias, path_rotation_bias);
    context.node_id_map[cam.id] = { cam.parent, rstd::Some(node.clone()) };
}

void InitContext(ParseContext& context, fs::VFS& vfs, wpscene::SceneMetadata& sc) {
    context.scene            = std::make_shared<Scene>();
    context.vfs              = &vfs;
    auto& scene              = *context.scene;
    scene.imageParser        = std::make_unique<TextureAssetDecoder>(&vfs);
    scene.paritileSys->gener = std::make_unique<ParticleGeometryBuilder>();
    scene.shaderValueUpdater = std::make_unique<SceneUniformUpdater>(&scene);
    GenCardMesh(scene.default_effect_mesh, { 2, 2 });
    context.shader_updater = static_cast<SceneUniformUpdater*>(scene.shaderValueUpdater.get());

    scene.clearColor = sc.general.clearcolor;
    if (auto it = sc.general.user_bindings.find("clearcolor");
        it != sc.general.user_bindings.end()) {
        scene.clearColorUserKey = it->second;
    }
    scene.ortho[0]  = sc.general.orthogonalprojection.width;
    scene.ortho[1]  = sc.general.orthogonalprojection.height;
    context.ortho_w = scene.ortho[0];
    context.ortho_h = scene.ortho[1];

    {
        auto& gb              = context.global_base_uniforms;
        gb["g_ViewUp"]        = std::array { 0.0f, 1.0f, 0.0f };
        gb["g_ViewRight"]     = std::array { 1.0f, 0.0f, 0.0f };
        gb["g_ViewForward"]   = std::array { 0.0f, 0.0f, -1.0f };
        gb["g_EyePosition"]   = std::array { 0.0f, 0.0f, 0.0f };
        gb["g_TexelSize"]     = std::array { 1.0f / 1920.0f, 1.0f / 1080.0f };
        gb["g_TexelSizeHalf"] = std::array { 1.0f / 1920.0f / 2.0f, 1.0f / 1080.0f / 2.0f };

        gb["g_LightAmbientColor"]  = sc.general.ambientcolor;
        gb["g_LightSkylightColor"] = sc.general.skylightcolor;
        gb["g_NormalModelMatrix"]  = ShaderValue::fromMatrix(Matrix4f::Identity());
    }

    {
        SceneCameraParallax cam_para;
        cam_para.enable         = sc.general.cameraparallax;
        cam_para.amount         = sc.general.cameraparallaxamount;
        cam_para.delay          = sc.general.cameraparallaxdelay;
        cam_para.mouseinfluence = sc.general.cameraparallaxmouseinfluence;
        context.shader_updater->SetCameraParallax(cam_para);
        if (auto it = sc.general.user_bindings.find("cameraparallaxmouseinfluence");
            it != sc.general.user_bindings.end()) {
            scene.camera_parallax_user_var_index[it->second].push_back(it->first);
        }
    }
    {
        SceneCameraShake cam_shake;
        cam_shake.enable    = sc.general.camerashake;
        cam_shake.amplitude = sc.general.camerashakeamplitude;
        cam_shake.speed     = sc.general.camerashakespeed;
        cam_shake.roughness = sc.general.camerashakeroughness;
        context.shader_updater->SetCameraShake(cam_shake);
        for (const auto& [field, key] : sc.general.user_bindings) {
            if (field == "camerashake" || field == "camerashakeamplitude" ||
                field == "camerashakespeed" || field == "camerashakeroughness") {
                scene.camera_shake_user_var_index[key].push_back(field);
            }
        }
        WireCameraShakeScripts(context, sc.general.field_bindings);
    }
}

void ParseImageObj(ParseContext& context, wpscene::ImageObject& img_obj) {
    auto& wpimgobj = img_obj;
    // Invisible image layers are kept in the scene tree because their composite
    // may be sampled by other layers via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder decides whether to actually emit passes for them.
    if (! wpimgobj.visible) {
        context.scene->elidable_layer_ids.insert(wpimgobj.id);
    }

    auto& vfs = *context.vfs;

    // coloBlendMode load passthrough manaully
    if (wpimgobj.colorBlendMode != 0) {
        wpscene::ImageEffect colorEffect;
        wpscene::Material    colorMat;
        nlohmann::json       json;
        if (! sr::ParseJson(
                fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"), json))
            return;
        colorMat.FromJson(json);
        colorMat.combos["BONECOUNT"] = 1;
        colorMat.combos["BLENDMODE"] = wpimgobj.colorBlendMode;
        colorMat.blending            = "disabled";
        colorEffect.materials.push_back(colorMat);
        wpimgobj.effects.push_back(colorEffect);
    }

    int32_t count_eff = 0;
    for (const auto& wpeffobj : wpimgobj.effects) {
        if (wpeffobj.visible) count_eff++;
    }
    bool hasEffect     = count_eff > 0;
    bool isPassthrough = wpimgobj.config.passthrough;

    // No-effect fullscreen / compose layers contribute nothing on their own
    // (they just sample `_rt_default` and write it back). Mark as elidable
    // so the render-graph builder drops them when unreferenced, or routes
    // them to `_rt_link_<id>` when another layer reads their composite.
    if (! hasEffect && wpimgobj.visible && (wpimgobj.fullscreen || isPassthrough)) {
        context.scene->elidable_layer_ids.insert(wpimgobj.id);
    }

    bool hasPuppet = ! wpimgobj.puppet.empty();
    (void)hasPuppet;

    std::unique_ptr<WPMdl> puppet;
    bool                   has_bones = false;
    bool                   has_mesh  = false;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! ModelAssetCompiler::Parse(wpimgobj.puppet, vfs, *puppet)) {
            rstd_error("parse puppet failed: {}", wpimgobj.puppet);
            puppet = nullptr;
        } else {
            has_bones = (puppet->puppet && puppet->puppet->bones.size() > 0);
            has_mesh  = false;
            for (const auto& m : puppet->meshes) {
                if (! m.positions.empty()) {
                    has_mesh = true;
                    break;
                }
            }
            if (! has_bones && ! has_mesh) {
                rstd_error("puppet has no mesh data: {}", wpimgobj.puppet);
                puppet = nullptr;
            }
        }
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto spImgNode = rstd::sync::Arc<SceneNode>::make(Vector3f(wpimgobj.origin.data()),
                                                      Vector3f(wpimgobj.scale.data()),
                                                      Vector3f(wpimgobj.angles.data()),
                                                      wpimgobj.name);
    LoadAlignment(*spImgNode.as_ptr(), wpimgobj.alignment, { wpimgobj.size[0], wpimgobj.size[1] });
    spImgNode->SetSize({ wpimgobj.size[0], wpimgobj.size[1] });
    spImgNode->SetPerspective(wpimgobj.perspective);
    spImgNode->SetBaseColor(Vector3f(wpimgobj.color.data()), wpimgobj.alpha);
    spImgNode->ID() = wpimgobj.id;
    if (! wpimgobj.visible_user_key.empty())
        spImgNode->SetVisibleUserKey(wpimgobj.visible_user_key);
    std::vector<SceneMaterial*> image_color_materials;
    auto                        track_image_color_material = [&](SceneMaterial* mat) {
        if (! wpimgobj.color_user_key.empty() && mat != nullptr)
            image_color_materials.push_back(mat);
    };
    std::shared_ptr<WPPuppetLayer> image_puppet_layer;
    if (puppet && has_bones) {
        image_puppet_layer = MakePuppetLayer(puppet->puppet, wpimgobj.puppet_layers);
        RegisterPuppetLayer(context, spImgNode.as_ptr(), image_puppet_layer);
    }

    // Puppet clipping masks: register the half-res shared RT here; per-mask
    // submeshes (pre-pass + clipped main) are emitted below after the base
    // material/mesh are built. Main material stays unmodified — only the
    // clipped-main submesh gets a CLIPPINGTARGET combo + g_Texture8 binding.
    constexpr std::string_view PUPPET_MASK_RT   = "_rt_puppet_mask";
    bool                       puppet_has_masks = false;
    if (puppet) {
        for (const auto& pmesh : puppet->meshes) {
            if (! pmesh.masks.empty()) {
                puppet_has_masks = true;
                break;
            }
        }
    }
    if (puppet_has_masks && has_bones &&
        context.scene->renderTargets.count(std::string(PUPPET_MASK_RT)) == 0) {
        SceneRenderTarget rt {};
        rt.width                                                  = 2;
        rt.height                                                 = 2;
        rt.allowReuse                                             = true;
        rt.force_clear                                            = true;
        rt.bind.enable                                            = true;
        rt.bind.screen                                            = true;
        rt.bind.scale                                             = 0.5f;
        context.scene->renderTargets[std::string(PUPPET_MASK_RT)] = rt;
    }

    SceneMaterial        material;
    SceneUniformNodeData svData;
    svData.puppet_layer = image_puppet_layer;

    ShaderValueMap    baseConstSvs = context.global_base_uniforms;
    WPShaderInfo      shaderInfo;
    wpscene::Material image_wpmat = wpimgobj.material;
    ApplyUserTextureBindings(context, image_wpmat);
    {
        svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
        svData.propagatedParallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
        if (! hasEffect) {
            svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
            if (puppet && has_bones) {
                ModelAssetCompiler::AddPuppetShaderInfo(shaderInfo, *puppet);
            }
        }

        baseConstSvs["g_Color4"] = std::array<float, 4> {
            wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2], wpimgobj.alpha
        };
        baseConstSvs["g_UserAlpha"]  = wpimgobj.alpha;
        baseConstSvs["g_Brightness"] = wpimgobj.brightness;

        shaderInfo.baseConstSvs = baseConstSvs;

        if (! LoadMaterial(vfs,
                           image_wpmat,
                           context.scene.get(),
                           spImgNode.as_ptr(),
                           &material,
                           &svData,
                           &shaderInfo)) {
            rstd_error("load imageobj '{}' material faild", wpimgobj.name);
            return;
        };
        LoadConstvalue(material, image_wpmat, shaderInfo);
    }

    // Whether the layer's base texture is point-sampled (noInterpolation).
    // Captured here because `material` is moved into the mesh below, well
    // before the effect ping-pong RTs are created.
    bool point_source = false;
    if (! material.textures.empty()) {
        auto& textures = context.scene->textures;
        auto  it       = textures.find(material.textures.front());
        point_source =
            it != textures.end() && it->second.sample.magFilter == TextureFilter::NEAREST;
    }

    for (const auto& cs : image_wpmat.constantshadervalues) {
        const auto&               name  = cs.first;
        const std::vector<float>& value = cs.second;
        std::string               glname;
        if (shaderInfo.alias.count(name) != 0) {
            glname = shaderInfo.alias.at(name);
        } else {
            for (const auto& el : shaderInfo.alias) {
                if (el.second.substr(2) == name) {
                    glname = el.second;
                    break;
                }
            }
        }
        if (glname.empty()) {
            rstd_error("ShaderValue: {} not found in glsl", name);
        } else {
            material.customShader.constValues[glname] = value;
        }
    }

    // mesh
    SceneMesh                  effct_final_mesh {};
    auto                       spMesh  = std::make_shared<SceneMesh>();
    auto&                      mesh    = *spMesh;
    const std::array<float, 2> mapRate = Texture0UvScale(material, wpimgobj.nopadding);
    auto add_puppet_mask_submeshes     = [&](SceneMesh& target, uint32_t first_mask_slot) {
        if (! puppet_has_masks) return;
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (pmesh.positions.empty()) continue;
                if (smi >= target.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.size());
                for (size_t i = 0; i < pmesh.parts.size(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count((uint32_t)i) != 0) continue;
                    kept.push_back({ p.start, p.size });
                }
                target.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        uint32_t slot = first_mask_slot;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                target.Submeshes().emplace_back();
                auto& pre_sm = target.Submeshes().back();
                ModelAssetCompiler::GenMaskSubmeshFromMdl(pre_sm, pmesh, mb.part_ids_b, mapRate);
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().back();
                ModelAssetCompiler::GenMaskSubmeshFromMdl(clip_sm, pmesh, mb.part_ids_a, mapRate);
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet) {
        if (hasEffect) {
            GenCardMesh(mesh, { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] }, mapRate);
            for (const auto& m : puppet->meshes) {
                if (m.positions.empty()) continue;
                effct_final_mesh.Submeshes().emplace_back();
                ModelAssetCompiler::GenMeshFromMdl(effct_final_mesh.Submeshes().back(), m, mapRate);
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, 1);

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat;
                puppet_mat             = image_wpmat;
                puppet_mat.textures[0] = "";
                ModelAssetCompiler::AddPuppetMatInfo(puppet_mat, *puppet);
                puppet_effect.materials.push_back(puppet_mat);
                wpimgobj.effects.push_back(puppet_effect);
            }
        } else {
            for (const auto& m : puppet->meshes) {
                if (m.positions.empty()) continue;
                mesh.Submeshes().emplace_back();
                ModelAssetCompiler::GenMeshFromMdl(mesh.Submeshes().back(), m, mapRate);
            }
        }
    }
    if (! puppet) {
        GenCardMesh(mesh, { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] }, mapRate);
        GenCardMesh(effct_final_mesh, { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] });
    }
    // material blendmode for last step to use
    auto finalMaterialState = material;
    // disable img material blend, as it's the first effect node now
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }
    mesh.AddMaterial(std::move(material));
    track_image_color_material(mesh.MaterialSlots().back().get());
    RegisterShaderUserVarIndex(context.scene.get(), mesh.Material(), image_wpmat, shaderInfo);

    // Puppet clipping masks: each MaskBlock becomes a pair of submeshes.
    // 1) Pre-pass: clippingmaskimage4 over `part_ids_b` (mask shape mesh)
    //    writes the mask RT.
    // 2) Clipped main: a clone of the main material with CLIPPINGTARGET combo
    //    + g_Texture8 = mask RT, draw range = `part_ids_a` (the clipped parts).
    // The original main submesh has all `part_ids_a` parts removed so the
    // clipped region is only drawn through the masked variant.
    if (puppet && ! hasEffect && has_bones && puppet_has_masks) {
        // `part_ids_a` indexes into pmesh.parts[] (position), not part.id.
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        // Rebuild main submeshes' draw_ranges: drop any part whose position
        // index is in `part_ids_a` of any mask block.
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (pmesh.positions.empty()) continue;
                if (smi >= mesh.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.size());
                for (size_t i = 0; i < pmesh.parts.size(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count((uint32_t)i) != 0) continue;
                    kept.push_back({ p.start, p.size });
                }
                mesh.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        const std::string albedo_tex =
            image_wpmat.textures.empty() ? std::string {} : image_wpmat.textures[0];
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                // (1) mask pre-pass submesh
                wpscene::Material mask_wpmat;
                mask_wpmat.shader     = "clippingmaskimage4";
                mask_wpmat.blending   = "translucent";
                mask_wpmat.depthtest  = "disabled";
                mask_wpmat.depthwrite = "disabled";
                mask_wpmat.cullmode   = "nocull";
                mask_wpmat.textures.resize(2);
                mask_wpmat.textures[0] = albedo_tex;
                mask_wpmat.textures[1] = mb.mat_json;
                ModelAssetCompiler::AddPuppetMatInfo(mask_wpmat, *puppet);

                SceneMaterial        mask_scene_mat;
                SceneUniformNodeData mask_svData;
                WPShaderInfo         mask_shaderInfo;
                mask_shaderInfo.baseConstSvs = baseConstSvs;
                if (! LoadMaterial(vfs,
                                   mask_wpmat,
                                   context.scene.get(),
                                   spImgNode.as_ptr(),
                                   &mask_scene_mat,
                                   &mask_svData,
                                   &mask_shaderInfo)) {
                    rstd_warn("load mask pre-pass material failed for '{}'", wpimgobj.name);
                    continue;
                }
                uint32_t pre_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(mask_scene_mat));
                track_image_color_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().back();
                ModelAssetCompiler::GenMaskSubmeshFromMdl(pre_sm, pmesh, mb.part_ids_b, mapRate);
                pre_sm.material_slot   = pre_slot;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                // (2) clipped-main submesh: main material + CLIPPINGTARGET
                wpscene::Material clip_wpmat        = image_wpmat;
                clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                ModelAssetCompiler::AddPuppetMatInfo(clip_wpmat, *puppet);

                SceneMaterial        clip_scene_mat;
                SceneUniformNodeData clip_svData;
                WPShaderInfo         clip_shaderInfo;
                clip_shaderInfo.baseConstSvs = baseConstSvs;
                if (! LoadMaterial(vfs,
                                   clip_wpmat,
                                   context.scene.get(),
                                   spImgNode.as_ptr(),
                                   &clip_scene_mat,
                                   &clip_svData,
                                   &clip_shaderInfo)) {
                    rstd_warn("load clipped main material failed for '{}'", wpimgobj.name);
                    continue;
                }
                LoadConstvalue(clip_scene_mat, clip_wpmat, clip_shaderInfo);
                uint32_t clip_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(clip_scene_mat));
                track_image_color_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().back();
                ModelAssetCompiler::GenMaskSubmeshFromMdl(clip_sm, pmesh, mb.part_ids_a, mapRate);
                clip_sm.material_slot = clip_slot;
            }
        }
    }

    spImgNode->AddMesh(spMesh);

    context.shader_updater->SetNodeData(spImgNode.as_ptr(), svData);
    if (hasEffect) {
        auto& scene = *context.scene;
        // currently use addr for unique
        std::string nodeAddr = getAddr(spImgNode.as_ptr());
        // set camera to attatch effect
        if (isPassthrough) {
            scene.cameras[nodeAddr] =
                std::make_shared<SceneCamera>((int32_t)scene.activeCamera->Width(),
                                              (int32_t)scene.activeCamera->Height(),
                                              -1.0f,
                                              1.0f);
            auto attached = scene.activeCamera->GetAttachedNode();
            if (attached.is_some()) scene.cameras.at(nodeAddr)->AttatchNode(attached.unwrap());
            if (scene.linkedCameras.count("global") == 0) scene.linkedCameras["global"] = {};
            scene.linkedCameras.at("global").push_back(nodeAddr);
        } else {
            // applly scale to crop
            i32 w                   = (i32)wpimgobj.size[0];
            i32 h                   = (i32)wpimgobj.size[1];
            scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(w, h, -1.0f, 1.0f);
            // Attach the per-layer effect camera to spImgNode itself so the
            // camera follows the layer through any parent-container world
            // translation. Otherwise the layer's quad ends up off-center in
            // the ping-pong RT whenever the layer is nested under a non-zero
            // container.
            scene.cameras.at(nodeAddr)->AttatchNode(spImgNode.as_ptr());
        }
        spImgNode->SetCamera(nodeAddr);
        std::string effect_ppong_a, effect_ppong_b;
        effect_ppong_a = SR_EFFECT_PPONG_PREFIX_A.data() + nodeAddr;
        effect_ppong_b = SR_EFFECT_PPONG_PREFIX_B.data() + nodeAddr;
        // set image effect
        auto imgEffectLayer = std::make_shared<SceneImageEffectLayer>(
            spImgNode.as_ptr(), wpimgobj.size[0], wpimgobj.size[1], effect_ppong_a, effect_ppong_b);
        {
            imgEffectLayer->SetFullscreen(wpimgobj.fullscreen);
            imgEffectLayer->SetFinalMaterialState(finalMaterialState);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
        }
        // set renderTarget for ping-pong operate
        {
            scene.renderTargets[effect_ppong_a] = {
                .width                = (uint16_t)wpimgobj.size[0],
                .height               = (uint16_t)wpimgobj.size[1],
                .allowReuse           = true,
                .force_clear          = ! wpimgobj.fullscreen,
                .clear_on_first_write = true,
            };
            if (wpimgobj.fullscreen) {
                scene.renderTargets[effect_ppong_a].bind = { .enable = true, .screen = true };
            }
            // Point-art images (noInterpolation) must stay point-sampled through
            // the whole effect chain.
            if (point_source) {
                auto& s     = scene.renderTargets[effect_ppong_a].sample;
                s.magFilter = s.minFilter = TextureFilter::NEAREST;
            }
            scene.renderTargets[effect_ppong_b] = scene.renderTargets.at(effect_ppong_a);
        }

        int32_t     i_eff = -1;
        std::string last_effect_shader;
        for (const auto& wpeffobj : wpimgobj.effects) {
            i_eff++;
            if (! wpeffobj.visible) {
                i_eff--;
                continue;
            }
            std::shared_ptr<SceneImageEffect> imgEffect = std::make_shared<SceneImageEffect>();

            // this will be replace when resolve, use here to get rt info
            const std::string inRT { effect_ppong_a };

            // fbo name map and effect command
            std::string effaddr = getAddr(imgEffectLayer.get());

            std::unordered_map<std::string, std::string> fboMap;
            {
                fboMap["previous"] = inRT;
                for (usize i = 0; i < wpeffobj.fbos.size(); i++) {
                    const auto& wpfbo = wpeffobj.fbos.at(i);
                    // Some effects (e.g. WE DOF) use fbo names without the
                    // `_rt_` prefix (`_coc`, `_downscaled1`, ...). Force the
                    // prefix so IsSpecTex / render-target lookups treat them
                    // as render targets instead of disk textures.
                    std::string rtname =
                        sstart_with(wpfbo.name, WE_SPEC_PREFIX)
                            ? wpfbo.name + "_" + effaddr
                            : std::string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
                    if (wpimgobj.fullscreen) {
                        scene.renderTargets[rtname]      = { 2, 2, true };
                        scene.renderTargets[rtname].bind = {
                            .enable = true,
                            .screen = true,
                            .scale  = 1.0 / wpfbo.scale,
                        };
                    } else {
                        auto fbo_size = [&]() -> std::array<uint16_t, 2> {
                            if (wpfbo.fit > 0) {
                                const float max_size = std::max(wpimgobj.size[0], wpimgobj.size[1]);
                                if (max_size > 0.0f) {
                                    const float fit_scale =
                                        static_cast<float>(wpfbo.fit) / max_size;
                                    return { static_cast<uint16_t>(std::max(
                                                 1.0f, std::round(wpimgobj.size[0] * fit_scale))),
                                             static_cast<uint16_t>(std::max(
                                                 1.0f, std::round(wpimgobj.size[1] * fit_scale))) };
                                }
                            }
                            return { static_cast<uint16_t>(wpimgobj.size[0] / (float)wpfbo.scale),
                                     static_cast<uint16_t>(wpimgobj.size[1] / (float)wpfbo.scale) };
                        }();
                        scene.renderTargets[rtname] = { .width      = fbo_size[0],
                                                        .height     = fbo_size[1],
                                                        .allowReuse = true };
                    }
                    fboMap[wpfbo.name] = rtname;
                }
            }
            // load! effect commands
            {
                for (const auto& el : wpeffobj.commands) {
                    if (el.command != "copy") {
                        rstd_error("Unknown effect command: {}", el.command);
                        continue;
                    }
                    if (fboMap.count(el.target) + fboMap.count(el.source) < 2) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", el.target, el.source);
                        continue;
                    }
                    imgEffect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                    .dst      = fboMap[el.target],
                                                    .src      = fboMap[el.source],
                                                    .afterpos = el.afterpos });
                }
            }

            bool eff_mat_ok { true };

            for (usize i_mat = 0; i_mat < wpeffobj.materials.size(); i_mat++) {
                wpscene::Material wpmat = wpeffobj.materials.at(i_mat);
                std::string       matOutRT { SR_EFFECT_PPONG_PREFIX_B };
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    ApplyTextureBinds(wpmat, std::span(wppass.bind), fboMap);
                    ApplyUserTextureBindings(context, wpmat);
                    if (! wppass.target.empty()) {
                        if (fboMap.count(wppass.target) == 0) {
                            rstd_error("fbo {} not found", wppass.target);
                        } else {
                            matOutRT = fboMap.at(wppass.target);
                        }
                    }
                }
                // A layer's own effect referencing its composite
                // (`_rt_imageLayerComposite_<self>[_a|_b]`) wants this layer's
                // running chain result.
                for (auto& t : wpmat.textures) {
                    if (ParseImageLayerCompositeId(t) == static_cast<std::uint32_t>(wpimgobj.id))
                        t = effect_ppong_a;
                }
                if (wpmat.textures.size() == 0) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) {
                    wpmat.textures[0] = inRT;
                }
                auto         spEffNode  = rstd::sync::Arc<SceneNode>::make();
                std::string  effmataddr = getAddr(spEffNode.as_ptr());
                WPShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrix"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrixInverse"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                SceneMaterial        material;
                SceneUniformNodeData svData;
                svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
                if (! LoadMaterial(vfs,
                                   wpmat,
                                   context.scene.get(),
                                   spEffNode.as_ptr(),
                                   &material,
                                   &svData,
                                   &wpEffShaderInfo)) {
                    eff_mat_ok = false;
                    break;
                }

                // load glname from alias and load to constvalue
                LoadConstvalue(material, wpmat, wpEffShaderInfo);
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.propagatedParallaxDepth = { wpimgobj.parallaxDepth[0],
                                                       wpimgobj.parallaxDepth[1] };
                    svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
                    svData.effect_projection_node = spImgNode.as_ptr();
                    svData.effect_projection_size = { wpimgobj.size[0], wpimgobj.size[1] };
                    if (puppet && wpmat.use_puppet) {
                        svData.puppet_layer =
                            MakePuppetLayer(puppet->puppet, wpimgobj.puppet_layers);
                        RegisterPuppetLayer(context, spEffNode.as_ptr(), svData.puppet_layer);
                    }
                }
                spMesh->AddMaterial(std::move(material));
                track_image_color_material(spMesh->MaterialSlots().back().get());
                RegisterShaderUserVarIndex(
                    context.scene.get(), spMesh->Material(), wpmat, wpEffShaderInfo);
                auto add_puppet_mask_materials = [&]() -> bool {
                    if (! (puppet && wpmat.use_puppet && puppet_has_masks)) return true;
                    const std::string source_tex =
                        wpmat.textures.empty() ? std::string {} : wpmat.textures[0];
                    for (const auto& pmesh : puppet->meshes) {
                        for (const auto& mb : pmesh.masks) {
                            wpscene::Material mask_wpmat;
                            mask_wpmat.shader     = "clippingmaskimage4";
                            mask_wpmat.blending   = "translucent";
                            mask_wpmat.depthtest  = "disabled";
                            mask_wpmat.depthwrite = "disabled";
                            mask_wpmat.cullmode   = "nocull";
                            mask_wpmat.textures.resize(2);
                            mask_wpmat.textures[0] = source_tex;
                            mask_wpmat.textures[1] = mb.mat_json;
                            ModelAssetCompiler::AddPuppetMatInfo(mask_wpmat, *puppet);

                            SceneMaterial        mask_material;
                            SceneUniformNodeData mask_svData;
                            WPShaderInfo         mask_shaderInfo;
                            mask_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
                            if (! LoadMaterial(vfs,
                                               mask_wpmat,
                                               context.scene.get(),
                                               spEffNode.as_ptr(),
                                               &mask_material,
                                               &mask_svData,
                                               &mask_shaderInfo)) {
                                return false;
                            }
                            LoadConstvalue(mask_material, mask_wpmat, mask_shaderInfo);
                            spMesh->AddMaterial(std::move(mask_material));
                            track_image_color_material(spMesh->MaterialSlots().back().get());

                            wpscene::Material clip_wpmat        = wpmat;
                            clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                            clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                            if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                            clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                            ModelAssetCompiler::AddPuppetMatInfo(clip_wpmat, *puppet);

                            SceneMaterial        clip_material;
                            SceneUniformNodeData clip_svData;
                            WPShaderInfo         clip_shaderInfo;
                            clip_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
                            if (! LoadMaterial(vfs,
                                               clip_wpmat,
                                               context.scene.get(),
                                               spEffNode.as_ptr(),
                                               &clip_material,
                                               &clip_svData,
                                               &clip_shaderInfo)) {
                                return false;
                            }
                            LoadConstvalue(clip_material, clip_wpmat, clip_shaderInfo);
                            spMesh->AddMaterial(std::move(clip_material));
                            track_image_color_material(spMesh->MaterialSlots().back().get());
                        }
                    }
                    return true;
                };
                if (! add_puppet_mask_materials()) {
                    eff_mat_ok = false;
                    break;
                }
                if (auto* mat = spMesh->Material(); mat != nullptr) last_effect_shader = mat->name;
                spEffNode->AddMesh(spMesh);

                context.shader_updater->SetNodeData(spEffNode.as_ptr(), svData);
                imgEffect->nodes.push_back(SceneImageEffectNode { matOutRT, spEffNode.clone() });
            }

            if (eff_mat_ok)
                imgEffectLayer->AddEffect(imgEffect);
            else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        if (! wpimgobj.fullscreen && ! isPassthrough &&
            ! IsLayerCompositeShader(last_effect_shader)) {
            nlohmann::json    json;
            wpscene::Material passthrough_mat;
            if (! sr::ParseJson(
                    fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"),
                    json) ||
                ! passthrough_mat.FromJson(json)) {
                rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
            } else {
                if (passthrough_mat.textures.empty())
                    passthrough_mat.textures.push_back(effect_ppong_a);
                else
                    passthrough_mat.textures[0] = effect_ppong_a;

                auto finalEffect = std::make_shared<SceneImageEffect>();
                auto spFinalNode = rstd::sync::Arc<SceneNode>::make();

                WPShaderInfo wpFinalShaderInfo;
                wpFinalShaderInfo.baseConstSvs = baseConstSvs;
                SceneMaterial        finalMaterial;
                SceneUniformNodeData finalSvData;
                finalSvData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
                finalSvData.propagatedParallaxDepth        = { wpimgobj.parallaxDepth[0],
                                                               wpimgobj.parallaxDepth[1] };
                finalSvData.parallaxDepth                  = { wpimgobj.parallaxDepth[0],
                                                               wpimgobj.parallaxDepth[1] };
                if (LoadMaterial(vfs,
                                 passthrough_mat,
                                 context.scene.get(),
                                 spFinalNode.as_ptr(),
                                 &finalMaterial,
                                 &finalSvData,
                                 &wpFinalShaderInfo)) {
                    LoadConstvalue(finalMaterial, passthrough_mat, wpFinalShaderInfo);
                    auto spFinalMesh = std::make_shared<SceneMesh>();
                    spFinalMesh->AddMaterial(std::move(finalMaterial));
                    track_image_color_material(spFinalMesh->MaterialSlots().back().get());
                    RegisterShaderUserVarIndex(context.scene.get(),
                                               spFinalMesh->Material(),
                                               passthrough_mat,
                                               wpFinalShaderInfo);
                    spFinalNode->AddMesh(spFinalMesh);
                    context.shader_updater->SetNodeData(spFinalNode.as_ptr(), finalSvData);
                    finalEffect->nodes.push_back(
                        SceneImageEffectNode { effect_ppong_b, spFinalNode.clone() });
                    imgEffectLayer->AddEffect(finalEffect);
                } else {
                    rstd_error("effect passthrough failed to load for '{}'", wpimgobj.name);
                }
            }
        }
    }
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (! wpimgobj.color_user_key.empty()) {
        context.scene->image_color_user_index[wpimgobj.color_user_key].push_back(
            { spImgNode.as_ptr(), std::move(image_color_materials) });
    }
    context.node_id_map[wpimgobj.id] = {
        wpimgobj.parent,
        rstd::Some(spImgNode.clone()),
        (puppet && puppet->puppet) ? puppet->puppet : nullptr,
        wpimgobj.attachment,
        image_puppet_layer,
    };
}

struct ParticleChildPtr {
    wpscene::ParticleChild* child { nullptr };
    SceneNode*              node_parent { nullptr };
    ParticleSubSystem*      particle_parent { nullptr };

    i32 max_instancecount { 1 };

    // Effective world scale at node_parent. Particle child origins are
    // pre-divided by this so the shader's MVP scale recovers the authored
    // parent-relative world-pixel offset.
    Eigen::Vector3f world_scale { 1.f, 1.f, 1.f };
};

void ParseParticleObj(ParseContext& context, wpscene::ParticleObject& wppartobj,
                      ParticleChildPtr child_ptr = {}) {
    struct ChildData {
        ChildData() = default;
        ChildData(const wpscene::ParticleChild& o)
            : type(o.type),
              maxcount(o.maxcount),
              controlpointstartindex(o.controlpointstartindex),
              probability(o.probability) {}
        std::string type { "static" };
        i32         maxcount { 20 };
        i32         controlpointstartindex { 0 };
        float       probability { 1.0f };
    };

    wpscene::Particle*                       p_particle_obj { nullptr };
    rstd::Option<rstd::sync::Arc<SceneNode>> spNodeOpt;
    ChildData                                child_data;

    bool is_child = child_ptr.child != nullptr;
    if (is_child) {
        p_particle_obj = &(child_ptr.child->obj);
        // ParticleChild::origin is a WE world-pixel offset from the parent
        // particle. SceneNode hierarchy composes T(local) * S(parent) so
        // the local translation gets multiplied by parent scale at render
        // time; pre-divide so the world translation matches the JSON.
        Vector3f corigin(child_ptr.child->origin.data());
        for (int i = 0; i < 3; ++i) {
            float s = child_ptr.world_scale[i];
            if (std::abs(s) > 1e-6f) corigin[i] /= s;
        }
        spNodeOpt =
            rstd::Some(rstd::sync::Arc<SceneNode>::make(corigin,
                                                        Vector3f(child_ptr.child->scale.data()),
                                                        Vector3f(child_ptr.child->angles.data()),
                                                        child_ptr.child->name));
        child_data = ChildData(*child_ptr.child);

        child_ptr.max_instancecount *= child_data.maxcount;

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNodeOpt = rstd::Some(rstd::sync::Arc<SceneNode>::make(Vector3f(wppartobj.origin.data()),
                                                                Vector3f(wppartobj.scale.data()),
                                                                Vector3f(wppartobj.angles.data()),
                                                                wppartobj.name));
        auto& spNode = *spNodeOpt;
        if (! wppartobj.visible_user_key.empty())
            spNode->SetVisibleUserKey(wppartobj.visible_user_key);
    }
    auto& spNode = *spNodeOpt;

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    // shared_ptr so RenderSetUserProperty can mutate the override at runtime
    // and the new value is observed by every initializer / operator closure.
    auto override_state =
        std::make_shared<wpscene::ParticleInstanceoverride>(wppartobj.instanceoverride);
    auto& override = *override_state;

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;

    auto wppartRenderer = particle_obj.renderers.at(0);
    auto render_desc    = DescribeParticleRender(wppartRenderer);
    bool render_rope    = render_desc.rope;
    bool hastrail       = render_desc.trail;

    if (render_rope) particle_obj.material.shader = "genericropeparticle";

    // wppartobj.origin[1] = context.ortho_h - wppartobj.origin[1];

    if (particle_obj.flags[wpscene::Particle::FlagEnum::perspective]) {
        spNode->SetCamera("global_perspective");
    }

    SceneMaterial        material;
    SceneUniformNodeData svData;

    if (! is_child) {
        svData.parallaxDepth           = { wppartobj.parallaxDepth[0], wppartobj.parallaxDepth[1] };
        svData.propagatedParallaxDepth = { wppartobj.parallaxDepth[0], wppartobj.parallaxDepth[1] };
    }
    svData.use_camera_eye_position = particle_obj.flags[wpscene::Particle::FlagEnum::perspective];

    WPShaderInfo shaderInfo;
    shaderInfo.baseConstSvs                         = context.global_base_uniforms;
    shaderInfo.baseConstSvs["g_OrientationUp"]      = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs["g_OrientationRight"]   = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs["g_OrientationForward"] = std::array { 0.0f, 0.0f, 1.0f };
    shaderInfo.baseConstSvs["g_ViewUp"]             = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs["g_ViewRight"]          = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs["g_EyePosition"]        = std::array {
        static_cast<float>(context.ortho_w) / 2.0f,
        static_cast<float>(context.ortho_h) / 2.0f,
        1000.0f,
    };

    u32 maxcount = particle_obj.maxcount;
    maxcount     = std::min(maxcount, 20000u);

    if (hastrail) {
        double in_SegmentUVTimeOffset           = 0.0;
        double in_SegmentMaxCount               = maxcount - 1.0;
        shaderInfo.baseConstSvs["g_RenderVar0"] = std::array {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.combos["TRAILRENDERER"] = "1";
        // Only the authored "rope" renderer uses genericropeparticle's segment
        // layout. "*trail" renderers stay on genericparticle and need velocity
        // in TexCoordVec4C1 for ComputeParticleTrailTangents.
        if (! render_rope) shaderInfo.combos["THICKFORMAT"] = "1";
    }
    if (render_rope) {
        // genericropeparticle.geom branches on TRAILSUBDIVISION when present;
        // 0 = no subdivision (straight quad per segment), positive = cubic
        // Bezier subdivided into N+1 quads.
        i32 subdiv = (i32)std::round(wppartRenderer.subdivision);
        if (subdiv < 0) subdiv = 0;
        shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(subdiv);
    }

    if (! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
        shaderInfo.combos["SPRITESHEETBLEND"] = "1";
    }

    bool mat_ok              = false;
    bool use_geometry_shader = false;
    try {
        mat_ok = LoadMaterial(vfs,
                              particle_obj.material,
                              context.scene.get(),
                              spNode.as_ptr(),
                              &material,
                              &svData,
                              &shaderInfo,
                              render_desc.geometry_shader,
                              &use_geometry_shader);
    } catch (const std::exception& e) {
        rstd_error("load particleobj '{}' material exception: {}", wppartobj.name, e.what());
    }
    if (! mat_ok) {
        rstd_error("load particleobj '{}' material faild", wppartobj.name);
        return;
    }
    LoadConstvalue(material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
    auto  animationmode      = ToAnimMode(particle_obj.animationmode);
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool thick_format = material.hasSprite || (hastrail && ! render_rope);
    // Trail history depth per rope-head particle. Clamp to [2, 256] so a buggy
    // renderer spec can't allocate gigabytes; segments<2 would produce zero
    // segments anyway.
    u32 trail_length = 0;
    if (render_rope) {
        i32 seg = wppartRenderer.segments;
        if (seg < 2) seg = 2;
        if (seg > 256) seg = 256;
        trail_length = (u32)seg;
    }
    ParticleFollowAnchor follow_anchor;
    if (hastrail && ! render_rope) {
        follow_anchor.trail_renderer = true;
        follow_anchor.length         = wppartRenderer.length;
        follow_anchor.max_length     = wppartRenderer.maxlength;
        follow_anchor.texture_ratio  = ParticleTextureRatio(material);
    }
    {
        // Rope mesh capacity = maxcount * (trail_length-1) since each live
        // particle produces (trail_length-1) GS-input segments. Non-rope path
        // is unchanged: per-particle quad fan-out.
        u32 mesh_maxcount = maxcount * (u32)child_ptr.max_instancecount;
        if (render_rope) {
            u32 rope_segs = mesh_maxcount * (trail_length - 1);
            SetRopeParticleMesh(mesh, particle_obj, rope_segs, thick_format, use_geometry_shader);
        } else {
            SetParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format, use_geometry_shader);
        }
    }

    auto particleSub = std::make_unique<ParticleSubSystem>(
        *context.scene->paritileSys,
        spMesh,
        maxcount,
        override.rate,
        child_data.maxcount,
        child_data.probability,
        ParseSpawnType(child_data.type),
        [=](const Particle& p, const ParticleRawGenSpec& spec) {
            auto& lifetime = *(spec.lifetime);
            if (lifetime <= 0.0f) {
                lifetime = 0.0f;
                return;
            }
            switch (animationmode) {
            case ParticleAnimationMode::RANDOMONE: lifetime = std::floor(p.init.lifetime); break;
            case ParticleAnimationMode::SEQUENCE:
                lifetime = (1.0f - (p.lifetime / p.init.lifetime)) * sequencemultiplier;
                break;
            }
        },
        follow_anchor,
        trail_length,
        static_cast<double>(particle_obj.starttime));

    particleSub->SetOwnerNode(spNode.as_ptr());
    LoadEmitter(*particleSub,
                particle_obj,
                override.count,
                render_rope,
                is_child ? child_data.controlpointstartindex : 0);
    LoadInitializer(*particleSub, particle_obj, override_state);
    LoadOperator(*particleSub, particle_obj, override_state);
    LoadControlPoint(*particleSub, particle_obj);

    // Register every {user:"<key>", value:...} binding on instanceoverride
    // so RenderSetUserProperty can mutate the shared state at runtime.
    for (const auto& [field, key] : override.bindings) {
        context.scene->particle_user_var_index[key].push_back({ override_state, field });
    }

    mesh.AddMaterial(std::move(material));
    RegisterShaderUserVarIndex(
        context.scene.get(), mesh.Material(), particle_obj.material, shaderInfo);
    spNode->AddMesh(spMesh);
    context.shader_updater->SetNodeData(spNode.as_ptr(), svData);

    for (auto& child : particle_obj.children) {
        ParseParticleObj(context,
                         wppartobj,
                         {
                             .child             = &child,
                             .node_parent       = spNode.as_ptr(),
                             .particle_parent   = particleSub.get(),
                             .max_instancecount = child_ptr.max_instancecount,
                             .world_scale       = node_world_scale,
                         });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(std::move(particleSub));
    else
        context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));

    WireFieldScripts(context, spNode, wppartobj.field_bindings);
    if (is_child)
        child_ptr.node_parent->AppendChild(spNode.clone());
    else {
        context.node_id_map[wppartobj.id] = {
            wppartobj.parent,
            rstd::Some(spNode.clone()),
            nullptr,
            wppartobj.attachment,
        };
    }
}

void ParseSoundObj(ParseContext& context, wpscene::SoundObject& obj,
                   wavsen::audio::SoundManager& sm) {
    auto node  = rstd::sync::Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                                  Vector3f(obj.scale.data()),
                                                  Vector3f(obj.angles.data()),
                                                  obj.name);
    node->ID() = obj.id;
    if (! obj.visible) node->SetVisible(false);
    if (! obj.visible_user_key.empty()) node->SetVisibleUserKey(obj.visible_user_key);

    auto control = SoundAssetCompiler::Parse(obj, *context.vfs, sm, context.scene.get());
    node->SetSoundControl(control);
    if (control && ! obj.volume_user_key.empty())
        context.scene->sound_volume_user_index[obj.volume_user_key].push_back(control);

    WireFieldScripts(context, node, obj.field_bindings);
    context.node_id_map[obj.id] = { obj.parent, rstd::Some(node.clone()) };
}

void ParseLightObj(ParseContext& context, wpscene::LightObject& light_obj) {
    auto node = rstd::sync::Arc<SceneNode>::make(Vector3f(light_obj.origin.data()),
                                                 Vector3f(light_obj.scale.data()),
                                                 Vector3f(light_obj.angles.data()),
                                                 light_obj.name);

    SceneLight::Desc desc;
    if (light_obj.light == "spot") {
        desc.type = SceneLightType::Spot;
    } else if (light_obj.light == "directional") {
        desc.type = SceneLightType::Directional;
    } else {
        desc.type = SceneLightType::Point; // default + "point"
    }
    desc.color       = Vector3f(light_obj.color.data());
    desc.radius      = light_obj.radius;
    desc.intensity   = light_obj.intensity;
    desc.exponent    = light_obj.exponent;
    desc.attenuation = light_obj.attenuation;
    desc.mindistance = light_obj.mindistance;
    // WE cone fields are full angles in degrees; convert to cos(half-angle).
    const float kDegToRad     = rstd::f32_::consts::PI / 180.0f;
    desc.inner_cone_cos       = std::cos(light_obj.innercone * 0.5f * kDegToRad);
    desc.outer_cone_cos       = std::cos(light_obj.outercone * 0.5f * kDegToRad);
    desc.light_source_size    = light_obj.lightsourcesize;
    desc.cascade_distances[0] = light_obj.cascadedistance0;
    desc.cascade_distances[1] = light_obj.cascadedistance1;
    desc.cascade_distances[2] = light_obj.cascadedistance2;
    desc.cast_shadow          = light_obj.castshadow;
    desc.cast_volumetrics     = light_obj.castvolumetrics;

    context.scene->lights.emplace_back(std::make_unique<SceneLight>(desc));
    auto& light = *(context.scene->lights.back());
    light.setNode(node.as_ptr());
    light.setRuntimeVisible(light_obj.visible);
    if (! light_obj.visible_user_key.empty()) {
        light.setVisibleUserKey(light_obj.visible_user_key);
    }

    context.node_id_map[light_obj.id] = { light_obj.parent, rstd::Some(node.clone()) };
}

void ParseModelObj(ParseContext& context, wpscene::ModelObject& model_obj) {
    auto& vfs = *context.vfs;

    WPMdl mdl;
    if (! ModelAssetCompiler::Parse(model_obj.model, vfs, mdl)) {
        rstd_error("parse model failed: {}", model_obj.model);
        return;
    }

    auto node  = rstd::sync::Arc<SceneNode>::make(Vector3f(model_obj.origin.data()),
                                                  Vector3f(model_obj.scale.data()),
                                                  Vector3f(model_obj.angles.data()),
                                                  model_obj.name);
    node->ID() = model_obj.id;
    MarkHiddenLinkSource(context, model_obj.id);
    if (! model_obj.visible_user_key.empty()) node->SetVisibleUserKey(model_obj.visible_user_key);

    auto mesh = std::make_shared<SceneMesh>();

    SceneUniformNodeData svData;
    svData.parallaxDepth           = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.propagatedParallaxDepth = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.use_camera_eye_position = true;
    if (mdl.puppet && ! mdl.puppet->bones.empty()) {
        svData.puppet_layer = MakePuppetLayer(
            mdl.puppet, std::span<WPPuppetLayer::AnimationLayer>(model_obj.puppet_layers));
        RegisterPuppetLayer(context, node.as_ptr(), svData.puppet_layer);
    }

    for (const auto& mdl_mesh : mdl.meshes) {
        if (mdl_mesh.positions.empty()) continue;

        auto wpmat = ModelAssetCompiler::ParseMaterial(mdl_mesh.mat_json_file, vfs);
        if (! wpmat) continue;
        if (mdl.puppet && ! mdl.puppet->bones.empty()) ModelAssetCompiler::AddPuppetMatInfo(*wpmat, mdl);

        SceneMaterial scene_mat;
        WPShaderInfo  shader_info;
        shader_info.baseConstSvs            = context.global_base_uniforms;
        shader_info.normalize_tangent_space = true;
        if (mdl.puppet && ! mdl.puppet->bones.empty()) {
            ModelAssetCompiler::AddPuppetShaderInfo(shader_info, mdl);
        }

        if (! LoadMaterial(vfs,
                           *wpmat,
                           context.scene.get(),
                           node.as_ptr(),
                           &scene_mat,
                           &svData,
                           &shader_info)) {
            rstd_error(
                "load model material '{}' failed for '{}'", mdl_mesh.mat_json_file, model_obj.name);
            continue;
        }
        LoadConstvalue(scene_mat, *wpmat, shader_info);

        const uint32_t material_slot  = static_cast<uint32_t>(mesh->MaterialSlots().size());
        const auto     texcoord_scale = Texture0UvScale(scene_mat);
        mesh->AddMaterial(std::move(scene_mat));
        RegisterShaderUserVarIndex(
            context.scene.get(), mesh->MaterialSlots().back().get(), *wpmat, shader_info);

        mesh->Submeshes().emplace_back();
        auto& submesh = mesh->Submeshes().back();
        ModelAssetCompiler::GenMeshFromMdl(submesh, mdl_mesh, texcoord_scale);
        submesh.material_slot = material_slot;
    }

    if (mesh->Submeshes().empty()) {
        rstd_error("model '{}' has no renderable mesh", model_obj.model);
        return;
    }

    node->AddMesh(mesh);
    context.shader_updater->SetNodeData(node.as_ptr(), svData);
    WireFieldScripts(context, node, model_obj.field_bindings);
    context.node_id_map[model_obj.id] = { model_obj.parent,
                                          rstd::Some(node.clone()),
                                          mdl.puppet,
                                          model_obj.attachment,
                                          svData.puppet_layer };
}

// Wrapping image parser: serves text-atlas Images for synthetic urls (set
// via Register) and delegates everything else to the underlying parser.
// Installed lazily on first text object so the WE .tex path is unchanged
// for image-only wallpapers.
class TextRenderImageParser : public IImageParser {
public:
    explicit TextRenderImageParser(std::unique_ptr<IImageParser> inner)
        : m_inner(std::move(inner)) {}
    std::shared_ptr<Image> Parse(const std::string& name) override {
        if (auto it = m_synth.find(name); it != m_synth.end()) return it->second;
        return m_inner ? m_inner->Parse(name) : nullptr;
    }
    ImageHeader ParseHeader(const std::string& name) override {
        if (auto it = m_synth.find(name); it != m_synth.end()) return it->second->header;
        return m_inner ? m_inner->ParseHeader(name) : ImageHeader {};
    }
    void Register(std::string name, std::shared_ptr<Image> img) {
        m_synth[std::move(name)] = std::move(img);
    }

private:
    std::unique_ptr<IImageParser>                           m_inner;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_synth;
};

TextRenderImageParser& EnsureTextImageParser(Scene& scene) {
    auto* p = dynamic_cast<TextRenderImageParser*>(scene.imageParser.get());
    if (p != nullptr) return *p;
    auto  inner       = std::unique_ptr<IImageParser>(scene.imageParser.release());
    auto  wrapped     = std::make_unique<TextRenderImageParser>(std::move(inner));
    auto* raw         = wrapped.get();
    scene.imageParser = std::move(wrapped);
    return *raw;
}

void ParseTextObj(ParseContext& context, wpscene::TextObject& obj) {
    if (! obj.visible) return;
    MarkHiddenLinkSource(context, obj.id);

    // --- determine initial text + whether a script binding will rewrite it
    auto text_binding_it = obj.field_bindings.scripts.find("text");
    bool has_text_script = (text_binding_it != obj.field_bindings.scripts.end());
    // Scripts can also drive `text` indirectly: a script attached to any
    // other field (commonly `visible`) writes `thisLayer.text = "..."` from
    // its update() side-effect (e.g. workshop 2283810443's clock). Treat
    // those layers as scripted-text for mesh sizing + setter registration.
    bool has_indirect_text_script = ! has_text_script && ! obj.field_bindings.scripts.empty();
    bool wants_dynamic_text       = has_text_script || has_indirect_text_script;

    std::string s_text;
    if (obj.text.is_string()) {
        s_text = obj.text.get<std::string>();
    } else if (obj.text.is_object()) {
        if (obj.text.contains("value") && obj.text.at("value").is_string())
            s_text = obj.text.at("value").get<std::string>();
        else if (obj.text.contains("text") && obj.text.at("text").is_string())
            s_text = obj.text.at("text").get<std::string>();
    }
    if (s_text.empty() && ! wants_dynamic_text) return;

    // --- font resolution: VFS first (WE shared /assets + pkg overlay),
    //     then host system font dirs.
    std::string font_name;
    if (obj.font.is_string()) {
        font_name = obj.font.get<std::string>();
    } else if (obj.font.is_object()) {
        if (obj.font.contains("value") && obj.font.at("value").is_string())
            font_name = obj.font.at("value").get<std::string>();
    }

    text::FontCache::ResolvedBlob resolved;
    // `systemfont_<family>` is WE's alias for a host system font — never exists
    // in the pkg, so skip the VFS round-trip and let fontconfig resolve it.
    // Some scenes write it with a leading dir (e.g. `fonts/systemfont_arial`),
    // so match on the basename.
    const bool is_systemfont =
        std::filesystem::path(font_name).filename().native().starts_with("systemfont_");
    if (! font_name.empty() && ! is_systemfont) {
        // scene.json's `font` is a pkg-relative path, e.g. `fonts/2.ttf` or
        // `fonts/workshop/<id>/X.otf`. The pkg mounts at /assets so the full
        // VFS path is /assets/<font_name>.
        std::string vfs_path =
            (std::filesystem::path("/assets") / font_name).lexically_normal().native();
        std::string blob_str = fs::GetFileContent(*context.vfs, vfs_path);
        if (! blob_str.empty()) {
            auto bytes = std::make_shared<std::vector<std::byte>>(blob_str.size());
            std::memcpy(bytes->data(), blob_str.data(), blob_str.size());
            resolved.bytes  = std::move(bytes);
            resolved.source = vfs_path;
        }
    }
    if (! resolved.bytes) {
        resolved = text::FontCache::ResolveSystemFont(font_name, /*fallback_to_any=*/true);
    }
    if (! resolved.bytes) {
        rstd_error("text '{}': could not resolve font '{}'", obj.name, font_name);
        return;
    }

    // --- pointsize → px (empirical 4× — see earlier comment).
    constexpr float kPointsizeToPx = 4.0f;
    std::uint32_t   px = static_cast<std::uint32_t>(std::round(obj.pointsize * kPointsizeToPx));
    if (px < 1) px = 1;
    if (px > 1024) px = 1024;

    auto& font_cache = text::EnsureSceneFontCache(*context.scene);
    auto* face       = font_cache.GetFace(resolved.bytes, px);
    if (face == nullptr) {
        rstd_error("text '{}': FreeType failed to open '{}'", obj.name, resolved.source);
        return;
    }

    auto shader = text::GetTextSceneShader();
    if (! shader) {
        rstd_error("text '{}': text shader compile failed", obj.name);
        return;
    }

    // Populate the seed text's glyphs up front so the first SetText has the
    // initial layout's bbox. Runtime SetText calls (from the script actuator)
    // do their own Populate of the latest string each tick.
    {
        auto seed = text::DecodeUtf8(s_text);
        face->Populate(seed);
    }

    // --- atlas-texture registration. The face's atlas is fixed at 1024² R8;
    // we snapshot whatever the CPU buffer holds right now (seed glyphs + the
    // white cell) and register it with the imageParser. TextureCache::CreateTex
    // will pick this up on first material bind, allocating the VkImage at
    // 1024². Subsequent glyph adds emit dirty rects which the renderer
    // re-uploads each frame via TextureCache::PumpFontAtlases.
    const std::string& atlas_url = face->AtlasUrl();
    if (! context.scene->textures.contains(atlas_url)) {
        auto atlas_img = text::BuildAtlasImage(*face, atlas_url);
        if (! atlas_img) {
            rstd_error("text '{}': atlas snapshot failed", obj.name);
            return;
        }
        EnsureTextImageParser(*context.scene).Register(atlas_url, atlas_img);
        SceneTexture stex;
        stex.url                           = atlas_url;
        stex.sample                        = atlas_img->header.sample;
        context.scene->textures[atlas_url] = stex;
        face->ClearDirtyRects();
    }

    // --- mesh capacity. Static text exactly fits its initial layout;
    //     dynamic (script-bound) text reserves headroom so SetText can grow
    //     the string at runtime without reallocating GPU buffers.
    std::size_t initial_codepoints = text::DecodeUtf8(s_text).size();
    bool        has_bg             = obj.opaquebackground;
    std::size_t peak_quads;
    if (wants_dynamic_text) {
        // The glyph mesh renders into the layer RT (sized below to the same
        // ceiling), so the only quads that can ever be visible are those that
        // fit the RT grid. Budget to that cap — terminal/log scripts (e.g.
        // 2268178377) append unbounded text but the layouter clips everything
        // past the RT anyway. Conservative narrow-glyph advance avoids
        // undercounting columns for tight fonts.
        const auto& fm  = face->Metrics();
        const float adv = std::max(1.0f, static_cast<float>(fm.pixel_size) * 0.25f);
        const float lh = fm.line_height > 1.0f ? fm.line_height : static_cast<float>(fm.pixel_size);
        const float rt_w = std::min<float>(static_cast<float>(context.scene->ortho[0]), 1024.0f);
        const float rt_h = std::min<float>(static_cast<float>(context.scene->ortho[1]), 256.0f);
        const std::size_t cols   = static_cast<std::size_t>(std::ceil(rt_w / adv));
        const std::size_t rows   = static_cast<std::size_t>(std::ceil(rt_h / std::max(1.0f, lh)));
        const std::size_t rt_cap = std::clamp<std::size_t>(cols * rows, 64, 16384);
        peak_quads               = std::max<std::size_t>(initial_codepoints * 4, rt_cap);
        if (has_bg) ++peak_quads;
    } else {
        peak_quads = initial_codepoints + (has_bg ? 1u : 0u);
        if (peak_quads == 0) return;
    }

    auto sp_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
    {
        SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord, VAttr::Color }),
                                peak_quads * 4);
        sp_mesh->AddVertexArray(std::move(vertex));
        sp_mesh->AddIndexArray(SceneIndexArray(peak_quads * 6));
    }
    {
        SceneMaterial material;
        material.name                = "text";
        material.textures            = { atlas_url };
        material.defines             = { "g_Texture0" };
        material.blenmode            = BlendMode::Translucent;
        material.customShader.shader = shader;
        sp_mesh->AddMaterial(std::move(material));
    }

    // --- layouter owns the cache (FontFace lifetime) + mesh ref + style.
    text::TextLayoutStyle style;
    style.color                 = { obj.color[0], obj.color[1], obj.color[2] };
    style.alpha                 = obj.alpha;
    style.brightness            = obj.brightness;
    style.opaquebackground      = has_bg;
    style.background_color      = { obj.backgroundcolor[0],
                                    obj.backgroundcolor[1],
                                    obj.backgroundcolor[2] };
    style.background_brightness = obj.backgroundbrightness;
    style.halign                = obj.horizontalalign.empty() ? obj.alignment : obj.horizontalalign;
    style.padding               = static_cast<float>(obj.padding);

    auto align_or_default = [](std::string      value,
                               std::string_view fallback,
                               std::string_view negative,
                               std::string_view positive) {
        if (! value.empty()) return value;
        if (fallback.find(negative) != std::string::npos) return std::string(negative);
        if (fallback.find(positive) != std::string::npos) return std::string(positive);
        return std::string("center");
    };
    const std::string initial_halign =
        align_or_default(obj.horizontalalign, obj.alignment, "left", "right");
    const std::string initial_valign =
        align_or_default(obj.verticalalign, obj.alignment, "top", "bottom");
    style.halign = initial_halign;

    auto layouter = std::make_shared<text::TextLayouter>(face, sp_mesh, style, peak_quads);
    layouter->SetText(s_text);

    float text_w = layouter->TextWidth();
    float text_h = layouter->TextHeight();
    if (text_w <= 0.0f || text_h <= 0.0f) {
        // Empty seed (scripted-only text). Fake a 1×1 bbox so SceneNode /
        // parallax setup still works; the runtime actuator scales the
        // compose node to actual text dims each tick.
        text_w = 1.0f;
        text_h = 1.0f;
    }

    auto sp_node            = rstd::sync::Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                                               Vector3f(obj.scale.data()),
                                                               Vector3f(obj.angles.data()),
                                                               obj.name);
    sp_node->ID()           = obj.id;
    const float text_bbox_w = text_w + 2.0f * style.padding;
    const float text_bbox_h = text_h + 2.0f * style.padding;
    sp_node->SetSize({ text_bbox_w, text_bbox_h });
    sp_node->AddMesh(sp_mesh);

    // sp_node renders into the layer's private ortho RT. Parallax must NOT
    // apply at this stage — the world-space mouse vector would shift glyphs
    // inside ppong_a, but the compose pass samples a fixed UV window, so the
    // shift would manifest as the text appearing to drift in the wrong frame
    // of reference. Parallax goes on compose_node below (world-space quad).
    SceneUniformNodeData svData;
    context.shader_updater->SetNodeData(sp_node.as_ptr(), svData);

    // --- per-layer compose -------------------------------------------------
    // Render the glyphs into a private bbox-sized RT via an ortho camera
    // that maps text-mesh pixel coords 1:1 onto the RT, then composite that
    // RT onto _rt_default with a Translucent fullscreen-quad pass.
    //
    // Two sibling nodes:
    //   * sp_node — text glyphs, layer camera, identity world transform,
    //               appended at scene root (NOT in node_id_map → never
    //               reparented; parent-chain world translation would
    //               otherwise leak through modelTrans and push the glyph
    //               quads past the layer camera's [-bbox/2, +bbox/2]
    //               ortho range, producing post-VS clip outside NDC).
    //   * compose_node — fullscreen quad sampling ppong_a, Translucent
    //               blend, world-positioned. node_id_map registers IT for
    //               parent-chain reparenting + script transforms.
    struct TextAnchorState {
        std::string horizontal;
        std::string vertical;
        Vector3f    origin;
        float       width { 1.0f };
        float       height { 1.0f };
    };
    auto anchor_state = std::make_shared<TextAnchorState>(TextAnchorState {
        .horizontal = initial_halign,
        .vertical   = initial_valign,
        .origin     = Vector3f(obj.origin.data()),
        .width      = text_bbox_w,
        .height     = text_bbox_h,
    });

    auto compose_node = rstd::sync::Arc<SceneNode>::make();
    // Layer RT must be a worst-case sandbox: runtime SetText may expand the
    // glyph bbox well past the seed (clock/date/locale strings). Bound by
    // canvas so we don't waste VRAM, and floor at the parse-time bbox so a
    // narrow runtime string still fits without clipping artifacts.
    const float layer_max_w = std::min<float>(context.scene->ortho[0], 1024.0f);
    const float layer_max_h = std::min<float>(context.scene->ortho[1], 256.0f);
    const i32   layer_w     = std::max<i32>(1, (i32)std::max(text_bbox_w, layer_max_w));
    const i32   layer_h     = std::max<i32>(1, (i32)std::max(text_bbox_h, layer_max_h));
    {
        auto&             scene   = *context.scene;
        const std::string addr    = getAddr(sp_node.as_ptr());
        const std::string ppong_a = std::string(SR_EFFECT_PPONG_PREFIX_A) + addr;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        scene.cameras[addr] = std::make_shared<SceneCamera>(layer_w, layer_h, -1.0f, 1.0f);
        scene.cameras.at(addr)->AttatchNode((*context.effect_camera_node).as_ptr());

        scene.renderTargets[ppong_a] = {
            .width       = layer_w,
            .height      = layer_h,
            .allowReuse  = true,
            .force_clear = true,
        };

        // Empty SceneImageEffectLayer so graph assembly
        // routes sp_node's output to ppong_a (FirstTarget()) without
        // emitting any extra effect passes (m_effects is empty).
        auto layer = std::make_shared<SceneImageEffectLayer>(
            sp_node.as_ptr(), (float)layer_w, (float)layer_h, ppong_a, ppong_a);
        scene.cameras.at(addr)->AttatchImgEffect(layer);

        // Compose quad — sized to the visible text bbox in world space, with
        // UVs subsampling just the central portion of ppong_a where the
        // glyphs actually live (the rest of the RT is transparent slack).
        // Scripted texts mutate text_w/text_h each frame, so the mesh is
        // dynamic and rebuilt by the actuator below.
        compose_node->CopyTrans(*sp_node.as_ptr());
        compose_node->ID() = obj.id;
        compose_node->SetSize({ text_bbox_w, text_bbox_h });
        auto compose_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
        GenCardMesh(*compose_mesh, { (uint16_t)layer_w, (uint16_t)layer_h });

        nlohmann::json pt_json;
        if (! sr::ParseJson(
                fs::GetFileContent(*context.vfs, "/assets/materials/util/effectpassthrough.json"),
                pt_json)) {
            rstd_error("text '{}': parse effectpassthrough.json failed", obj.name);
            return;
        }
        wpscene::Material pt_mat;
        if (! pt_mat.FromJson(pt_json)) {
            rstd_error("text '{}': Material::FromJson failed", obj.name);
            return;
        }
        if (pt_mat.textures.empty())
            pt_mat.textures.push_back(ppong_a);
        else
            pt_mat.textures[0] = ppong_a;

        SceneMaterial        compose_mat;
        SceneUniformNodeData compose_sv;
        WPShaderInfo         compose_si;
        compose_si.baseConstSvs = context.global_base_uniforms;
        // genericimage3 multiplies samples by g_Color4 / g_UserAlpha /
        // g_Brightness — they default to zero when uninitialised, blacking
        // out the composite. Seed neutral values so the layer RT passes
        // through; per-text color/alpha lives in the glyph vertex color
        // attribute on sp_node already.
        compose_si.baseConstSvs["g_Color4"]     = std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
        compose_si.baseConstSvs["g_UserAlpha"]  = 1.0f;
        compose_si.baseConstSvs["g_Brightness"] = 1.0f;
        if (! LoadMaterial(*context.vfs,
                           pt_mat,
                           &scene,
                           compose_node.as_ptr(),
                           &compose_mat,
                           &compose_sv,
                           &compose_si)) {
            rstd_error("text '{}': compose LoadMaterial failed", obj.name);
            return;
        }
        LoadConstvalue(compose_mat, pt_mat, compose_si);
        compose_mat.blenmode = BlendMode::Translucent;
        compose_mesh->AddMaterial(std::move(compose_mat));
        RegisterShaderUserVarIndex(&scene, compose_mesh->Material(), pt_mat, compose_si);
        compose_node->AddMesh(compose_mesh);
        compose_sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        context.shader_updater->SetNodeData(compose_node.as_ptr(), compose_sv);

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
    }

    SceneNode* compose_ptr       = compose_node.as_ptr();
    auto       apply_text_anchor = [compose_ptr, anchor_state]() {
        auto contains = [](const std::string& value, std::string_view token) {
            return value.find(token) != std::string::npos;
        };
        const auto& scale = compose_ptr->Scale();
        Vector3f    pos   = anchor_state->origin;
        if (contains(anchor_state->horizontal, "left"))
            pos.x() += anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->horizontal, "right"))
            pos.x() -= anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->vertical, "top"))
            pos.y() -= anchor_state->height * scale.y() * 0.5f;
        if (contains(anchor_state->vertical, "bottom"))
            pos.y() += anchor_state->height * scale.y() * 0.5f;
        compose_ptr->SetTranslate(pos);
    };

    // Per-frame compose-quad rebuild: world card sized to current text
    // bbox; UVs subsample the central text region of ppong_a (since the
    // ortho is layer-sized but glyphs occupy only text-bbox in the
    // middle).
    auto rebuild_compose =
        [compose_ptr, anchor_state, apply_text_anchor, layer_w, layer_h](float tw, float th) {
            if (tw <= 0.0f) tw = 1.0f;
            if (th <= 0.0f) th = 1.0f;
            anchor_state->width  = tw;
            anchor_state->height = th;
            compose_ptr->SetSize({ tw, th });
            apply_text_anchor();
            const float                 hx = tw * 0.5f;
            const float                 hy = th * 0.5f;
            const std::array<float, 12> pos {
                -hx, -hy, 0.0f, -hx, +hy, 0.0f, +hx, -hy, 0.0f, +hx, +hy, 0.0f,
            };
            const float                u_half = 0.5f * std::min(1.0f, tw / float(layer_w));
            const float                v_half = 0.5f * std::min(1.0f, th / float(layer_h));
            const float                u_l    = 0.5f - u_half;
            const float                u_r    = 0.5f + u_half;
            const float                v_t    = 0.5f - v_half;
            const float                v_b    = 0.5f + v_half;
            const std::array<float, 8> uv {
                u_l, v_b, u_l, v_t, u_r, v_b, u_r, v_t,
            };
            auto* mesh = compose_ptr->Mesh();
            if (mesh == nullptr) return;
            auto& v = mesh->GetVertexArray(0);
            v.SetVertex(WE_IN_POSITION, pos);
            v.SetVertex(WE_IN_TEXCOORD, uv);
            mesh->SetDirty();
        };
    rebuild_compose(text_w, text_h);

    auto apply_text_origin = [anchor_state, apply_text_anchor](const script::ScriptValue& value) {
        Vector3f current = anchor_state->origin;
        auto     next    = ScriptValueAsVec3(value, current);
        if (! next) return;
        anchor_state->origin = *next;
        apply_text_anchor();
    };
    auto apply_text_scale = [compose_ptr, apply_text_anchor](const script::ScriptValue& value) {
        Vector3f current = compose_ptr->Scale();
        auto     next    = ScriptValueAsVec3(value, current);
        if (! next) return;
        compose_ptr->SetScale(*next);
        apply_text_anchor();
    };

    auto set_halign = [layouter, rebuild_compose, anchor_state](std::string_view align) {
        anchor_state->horizontal = std::string(align);
        layouter->SetHorizontalAlign(align);
        rebuild_compose(layouter->TextWidth(), layouter->TextHeight());
    };
    auto set_valign = [anchor_state, apply_text_anchor](std::string_view align) {
        anchor_state->vertical = std::string(align);
        apply_text_anchor();
    };

    if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
    context.script_scene->runtime().RegisterTextAlignSetters(compose_node.as_ptr(),
                                                             anchor_state->horizontal,
                                                             anchor_state->vertical,
                                                             obj.pointsize,
                                                             set_halign,
                                                             set_valign);

    // Transform-style script bindings (origin/scale/angles) animate the
    // composite quad in world space, not the layer-space glyph node.
    WireFieldScripts(
        context, compose_node, obj.field_bindings, apply_text_origin, apply_text_scale);
    if (! obj.visible_user_key.empty()) compose_node->SetVisibleUserKey(obj.visible_user_key);

    // --- text-content actuator. Captures the layouter + a closure that
    // re-rasterises new codepoints, lays them out, and rebuilds the
    // compose quad to the new text dims. Runs on the render thread, which
    // is also the JS thread — no synchronization needed.
    auto set_text = [layouter, face, rebuild_compose](std::string_view s) {
        face->Populate(text::DecodeUtf8(s));
        layouter->SetText(s);
        rebuild_compose(layouter->TextWidth(), layouter->TextHeight());
    };
    if (has_text_script) {
        const auto& sb = text_binding_it->second;
        if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
        auto&       ss  = *context.script_scene;
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::String,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       sp_node.as_ptr());
        if (fs) {
            ss.AddActuator({
                fs,
                [set_text](const script::ScriptValue& v) {
                    if (auto* p = std::get_if<script::StringValue>(&v)) set_text(p->s);
                },
            });
        }
    }
    // Scripts attached to non-text fields can mutate `thisLayer.text`
    // directly. Register the setter so NodeSetText routes those writes
    // back into the layouter. compose_node is the SceneNode every
    // field-bound script's `thisLayer` resolves to (WireFieldScripts at
    // line above).
    if (wants_dynamic_text) {
        if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
        context.script_scene->runtime().RegisterTextSetter(compose_node.as_ptr(),
                                                           [set_text](std::string_view s) {
                                                               set_text(s);
                                                           });
    }

    // sp_node renders glyphs to its private RT and stays outside the
    // parent chain — append directly to scene root, never reparented.
    // compose_node owns the world position and goes through the JSON-order
    // attach phase (FinalizeScene) like any other layer.
    context.scene->sceneGraph->AppendChild(sp_node.clone());
    context.node_id_map[obj.id] = {
        obj.parent,
        rstd::Some(compose_node.clone()),
        nullptr,
        obj.attachment,
        nullptr,
        [anchor_state, apply_text_anchor](const Vector3f& offset) {
            anchor_state->origin += offset;
            apply_text_anchor();
        },
    };

    const char* scripted_tag = has_text_script            ? " [scripted]"
                               : has_indirect_text_script ? " [scripted-indirect]"
                                                          : "";
    rstd_info("text '{}': initial=\"{}\" px={} peak_quads={} bbox={}x{}{} ({})",
              obj.name,
              s_text,
              px,
              peak_quads,
              static_cast<int>(text_w),
              static_cast<int>(text_h),
              std::string_view(scripted_tag),
              resolved.source);
}

const nlohmann::json*
UserPropertyValue(const std::unordered_map<std::string, nlohmann::json>* user_props,
                  std::string_view                                       key) {
    if (user_props == nullptr || key.empty()) return nullptr;
    auto it = user_props->find(std::string(key));
    if (it == user_props->end()) return nullptr;
    if (it->second.is_object() && it->second.contains("value")) return &it->second.at("value");
    return &it->second;
}

std::optional<std::string> JsonScalarString(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<std::int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<std::uint64_t>());
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    return std::nullopt;
}

bool JsonScalarEquals(const nlohmann::json& a, const nlohmann::json& b) {
    if (a == b) return true;
    auto as = JsonScalarString(a);
    auto bs = JsonScalarString(b);
    if (! as || ! bs) return false;
    if (*as == *bs) return true;
    if (a.is_boolean() && b.is_string()) {
        const auto s = b.get<std::string>();
        return (a.get<bool>() && s == "1") || (! a.get<bool>() && s == "0");
    }
    if (a.is_string() && b.is_boolean()) {
        const auto s = a.get<std::string>();
        return (b.get<bool>() && s == "1") || (! b.get<bool>() && s == "0");
    }
    return false;
}

bool ResolveVisibleUserBinding(bool& visible, const wpscene::VisibleUserBinding& binding,
                               const std::unordered_map<std::string, nlohmann::json>* user_props) {
    if (binding.empty()) return false;
    const nlohmann::json* value = UserPropertyValue(user_props, binding.name);
    if (binding.has_condition) {
        if (value != nullptr) visible = JsonScalarEquals(*value, binding.condition);
        return true;
    }
    if (value != nullptr && value->is_boolean()) visible = value->get<bool>();
    return true;
}

struct ObjectVisibilityInfo {
    std::uint32_t parent { 0 };
    bool          visible { true };
    bool          user_bound { false };
};

ObjectVisibilityInfo
ResolveObjectVisibility(const nlohmann::json&                                  json_obj,
                        const std::unordered_map<std::string, nlohmann::json>* user_props) {
    ObjectVisibilityInfo info;
    sr::GetJsonValue(json_obj, "parent", info.parent, false);
    sr::GetJsonValue(json_obj, "visible", info.visible, false);
    wpscene::VisibleUserBinding binding;
    wpscene::ReadVisibleUserBinding(json_obj, binding);
    info.user_bound = ! binding.empty();
    ResolveVisibleUserBinding(info.visible, binding, user_props);
    return info;
}

std::unordered_map<std::int32_t, ObjectVisibilityInfo>
BuildObjectVisibilityInfo(const nlohmann::json&                                  json,
                          const std::unordered_map<std::string, nlohmann::json>* user_props) {
    std::unordered_map<std::int32_t, ObjectVisibilityInfo> out;
    if (! json.contains("objects") || ! json.at("objects").is_array()) return out;
    for (const auto& obj : json.at("objects")) {
        if (! obj.is_object() || ! obj.contains("id") || ! obj.at("id").is_number_integer())
            continue;
        out[obj.at("id").get<std::int32_t>()] = ResolveObjectVisibility(obj, user_props);
    }
    return out;
}

bool HasHiddenUserAncestor(std::uint32_t                                                 id,
                           const std::unordered_map<std::int32_t, ObjectVisibilityInfo>& objects) {
    std::unordered_set<std::uint32_t> seen;
    auto                              it = objects.find(static_cast<std::int32_t>(id));
    if (it == objects.end()) return false;
    std::uint32_t parent = it->second.parent;
    while (parent != 0 && seen.insert(parent).second) {
        auto pit = objects.find(static_cast<std::int32_t>(parent));
        if (pit == objects.end()) return false;
        if (pit->second.user_bound && ! pit->second.visible) return true;
        parent = pit->second.parent;
    }
    return false;
}

Set<std::int32_t>
CollectHiddenLinkedSourceIds(const nlohmann::json& json, const Set<std::int32_t>& linked_source_ids,
                             const std::unordered_map<std::string, nlohmann::json>* user_props) {
    Set<std::int32_t> out;
    auto              visibility_info = BuildObjectVisibilityInfo(json, user_props);
    for (std::int32_t id : linked_source_ids) {
        auto it = visibility_info.find(id);
        if (it == visibility_info.end()) continue;
        if (! it->second.visible ||
            HasHiddenUserAncestor(static_cast<std::uint32_t>(id), visibility_info)) {
            out.insert(id);
        }
    }
    return out;
}

template<typename T>
void AddSceneObject(std::vector<SceneObjectVar>& objs, const nlohmann::json& json_obj, fs::VFS& vfs,
                    wpscene::SceneVersion                                  v,
                    const std::unordered_map<std::string, nlohmann::json>* user_props,
                    const Set<std::int32_t>* linked_source_ids, bool force_invisible) {
    T scene_obj;
    if (! scene_obj.FromJson(json_obj, vfs, v)) {
        rstd_error("parse scene object failed, name: {}", scene_obj.name);
        return;
    }
    ResolveVisibleUserBinding(scene_obj.visible, scene_obj.visible_user, user_props);
    if (force_invisible) scene_obj.visible = false;
    const bool preserve_hidden_link_source =
        ! scene_obj.visible && linked_source_ids != nullptr &&
        linked_source_ids->count(static_cast<std::int32_t>(scene_obj.id)) != 0;
    // Image objects keep going even when visible=false: another layer's
    // material may reference them via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder later decides whether to actually emit passes.
    if constexpr (! std::is_same_v<T, wpscene::ImageObject>) {
        if (! scene_obj.visible && ! preserve_hidden_link_source) return;
        if (preserve_hidden_link_source) scene_obj.visible = true;
    }
    objs.push_back(scene_obj);
}
} // namespace

namespace sr
{

std::vector<SceneObjectVar>
ExpandObjects(const nlohmann::json& json, fs::VFS& vfs, wpscene::SceneVersion v,
              const std::unordered_map<std::string, nlohmann::json>* user_props,
              const Set<std::int32_t>*                               linked_source_ids) {
    std::vector<SceneObjectVar> scene_objs;
    if (! json.contains("objects")) return scene_objs;
    auto visibility_info = BuildObjectVisibilityInfo(json, user_props);
    for (auto& obj : json.at("objects")) {
        bool force_invisible = false;
        if (obj.is_object() && obj.contains("id") && obj.at("id").is_number_integer()) {
            force_invisible = HasHiddenUserAncestor((std::uint32_t)obj.at("id").get<std::int32_t>(),
                                                    visibility_info);
        }
        // Order matters: text/model/camera kinds coexist with null
        // image/particle/sound/light fields, so the renderer-supported
        // kinds get first pick. Falls through to the parsing-only kinds
        // (no rendering yet) so the data stays absorbed.
        if (obj.contains("image") && ! obj.at("image").is_null()) {
            AddSceneObject<wpscene::ImageObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("particle") && ! obj.at("particle").is_null()) {
            AddSceneObject<wpscene::ParticleObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("sound") && ! obj.at("sound").is_null()) {
            AddSceneObject<wpscene::SoundObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("light") && ! obj.at("light").is_null()) {
            AddSceneObject<wpscene::LightObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("text") && ! obj.at("text").is_null()) {
            AddSceneObject<wpscene::TextObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("model") && ! obj.at("model").is_null()) {
            AddSceneObject<wpscene::ModelObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (obj.contains("camera") && ! obj.at("camera").is_null()) {
            AddSceneObject<wpscene::CameraObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        }
    }
    return scene_objs;
}

void AdjustAutoOrthoProjection(wpscene::SceneMetadata&         sc,
                               std::span<const SceneObjectVar> scene_objs) {
    if (! sc.general.orthogonalprojection.auto_) return;
    i32 w = 0, h = 0;
    for (const auto& obj : scene_objs) {
        const auto* img = std::get_if<wpscene::ImageObject>(&obj);
        if (img == nullptr) continue;
        i32 size = (i32)(img->size.at(0) * img->size.at(1));
        if (size > w * h) {
            w = (i32)img->size.at(0);
            h = (i32)img->size.at(1);
        }
    }
    sc.general.orthogonalprojection.width  = w;
    sc.general.orthogonalprojection.height = h;
}

ParseContext BuildContext(fs::VFS& vfs, std::string_view scene_id, wpscene::SceneMetadata& sc,
                          const std::unordered_map<std::string, nlohmann::json>* user_properties) {
    ParseContext context;
    InitContext(context, vfs, sc);
    ParseCamera(context, sc);
    context.user_properties = user_properties;

    context.scene->renderTargets[SpecTex_Default.data()] = {
        .width             = context.ortho_w,
        .height            = context.ortho_h,
        .withDepth         = true,
        .bind              = { .enable = true, .screen = true },
        .preserve_on_write = true,
    };
    context.scene->renderTargets[WE_MIP_MAPPED_FRAME_BUFFER.data()] = {
        .width      = context.ortho_w,
        .height     = context.ortho_h,
        .has_mipmap = true,
        .bind       = { .enable = true, .name = SpecTex_Default.data() },
    };

    context.scene->scene_id = scene_id;
    return context;
}

void ProcessObjects(ParseContext& context, std::span<SceneObjectVar> scene_objs,
                    wavsen::audio::SoundManager* sm, ProcessOpts opts) {
    MaterialProgramCompiler::InitGlslang();
    IndexImageTextureFallbacks(context, scene_objs);

    for (SceneObjectVar& obj : scene_objs) {
        std::visit(visitor::overload {
                       [&context, opts](wpscene::ImageObject& obj) {
                           if (opts.kinds & ProcessOpts::Image) ParseImageObj(context, obj);
                       },
                       [&context, opts](wpscene::ParticleObject& obj) {
                           if (opts.kinds & ProcessOpts::Particle) ParseParticleObj(context, obj);
                       },
                       [&context, opts, sm](wpscene::SoundObject& obj) {
                           if ((opts.kinds & ProcessOpts::Sound) && sm)
                               ParseSoundObj(context, obj, *sm);
                       },
                       [&context, opts](wpscene::LightObject& obj) {
                           if (opts.kinds & ProcessOpts::Light) ParseLightObj(context, obj);
                       },
                       // Stage A text-layer support: ParseTextObj loads the
                       // font, lays glyphs into a CPU-side atlas, and logs
                       // the resolved layout. Scene-graph emission is still
                       // pending Stage B (custom shader + atlas texture
                       // through the existing imageParser path).
                       [&context, opts](wpscene::TextObject& obj) {
                           if (opts.kinds & ProcessOpts::Text) ParseTextObj(context, obj);
                       },
                       [&context, opts](wpscene::ModelObject& obj) {
                           if (opts.kinds & ProcessOpts::Model) ParseModelObj(context, obj);
                       },
                       [&context](wpscene::CameraObject& obj) {
                           ParseCameraObj(context, obj);
                       },
                   },
                   obj);
    }

    MaterialProgramCompiler::FinalGlslang();
}

std::shared_ptr<Scene> FinalizeScene(ParseContext& context) {
    // Single attach phase. Each registered node was created in JSON
    // declaration order (node_id_order) but not yet inserted into the scene
    // graph. Walk that order and AppendChild to parent (or root). Result:
    // child lists at every depth match scene.json declaration order, which
    // is what WE treats as z-order.
    int attached = 0, missing_parent = 0;
    for (auto id : context.node_id_order) {
        auto rit = context.node_id_map.find(id);
        if (rit == context.node_id_map.end() || rit->second.node.is_none()) continue;
        auto&                        ref         = rit->second;
        SceneNode*                   parent_node = context.scene->sceneGraph.as_ptr();
        const ParseContext::NodeRef* parent_ref  = nullptr;
        if (ref.parent_id != 0) {
            auto pit = context.node_id_map.find(static_cast<std::int32_t>(ref.parent_id));
            if (pit == context.node_id_map.end() || pit->second.node.is_none()) {
                missing_parent++;
                continue;
            }
            parent_node = (*pit->second.node).as_ptr();
            parent_ref  = &pit->second;
        }
        // MDAT bone-anchor: if the child carries `attachment = "<name>"` and
        // the parent owns a puppet with a matching MDAT entry, offset the
        // child by the attachment's local-space translation (e.g. forehead
        // bangs anchored to the head bone of the character body puppet).
        // Bone-following animation is deliberately omitted — static bind
        // position is enough for the visible-frame placement; sim bones do
        // not drift far from bind in practice.
        if (! ref.attachment.empty() && parent_ref && parent_ref->puppet) {
            const auto& puppet = *parent_ref->puppet;
            const auto& atts   = puppet.attachments;
            auto        ait    = std::find_if(atts.begin(), atts.end(), [&](const auto& a) {
                return a.name == ref.attachment;
            });
            if (ait != atts.end() && ait->bone_index < puppet.bones.size()) {
                // Walk the original on-file parent chain to compose the
                // anchored bone's puppet-local bind. ApplyMDLS3CentroidPivot
                // has already flattened bind_parent / anim_parent for the
                // skinning path; file_parent is preserved for this lookup.
                Eigen::Affine3f       bone_world = Eigen::Affine3f::Identity();
                std::vector<uint32_t> chain;
                uint32_t              bi = ait->bone_index;
                while (bi != WPPuppet::NO_PARENT && bi < puppet.bones.size()) {
                    chain.push_back(bi);
                    bi = puppet.bones[bi].file_parent;
                }
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    bone_world = bone_world * puppet.bones[*it].local_bind;
                }
                auto apply_bind_offset = [&]() {
                    Eigen::Affine3f anchor = bone_world * ait->local_xform;
                    Vector3f        offset = anchor.translation();
                    if (ref.apply_attachment_offset) {
                        ref.apply_attachment_offset(offset);
                    } else {
                        (*ref.node)->SetTranslate((*ref.node)->Translate() + offset);
                    }
                };
                if (! ref.apply_attachment_offset && parent_ref->puppet_layer) {
                    SceneNode* node       = (*ref.node).as_ptr();
                    auto       layer      = parent_ref->puppet_layer;
                    auto       attachment = *ait;
                    auto       base       = node->Translate();
                    auto       update     = [node, layer, attachment, base](double time) {
                        auto bone = layer->boneTransform(
                            static_cast<uint32_t>(attachment.bone_index) + 1u, time);
                        if (! bone) return;
                        node->SetTranslate(base + (*bone * attachment.local_xform).translation());
                    };
                    update(context.scene->elapsingTime);
                    context.scene->transform_updaters.push_back(std::move(update));
                } else {
                    apply_bind_offset();
                }
            }
        }
        parent_node->AppendChild((*ref.node).clone());
        attached++;

        // Attach this layer's fanout clones (audio bars) right after it, so
        // all bars sit at the template's z-position in the parent child list.
        if (auto cit = context.layer_clones.find(id); cit != context.layer_clones.end()) {
            for (auto& clone : cit->second) {
                parent_node->AppendChild(rstd::move(clone));
                attached++;
            }
        }
    }
    rstd_info("attach: {}/{} nodes ({} missing parents)",
              attached,
              context.node_id_map.size(),
              missing_parent);

    // If any object during the visit installed a script binding, hand the
    // ScriptScene off to the Scene now. The renderer ticks it once per
    // frame via sr::script::TickSceneScripts. Empty ScriptScenes are
    // skipped so image-only pkgs don't pay any runtime cost.
    if (context.script_scene && ! context.script_scene->empty()) {
        // Hand the scene root to the JS runtime so `thisScene.getLayer(name)`
        // can resolve against the live graph. The renderer also ticks the
        // ScriptScene once per frame via sr::script::TickSceneScripts.
        context.script_scene->runtime().SetSceneRoot(context.scene->sceneGraph.as_ptr());
        sr::script::InstallScriptScene(*context.scene, std::move(context.script_scene));
    }
    return context.scene;
}

void BuildBloomPostProcess(ParseContext& context, fs::VFS& vfs, const wpscene::SceneGeneral& g) {
    auto& scene = *context.scene;

    auto declare_rt = [&](std::string name, float inv_scale) {
        SceneRenderTarget rt {};
        rt.width                             = 2;
        rt.height                            = 2;
        rt.allowReuse                        = true;
        rt.bind.enable                       = true;
        rt.bind.screen                       = true;
        rt.bind.scale                        = inv_scale;
        scene.renderTargets[std::move(name)] = rt;
    };
    declare_rt("_rt_bloom_mip1", g.hdr ? 0.5f : 0.25f);
    declare_rt("_rt_bloom_mip2", 0.25f);
    declare_rt("_rt_bloom_combine", 1.0f);

    const std::unordered_map<std::string, std::string> fboMap {
        { "previous", std::string(SpecTex_Default) },
        { "_rt_default", std::string(SpecTex_Default) },
        { "_rt_bloom_mip1", "_rt_bloom_mip1" },
        { "_rt_bloom_mip2", "_rt_bloom_mip2" },
        { "_rt_bloom_combine", "_rt_bloom_combine" },
    };

    auto pp  = std::make_shared<ScenePostProcess>();
    pp->name = "__bloom";

    auto add_pass = [&](const char* mat_relpath,
                        std::vector<wpscene::MaterialPassBindItem>
                                                                binds,
                        std::string                             output_rt,
                        std::function<void(wpscene::Material&)> mutate         = nullptr,
                        std::function<void(WPShaderInfo&)>      configure_info = nullptr) -> bool {
        nlohmann::json jMat;
        if (! sr::ParseJson(fs::GetFileContent(vfs, std::string("/assets/") + mat_relpath),
                             jMat)) {
            rstd_error("bloom: parse material json failed: {}", mat_relpath);
            return false;
        }
        wpscene::Material wpmat;
        if (! wpmat.FromJson(jMat)) {
            rstd_error("bloom: Material::FromJson failed: {}", mat_relpath);
            return false;
        }
        ApplyTextureBinds(wpmat, std::span(binds), fboMap);
        if (mutate) mutate(wpmat);

        WPShaderInfo wpShaderInfo;
        wpShaderInfo.baseConstSvs = context.global_base_uniforms;
        if (configure_info) configure_info(wpShaderInfo);

        auto                 pp_node = rstd::sync::Arc<SceneNode>::make();
        SceneMaterial        material;
        SceneUniformNodeData svData;
        if (! LoadMaterial(
                vfs, wpmat, &scene, pp_node.as_ptr(), &material, &svData, &wpShaderInfo)) {
            rstd_error("bloom: LoadMaterial failed: {}", mat_relpath);
            return false;
        }
        LoadConstvalue(material, wpmat, wpShaderInfo);

        auto pp_mesh = std::make_shared<SceneMesh>();
        pp_mesh->ChangeMeshDataFrom(scene.default_effect_mesh);
        pp_mesh->AddMaterial(std::move(material));
        RegisterShaderUserVarIndex(&scene, pp_mesh->Material(), wpmat, wpShaderInfo);
        pp_node->AddMesh(pp_mesh);

        // Camera name drives CustomShaderPass color-write mask: empty or
        // "global" cameras strip the A bit. Anchor to the existing
        // "effect" cam (2x2 ortho, identity for our NDC fullscreen quads)
        // so A=1.0 from the shader survives in the present path.
        pp_node->SetCamera("effect");
        context.shader_updater->SetNodeData(pp_node.as_ptr(), svData);

        pp->steps.emplace_back(ScenePostProcessPass {
            .node   = rstd::move(pp_node),
            .output = std::move(output_rt),
        });
        return true;
    };

    if (g.hdr) {
        auto hdr_offsets = [](float source_scale) {
            float x = 1.0f / (1920.0f * source_scale);
            float y = 1.0f / (1080.0f * source_scale);
            return std::array { x, y, -x, -y };
        };
        auto set_render_var = [](WPShaderInfo& info, std::array<float, 4> value) {
            info.baseConstSvs["g_RenderVar0"] = value;
        };
        float threshold = g.bloomhdrthreshold;
        float knee      = threshold * g.bloomhdrfeather;
        float scatter   = g.bloomhdrscatter > 0.0f ? g.bloomhdrscatter : 1.0f;

        if (! add_pass(
                "materials/util/hdr_downsample_bloom.json",
                { { "previous", 0 } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["bloomstrength"] = { g.bloomhdrstrength };
                    m.constantshadervalues["blend"]         = {
                        threshold,
                        threshold - knee,
                        2.0f * knee,
                        knee > 0.0f ? 0.25f / knee : 0.0f,
                    };
                    m.constantshadervalues["bloomtint"] = {
                        g.bloomtint[0],
                        g.bloomtint[1],
                        g.bloomtint[2],
                    };
                },
                [&](WPShaderInfo& info) {
                    set_render_var(info, hdr_offsets(1.0f));
                }))
            return;

        if (! add_pass("materials/util/hdr_downsample.json",
                       { { "_rt_bloom_mip1", 0 } },
                       "_rt_bloom_mip2",
                       nullptr,
                       [&](WPShaderInfo& info) {
                           set_render_var(info, hdr_offsets(0.5f));
                       }))
            return;

        if (! add_pass(
                "materials/util/hdr_upsample.json",
                { { "_rt_bloom_mip2", 0 } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["scatter"] = { scatter };
                },
                [&](WPShaderInfo& info) {
                    set_render_var(info, hdr_offsets(0.25f));
                }))
            return;

        if (! add_pass("materials/util/combine_hdr_upsample_linear.json",
                       { { "previous", 0 }, { "_rt_bloom_mip1", 1 } },
                       "_rt_bloom_combine",
                       nullptr,
                       [&](WPShaderInfo& info) {
                           set_render_var(info, { 1.0f, 0.0f, 0.0f, 0.0f });
                       }))
            return;
    } else {
        if (! add_pass("materials/util/downsample_quarter_bloom.json",
                       { { "previous", 0 } },
                       "_rt_bloom_mip1",
                       [&](wpscene::Material& m) {
                           m.constantshadervalues["bloomstrength"]  = { g.bloomstrength };
                           m.constantshadervalues["bloomthreshold"] = { g.bloomthreshold };
                           m.constantshadervalues["bloomtint"]      = {
                               g.bloomtint[0],
                               g.bloomtint[1],
                               g.bloomtint[2],
                           };
                       }))
            return;

        if (! add_pass("materials/util/downsample_eighth_blur_v.json",
                       { { "_rt_bloom_mip1", 0 } },
                       "_rt_bloom_mip2"))
            return;

        if (! add_pass(
                "materials/util/blur_h_bloom.json", { { "_rt_bloom_mip2", 0 } }, "_rt_bloom_mip1"))
            return;

        if (! add_pass("materials/util/combine_ldr.json",
                       { { "previous", 0 }, { "_rt_bloom_mip1", 1 } },
                       "_rt_bloom_combine"))
            return;
    }

    pp->steps.emplace_back(ScenePostProcessCopy {
        .src = "_rt_bloom_combine",
        .dst = std::string(SpecTex_Default),
    });

    scene.post_processes.push_back(std::move(pp));
}

} // namespace sr

std::shared_ptr<Scene> WallpaperSceneCompiler::Parse(std::string_view scene_id, const std::string& buf,
                                            fs::VFS& vfs, wavsen::audio::SoundManager& sm,
                                            wpscene::SceneVersion pkg_version) {
    auto doc = wpscene::ParseSceneDocumentJson(buf, pkg_version);
    if (! doc) return nullptr;
    return Parse(scene_id, *doc, vfs, sm);
}

std::shared_ptr<Scene> WallpaperSceneCompiler::Parse(std::string_view              scene_id,
                                            const wpscene::SceneDocument& doc, fs::VFS& vfs,
                                            wavsen::audio::SoundManager& sm) {
    const auto& json = doc.root_json;
    auto        sc   = doc.metadata;
    rstd_info("scene: pkg_version={} scene_json_version={}",
              static_cast<unsigned>(sc.pkg_version),
              static_cast<unsigned>(sc.scene_json_version));

    auto linked_source_ids = CollectLinkedSourceIdsFromJson(json);
    auto scene_objs =
        ExpandObjects(json, vfs, sc.pkg_version, m_user_properties, &linked_source_ids);
    AdjustAutoOrthoProjection(sc, scene_objs);
    auto context = BuildContext(vfs, scene_id, sc, m_user_properties);
    context.hidden_link_source_ids =
        CollectHiddenLinkedSourceIds(json, linked_source_ids, m_user_properties);

    // Single JSON-order walk:
    // - record every object's id (and parent_id) in declaration order so the
    //   final attach phase can rebuild the scene tree with matching child
    //   ordering — z-order in WE is JSON declaration order.
    // - for transform-only "container" layers (no image/particle/sound/light/
    //   text/model/camera field, e.g. workshop 3327063360's "组件"), create the
    //   bare SceneNode here so ParseImageObj children can find their parent.
    //   Their `visible:false` form is preserved as a parent anchor.
    if (json.contains("objects")) {
        auto visibility_info = BuildObjectVisibilityInfo(json, m_user_properties);
        auto has_kind        = [](const nlohmann::json& o) {
            for (const char* k :
                 { "image", "particle", "sound", "light", "text", "model", "camera" }) {
                if (o.contains(k) && ! o.at(k).is_null()) return true;
            }
            return false;
        };
        auto read_vec3 = [](const nlohmann::json& o, const char* key, std::array<float, 3>& out) {
            if (! o.contains(key)) return;
            const auto& v = o.at(key);
            std::string s;
            if (v.is_string()) {
                s = v.get<std::string>();
            } else if (v.is_object() && v.contains("value") && v.at("value").is_string()) {
                s = v.at("value").get<std::string>();
            } else {
                return;
            }
            std::sscanf(s.c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
        };
        for (const auto& o : json.at("objects")) {
            if (! o.is_object() || ! o.contains("id")) continue;
            std::int32_t id = o.at("id").get<std::int32_t>();
            context.node_id_order.push_back(id);
            if (has_kind(o)) continue;
            std::uint32_t parent = 0;
            if (o.contains("parent")) parent = o.at("parent").get<std::uint32_t>();
            std::string name;
            if (o.contains("name") && o.at("name").is_string())
                name = o.at("name").get<std::string>();
            std::array<float, 3> origin { 0, 0, 0 }, scale { 1, 1, 1 }, angles { 0, 0, 0 };
            read_vec3(o, "origin", origin);
            read_vec3(o, "scale", scale);
            read_vec3(o, "angles", angles);
            auto node = rstd::sync::Arc<SceneNode>::make(
                Vector3f(origin.data()), Vector3f(scale.data()), Vector3f(angles.data()), name);
            node->ID() = id;
            auto vit   = visibility_info.find(id);
            if (vit != visibility_info.end()) {
                bool visible = vit->second.visible &&
                               ! HasHiddenUserAncestor((std::uint32_t)id, visibility_info);
                if (! visible) node->SetVisible(false);
            }
            wpscene::VisibleUserBinding visible_user;
            wpscene::ReadVisibleUserBinding(o, visible_user);
            if (! visible_user.empty()) node->SetVisibleUserKey(visible_user.name);
            wpscene::FieldBindings fb;
            wpscene::AbsorbAllFieldBindings(o, fb);
            WireFieldScripts(context, node, fb);
            std::string attachment;
            if (o.contains("attachment") && o.at("attachment").is_string()) {
                attachment = o.at("attachment").get<std::string>();
            }
            context.node_id_map[id] = {
                parent, rstd::Some(node.clone()), nullptr, std::move(attachment), nullptr
            };
        }
    }

    ProcessObjects(context, scene_objs, &sm);

    if (sc.general.bloom) {
        BuildBloomPostProcess(context, vfs, sc.general);
    }
    return FinalizeScene(context);
}
