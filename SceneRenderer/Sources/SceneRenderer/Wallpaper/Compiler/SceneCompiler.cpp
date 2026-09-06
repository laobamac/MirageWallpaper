module;

#include <algorithm>
#include <cmath>
#include <sstream>

#include <rstd/macro.hpp>

#include "Utils/StringUtil.h"
#include "Utils/Sha.hpp"

module sr.pkg.parse;
import eigen;
import sr.json;
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
struct SceneNodeArcHold {
    rstd::sync::Arc<SceneNode> node;

    explicit SceneNodeArcHold(rstd::sync::Arc<SceneNode> n): node(rstd::move(n)) {}
    SceneNodeArcHold(const SceneNodeArcHold& other): node(other.node.clone()) {}
    SceneNodeArcHold(SceneNodeArcHold&&) noexcept            = default;
    SceneNodeArcHold& operator=(SceneNodeArcHold&&) noexcept = default;
    SceneNodeArcHold& operator=(const SceneNodeArcHold&)     = delete;

    SceneNode* get() const { return node.as_ptr(); }
};

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

    // Public constants: registerAudioBuffers(engine.AUDIO_RESOLUTION_64).
    constexpr std::string_view resolution_prefix = "engine.AUDIO_RESOLUTION_";
    if (src.substr(pos).starts_with(resolution_prefix)) {
        const usize value_pos = pos + resolution_prefix.size();
        if (value_pos >= src.size() || ! is_digit(src[value_pos])) return 0;
        const unsigned value = read_num(value_pos);
        usize          end   = value_pos;
        while (end < src.size() && is_digit(src[end])) ++end;
        if (end < src.size() && is_ident(src[end])) return 0;
        if (value == 16 || value == 32 || value == 64) return value;
        return 0;
    }

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

bool SourceWritesLayerText(std::string_view src) {
    const bool writes_text = src.find(".text") != std::string_view::npos ||
                             src.find("[\"text\"]") != std::string_view::npos ||
                             src.find("['text']") != std::string_view::npos;
    if (! writes_text) return false;
    return src.find("getLayer") != std::string_view::npos;
}

bool FieldBindingsWriteLayerText(const wpscene::FieldBindings& fb) {
    for (const auto& [_, sb] : fb.scripts) {
        if (SourceWritesLayerText(sb.source)) return true;
    }
    return false;
}

bool SceneWritesLayerText(std::span<const SceneObjectVar> scene_objs) {
    for (const auto& obj : scene_objs) {
        bool found = false;
        std::visit(
            visitor::overload {
                [&found](const auto& scene_obj) {
                    found = FieldBindingsWriteLayerText(scene_obj.field_bindings);
                },
            },
            obj);
        if (found) return true;
    }
    return false;
}

bool SceneHasScripts(std::span<const SceneObjectVar> scene_objs) {
    for (const auto& obj : scene_objs) {
        bool found = false;
        std::visit(
            visitor::overload {
                [&found](const auto& scene_obj) {
                    found = ! scene_obj.field_bindings.scripts.empty();
                },
            },
            obj);
        if (found) return true;
    }
    return false;
}

bool SceneAccessesEffects(std::span<const SceneObjectVar> scene_objs) {
    for (const auto& obj : scene_objs) {
        bool found = false;
        std::visit(
            visitor::overload {
                [&found](const auto& scene_obj) {
                    for (const auto& [_, binding] : scene_obj.field_bindings.scripts) {
                        if (binding.source.find("getEffect") != std::string_view::npos) {
                            found = true;
                            break;
                        }
                    }
                },
            },
            obj);
        if (found) return true;
    }
    return false;
}

template<typename Predicate>
bool AnyObjectFieldBindings(const Json& json, Predicate&& predicate) {
    auto objects = json.get("objects");
    if (objects.is_none()) return false;
    auto object_array = (*objects)->as_array();
    if (object_array.is_none()) return false;
    for (const auto& o : **object_array) {
        if (! o.is_object()) continue;
        wpscene::FieldBindings fb;
        if (wpscene::AbsorbAllFieldBindings(o, fb) == 0) continue;
        if (predicate(fb)) return true;
    }
    return false;
}

bool FieldBindingsMatchSource(const wpscene::FieldBindings& fb, std::string_view needle) {
    for (const auto& [_, binding] : fb.scripts) {
        if (binding.source.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool SceneWritesLayerText(const Json& json, std::span<const SceneObjectVar> scene_objs) {
    if (SceneWritesLayerText(scene_objs)) return true;
    return AnyObjectFieldBindings(json, [](const wpscene::FieldBindings& fb) {
        return FieldBindingsWriteLayerText(fb);
    });
}

bool SceneAccessesEffects(const Json& json, std::span<const SceneObjectVar> scene_objs) {
    if (SceneAccessesEffects(scene_objs)) return true;
    return AnyObjectFieldBindings(json, [](const wpscene::FieldBindings& fb) {
        return FieldBindingsMatchSource(fb, "getEffect");
    });
}

bool SceneUsesAudioScripts(std::span<const SceneObjectVar> scene_objs) {
    for (const auto& obj : scene_objs) {
        bool found = false;
        std::visit(
            visitor::overload {
                [&found](const auto& scene_obj) {
                    for (const auto& [_, binding] : scene_obj.field_bindings.scripts) {
                        if (binding.source.find("registerAudioBuffers") != std::string_view::npos) {
                            found = true;
                            break;
                        }
                    }
                },
            },
            obj);
        if (found) return true;
    }
    return false;
}

bool SceneHasScripts(const Json& json, std::span<const SceneObjectVar> scene_objs) {
    if (SceneHasScripts(scene_objs)) return true;
    return AnyObjectFieldBindings(json, [](const wpscene::FieldBindings& fb) {
        return ! fb.scripts.empty();
    });
}

bool SceneUsesAudioScripts(const Json& json, std::span<const SceneObjectVar> scene_objs) {
    if (SceneUsesAudioScripts(scene_objs)) return true;
    return AnyObjectFieldBindings(json, [](const wpscene::FieldBindings& fb) {
        return FieldBindingsMatchSource(fb, "registerAudioBuffers");
    });
}

std::optional<std::array<float, 2>> ResolveImageAssetSize(ParseContext&    context,
                                                          std::string_view image_path) {    auto info = wpscene::LoadImageAssetInfo(*context.vfs, image_path);
    if (! info) return std::nullopt;
    if (info->size) return info->size;
    if (info->first_texture.empty()) return std::nullopt;

    int32_t    w      = 0;
    int32_t    h      = 0;
    const auto header = context.scene->imageParser->ParseHeader(info->first_texture);
    if (header.isSprite && header.spriteAnim.numFrames() > 0) {
        const auto& frame = header.spriteAnim.GetCurFrame();
        w                 = static_cast<int32_t>(std::round(frame.width));
        h                 = static_cast<int32_t>(std::round(frame.height));
    } else {
        w = header.width > 0 ? header.width : header.mapWidth;
        h = header.height > 0 ? header.height : header.mapHeight;
    }
    if (w <= 0 || h <= 0) return std::nullopt;
    return std::array { static_cast<float>(w), static_cast<float>(h) };
}

bool AppendLayerCompositePassthroughEffect(fs::VFS& vfs, wpscene::ImageObject& image) {
    wpscene::Material material;
    auto              parsed =
        sr::ParseJson(fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"));
    if (parsed.is_err()) {
        rstd_error(
            "parse effectpassthrough.json failed for '{}': {}", image.name, parsed.unwrap_err());
        return false;
    }
    auto json = parsed.unwrap();
    if (! material.FromJson(json)) {
        rstd_error("parse effectpassthrough.json failed for '{}'", image.name);
        return false;
    }

    wpscene::ImageEffect effect;
    effect.name    = "linked layer composite";
    effect.visible = true;
    effect.materials.push_back(std::move(material));
    image.effects.push_back(std::move(effect));
    return true;
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
    if (context.scene) {
        for (const auto& key : layer->animationVisibilityKeys()) {
            context.scene->RegisterPuppetAnimationVisibilityBinding(
                key, [layer, key](const Json& property) { layer->applyUserProperty(key, property); });
        }
    }
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

script::AnimationLayerControl
MakeAnimationLayerControl(std::shared_ptr<WPPuppetLayer> layer,
                          WPPuppetLayer::AnimationHandle handle) {
    return script::AnimationLayerControl {
        .snapshot = [layer, handle]() -> std::optional<script::AnimationLayerSnapshot> {
            auto state = layer->animationState(handle);
            if (! state) return std::nullopt;
            return script::AnimationLayerSnapshot {
                .fps         = state->fps,
                .frame_count = state->frame_count,
                .duration    = state->duration,
                .name        = state->name,
                .rate        = state->rate,
                .blend       = state->blend,
                .frame       = state->frame,
                .visible     = state->visible,
                .playing     = state->playing,
            };
        },
        .set_name = [layer, handle](std::string name) {
            layer->setAnimationName(handle, std::move(name));
        },
        .set_rate = [layer, handle](double rate) { layer->setAnimationRate(handle, rate); },
        .set_blend = [layer, handle](double blend) { layer->setAnimationBlend(handle, blend); },
        .set_visible = [layer, handle](bool visible) {
            layer->setAnimationVisible(handle, visible);
        },
        .set_frame = [layer, handle](double frame) { layer->setAnimationFrame(handle, frame); },
        .play      = [layer, handle] { layer->playAnimation(handle); },
        .pause     = [layer, handle] { layer->pauseAnimation(handle); },
        .stop      = [layer, handle] { layer->stopAnimation(handle); },
    };
}

SceneNode* RootOf(SceneNode* node) {
    if (! node) return nullptr;
    while (node->Parent()) node = node->Parent();
    return node;
}

void CollectLinkedSourceIdsFromJsonValue(
    const Json& value, Set<std::int32_t>& out,
    std::optional<std::int32_t> effect_owner = std::nullopt) {
    if (value.is_string()) {
        auto s = rstd::cppstd::to_string(*value.as_str());
        if (auto id = ParseImageLayerCompositeId(s);
            id && (! effect_owner || static_cast<std::int32_t>(*id) != *effect_owner))
            out.insert(static_cast<std::int32_t>(*id));
        if (IsSpecLinkTex(s)) out.insert(static_cast<std::int32_t>(ParseLinkTex(s)));
        return;
    }
    if (value.is_array()) {
        const auto values = value.as_array();
        for (const auto& el : **values)
            CollectLinkedSourceIdsFromJsonValue(el, out, effect_owner);
        return;
    }
    if (! value.is_object()) return;
    auto object = value.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& child             = *entry_value;
        if (key == "dependencies") {
            if (auto values = child.as_array(); values.is_some()) {
                for (const auto& dep : **values) {
                    auto id = dep.as_i64();
                    if (id.is_some() && *id >= std::numeric_limits<std::int32_t>::min() &&
                        *id <= std::numeric_limits<std::int32_t>::max())
                        out.insert(static_cast<std::int32_t>(*id));
                }
            }
        }
        CollectLinkedSourceIdsFromJsonValue(child, out, effect_owner);
    });
}

Set<std::int32_t> CollectLinkedSourceIdsFromJson(const Json& json) {
    Set<std::int32_t> out;
    if (auto objects = json.get("objects"); objects.is_some()) {
        auto array = (*objects)->as_array();
        if (array.is_some()) {
            for (const auto& object : **array) {
                if (! object.is_object()) {
                    CollectLinkedSourceIdsFromJsonValue(object, out);
                    continue;
                }
                std::optional<std::int32_t> owner;
                if (auto id_value = object.get("id"); id_value.is_some()) {
                    auto id = (*id_value)->as_i64();
                    if (id.is_some() && *id >= std::numeric_limits<std::int32_t>::min() &&
                        *id <= std::numeric_limits<std::int32_t>::max())
                        owner = static_cast<std::int32_t>(*id);
                }
                auto map = object.as_object();
                (*map)->iter().for_each([&](auto entry) {
                    auto [key, value] = entry;
                    const auto name = rstd::cppstd::as_string_view(key->as_str());
                    CollectLinkedSourceIdsFromJsonValue(
                        *value, out, name == "effects" ? owner : std::nullopt);
                });
            }
        } else {
            CollectLinkedSourceIdsFromJsonValue(**objects, out);
        }
    }
    return out;
}

void MarkHiddenLinkSource(ParseContext& context, std::int32_t id) {
    if (context.hidden_link_source_ids.count(id) != 0)
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = id });
}

SceneUserVisibilityBinding
ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding& binding) {
    SceneUserVisibilityBinding out;
    out.key           = binding.name;
    out.condition     = binding.condition.clone();
    out.has_condition = binding.has_condition;
    return out;
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

void InstallImageAlignmentBinding(script::JsRuntime& runtime, SceneNode* node,
                                  std::string_view alignment,
                                  const ParseContext::ImageAlignmentSetter& setter) {
    runtime.RegisterImageAlignmentSetter(
        node,
        std::string(alignment),
        [node, setter](std::string_view value) { setter(node, value); });
}

void RegisterImageAlignmentBinding(ParseContext& context, SceneNode* node,
                                   std::string_view alignment,
                                   ParseContext::ImageAlignmentSetter setter) {
    if (context.script_scene) {
        InstallImageAlignmentBinding(
            context.script_scene->runtime(), node, alignment, setter);
    }
    context.image_alignment_bindings.push_back({
        .node      = node,
        .alignment = std::string(alignment),
        .setter    = std::move(setter),
    });
}

void CloneImageAlignmentBinding(ParseContext& context, SceneNode* source, SceneNode* clone) {
    for (const auto& binding : context.image_alignment_bindings) {
        if (binding.node != source) continue;
        RegisterImageAlignmentBinding(context, clone, binding.alignment, binding.setter);
        return;
    }
}

std::vector<sr::SceneNode*> SpawnLayerClones(ParseContext& context, SceneNode* tmpl,
                                              unsigned count) {
    std::vector<sr::SceneNode*> out;
    if (! tmpl || count == 0) return out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        auto clone = rstd::sync::Arc<SceneNode>::make(
            tmpl->Translate(), tmpl->Scale(), tmpl->Rotation(), tmpl->Name());
        clone->SetLocalFrame(tmpl->LocalFrame());
        clone->SetSize(tmpl->Size());
        if (tmpl->HasHitCenter()) clone->SetHitCenter(tmpl->HitCenter());
        clone->SetGeometryTransform(tmpl->GeometryTransform());
        clone->SetPerspective(tmpl->Perspective());
        clone->SetReflected(tmpl->Reflected());
        clone->SetVisible(false);
        if (! tmpl->Camera().empty()) clone->SetCamera(tmpl->Camera());
        clone->AddMesh(tmpl->MeshShared());
        clone->ID() = -((i32)i + 1); // negative IDs reserved for clones
        context.shader_updater->CopyNodeData(tmpl, clone.as_ptr());
        if (auto layer = LookupPuppetLayer(context.puppet_layers, tmpl))
            RegisterPuppetLayer(context, clone.as_ptr(), std::move(layer));
        CloneImageAlignmentBinding(context, tmpl, clone.as_ptr());
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
        if (! context.script_persistence_path.empty())
            context.script_scene->runtime().SetPersistence(context.script_persistence_path);
        context.script_scene->runtime().SetCanvasSize(static_cast<float>(context.ortho_w),
                                                      static_cast<float>(context.ortho_h));
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
                Eigen::Vector3f t     = (world * *bone).translation();
                return script::BoneTranslation { t.x(), t.y(), t.z() };
            });
        context.script_scene->runtime().SetAnimationLayerResolvers(
            [layers](SceneNode* node) -> std::size_t {
                auto layer = LookupPuppetLayer(layers, node);
                return layer ? layer->animationLayerCount() : 0;
            },
            [layers](SceneNode* node,
                     const script::AnimationLayerKey& key)
                -> std::optional<script::AnimationLayerControl> {
                auto layer = LookupPuppetLayer(layers, node);
                if (! layer) return std::nullopt;
                std::optional<WPPuppetLayer::AnimationHandle> handle;
                if (const auto* index = std::get_if<std::size_t>(&key))
                    handle = layer->animationLayer(*index);
                else
                    handle = layer->animationLayer(std::get<std::string>(key));
                if (! handle) return std::nullopt;
                return MakeAnimationLayerControl(std::move(layer), *handle);
            },
            [layers](SceneNode* node,
                     std::string_view name) -> std::optional<script::AnimationLayerControl> {
                auto layer = LookupPuppetLayer(layers, node);
                if (! layer) return std::nullopt;
                auto handle = layer->playSingleAnimation(name);
                if (! handle) return std::nullopt;
                return MakeAnimationLayerControl(std::move(layer), *handle);
            });
        auto* updater = context.shader_updater;
        context.script_scene->runtime().SetCursorProjectionResolver(
            [updater](SceneNode* node) -> std::optional<script::CursorProjection> {
                if (updater == nullptr) return std::nullopt;
                auto transform = updater->NodeScreenTransform(node);
                if (! transform) return std::nullopt;
                return script::CursorProjection {
                    .model                 = transform->model,
                    .model_view_projection = transform->model_view_projection,
                };
            });
        if (context.user_properties.is_some())
            (*context.user_properties)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto key                      = rstd::cppstd::as_string_view(entry_key->as_str());
                context.script_scene->runtime().SetUserProperty(key, *entry_value);
            });
        for (const auto& binding : context.image_alignment_bindings) {
            InstallImageAlignmentBinding(context.script_scene->runtime(),
                                         binding.node,
                                         binding.alignment,
                                         binding.setter);
        }
    }
    return *context.script_scene;
}

void RegisterFieldScriptMetadata(ParseContext& context, SceneNode* node,
                                 script::FieldScript* field_script) {
    if (! field_script) return;
    if (node != nullptr) {
        if (auto order = context.script_initialization_orders.find(node->ID());
            order != context.script_initialization_orders.end()) {
            EnsureScriptScene(context).runtime().SetInitializationOrder(*field_script,
                                                                         order->second);
        }
        if (! field_script->RegisteredAssets().empty()) {
            context.create_layer_asset_requests.push_back({ field_script, node->ID() });
        }
    }
}

std::optional<float> ScriptValueAsFloat(const script::ScriptValue& value) {
    if (auto* p = std::get_if<script::ScalarValue>(&value)) return static_cast<float>(p->v);
    if (auto* p = std::get_if<script::BoolValue>(&value)) return p->v ? 1.0f : 0.0f;
    if (auto* p = std::get_if<script::Vec2Value>(&value)) return static_cast<float>(p->x);
    if (auto* p = std::get_if<script::Vec3Value>(&value)) return static_cast<float>(p->x);
    if (auto* p = std::get_if<script::Vec4Value>(&value)) return static_cast<float>(p->x);
    return std::nullopt;
}

std::optional<ShaderValue> ScriptValueAsShaderValue(const script::ScriptValue& value) {
    if (auto* scalar = std::get_if<script::ScalarValue>(&value))
        return ShaderValue(static_cast<float>(scalar->v));
    if (auto* boolean = std::get_if<script::BoolValue>(&value))
        return ShaderValue(boolean->v ? 1.0f : 0.0f);
    if (auto* vector = std::get_if<script::Vec2Value>(&value))
        return ShaderValue(std::array<float, 2> { static_cast<float>(vector->x),
                                                  static_cast<float>(vector->y) });
    if (auto* vector = std::get_if<script::Vec3Value>(&value))
        return ShaderValue(std::array<float, 3> { static_cast<float>(vector->x),
                                                  static_cast<float>(vector->y),
                                                  static_cast<float>(vector->z) });
    if (auto* vector = std::get_if<script::Vec4Value>(&value))
        return ShaderValue(std::array<float, 4> { static_cast<float>(vector->x),
                                                  static_cast<float>(vector->y),
                                                  static_cast<float>(vector->z),
                                                  static_cast<float>(vector->w) });
    if (auto* color = std::get_if<script::ColorValue>(&value))
        return ShaderValue(std::array<float, 3> { static_cast<float>(color->r),
                                                  static_cast<float>(color->g),
                                                  static_cast<float>(color->b) });
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

bool IsFractionSliderProperty(const ParseContext& context, const Json& binding) {
    if (context.user_properties.is_none() || ! binding.is_object()) return false;
    auto user = binding.get("user");
    if (user.is_none()) return false;
    auto key = (*user)->as_str();
    if (key.is_none()) return false;
    auto prop = (*context.user_properties)->get(*key);
    if (prop.is_none() || ! (*prop)->is_object()) return false;
    auto type = (*prop)->get("type");
    if (type.is_none()) return false;
    auto type_string = (*type)->as_str();
    if (type_string.is_none() || rstd::cppstd::as_string_view(*type_string) != "slider")
        return false;
    auto fraction = (*prop)->get("fraction");
    return fraction.is_some() && (*fraction)->as_bool().unwrap_or(false);
}

Json ScriptPropertiesForField(const ParseContext& context, std::string_view field,
                              const wpscene::ScriptBinding& binding) {
    Json props = binding.properties.clone();
    if (field != "scale" || binding.source.find("/10000") == std::string::npos ||
        ! props.is_object())
        return props;

    auto object = props.as_object_mut();
    (*object)->iter_mut().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto& item                    = *entry_value;
        if (IsFractionSliderProperty(context, item)) {
            auto item_object = item.as_object_mut();
            (*item_object)
                ->insert(::alloc::string::String::make(rstd::cppstd::as_str("__scriptValueScale")),
                         rstd::into<Json>(50.0));
        }
    });
    return props;
}

Json ScriptInitialValueForField(std::string_view field, const Json& value) {
    if (field != "angles") return value.clone();

    constexpr float kRadToDeg = 180.0f / rstd::f32_::consts::PI;
    if (value.is_null()) return Json::Null();
    if (value.is_number()) {
        auto number = value.as_f64();
        return number.is_some() && *number >= std::numeric_limits<float>::lowest() &&
                       *number <= std::numeric_limits<float>::max()
                   ? rstd::into<Json>(static_cast<float>(*number) * kRadToDeg)
                   : Json::Null();
    }

    if (value.is_object()) {
        auto out = value.clone();
        for (auto* axis : { "x", "y", "z" }) {
            auto member = out.get_mut(axis);
            if (member.is_none()) continue;
            auto number = (*member)->as_f64();
            if (number.is_some() && *number >= std::numeric_limits<float>::lowest() &&
                *number <= std::numeric_limits<float>::max())
                **member = rstd::into<Json>(static_cast<float>(*number) * kRadToDeg);
        }
        return out;
    }

    std::vector<float> values;
    if (sr::GetJsonValue(value, values) && ! values.empty()) {
        for (auto& axis : values) axis *= kRadToDeg;
        auto out = rstd::json::Array::make();
        for (float axis : values) out.push(rstd::into<Json>(axis));
        return Json::Array(rstd::move(out));
    }

    return value.clone();
}

std::uint32_t TextRenderScale(const text::TextGeometry& geometry, bool has_effect) {
    // Effect kernels express their radius/fit in layer pixels, so changing
    // their target resolution would change the authored look. Plain text has
    // no such dependency and benefits from a 2x intermediate target. Cap the
    // resulting target at 4096 on either axis for unusually large terminals
    // and other unbounded scripted layers.
    if (has_effect || geometry.rt_width > 2048.0f || geometry.rt_height > 2048.0f) return 1;
    return 2;
}

std::array<i32, 2> TextLayerExtent(const text::TextGeometry& geometry,
                                   std::uint32_t             render_scale = 1) {
    const float scale = static_cast<float>(std::max<std::uint32_t>(1, render_scale));
    return {
        std::max<i32>(1, static_cast<i32>(std::ceil(geometry.rt_width * scale))),
        std::max<i32>(1, static_cast<i32>(std::ceil(geometry.rt_height * scale))),
    };
}

std::uint32_t TextPointSizeToPx(float point_size) {
    constexpr float kPointsizeToPx = 4.0f;
    if (! std::isfinite(point_size) || point_size <= 0.0f) return 1;
    auto px = static_cast<std::uint32_t>(std::round(point_size * kPointsizeToPx));
    return std::clamp<std::uint32_t>(px, 1, 1024);
}

std::array<i32, 2> TextEffectFboExtent(const text::TextGeometry& geometry, std::uint32_t scale,
                                       std::uint32_t fit) {
    if (fit > 0) {
        const float max_size = std::max(geometry.effect_frame_width, geometry.effect_frame_height);
        if (max_size > 0.0f) {
            const float fit_scale = static_cast<float>(fit) / max_size;
            return {
                std::max<i32>(
                    1, static_cast<i32>(std::round(geometry.effect_frame_width * fit_scale))),
                std::max<i32>(
                    1, static_cast<i32>(std::round(geometry.effect_frame_height * fit_scale))),
            };
        }
    }
    const float fbo_scale = std::max(1.0f, static_cast<float>(scale));
    return {
        std::max<i32>(1, static_cast<i32>(std::round(geometry.effect_frame_width / fbo_scale))),
        std::max<i32>(1, static_cast<i32>(std::round(geometry.effect_frame_height / fbo_scale))),
    };
}

bool ResizeRenderTarget(Scene& scene, const std::string& name, i32 width, i32 height) {
    auto it = scene.renderTargets.find(name);
    if (it == scene.renderTargets.end()) return false;
    auto& rt = it->second;
    if (rt.width == width && rt.height == height) return false;
    rt.width  = width;
    rt.height = height;
    return true;
}

struct TextRuntimeFbo {
    std::string   name;
    std::uint32_t scale { 1 };
    std::uint32_t fit { 0 };
};

struct TextRuntimeEffectNode {
    SceneNode*           node { nullptr };
    SceneUniformNodeData data;
};

struct TextRuntimeTargets {
    Scene*                             scene { nullptr };
    SceneUniformUpdater*               shader_updater { nullptr };
    std::string                        camera_key;
    std::string                        ppong_a;
    std::string                        ppong_b;
    std::string                        effect_final;
    bool                               has_effect { false };
    std::uint32_t                      render_scale { 1 };
    i32                                layer_w { 1 };
    i32                                layer_h { 1 };
    i32                                logical_w { 1 };
    i32                                logical_h { 1 };
    std::vector<TextRuntimeFbo>        fbos;
    std::vector<TextRuntimeEffectNode> effect_nodes;

    bool Apply(const text::TextGeometry& geometry) {
        if (scene == nullptr) return false;

        bool changed = false;
        render_scale = TextRenderScale(geometry, has_effect);
        auto [next_logical_w, next_logical_h] = TextLayerExtent(geometry);
        auto [next_w, next_h]                 = TextLayerExtent(geometry, render_scale);
        changed |= ResizeRenderTarget(*scene, ppong_a, next_w, next_h);
        if (has_effect) {
            changed |= ResizeRenderTarget(*scene, ppong_b, next_w, next_h);
            changed |= ResizeRenderTarget(*scene, effect_final, next_w, next_h);
        }

        if (auto it = scene->cameras.find(camera_key); it != scene->cameras.end() && it->second) {
            auto& camera = *it->second;
            if (camera.Width() != static_cast<double>(next_logical_w) ||
                camera.Height() != static_cast<double>(next_logical_h)) {
                camera.SetWidth(next_logical_w);
                camera.SetHeight(next_logical_h);
                camera.Update();
                changed = true;
            }
        }

        for (const auto& fbo : fbos) {
            auto [w, h] = TextEffectFboExtent(geometry, fbo.scale, fbo.fit);
            changed |= ResizeRenderTarget(*scene, fbo.name, w, h);
        }

        const std::array<float, 2> effect_size {
            geometry.effect_frame_width,
            geometry.effect_frame_height,
        };
        for (auto& item : effect_nodes) {
            if (item.node == nullptr) continue;
            item.data.effect_projection_size = effect_size;
            if (shader_updater) shader_updater->SetNodeData(item.node, item.data);
        }

        layer_w = next_w;
        layer_h = next_h;
        logical_w = next_logical_w;
        logical_h = next_logical_h;
        return changed;
    }
};

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

std::vector<SceneAnimationEvent> ToSceneAnimationEvents(const Json& events) {
    std::vector<SceneAnimationEvent> out;
    auto array = events.as_array();
    if (array.is_none()) return out;
    out.reserve((*array)->len());
    for (const auto& value : **array) {
        SceneAnimationEvent event;
        if (! GetJsonValue(value, "name", event.name, false) || event.name.empty()) continue;
        GetJsonValue(value, "frame", event.frame, false);
        out.push_back(std::move(event));
    }
    return out;
}

void AssignCurve(SceneAnimationCurve& dst, const wpscene::FieldBindings& bindings,
                 std::string_view field) {
    auto it = bindings.animations.find(std::string(field));
    if (it != bindings.animations.end()) dst = ToSceneAnimationCurve(it->second);
}

std::optional<std::string> AnimationLinkKey(const Json& link) {
    std::string key;
    if (! GetJsonValue(link, "key", key, false) || key.empty()) return std::nullopt;
    return key;
}

void AssignNodeFieldAnimations(SceneNode& node, const wpscene::FieldBindings& bindings) {
    std::unordered_map<std::string, std::string> parents;
    for (const auto& [field, animation] : bindings.animations) {
        if (auto children = animation.options.children.as_array(); children.is_some()) {
            for (const auto& child : **children) {
                if (auto key = AnimationLinkKey(child)) parents[*key] = field;
            }
        }
    }
    for (const auto& [field, animation] : bindings.animations) {
        if (auto parent = AnimationLinkKey(animation.options.parent)) parents[field] = *parent;
    }

    auto root_field = [&](std::string field) {
        std::unordered_set<std::string> visited;
        while (visited.insert(field).second) {
            auto it = parents.find(field);
            if (it == parents.end() || ! bindings.animations.contains(it->second)) break;
            field = it->second;
        }
        return field;
    };

    std::unordered_map<std::string, std::shared_ptr<SceneAnimationPlayback>> playbacks;
    auto make_curve = [&](std::string_view field) -> std::optional<SceneAnimationCurve> {
        auto it = bindings.animations.find(std::string(field));
        if (it == bindings.animations.end()) return std::nullopt;
        auto curve = ToSceneAnimationCurve(it->second);
        auto root  = root_field(std::string(field));
        auto root_it = bindings.animations.find(root);
        const auto& source = root_it != bindings.animations.end() ? root_it->second : it->second;
        if (root != field) {
            curve.fps      = source.options.fps;
            curve.length   = source.options.length;
            curve.mode     = source.options.mode;
            curve.wraploop = source.options.wraploop;
        }
        auto events = ToSceneAnimationEvents(source.options.events);
        if (! source.options.name.empty() || source.options.startpaused || ! events.empty()) {
            auto& playback = playbacks[root];
            if (! playback) {
                playback = std::make_shared<SceneAnimationPlayback>(source.options.name,
                                                                    source.options.fps,
                                                                    source.options.length,
                                                                    source.options.mode,
                                                                    source.options.wraploop,
                                                                    source.options.startpaused,
                                                                    std::move(events));
            }
            curve.playback = playback;
        }
        return curve;
    };

    if (auto curve = make_curve("origin")) node.SetOriginAnimation(std::move(*curve));
    if (auto curve = make_curve("scale")) node.SetScaleAnimation(std::move(*curve));
    if (auto curve = make_curve("angles")) node.SetRotationAnimation(std::move(*curve));
    if (auto curve = make_curve("alpha")) node.SetAlphaAnimation(std::move(*curve));
}

std::optional<SceneCameraLookAtKey> ParseLookAtKey(const Json& json) {
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

std::optional<SceneCameraLookAtTrack> ParseLookAtTrack(const Json& json) {
    auto transforms = json.get("transforms");
    if (transforms.is_none()) return std::nullopt;
    auto transform_array = (*transforms)->as_array();
    if (transform_array.is_none()) return std::nullopt;

    SceneCameraLookAtTrack track;
    sr::GetJsonValue(json, "duration", track.duration, false);
    for (const auto& raw_key : **transform_array) {
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
        auto parsed = ParseJson(file->ReadAllStr());
        if (parsed.is_err()) {
            rstd_warn("Can't parse camera path json {}: {}", rel, parsed.unwrap_err());
            continue;
        }
        auto json   = parsed.unwrap();
        auto tracks = json.get("paths");
        if (tracks.is_none()) continue;
        auto track_array = (*tracks)->as_array();
        if (track_array.is_none()) continue;
        for (const auto& raw_track : **track_array) {
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
// closure). A `visible` binding drives layer visibility from update()'s
// return value; scripts that only mutate the scene as a side effect return
// undefined, which coerces to monostate and leaves visibility untouched.
void WireFieldScripts(ParseContext& context, const rstd::sync::Arc<SceneNode>& node_sp,
                      const wpscene::FieldBindings&                   fb,
                      std::function<void(const script::ScriptValue&)> origin_apply = {},
                      std::function<void(const script::ScriptValue&)> scale_apply  = {},
                      std::function<void(const script::ScriptValue&)> alpha_apply  = {}) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::NodeTransformTarget tgt = script::NodeTransformTarget::Translate;
        script::FieldKind           kind;
        bool                        is_visible = false;
        bool                        is_alpha   = false;
        bool                        is_volume  = false;
        bool                        is_color   = false;
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
            kind       = script::FieldKind::Bool;
            is_visible = true;
        } else if (field == "alpha") {
            kind     = script::FieldKind::Scalar;
            is_alpha = true;
        } else if (field == "volume") {
            kind      = script::FieldKind::Scalar;
            is_volume = true;
        } else if (field == "color") {
            kind     = script::FieldKind::Vec3;
            is_color = true;
        } else {
            // text/color/rate/intensity/... are wired elsewhere or not yet supported.
            continue;
        }
        std::string                  sha = utils::genSha1(std::span<const char>(sb.source));
        std::vector<sr::SceneNode*> clones;
        if (unsigned n = DetectAudioFanoutCount(sb.source); n > 1) {
            clones = SpawnLayerClones(context, node, n - 1);
        }
        auto  props         = ScriptPropertiesForField(context, field, sb);
        auto  initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs =
            rt.MakeFieldScript(sb.source, sha, kind, props, initial_value, node, std::move(clones));
        if (! fs) continue;
        RegisterFieldScriptMetadata(context, node, fs);
        if (is_visible && node != nullptr && node->ID() >= 0 && fs->HasUpdate()) {
            context.scene->EnableRuntimeLayerVisibility(
                WallpaperLayerId { .value = node->ID() });
        }
        if (is_visible)
            ss.AddActuator(
                { fs, script::MakeNodeVisibleApply(node_sp.clone(), context.scene.get()) });
        else if (is_alpha)
            ss.AddActuator(
                { fs, alpha_apply ? alpha_apply : script::MakeNodeAlphaApply(node_sp.clone()) });
        else if (is_volume)
            ss.AddActuator({ fs, script::MakeNodeVolumeApply(node_sp.clone()) });
        else if (is_color)
            ss.AddActuator({ fs, script::MakeNodeColorApply(node_sp.clone()) });
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

        std::string sha           = utils::genSha1(std::span<const char>(sb.source));
        auto        initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, initial_value, node);
        if (! fs) continue;
        RegisterFieldScriptMetadata(context, node, fs);

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
// Highest addressable material texture slot. The renderer only ever binds
// g_Texture0..g_Texture12 (see WE_GLTEX_NAMES in sr.spec_texs), so a slot
// index outside that range can never reach the GPU. Slot indices coming out
// of scene.json (`bind[].index`) or out of a `g_TextureN` uniform name are
// raw int32s, and using them unchecked to size a vector is a heap-corruption
// primitive: `(usize)(-1) + 1` wraps to 0 while `operator[]` still writes.
constexpr i32 kMaxMaterialTextureSlots = static_cast<i32>(WE_GLTEX_NAMES.size());

// mapRate < 1.0
void GenCardMesh(SceneMesh& mesh, const std::array<float, 2> size,
                 const std::array<float, 2> mapRate         = { 1.0f, 1.0f },
                 const Vector3f&            position_offset = Vector3f::Zero()) {
    float left   = -(size[0] / 2.0f) + position_offset.x();
    float right  = size[0] / 2.0f + position_offset.x();
    float bottom = -(size[1] / 2.0f) + position_offset.y();
    float top    = size[1] / 2.0f + position_offset.y();
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

using DirectDrawQuad = std::array<std::array<float, 2>, 4>;

std::optional<DirectDrawQuad> ReadDirectDrawQuad(const wpscene::Material& material) {
    constexpr std::array<std::string_view, 4> names { "point0", "point1", "point2", "point3" };
    DirectDrawQuad points {};
    for (std::size_t index = 0; index < points.size(); ++index) {
        auto value = material.constantshadervalues.find(std::string(names[index]));
        if (value == material.constantshadervalues.end() || value->second.size() != 2 ||
            ! std::isfinite(value->second[0]) || ! std::isfinite(value->second[1])) {
            return std::nullopt;
        }
        points[index] = { value->second[0], value->second[1] };
    }
    return points;
}

void GenDirectDrawQuadMesh(SceneMesh& mesh, float edge, const DirectDrawQuad& points) {
    const auto position = [&](std::size_t index) {
        return std::array<float, 3> { (points[index][0] - 0.5f) * edge,
                                      (0.5f - points[index][1]) * edge,
                                      0.0f };
    };
    const auto p0 = position(0);
    const auto p1 = position(1);
    const auto p2 = position(2);
    const auto p3 = position(3);
    const std::array<float, 12> positions {
        p0[0], p0[1], p0[2], p1[0], p1[1], p1[2],
        p2[0], p2[1], p2[2], p3[0], p3[1], p3[2],
    };
    const std::array<float, 8> tex_coords {
        points[0][0], points[0][1], points[1][0], points[1][1],
        points[2][0], points[2][1], points[3][0], points[3][1],
    };
    const std::array<std::uint32_t, 6> indices { 0u, 2u, 1u, 0u, 3u, 2u };

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), 4);
    vertex.SetVertex(WE_IN_POSITION, positions);
    vertex.SetVertex(WE_IN_TEXCOORD, tex_coords);
    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(std::span<const std::uint32_t>(indices)));
}

struct ImageParseGeometry {
    bool       requires_source_draw { true };
    SceneMesh* final_mesh { nullptr };
};

bool PlatformSupportsGeometryShaders() {
    // Metal has no geometry-shader stage; MoltenVK can't lower them.
    return false;
}

// Particle topology is a fixed sequence of independent quads. Build it at
// compile time instead of extending it while particles run: the mesh's vertex
// data is dynamic, but these indices never are.
void InitializeParticleQuadIndices(SceneIndexArray& indices, uint32_t quad_count) {
    std::array<uint32_t, 6> quad {};
    for (uint32_t i = 0; i < quad_count; ++i) {
        const uint32_t base = i * 4;
        quad                 = { base, base + 1, base + 3, base + 1, base + 2, base + 3 };
        indices.Assign(static_cast<usize>(i) * quad.size(), quad);
    }
    indices.SetRenderDataCount(0);
    indices.SetStaticTopology();
}

// The WE genericparticle shader normally receives four fully-expanded vertex
// records per particle. Keep assets immutable and synthesize an alternate
// input declaration for its built-in source: a static corner stream plus a
// compact per-instance stream reconstruct exactly the legacy attributes.
// This transform deliberately targets only the stock shader and fails closed
// if its known declaration block changes.
std::optional<std::string> MakeInstancedGenericParticleVertexSource(std::string source) {
    // WE ships this source with CRLF line endings. Normalize the transient
    // compiler copy so the exact declaration replacement below is stable on
    // every package; the asset on disk remains untouched.
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    static constexpr std::string_view kLegacyDecls = R"(attribute vec3 a_Position;
attribute vec4 a_TexCoordVec4;
attribute vec4 a_Color;
varying vec4 v_Color;

#if THICKFORMAT
attribute vec4 a_TexCoordVec4C1;
#endif)";
    static constexpr std::string_view kInstancedDecls = R"(#if PARTICLEINSTANCED
attribute vec2 a_ParticleCorner;
attribute vec4 a_ParticlePositionSize;
attribute vec3 a_ParticleRotation;
attribute vec4 a_ParticleColor;
#if THICKFORMAT
attribute vec4 a_ParticleVelocityLifetime;
#endif

#define a_Position (a_ParticlePositionSize.xyz)
#define a_TexCoordVec4 (vec4(a_ParticleCorner, a_ParticleRotation.z, a_ParticlePositionSize.w))
#define a_Color (a_ParticleColor)
#if THICKFORMAT
#define a_TexCoordVec4C1 (a_ParticleVelocityLifetime)
#endif
#else
attribute vec3 a_Position;
attribute vec4 a_TexCoordVec4;
attribute vec4 a_Color;

#if THICKFORMAT
attribute vec4 a_TexCoordVec4C1;
#endif
#endif
varying vec4 v_Color;)";
    static constexpr std::string_view kLegacyCorner = "attribute vec2 a_TexCoordC2;";
    static constexpr std::string_view kInstancedCorner = R"(#if PARTICLEINSTANCED
#define a_TexCoordC2 (a_ParticleRotation.xy)
#else
attribute vec2 a_TexCoordC2;
#endif)";

    auto decl_pos = source.find(kLegacyDecls);
    if (decl_pos == std::string::npos) return std::nullopt;
    source.replace(decl_pos, kLegacyDecls.size(), kInstancedDecls);
    auto corner_pos = source.find(kLegacyCorner);
    if (corner_pos == std::string::npos) return std::nullopt;
    source.replace(corner_pos, kLegacyCorner.size(), kInstancedCorner);
    return source;
}

std::optional<std::string> MakeCpuRopeParticleVertexSource(std::string source) {
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());

    static constexpr std::string_view kVaryingAndMain = R"(varying vec2 v_TexCoord;

void main() {)";
    static constexpr std::string_view kVaryingAndHelper = R"(varying vec2 v_TexCoord;

vec3 cubicBezier(vec3 A, vec3 B, vec3 C, vec3 D, float t)
{
    float oneMinusT = 1.0 - t;
    float b0 = oneMinusT * oneMinusT * oneMinusT;
    float b1 = 3.0 * t * oneMinusT * oneMinusT;
    float b2 = 3.0 * t * t * oneMinusT;
    float b3 = t * t * t;
    return b0 * A + b1 * B + b2 * C + b3 * D;
}

void main() {)";
    static constexpr std::string_view kLinearPosition = R"(vec3 position = mix(startPosition, endPosition, uvs.y);
	vec3 right = mix(trailRightStart, trailRightEnd, uvs.y);
	position += right * uvs.x * 2.0 - 1.0;)";
    static constexpr std::string_view kBezierPosition = R"(vec3 curveControlStart = startPosition + (trailDelta + CPStart) * 0.15;
	vec3 curveControlEnd = endPosition - (trailDelta - CPEnd) * 0.15;
	vec3 position = cubicBezier(startPosition, curveControlStart, curveControlEnd, endPosition, uvs.y);
	vec3 right = mix(trailRightStart, trailRightEnd, uvs.y);
	position += right * (uvs.x * 2.0 - 1.0);)";

    auto main_pos = source.find(kVaryingAndMain);
    if (main_pos == std::string::npos) return std::nullopt;
    source.replace(main_pos, kVaryingAndMain.size(), kVaryingAndHelper);
    auto linear_pos = source.find(kLinearPosition);
    if (linear_pos == std::string::npos) return std::nullopt;
    source.replace(linear_pos, kLinearPosition.size(), kBezierPosition);
    return source;
}

void SetParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                     bool thick_format, bool geometry_shader, bool instanced) {
    (void)particle;
    if (instanced && ! geometry_shader) {
        // One immutable unit quad, followed by one vertex-rate-instance record
        // per live particle. The generated genericparticle shader reconstructs
        // WE's historical per-corner input layout from these two streams.
        std::vector<VertexAttrSpec> corner_specs {
            { "a_ParticleCorner", VertexType::FLOAT2 },
        };
        SceneVertexArray corners(MakeAttrSet(corner_specs), 4);
        constexpr std::array<float, 8> kCorners {
            0.0f, 1.0f,
            1.0f, 1.0f,
            1.0f, 0.0f,
            0.0f, 0.0f,
        };
        corners.SetVertex("a_ParticleCorner", kCorners);
        corners.SetStaticData();

        std::vector<VertexAttrSpec> instance_specs {
            { "a_ParticlePositionSize", VertexType::FLOAT4 },
            { "a_ParticleRotation", VertexType::FLOAT3 },
            { "a_ParticleColor", VertexType::FLOAT4 },
        };
        if (thick_format)
            instance_specs.push_back({ "a_ParticleVelocityLifetime", VertexType::FLOAT4 });
        SceneVertexArray instances(MakeAttrSet(instance_specs), count);
        instances.SetInstanceRate();

        mesh.SetParticleInstanced();
        mesh.SetParticleInstanceCount(0);
        mesh.AddVertexArray(std::move(corners));
        mesh.AddVertexArray(std::move(instances));
        mesh.AddIndexArray(SceneIndexArray(6));
        InitializeParticleQuadIndices(mesh.GetIndexArray(0), 1);
        mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
        mesh.GetVertexArray(1).SetOption(WE_CB_THICK_FORMAT, thick_format);
        return;
    }

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
        InitializeParticleQuadIndices(mesh.GetIndexArray(0), count);
    }
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

bool IsLayerCompositeShader(std::string_view shader) {
    return shader == "genericimage" || shader == "genericimage2" || shader == "genericimage3" ||
           shader == "genericimage4" || shader == "passthrough";
}

// Render targets must be at least 1 pixel on Vulkan. Zero-height audio-buffer
// layers are clamped to the smallest valid target so the layer stays renderable.
i32 NonZeroRenderTargetDimension(float value) {
    if (! std::isfinite(value) || value < 1.0f) return 1;
    return static_cast<i32>(value);
}

std::array<i32, 2> NonZeroRenderTargetExtent(float width, float height) {
    return { NonZeroRenderTargetDimension(width), NonZeroRenderTargetDimension(height) };
}

void ApplyEffectRenderTargetFormat(SceneRenderTarget& target, std::string_view format,
                                   bool scene_hdr) {
    if (format.empty() || format == "rgba_backbuffer") {
        target.hdr_format           = scene_hdr;
        target.inherit_scene_format = true;
        return;
    }
    if (format == "rgba8888" || format == "r8") {
        target.hdr_format           = false;
        target.inherit_scene_format = false;
        return;
    }
    if (format == "r16f" || format == "rg1616f") {
        target.hdr_format           = true;
        target.inherit_scene_format = false;
        return;
    }
    rstd_warn("unknown effect render target format '{}', using scene format", format);
    target.hdr_format           = scene_hdr;
    target.inherit_scene_format = true;
}

std::array<float, 2> ImageEffectTargetSize(const ParseContext&         context,
                                           const wpscene::ImageObject& obj) {
    if (obj.fullscreen && context.scene && context.scene->activeCamera) {
        return { static_cast<float>(context.scene->activeCamera->Width()),
                 static_cast<float>(context.scene->activeCamera->Height()) };
    }
    return { obj.size[0], obj.size[1] };
}

void SetRopeParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                         bool thick_format, bool trail_renderer, bool geometry_shader) {
    (void)particle;
    std::vector<VertexAttrSpec> specs {
        VAttr::PositionVec4,
        VAttr::TexCoordVec4,
        VAttr::TexCoordVec4C1,
    };
    if (thick_format) {
        specs.push_back(VAttr::TexCoordVec4C2);
        specs.push_back(VAttr::TexCoordVec4C3);
        // Without a geometry shader the rope quads are expanded on the CPU
        // (see WPParticleRawGener::GenGLData), which needs a per-corner UV
        // attribute the geometry-shader path would otherwise synthesize.
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
        InitializeParticleQuadIndices(mesh.GetIndexArray(0), count);
    }
    mesh.GetVertexArray(0).SetOption(
        trail_renderer ? WE_PRENDER_ROPE_TRAIL : WE_PRENDER_ROPE, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

struct ParticleRenderDesc {
    bool rope { false };
    bool rope_trail { false };
    bool trail { false };
    bool geometry_shader { false };
};

ParticleRenderDesc DescribeParticleRender(const wpscene::ParticleRender& render) {
    ParticleRenderDesc desc;
    desc.rope            = render.name == "rope";
    desc.rope_trail      = render.name == "ropetrail";
    desc.trail           = send_with(render.name, "trail");
    // Metal has no geometry-shader stage and MoltenVK can't lower it, so
    // rope/sprite/trail particles fall back to CPU quad expansion on macOS.
    desc.geometry_shader =
        PlatformSupportsGeometryShaders() &&
        (desc.rope || desc.rope_trail || render.name == "sprite" || desc.trail);
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

void LoadControlPoint(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                      ParticleInstanceModifiers modifiers) {
    std::span<ParticleControlpoint> pcs = pSys.Controlpoints();
    usize                           s   = std::min(pcs.size(), wp.controlpoints.size());
    for (usize i = 0; i < s; i++) {
        pcs[i].base_offset =
            Eigen::Vector3d { array_cast<double>(wp.controlpoints[i].offset).data() };
        pcs[i].offset = pcs[i].base_offset;
        pcs[i].link_mouse =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        if (pcs[i].link_mouse) pSys.SetUsesMouseControlpoint();
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
    pSys.SetControlpointOverrideOp(
        [modifiers](std::span<ParticleControlpoint> points) {
            if (! modifiers.ControlpointsEnabled()) return;
            const usize count = std::min<usize>(points.size(), 8u);
            for (usize index = 0; index < count; ++index) {
                if (modifiers.Controlpoint(index).has_value())
                    points[index].runtime_position = Eigen::Vector3f {
                        modifiers.Controlpoint(index)->data()
                    }.cast<double>();
                points[index].runtime_angles =
                    Eigen::Vector3f { modifiers.ControlpointAngle(index).data() }.cast<double>();
            }
        });
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     ParticleInstanceModifiers modifiers) {
    enum class SequenceLimit { Repeat, Mirror, Clamp };
    auto parse_limit = [](const Json& json) {
        std::string value { "repeat" };
        GetJsonValue(json, "limitbehavior", value, false);
        if (value == "mirror") return SequenceLimit::Mirror;
        if (value == "clamp") return SequenceLimit::Clamp;
        return SequenceLimit::Repeat;
    };
    auto sequence_index = [](u64 sequence, u64 count, SequenceLimit limit) {
        if (count <= 1) return u64 { 0 };
        if (limit == SequenceLimit::Clamp) return std::min(sequence, count - 1);
        if (limit == SequenceLimit::Mirror) {
            const u64 period = (count - 1) * 2;
            const u64 value  = sequence % period;
            return value < count ? value : period - value;
        }
        return sequence % count;
    };
    u32 implicit_sequence_count = 2;
    for (const auto& emitter : wp.emitters) {
        if (emitter.max_emit_per_period > 0) {
            implicit_sequence_count = emitter.max_emit_per_period;
            break;
        }
    }

    for (const auto& ini : wp.initializers) {
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(ini));
        std::string name;
        GetJsonValue(ini, "name", name, false);
        if (name == "mapsequencebetweencontrolpoints") {
            i32 start_index = 0;
            i32 end_index   = 1;
            u32 count       = std::max(implicit_sequence_count, 2u);
            GetJsonValue(ini, "controlpointstart", start_index, false);
            GetJsonValue(ini, "controlpointend", end_index, false);
            GetJsonValue(ini, "count", count, false);
            count = std::max(count, 2u);
            const auto limit = parse_limit(ini);
            pSys.SetRopeSequenceCount(count);
            pSys.AddContextInitializer(
                [start_index, end_index, count, limit, sequence_index](
                    Particle& particle, std::span<const ParticleControlpoint> controlpoints) {
                    const usize start = static_cast<usize>((start_index % 8 + 8) % 8);
                    const usize end   = static_cast<usize>((end_index % 8 + 8) % 8);
                    const Eigen::Vector3d origin = controlpoints[start].offset;
                    const Eigen::Vector3d path   = controlpoints[end].offset - origin;
                    const u64 index = sequence_index(particle.spawn_sequence, count, limit);
                    const double amount = static_cast<double>(index) /
                                          static_cast<double>(count - 1);
                    Eigen::Vector3d relative = particle.position.cast<double>() - origin;
                    if (path.squaredNorm() > 1e-12) {
                        const auto direction = path.normalized();
                        relative -= direction * relative.dot(direction);
                    }
                    particle.position = (origin + amount * path + relative).cast<float>();
                });
        } else if (name == "mapsequencearoundcontrolpoint") {
            i32 controlpoint = 0;
            float count = 1.0f;
            std::array<float, 2> bounds { 0.0f, 1.0f };
            std::array<float, 3> axis { 0.0f, 0.0f, 1.0f };
            std::array<float, 3> speed_min { 0.0f, 0.0f, 0.0f };
            std::array<float, 3> speed_max { 0.0f, 0.0f, 0.0f };
            GetJsonValue(ini, "controlpoint", controlpoint, false);
            GetJsonValue(ini, "count", count, false);
            GetJsonValue(ini, "bounds", bounds, false);
            GetJsonValue(ini, "axis", axis, false);
            GetJsonValue(ini, "speedmin", speed_min, false);
            GetJsonValue(ini, "speedmax", speed_max, false);
            pSys.AddContextInitializer(
                [controlpoint, count, bounds, axis, speed_min, speed_max](
                    Particle& particle, std::span<const ParticleControlpoint> controlpoints) {
                    const usize cp = static_cast<usize>((controlpoint % 8 + 8) % 8);
                    const Eigen::Vector3d center = controlpoints[cp].offset;
                    Eigen::Vector3d normal = Eigen::Vector3f { axis.data() }.cast<double>();
                    if (normal.squaredNorm() <= 1e-12) normal = Eigen::Vector3d::UnitZ();
                    normal.normalize();
                    Eigen::Vector3d basis = std::abs(normal.z()) > 0.5
                                                ? Eigen::Vector3d { 0.0, 1.0, 0.0 }
                                                : Eigen::Vector3d { 1.0, 0.0, 0.0 };
                    basis = (basis - normal * basis.dot(normal)).normalized();
                    const Eigen::Vector3d tangent = basis.cross(normal).normalized();
                    const double angle = 6.28318530717958647692 *
                                         static_cast<double>(particle.spawn_sequence) /
                                         std::max(1e-6, static_cast<double>(count)) *
                                             static_cast<double>(bounds[1] - bounds[0]) +
                                         6.28318530717958647692 * bounds[0];
                    const Eigen::Vector3d relative = particle.position.cast<double>() - center;
                    const Eigen::Vector3d parallel = normal * relative.dot(normal);
                    const double radius = (relative - parallel).norm();
                    particle.position =
                        (center + parallel +
                         radius * (std::cos(angle) * basis + std::sin(angle) * tangent))
                            .cast<float>();
                    Eigen::Vector3d velocity;
                    for (usize component = 0; component < 3; ++component)
                        velocity[component] = Random::get(
                            static_cast<double>(speed_min[component]),
                            static_cast<double>(speed_max[component]));
                    if (velocity.squaredNorm() > 1e-12)
                        particle.velocity +=
                            (Eigen::AngleAxisd(-angle, normal) * velocity).cast<float>();
                });
        }
    }
    if (modifiers.Enabled()) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(modifiers));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  ParticleInstanceModifiers modifiers) {
    for (const auto& op : wp.operators) {
        pSys.AddOperator(WPParticleParser::genParticleOperatorOp(op, modifiers));
    }
}
void LoadEmitter(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                 const ParticleInstanceModifiers& modifiers) {
    bool sort = false;
    for (const auto& em : wp.emitters) {
        auto newEm = em;
        newEm.rate *= modifiers.Count();
        if (newEm.audioprocessingmode != 0) pSys.SetUsesAudioResponse();
        pSys.AddEmitter(WPParticleParser::genParticleEmittOp(newEm, sort));
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
    } else if (str == "alphatocoverage") {
        bm = BlendMode::AlphaToCoverage;
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

std::optional<BlendMode> ApplyImageColorBlend(wpscene::Material&          material,
                                              const wpscene::ImageObject& image) {
    if (image.colorBlendMode == 0) return std::nullopt;

    // Wallpaper Engine's "Linear Dodge" mode is attachment-level additive
    // blending, not a BLENDMODE shader variant. Keeping it as combo 31 makes
    // the final effect pass overwrite the framebuffer instead of adding to it.
    if (image.colorBlendMode == 31) {
        material.combos.erase(std::string(WE_CB_BLENDMODE));
        material.blending = "additive";
        return BlendMode::Additive;
    }
    material.combos[std::string(WE_CB_BLENDMODE)] = image.colorBlendMode;
    return std::nullopt;
}

ShaderValueMap NeutralColorUniforms(ShaderValueMap values) {
    values[std::string(G_COLOR4)]     = std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
    values[std::string(G_COLOR)]      = std::array<float, 3> { 1.0f, 1.0f, 1.0f };
    values[std::string(G_ALPHA)]      = 1.0f;
    values[std::string(G_USERALPHA)]  = 1.0f;
    values[std::string(G_BRIGHTNESS)] = 1.0f;
    return values;
}

i32 CountVisibleImageEffects(std::span<const wpscene::ImageEffect> effects) {
    i32 count = 0;
    for (const auto& effect : effects) {
        if (effect.visible || ! effect.visible_user.empty() ||
            effect.field_bindings.scripts.contains("visible"))
            ++count;
    }
    return count;
}

i32 CountRuntimeImageEffects(std::span<const wpscene::ImageEffect> effects,
                             bool preserve_hidden) {
    if (! preserve_hidden) return CountVisibleImageEffects(effects);
    return static_cast<i32>(effects.size());
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
                      Scene& scene) {
    if (IsSpecTex(name)) {
        if (name == WE_FULL_FRAME_BUFFER) {
            name = SpecTex_Default;
            if (wpmat.shader == "genericimage2" &&
                ! exists(sinfo.combos, std::string(WE_CB_BLENDMODE)))
                name = "";
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
            name = std::string(WE_REFLECTION_PREFIX);
            scene.EnsurePlanarReflectionRenderTarget();
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

SceneShaderTextureCompileInfo ToSceneShaderTextureCompileInfo(const WPShaderTexInfo& info) {
    return SceneShaderTextureCompileInfo {
        .enabled    = info.enabled,
        .components = info.composEnabled,
    };
}

sr::Map<std::string, std::string> MaterialCombosToShaderCombos(const wpscene::Material& material) {
    sr::Map<std::string, std::string> combos;
    for (const auto& [key, value] : material.combos) combos[key] = std::to_string(value);
    return combos;
}

bool IsLegacyAtmosphereMaterial(const wpscene::Material& material) {
    return material.shader == "workshop/2839476907/effects/atmosphere";
}

void ApplyLegacyAtmosphereLightCombo(const wpscene::Material& material, WPShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    if (! info.combos.contains("LIGHT_INDEX") || material.combos.contains("LIGHT_INDEX")) return;
    if (! material.combos.contains("LIGHT1")) return;

    info.combos["LIGHT_INDEX"] = "4";
}

void ApplySceneFogCombos(const Scene& scene, WPShaderInfo& info) {
    auto fog = info.combos.find("FOG");
    if (fog == info.combos.end() || fog->second == "0") return;

    if (scene.fog_distance_enabled) info.combos["FOG_DIST"] = "1";
    if (scene.fog_height_enabled) info.combos["FOG_HEIGHT"] = "1";
    if (scene.fog_distance_enabled || scene.fog_height_enabled) info.combos["FOG_COMPUTED"] = "1";
}

void ApplySceneHdrCombo(const Scene& scene, const wpscene::Material& material,
                        WPShaderInfo& info) {
    if (! scene.hdr_enabled) return;
    if (material.combos.contains("HDR")) return;
    info.combos["HDR"] = "1";
}

void ApplyLegacyAtmosphereUniformAliases(const wpscene::Material& material, WPShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    info.baseConstSvs[std::string(G_VIEWFORWARD)] = std::array { 0.0f, 0.0f, 1.0f };

    auto prefer_legacy = [&](std::string_view legacy, std::string_view current) {
        if (! material.constantshadervalues.contains(std::string(legacy))) return;
        auto current_it = info.alias.find(std::string(current));
        if (current_it == info.alias.end()) return;
        info.alias[std::string(legacy)] = current_it->second;
        info.alias.erase(current_it);
    };

    prefer_legacy("Planet position", "Position");
    prefer_legacy("Planet radius", "Planet size");
    prefer_legacy("Atmosphere radius", "Atmosphere size");
    prefer_legacy("Thickness", "Density falloff");
    prefer_legacy("Color", "Light color");
    prefer_legacy("Intensity", "Brightness");
}

void ReplaceAllInPlace(std::string& body, std::string_view needle, std::string_view repl) {
    for (usize pos = 0; (pos = body.find(needle, pos)) != std::string::npos; pos += repl.size()) {
        body.replace(pos, needle.size(), repl);
    }
}

void ApplyLegacyAtmosphereShaderCompat(const wpscene::Material&   material,
                                       std::vector<WPShaderUnit>& units) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    for (auto& unit : units) {
        if (unit.stage != ShaderType::FRAGMENT) continue;
        ReplaceAllInPlace(unit.src,
                          "float pointDensity, opticalDepth;",
                          "float pointDensity = 0.0, opticalDepth = 0.0;");
        ReplaceAllInPlace(unit.src,
                          "float localDensity, cameraOpticalDepth, sunRayLength, "
                          "sunOpticalDepth, lightInstensity = 1.0;",
                          "float localDensity = 0.0, cameraOpticalDepth = 0.0, "
                          "sunRayLength = 0.0, sunOpticalDepth = 0.0, lightInstensity = 1.0;");
    }
}

bool IsLegacyAtmosphereShadowValue(const wpscene::Material& material, std::string_view name) {
    if (! IsLegacyAtmosphereMaterial(material)) return false;

    static constexpr std::string_view shadow_values[] = {
        "Position",    "Planet size", "Atmosphere size", "Density falloff",
        "Light color", "Brightness",  "Radius",
    };

    for (std::string_view shadow_value : shadow_values) {
        if (name == shadow_value) return true;
    }
    return false;
}

std::vector<SceneShaderDefaultTexture> ToSceneShaderDefaultTextures(const WPShaderInfo& info) {
    std::vector<SceneShaderDefaultTexture> out;
    out.reserve(info.defTexs.size());
    for (const auto& [slot, texture] : info.defTexs) {
        out.push_back(SceneShaderDefaultTexture { .slot = slot, .texture = texture });
    }
    return out;
}

SceneShaderVariantDesc MakeSceneShaderVariantDesc(
    std::string_view scene_id, const wpscene::Material& material, const WPShaderInfo& info,
    std::span<const WPShaderUnit> units, std::span<const std::string> source_keys,
    std::span<const std::string> stage_sources, std::span<const WPShaderTexInfo> texinfos,
    bool geometry_shader_enabled) {
    SceneShaderVariantDesc desc;
    desc.scene_id                = std::string(scene_id);
    desc.shader_name             = material.shader;
    desc.input_combos            = MaterialCombosToShaderCombos(material);
    desc.resolved_combos         = info.combos;
    desc.uniform_aliases         = info.alias;
    desc.default_uniforms        = info.svs;
    desc.default_textures        = ToSceneShaderDefaultTextures(info);
    desc.geometry_shader_enabled = geometry_shader_enabled;

    desc.texture_infos.reserve(texinfos.size());
    for (const auto& texinfo : texinfos) {
        desc.texture_infos.push_back(ToSceneShaderTextureCompileInfo(texinfo));
    }

    desc.stages.reserve(units.size());
    for (usize i = 0; i < units.size(); ++i) {
        desc.stages.push_back(SceneShaderVariantStage {
            .stage      = units[i].stage,
            .source_key = i < source_keys.size() ? source_keys[i] : std::string {},
            .source     = i < stage_sources.size() ? stage_sources[i] : units[i].src,
        });
    }
    return desc;
}

bool LoadMaterial(fs::VFS& vfs, const wpscene::Material& wpmat, Scene* pScene, SceneNode* pNode,
                  SceneMaterial* pMaterial, SceneUniformNodeData* pSvData,
                  WPShaderInfo* pWPShaderInfo = nullptr, bool enable_geometry_shader = false,
                  bool* out_geometry_shader = nullptr) {
    (void)pNode;
    if (out_geometry_shader) *out_geometry_shader = false;

    auto& svData    = *pSvData;
    auto& material  = *pMaterial;
    auto  blendMode = ParseBlendMode(wpmat.blending);

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
    std::vector<std::string>  sd_source_keys;
    std::vector<std::string>  sd_original_sources;
    auto                      add_shader_unit = [&](ShaderType stage, std::string source_key) {
        auto source = fs::GetFileContent(vfs, source_key);
        sd_source_keys.push_back(std::move(source_key));
        sd_original_sources.push_back(source);
        sd_units.push_back({
            .stage           = stage,
            .src             = std::move(source),
            .preprocess_info = {},
        });
    };
    add_shader_unit(ShaderType::VERTEX, shaderPath + ".vert");
    bool geometry_shader_enabled = false;
    if (enable_geometry_shader && PlatformSupportsGeometryShaders()) {
        std::string geom_path = shaderPath + ".geom";
        if (vfs.Contains(geom_path)) {
            add_shader_unit(ShaderType::GEOMETRY, std::move(geom_path));
            pWPShaderInfo->combos[std::string(WE_CB_GS_ENABLED)] = "1";
            geometry_shader_enabled                              = true;
            if (out_geometry_shader) *out_geometry_shader = true;
        }
    }
    add_shader_unit(ShaderType::FRAGMENT, shaderPath + ".frag");

    if (wpmat.shader == "genericropeparticle" && ! geometry_shader_enabled) {
        auto rope_source = MakeCpuRopeParticleVertexSource(sd_units.front().src);
        if (! rope_source) {
            rstd_error("genericropeparticle vertex source changed; cannot enable CPU rope path");
            return false;
        }
        sd_units.front().src        = *rope_source;
        sd_original_sources.front() = std::move(*rope_source);
    }

    if (wpmat.shader == "genericparticle" &&
        pWPShaderInfo->combos.contains("PARTICLEINSTANCED") &&
        pWPShaderInfo->combos.at("PARTICLEINSTANCED") == "1") {
        auto instanced_source = MakeInstancedGenericParticleVertexSource(sd_units.front().src);
        if (! instanced_source) {
            rstd_error("genericparticle vertex layout changed; cannot enable instanced particle path");
            return false;
        }
        sd_units.front().src          = *instanced_source;
        sd_original_sources.front()   = std::move(*instanced_source);
    }

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
                                     (bool)texh.extraHeader.at("compo4").val,
                                 } });
        } else
            texinfos.push_back({ true });
    }

    for (auto& unit : sd_units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, pWPShaderInfo, texinfos);
    }
    for (const auto& unit : sd_units) {
        if (unit.src.find("g_AudioSpectrum") != std::string::npos) {
            pScene->uses_audio_spectrum = true;
            break;
        }
    }
    ApplyLegacyAtmosphereUniformAliases(wpmat, *pWPShaderInfo);
    ApplyLegacyAtmosphereShaderCompat(wpmat, sd_units);

    for (const auto& el : wpmat.combos) {
        pWPShaderInfo->combos[el.first] = std::to_string(el.second);
    }
    if (blendMode == BlendMode::AlphaToCoverage) {
        pWPShaderInfo->combos["ALPHATOCOVERAGE"] = "1";
    }
    ApplySceneFogCombos(*pScene, *pWPShaderInfo);
    ApplySceneHdrCombo(*pScene, wpmat, *pWPShaderInfo);
    ApplyLegacyAtmosphereLightCombo(wpmat, *pWPShaderInfo);

    auto textures = wpmat.textures;
    if (pWPShaderInfo->defTexs.size() > 0) {
        for (auto& t : pWPShaderInfo->defTexs) {
            // `t.first` is a slot index parsed out of a `g_TextureN` uniform
            // name in shader source (see ShaderAnnotations.cpp). Re-check it
            // here so this resize can never be driven by shader text, even if
            // an entry reaches defTexs through another path.
            if (t.first < 0 || t.first >= kMaxMaterialTextureSlots) {
                rstd_error("shader default texture slot {} out of range [0,{})",
                           t.first,
                           kMaxMaterialTextureSlots);
                continue;
            }
            const usize slot = static_cast<usize>(t.first);
            if (textures.size() > slot) {
                if (! textures.at(slot).empty()) continue;
            } else {
                textures.resize(slot + 1);
            }
            textures.at(slot) = t.second;
        }
    }

    for (usize i = 0; i < textures.size(); i++) {
        std::string name = textures.at(i);
        ParseSpecTexName(name, wpmat, *pWPShaderInfo, *pScene);
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
                stex.sample  = texh.sample;
                stex.url     = name;
                stex.isVideo = texh.type == ImageType::VIDEO;
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
                    pWPShaderInfo->combos[std::string(WE_CB_SPRITESHEET)]  = "1";
                    pWPShaderInfo->combos[std::string(WE_CB_THICK_FORMAT)] = "1";
                    if (algorism::IsPowOfTwo((u32)texh.width) &&
                        algorism::IsPowOfTwo((u32)texh.height)) {
                        pWPShaderInfo->combos[std::string(WE_CB_SPRITESHEETBLENDNPOT)] = "1";
                        resolution[2] = resolution[0] - resolution[0] % (int)f1.width;
                        resolution[3] = resolution[1] - resolution[1] % (int)f1.height;
                    }
                    materialShader.constValues[std::string(G_RENDERVAR1)] = std::array {
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
    if (exists(pWPShaderInfo->combos, std::string(WE_CB_LIGHTING))) {
        // pWPShaderInfo->combos["PRELIGHTING"] =
        // pWPShaderInfo->combos.at(std::string(WE_CB_LIGHTING));
    }

    auto variant_desc          = MakeSceneShaderVariantDesc(pScene->scene_id,
                                                            wpmat,
                                                            *pWPShaderInfo,
                                                            sd_units,
                                                            sd_source_keys,
                                                            sd_original_sources,
                                                            texinfos,
                                                            geometry_shader_enabled);
    variant_desc.texture_slots = material.textures;

    if (! WPShaderParser::CompileToSpv(
            pScene->scene_id, sd_units, shader->codes, vfs, pWPShaderInfo, texinfos)) {
        return false;
    }
    shader->default_uniforms      = pWPShaderInfo->svs;
    variant_desc.default_uniforms = pWPShaderInfo->svs;
    WPShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        variant_desc, sd_units, shader->codes);

    material.blenmode    = blendMode;
    material.depth_test  = ParseEnabled(wpmat.depthtest);
    material.depth_write = ParseEnabled(wpmat.depthwrite);
    material.cull_mode   = ParseCullMode(wpmat.cullmode);

    // FS is always the last unit (VS may be followed by optional GS, then FS).
    const auto& fs_active     = sd_units.back().preprocess_info.active_tex_slots;
    const auto& fs_referenced = sd_units.back().preprocess_info.referenced_tex_slots;
    for (unsigned i = 0; i < material.textures.size(); i++) {
        if (! exists(fs_active, i)) material.textures[i].clear();
    }
    for (unsigned i = 0; i < material.textures.size(); i++) {
        if (material.textures[i] != WE_REFLECTION_PREFIX) continue;
        if (exists(fs_referenced, i))
            pScene->EnablePlanarReflection();
        else
            material.textures[i].clear();
    }

    for (const auto& el : pWPShaderInfo->baseConstSvs) {
        materialShader.constValues[el.first] = el.second;
    }
    // u_* user-variable uniforms: stage records into pWPShaderInfo so the
    // caller can register them into `Scene::shader_user_var_index` AFTER
    // moving `material` into a shared_ptr. Registering here would store a
    // stack-local pointer, freed once `AddMaterial(std::move(material))`
    // runs — a use-after-free as soon as ApplyUserPropertyToShaders fires.
    // Default values still seed constValues here; the values get carried
    // along by the move into the shared_ptr.
    for (const auto& var : pWPShaderInfo->scalar_uniforms) {
        if (! var.is_user || var.material.empty()) continue;
        pWPShaderInfo->user_var_staging.push_back(
            { var.material, var.name, var.default_value.clone() });
        if (auto value = shader->default_uniforms.find(var.name);
            value != shader->default_uniforms.end()) {
            materialShader.constValues[var.name] = value->second;
        }
    }

    material.customShader         = std::move(materialShader);
    material.customShader.variant = std::move(variant_desc);
    material.name                 = wpmat.shader;

    return true;
}

std::string ResolveShaderMaterialKey(const WPShaderInfo& info, const std::string& material_key) {
    if (auto it = info.alias.find(material_key); it != info.alias.end()) return it->second;

    for (const auto& el : info.alias) {
        if (el.second.size() > 2 && el.second.substr(2) == material_key) return el.second;
    }
    return {};
}

bool IsFrequencyRangeSummary(const WPShaderInfo& info, std::string_view name) {
    return name == "frequencyRange" &&
           ! ResolveShaderMaterialKey(info, "frequencyRangeStart").empty() &&
           ! ResolveShaderMaterialKey(info, "frequencyRangeEnd").empty();
}

bool IsShaderPositionUniform(const WPShaderInfo& info, const std::string& glname) {
    for (const auto& var : info.scalar_uniforms) {
        if (var.name == glname) return var.position;
    }
    return false;
}

bool UsesEffectPositionSpace(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/spin" && wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == 1;
}

bool UsesUnitFinalQuad(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == 1;
}

bool CanCompositeFinalEffectShader(std::string_view shader) {
    return IsLayerCompositeShader(shader) || shader == "effects/transform" ||
           shader == "effects/scroll" || shader == "effects/spin" ||
           shader == "effects/perspective" || shader == "effects/foliagesway";
}

bool HasShaderCombo(const WPShaderInfo& info, std::string_view combo_name) {
    return std::ranges::any_of(info.combo_defs, [&](const auto& combo) {
        return combo.combo == combo_name;
    });
}

bool HasShaderTextureMaterial(const WPShaderInfo& info, std::string_view material_key) {
    return std::ranges::any_of(info.texture_uniforms, [&](const auto& tex) {
        return tex.material == material_key;
    });
}

bool HasSolidCompositeContext(const ParseContext& context, const wpscene::ImageObject& obj) {
    if (obj.solid || context.solid_layer_ids.contains(obj.id)) return true;

    std::unordered_set<std::int32_t> seen;
    std::uint32_t                    parent = obj.parent;
    while (parent != 0 && seen.insert(static_cast<std::int32_t>(parent)).second) {
        const auto parent_id = static_cast<std::int32_t>(parent);
        if (context.solid_layer_ids.contains(parent_id)) return true;

        auto it = context.object_parent_ids.find(parent_id);
        if (it == context.object_parent_ids.end()) break;
        parent = it->second;
    }

    return false;
}

bool CanCompositeFinalEffectMaterial(std::string_view shader, const WPShaderInfo& info,
                                     bool allow_transparent_previous) {
    if (CanCompositeFinalEffectShader(shader)) return true;
    if (! allow_transparent_previous) return false;

    // WE's files in the wild use TRANSPARENCY + previous as a final-composite
    // fallback in non-solid layer contexts.
    return HasShaderCombo(info, "TRANSPARENCY") && HasShaderTextureMaterial(info, "previous");
}

void NormalizeEffectPositionCurve(SceneAnimationCurve& curve) {
    auto normalize_axis = [&](std::vector<SceneAnimationKey>& keys) {
        for (auto& key : keys) {
            key.value = curve.relative ? key.value * 2.0f : key.value * 2.0f - 1.0f;
        }
    };
    normalize_axis(curve.c0);
    normalize_axis(curve.c1);
}

script::FieldScript* RegisterMaterialValueScript(ParseContext&                  context,
                                                 SceneNode*                     owner,
                                                 const wpscene::Material&       material,
                                                 const std::string&             material_key,
                                                 const wpscene::ScriptBinding& binding) {
    if (! owner) return nullptr;
    auto value = material.constantshadervalues.find(material_key);
    if (value == material.constantshadervalues.end()) return nullptr;

    script::FieldKind kind = script::FieldKind::Unknown;
    switch (value->second.size()) {
    case 1: kind = script::FieldKind::Scalar; break;
    case 2: kind = script::FieldKind::Vec2; break;
    case 3: kind = script::FieldKind::Vec3; break;
    case 4: kind = script::FieldKind::Vec4; break;
    default: return nullptr;
    }

    auto  sha = utils::genSha1(std::span<const char>(binding.source));
    auto* field_script = EnsureScriptScene(context).runtime().MakeFieldScript(binding.source,
                                                                               sha,
                                                                               kind,
                                                                               binding.properties,
                                                                               binding.initial_value,
                                                                               owner);
    RegisterFieldScriptMetadata(context, owner, field_script);
    return field_script;
}

void RegisterImageEffectVisibilityScript(
    ParseContext& context, SceneNode* owner,
    const std::shared_ptr<SceneImageEffectLayer>& effect_layer,
    const std::shared_ptr<SceneImageEffect>& effect,
    const wpscene::FieldBindings& bindings) {
    auto binding = bindings.scripts.find("visible");
    if (binding == bindings.scripts.end() || ! owner || ! effect_layer || ! effect) return;
    auto& scripts = EnsureScriptScene(context);
    auto  sha     = utils::genSha1(std::span<const char>(binding->second.source));
    auto* field_script = scripts.runtime().MakeFieldScript(binding->second.source,
                                                            sha,
                                                            script::FieldKind::Bool,
                                                            binding->second.properties,
                                                            binding->second.initial_value,
                                                            owner);
    RegisterFieldScriptMetadata(context, owner, field_script);
    if (! field_script) return;
    scripts.runtime().SetFieldScriptEffectSelf(
        *field_script, { .layer = effect_layer.get(), .effect = effect });
    auto* scene = context.scene.get();
    scripts.AddActuator({
        field_script,
        [scene, effect_layer, effect](const script::ScriptValue& value) {
            auto visible = std::get_if<script::BoolValue>(&value);
            if (! visible) return;
            scene->SetImageEffectRuntimeVisible(
                { .layer = effect_layer.get(), .effect = effect }, visible->v);
        },
    });
}

void RegisterHiddenTextEffectScripts(ParseContext&                         context,
                                     SceneNode*                            owner,
                                     std::span<const wpscene::ImageEffect> effects) {
    for (const auto& effect : effects) {
        if (effect.visible || ! effect.visible_user.empty()) continue;
        for (usize index = 0; index < effect.materials.size(); ++index) {
            auto material = effect.materials[index].clone();
            if (index < effect.passes.size()) material.MergePass(effect.passes[index]);
            for (const auto& [material_key, binding] :
                 material.constantshadervalues_bindings.scripts) {
                RegisterMaterialValueScript(context, owner, material, material_key, binding);
            }
        }
    }
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
void RegisterShaderUserVarIndex(ParseContext& context, SceneNode* owner,
                                const std::shared_ptr<SceneMaterial>& stable_mat,
                                const wpscene::Material& wpmat,
                                const WPShaderInfo& info) {
    Scene* pScene = context.scene.get();
    if (! pScene || ! stable_mat) return;
    for (const auto& combo : info.combo_defs) {
        if (combo.material.empty() || combo.combo.empty()) continue;
        Scene::ShaderComboUserBinding binding {
            .material = stable_mat,
            .combo    = combo.combo,
            .fallback = std::to_string(combo.default_),
        };
        for (const auto& [label, value] : combo.options) {
            binding.options[label] = std::to_string(value);
        }
        pScene->shader_combo_user_index[combo.material].push_back(std::move(binding));
    }
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

    if (owner) {
        for (const auto& [_, animation] : stable_mat->customShader.valueAnimations) {
            if (animation.curve && animation.curve->playback)
                owner->RegisterAnimationPlayback(animation.curve->playback);
        }
    }

    if (! owner || wpmat.constantshadervalues_bindings.scripts.empty()) return;
    auto& scripts = EnsureScriptScene(context);
    for (const auto& [material_key, binding] :
         wpmat.constantshadervalues_bindings.scripts) {
        auto uniform_name = ResolveShaderMaterialKey(info, material_key);
        if (uniform_name.empty()) continue;
        auto* field_script =
            RegisterMaterialValueScript(context, owner, wpmat, material_key, binding);
        if (! field_script) continue;
        if (auto animation = stable_mat->customShader.valueAnimations.find(uniform_name);
            animation != stable_mat->customShader.valueAnimations.end() &&
            animation->second.curve && animation->second.curve->playback) {
            scripts.runtime().SetImplicitAnimation(*field_script,
                                                    animation->second.curve->playback);
        }
        scripts.AddActuator({
            field_script,
            [pScene, stable_mat, uniform_name = std::move(uniform_name)](
                const script::ScriptValue& script_value) {
                auto value = ScriptValueAsShaderValue(script_value);
                if (! value) return;
                (void)pScene->SetMaterialShaderValue(*stable_mat, uniform_name, *value);
            },
        });
    }
}

std::optional<std::string> UserTexturePropertyKey(const Json& binding) {
    if (binding.is_string()) {
        auto key = rstd::cppstd::to_string(*binding.as_str());
        if (key.empty()) return std::nullopt;
        return key;
    }
    if (! binding.is_object()) return std::nullopt;
    auto type  = binding.get("type");
    auto value = binding.get("name");
    if (type.is_none() || value.is_none()) return std::nullopt;
    auto type_string  = (*type)->as_str();
    auto value_string = (*value)->as_str();
    if (type_string.is_none() || value_string.is_none()) return std::nullopt;
    auto binding_type = rstd::cppstd::as_string_view(*type_string);
    auto name = rstd::cppstd::as_string_view(*value_string);
    if (binding_type == "usershortcut") return std::string(name);
    if (binding_type != "system") return std::nullopt;
    if (name != "$mediaThumbnail" && name != "$mediaPreviousThumbnail") return std::nullopt;
    return std::string(name);
}

bool IsSystemMediaTextureBinding(const Json& binding) {
    if (! binding.is_object()) return false;
    auto type = binding.get("type");
    if (type.is_none()) return false;
    auto value = (*type)->as_str();
    return value.is_some() && rstd::cppstd::as_string_view(*value) == "system";
}

bool IsUserShortcutTextureBinding(const Json& binding) {
    if (! binding.is_object()) return false;
    auto type = binding.get("type");
    if (type.is_none()) return false;
    auto value = (*type)->as_str();
    return value.is_some() && rstd::cppstd::as_string_view(*value) == "usershortcut";
}

struct SolidColorNeutralizationSource {
    const rstd::sync::Arc<SceneNode>* node { nullptr };
    usize                             slot { 0 };
    std::array<float, 3>              color { 1.0f, 1.0f, 1.0f };
};

void RegisterMaterialUserTextureIndex(Scene*                                pScene,
                                      const std::shared_ptr<SceneMaterial>& stable_mat,
                                      const wpscene::Material&              fallback_material,
                                      const SolidColorNeutralizationSource& neutralization = {}) {
    if (! pScene || ! stable_mat) return;
    for (usize i = 0; i < fallback_material.usertextures.len(); ++i) {
        auto key = UserTexturePropertyKey(fallback_material.usertextures[i]);
        if (! key.has_value()) continue;
        std::string fallback;
        if (i < fallback_material.textures.size()) fallback = fallback_material.textures[i];
        if (IsSystemMediaTextureBinding(fallback_material.usertextures[i]) &&
            i < stable_mat->textures.size()) {
            fallback = stable_mat->textures[i];
        }
        Scene::MaterialTextureUserBinding binding { .material = stable_mat,
                                                    .slot     = static_cast<uint32_t>(i),
                                                    .fallback = std::move(fallback) };
        if (IsSystemMediaTextureBinding(fallback_material.usertextures[i]))
            binding.kind = Scene::MaterialTextureUserBinding::Kind::System;
        if (IsUserShortcutTextureBinding(fallback_material.usertextures[i]))
            binding.kind = Scene::MaterialTextureUserBinding::Kind::UserShortcut;
        if (neutralization.node != nullptr && i == neutralization.slot) {
            binding.solid_color = Scene::MaterialSolidColorNeutralization {
                .node           = neutralization.node->clone(),
                .authored_color = Vector3f(neutralization.color.data())
            };
        }
        pScene->material_texture_user_index[*key].push_back(std::move(binding));
    }
}

Vector3f AlignmentOffset(std::string_view align, Vector2f size) {
    Vector3f offset = Vector3f::Zero();
    size *= 0.5f;
    size.y() *= 1.0f;

    auto contains = [&](std::string_view s) {
        return align.find(s) != std::string::npos;
    };

    // topleft top center ...
    if (contains("top")) offset.y() -= size.y();
    if (contains("left")) offset.x() += size.x();
    if (contains("right")) offset.x() -= size.x();
    if (contains("bottom")) offset.y() += size.y();

    return offset;
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
        if (el.index < 0 || el.index >= kMaxMaterialTextureSlots) {
            rstd_error("material bind '{}' texture index {} out of range [0,{})",
                       el.name,
                       el.index,
                       kMaxMaterialTextureSlots);
            continue;
        }
        const usize slot = static_cast<usize>(el.index);
        if (wpmat.textures.size() <= slot) wpmat.textures.resize(slot + 1);
        wpmat.textures.at(slot) = fboMap.at(el.name);
    }
}

std::string ResolveSceneTextureProperty(const ParseContext& context, std::string_view key) {
    if (context.user_properties.is_none()) return {};
    auto prop = (*context.user_properties)->get(rstd::cppstd::as_str(key));
    if (prop.is_none()) return {};
    const auto& payload = **prop;
    if (payload.is_string()) {
        auto text = rstd::cppstd::to_string(*payload.as_str());
        return text.empty() ? std::string {} : text;
    }
    if (! payload.is_object()) return {};

    std::string type;
    if (auto value = payload.get("type"); value.is_some()) {
        auto string = (*value)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return {};
    auto value = payload.get("value");
    if (value.is_none()) return {};
    auto string = (*value)->as_str();
    return string.is_none() ? std::string {} : rstd::cppstd::to_string(*string);
}

std::string ResolveUserShortcutTextureProperty(const ParseContext& context, std::string_view key) {
    if (context.user_properties.is_none()) return {};
    auto prop = (*context.user_properties)->get(rstd::cppstd::as_str(key));
    if (prop.is_none() || ! (**prop).is_object()) return {};
    auto icon = (**prop).get("icon");
    if (icon.is_none()) return {};
    auto string = (*icon)->as_str();
    return string.is_none() ? std::string {} : rstd::cppstd::to_string(*string);
}

std::string ResolveUserTextureProperty(const ParseContext& context, const Json& binding) {
    auto key = UserTexturePropertyKey(binding);
    if (! key.has_value()) return {};
    if (IsUserShortcutTextureBinding(binding))
        return ResolveUserShortcutTextureProperty(context, *key);
    return ResolveSceneTextureProperty(context, *key);
}

std::string ResolveMaterialTextureSlot(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    std::string fallback;
    if (slot < material.textures.size()) fallback = material.textures[slot];
    if (slot >= material.usertextures.len()) return fallback;

    if (auto prop = ResolveUserTextureProperty(context, material.usertextures[slot]);
        ! prop.empty())
        return prop;
    return fallback;
}

bool CanUseImageAsSystemMediaFallback(const wpscene::ImageObject& image) {
    if (! image.puppet.empty()) return false;
    if (image.fullscreen || image.config.passthrough) return false;
    return CountVisibleImageEffects(image.effects) == 0;
}

std::string ResolveLinkedImageFallback(const ParseContext& context, std::string_view texture) {
    std::optional<std::uint32_t> linked_id = ParseImageLayerCompositeId(texture);
    if (! linked_id && IsSpecLinkTex(texture)) linked_id = ParseLinkTex(texture);
    if (! linked_id) return {};

    auto it = context.system_media_image_fallbacks.find(static_cast<std::int32_t>(*linked_id));
    if (it == context.system_media_image_fallbacks.end()) return {};
    return it->second;
}

std::string ResolveSystemMediaFallback(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    if (slot >= material.textures.size()) return {};
    return ResolveLinkedImageFallback(context, material.textures[slot]);
}

void ApplyUserTextureBindings(ParseContext& context, wpscene::Material& material) {
    for (usize i = 0; i < material.usertextures.len(); ++i) {
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

void IndexSystemMediaImageFallbacks(ParseContext& context, std::span<SceneObjectVar> scene_objs) {
    context.system_media_image_fallbacks.clear();
    for (const auto& obj : scene_objs) {
        const auto* image = std::get_if<wpscene::ImageObject>(&obj);
        if (image == nullptr || ! CanUseImageAsSystemMediaFallback(*image)) continue;

        auto texture = ResolveMaterialTextureSlot(context, image->material, 0);
        if (texture.empty() || IsSpecTex(texture)) continue;
        context.system_media_image_fallbacks[image->id] = std::move(texture);
    }
}

void LoadConstvalue(
    ParseContext& context, SceneMaterial& material, const wpscene::Material& wpmat,
    const WPShaderInfo& info,
    sr::Map<std::string, SceneShaderValueAnimation>* final_quad_shader_values = nullptr) {
    std::unordered_map<std::string, std::string> parents;
    for (const auto& [field, animation] : wpmat.constantshadervalues_animations) {
        if (auto children = animation.options.children.as_array(); children.is_some()) {
            for (const auto& child : **children) {
                if (auto key = AnimationLinkKey(child)) parents[*key] = field;
            }
        }
        if (auto parent = AnimationLinkKey(animation.options.parent)) parents[field] = *parent;
    }
    auto root_field = [&](std::string field) {
        std::unordered_set<std::string> visited;
        while (visited.insert(field).second) {
            auto it = parents.find(field);
            if (it == parents.end() ||
                ! wpmat.constantshadervalues_animations.contains(it->second))
                break;
            field = it->second;
        }
        return field;
    };
    std::unordered_map<std::string, std::shared_ptr<SceneAnimationPlayback>> playbacks;
    auto playback_for = [&](std::string_view field) -> std::shared_ptr<SceneAnimationPlayback> {
        auto root = root_field(std::string(field));
        auto source_it = wpmat.constantshadervalues_animations.find(root);
        if (source_it == wpmat.constantshadervalues_animations.end()) return nullptr;
        const auto& source = source_it->second;
        auto events        = ToSceneAnimationEvents(source.options.events);
        if (source.options.name.empty() && ! source.options.startpaused && events.empty())
            return nullptr;
        auto& playback = playbacks[root];
        if (! playback) {
            playback = std::make_shared<SceneAnimationPlayback>(source.options.name,
                                                                 source.options.fps,
                                                                 source.options.length,
                                                                 source.options.mode,
                                                                 source.options.wraploop,
                                                                 source.options.startpaused,
                                                                 std::move(events));
        }
        return playback;
    };
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name   = cs.first;
        const std::vector<float>& value  = cs.second;
        std::string               glname = ResolveShaderMaterialKey(info, name);
        if (glname.empty()) {
            if (IsLegacyAtmosphereShadowValue(wpmat, name)) continue;
            if (IsFrequencyRangeSummary(info, name)) continue;
            std::string warning_key = wpmat.shader;
            warning_key.push_back('\0');
            warning_key.append(name);
            if (context.unresolved_shader_values.insert(std::move(warning_key)).second) {
                if (wpmat.constantshadervalues_animations.contains(name)) {
                    rstd_warn("animated shader value '{}' has no uniform in '{}'",
                              name,
                              wpmat.shader);
                } else {
                    rstd_debug("ignoring shader value '{}' without a uniform in '{}'",
                               name,
                               wpmat.shader);
                }
            }
        } else {
            std::vector<float> const_value = value;
            bool               normalize_position =
                UsesEffectPositionSpace(wpmat) && IsShaderPositionUniform(info, glname);
            std::optional<SceneShaderValueAnimation> final_quad_value;
            if (normalize_position && const_value.size() >= 2) {
                final_quad_value.emplace();
                final_quad_value->base = ShaderValue(value);
                const_value[0]         = const_value[0] * 2.0f - 1.0f;
                const_value[1]         = const_value[1] * 2.0f - 1.0f;
            }
            material.SetShaderValue(glname, ShaderValue(const_value));
            if (auto it = wpmat.constantshadervalues_animations.find(name);
                it != wpmat.constantshadervalues_animations.end()) {
                auto curve =
                    std::make_shared<SceneAnimationCurve>(ToSceneAnimationCurve(it->second));
                curve->playback = playback_for(name);
                if (final_quad_value) final_quad_value->curve = curve;
                if (normalize_position) {
                    curve = std::make_shared<SceneAnimationCurve>(*curve);
                    NormalizeEffectPositionCurve(*curve);
                }
                material.SetShaderValueAnimation(glname, std::move(curve));
            }
            if (final_quad_value && final_quad_shader_values) {
                (*final_quad_shader_values)[glname] = std::move(*final_quad_value);
            }
        }
    }
}

// parse

void ParseCamera(ParseContext& context, const wpscene::SceneMetadata& sc) {
    auto& scene   = *context.scene;
    auto& general = sc.general;
    // effect camera
    scene.cameras["effect"] =
        std::make_shared<SceneCamera>(SceneCamera::MakeOrthographic(2, 2, -1.0, 1.0));
    context.effect_camera_node = rstd::Some(rstd::sync::Arc<SceneNode>::make()); // at 0,0,0
    scene.cameras.at("effect")->AttatchNode((*context.effect_camera_node).as_ptr());
    scene.sceneGraph->AppendChild((*context.effect_camera_node).clone());

    // global camera
    const auto projection_extent = scene.OrthographicProjectionExtent();
    scene.cameras["global"] = std::make_shared<SceneCamera>(SceneCamera::MakeOrthographic(
        projection_extent[0], projection_extent[1], -5000.0, 5000.0));
    scene.activeCamera      = scene.cameras.at("global").get();
    Vector3f cori { (float)context.ortho_w / 2.0f, (float)context.ortho_h / 2.0f, 0 },
        cscale { 1.0f, 1.0f, 1.0f }, cangle(Vector3f::Zero());

    context.global_camera_node = rstd::Some(rstd::sync::Arc<SceneNode>::make(cori, cscale, cangle));
    scene.activeCamera->AttatchNode((*context.global_camera_node).as_ptr());
    scene.sceneGraph->AppendChild((*context.global_camera_node).clone());

    const bool override_perspective_fov = general.perspectiveoverridefov > 0.0f;
    const double perspective_fov =
        override_perspective_fov
            ? static_cast<double>(general.perspectiveoverridefov)
            : algorism::CalculatePersperctiveFov(1000.0, projection_extent[1]);
    const double perspective_distance =
        override_perspective_fov
            ? algorism::CalculatePersperctiveDistance(perspective_fov, projection_extent[1])
            : 1000.0;
    // WE uses a reverse-Z perspective projection for 3D layers embedded in
    // an orthographic scene.
    const double perspective_near = general.isOrtho ? 15000.0 : general.nearz;
    const double perspective_far  = general.isOrtho ? 5.0 : general.farz;
    scene.cameras["global_perspective"] = std::make_shared<SceneCamera>(
        SceneCamera::MakePerspective(static_cast<double>(context.ortho_w) / context.ortho_h,
                                     perspective_near,
                                     perspective_far,
                                     perspective_fov));

    Vector3f cperori = cori;
    cperori[2]       = static_cast<float>(perspective_distance);
    context.global_perspective_camera_node =
        rstd::Some(rstd::sync::Arc<SceneNode>::make(cperori, cscale, cangle));
    scene.cameras["global_perspective"]->AttatchNode(
        (*context.global_perspective_camera_node).as_ptr());
    if (override_perspective_fov && general.isOrtho) {
        scene.cameras["global_perspective"]->SetLookAt(
            Vector3d { cperori.x(), cperori.y(), cperori.z() },
            Vector3d { cori.x(), cori.y(), 0.0 },
            Vector3d::UnitY());
    }
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
    if (! cam.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(cam.visible_user));

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
    if (! cam.visible_user.empty())
        path->visible_user_binding = ToSceneUserVisibilityBinding(cam.visible_user);
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

void InitContext(ParseContext& context, fs::VFS& vfs, const wpscene::SceneMetadata& sc,
                 std::array<i32, 2> ortho_extent) {
    context.scene            = std::make_shared<Scene>();
    context.vfs              = &vfs;
    auto& scene              = *context.scene;
    scene.imageParser        = std::make_unique<TextureAssetDecoder>(&vfs);
    scene.paritileSys->gener = std::make_unique<WPParticleRawGener>();
    scene.shaderValueUpdater = std::make_unique<SceneUniformUpdater>(&scene);
    GenCardMesh(scene.default_effect_mesh, { 2.0f, 2.0f });
    context.shader_updater = static_cast<SceneUniformUpdater*>(scene.shaderValueUpdater.get());

    scene.clearColor = sc.general.clearcolor;
    if (auto it = sc.general.user_bindings.find("clearcolor");
        it != sc.general.user_bindings.end()) {
        scene.clearColorUserKey = it->second;
    }
    scene.ortho[0]  = ortho_extent[0];
    scene.ortho[1]  = ortho_extent[1];
    scene.hdr_enabled        = sc.general.hdr;
    scene.hdr_render_targets = sc.general.hdr;
    scene.SetProjectionKind(sc.general.isOrtho ? SceneProjectionKind::OrthographicCanvas
                                                : SceneProjectionKind::Perspective3D);
    scene.SetViewportScale(sc.general.zoom);
    if (sc.general.isOrtho) {
        SceneAnimationCurve viewport_scale_curve;
        AssignCurve(viewport_scale_curve, sc.general.field_bindings, "zoom");
        scene.SetViewportScaleAnimation(std::move(viewport_scale_curve));
    }
    context.ortho_w = scene.ortho[0];
    context.ortho_h = scene.ortho[1];
    context.orthographic_scene = sc.general.isOrtho;

    {
        auto& gb                       = context.global_base_uniforms;
        gb[std::string(G_VIEWUP)]      = std::array { 0.0f, 1.0f, 0.0f };
        gb[std::string(G_VIEWRIGHT)]   = std::array { 1.0f, 0.0f, 0.0f };
        gb[std::string(G_VIEWFORWARD)] = std::array { 0.0f, 0.0f, -1.0f };
        gb[std::string(G_EYEPOSITION)] = std::array { 0.0f, 0.0f, 0.0f };
        gb[std::string(G_TEXELSIZE)]   = std::array { 1.0f / 1920.0f, 1.0f / 1080.0f };
        gb[std::string(G_TEXELSIZEHALF)] =
            std::array { 1.0f / 1920.0f / 2.0f, 1.0f / 1080.0f / 2.0f };
        gb[std::string(G_LIGHTAMBIENTCOLOR)]  = sc.general.ambientcolor;
        gb[std::string(G_LIGHTSKYLIGHTCOLOR)] = sc.general.skylightcolor;

        if (sc.general.fogdistance) {
            scene.fog_distance_enabled       = true;
            gb[std::string(G_FOGDISTANCECOLOR)] = sc.general.fogdistancecolor;
            gb[std::string(G_FOGDISTANCEPARAMS)] = std::array {
                sc.general.fogdistancestart,
                sc.general.fogdistanceend - sc.general.fogdistancestart,
                sc.general.fogdistancestartdensity,
                sc.general.fogdistanceenddensity - sc.general.fogdistancestartdensity,
            };
        }
        if (sc.general.fogheight) {
            scene.fog_height_enabled       = true;
            gb[std::string(G_FOGHEIGHTCOLOR)] = sc.general.fogheightcolor;
            gb[std::string(G_FOGHEIGHTPARAMS)] = std::array {
                sc.general.fogheightstart,
                sc.general.fogheightend - sc.general.fogheightstart,
                sc.general.fogheightstartdensity,
                sc.general.fogheightenddensity - sc.general.fogheightstartdensity,
            };
        }
    }

    {
        SceneCameraParallax cam_para;
        cam_para.enable         = sc.general.cameraparallax;
        cam_para.amount         = sc.general.cameraparallaxamount;
        cam_para.delay          = sc.general.cameraparallaxdelay;
        cam_para.mouseinfluence = sc.general.cameraparallaxmouseinfluence;
        context.shader_updater->SetCameraParallax(cam_para);
        for (const auto& [field, key] : sc.general.user_bindings) {
            if (field == "cameraparallax" || field == "cameraparallaxamount" ||
                field == "cameraparallaxdelay" || field == "cameraparallaxmouseinfluence") {
                scene.camera_parallax_user_var_index[key].push_back(field);
            }
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

void ParseImageObj(ParseContext& context, wpscene::ImageObject& img_obj,
                   ImageParseGeometry parse_geometry = {}) {
    auto& wpimgobj = img_obj;
    // Invisible image layers are kept in the scene tree because their composite
    // may be sampled by other layers via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder decides whether to actually emit passes for them.
    if (! wpimgobj.visible) {
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }

    auto& vfs = *context.vfs;

    bool       isPassthrough      = wpimgobj.config.passthrough;
    const bool alpha_can_change   = ! wpimgobj.alpha_user_key.empty() ||
                                    wpimgobj.field_bindings.animations.count("alpha") != 0 ||
                                    wpimgobj.field_bindings.scripts.count("alpha") != 0;
    const auto geometry_size      = wpimgobj.size;
    const auto effect_target_size = ImageEffectTargetSize(context, wpimgobj);

    bool hasPuppet = ! wpimgobj.puppet.empty();
    (void)hasPuppet;

    std::unique_ptr<WPMdl>        puppet;
    bool                          has_bones = false;
    bool                          has_mesh  = false;
    const WPMdl::Mesh*            primary_puppet_mesh { nullptr };
    std::vector<const WPMdl::Mesh*> supplemental_puppet_meshes;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! WPMdlParser::Parse(wpimgobj.puppet, vfs, *puppet)) {
            rstd_error("parse puppet failed: {}", wpimgobj.puppet);
            puppet = nullptr;
        } else {
            has_bones = puppet->puppet && ! puppet->puppet->bones.empty();
            if (! wpimgobj.material_path.empty()) {
                auto primary_index =
                    WPMdlParser::FindMeshByMaterial(*puppet, wpimgobj.material_path);
                if (primary_index && ! puppet->meshes[*primary_index].positions.empty()) {
                    primary_puppet_mesh = &puppet->meshes[*primary_index];
                }
            }
            if (primary_puppet_mesh == nullptr) {
                for (const auto& candidate : puppet->meshes) {
                    if (candidate.positions.empty()) continue;
                    primary_puppet_mesh = &candidate;
                    break;
                }
            }
            for (const auto& candidate : puppet->meshes) {
                if (candidate.positions.empty() || &candidate == primary_puppet_mesh) continue;
                supplemental_puppet_meshes.push_back(&candidate);
            }
            has_mesh = primary_puppet_mesh != nullptr;
            if (! has_bones && ! has_mesh) {
                rstd_error("puppet has no mesh data: {}", wpimgobj.puppet);
                puppet = nullptr;
            }
        }
    }

    const bool has_author_effect =
        CountRuntimeImageEffects(wpimgobj.effects, context.scene_accesses_effects) > 0;
    // A solid layer's flat material only produces its source color; a final compositor owns
    // BLENDMODE and the previous-framebuffer input.
    const bool layer_material_is_final =
        (! has_author_effect || has_bones) && ! wpimgobj.solid_layer;
    const bool color_blend_uses_layer_material =
        wpimgobj.colorBlendMode != 0 && layer_material_is_final;
    const bool append_color_blend_final_effect =
        wpimgobj.colorBlendMode != 0 && ! color_blend_uses_layer_material;
    std::optional<BlendMode> color_blend_attachment_override;
    if (append_color_blend_final_effect) {
        wpscene::ImageEffect colorEffect;
        wpscene::Material    colorMat;
        auto                 parsed = sr::ParseJson(
            fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"));
        if (parsed.is_err()) {
            rstd_error("parse effectpassthrough.json failed: {}", parsed.unwrap_err());
            return;
        }
        auto json = parsed.unwrap();
        colorMat.FromJson(json);
        colorMat.combos[std::string(WE_CB_BONECOUNT)] = 1;
        color_blend_attachment_override = ApplyImageColorBlend(colorMat, wpimgobj);
        colorEffect.materials.push_back(std::move(colorMat));
        wpimgobj.effects.push_back(std::move(colorEffect));
    }
    const bool is_hidden_link_source =
        context.hidden_link_source_ids.count(static_cast<std::int32_t>(wpimgobj.id)) != 0;
    const bool is_linked_composite =
        wpimgobj.composite_layer &&
        context.IsLinkedSource(static_cast<std::int32_t>(wpimgobj.id));
    if (! has_author_effect && (is_hidden_link_source || is_linked_composite)) {
        AppendLayerCompositePassthroughEffect(vfs, wpimgobj);
    }
    const bool composite_render_path =
        wpimgobj.composite_layer && ! (is_hidden_link_source || is_linked_composite);

    bool hasEffect =
        CountRuntimeImageEffects(wpimgobj.effects, context.scene_accesses_effects) > 0;

    // No-effect fullscreen / compose layers contribute nothing on their own
    // (they just sample `_rt_default` and write it back). Mark as elidable
    // so the render-graph builder drops them when unreferenced, or routes
    // them to `_rt_link_<id>` when another layer reads their composite.
    if (! hasEffect && wpimgobj.visible && (wpimgobj.fullscreen || isPassthrough)) {
        context.scene->MarkLayerStaticElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }
    if (! hasEffect && wpimgobj.visible && wpimgobj.alpha <= 0.0f && ! alpha_can_change) {
        context.scene->MarkLayerStaticElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto           spImgNode = rstd::sync::Arc<SceneNode>::make(Vector3f(wpimgobj.origin.data()),
                                                                Vector3f(wpimgobj.scale.data()),
                                                                Vector3f(wpimgobj.angles.data()),
                                                                wpimgobj.name);
    if (! wpimgobj.visible) spImgNode->SetVisible(false);
    const Vector3f alignment_offset =
        wpimgobj.fullscreen
            ? Vector3f::Zero()
            : AlignmentOffset(wpimgobj.alignment, { geometry_size[0], geometry_size[1] });
    const bool solid_scene_context = HasSolidCompositeContext(context, wpimgobj);
    spImgNode->SetSize({ geometry_size[0], geometry_size[1] });
    spImgNode->SetHitCenter({ alignment_offset.x(), alignment_offset.y() });
    if (hasEffect && composite_render_path) {
        spImgNode->SetGeometryTransform(
            Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
    }
    spImgNode->SetPerspective(wpimgobj.perspective);
    spImgNode->SetReflected(wpimgobj.reflected);
    spImgNode->SetBaseColor(Vector3f(wpimgobj.color.data()), wpimgobj.alpha);
    spImgNode->ID() = wpimgobj.id;
    if (! wpimgobj.visible_user.empty())
        spImgNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wpimgobj.visible_user));
    std::vector<std::shared_ptr<SceneMaterial>> image_property_materials;
    auto track_image_property_material = [&](const std::shared_ptr<SceneMaterial>& mat) {
        if ((wpimgobj.color_user_key.empty() && wpimgobj.alpha_user_key.empty()) || mat == nullptr)
            return;
        image_property_materials.push_back(mat);
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
    bool puppet_has_masks = primary_puppet_mesh != nullptr && ! primary_puppet_mesh->masks.empty();
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
    wpscene::Material image_wpmat                 = wpimgobj.material.clone();
    image_wpmat.combos[std::string(WE_CB_SCENE_ORTHO)]  = wpimgobj.perspective ? 0 : 1;
    image_wpmat.combos[std::string(OWE_CB_IMAGE_LAYER)] = 1;
    wpscene::Material image_user_texture_fallback = image_wpmat.clone();
    if (color_blend_uses_layer_material && ! hasEffect) ApplyImageColorBlend(image_wpmat, wpimgobj);
    ApplyUserTextureBindings(context, image_wpmat);
    const bool replaced_solid_color =
        wpimgobj.solid_layer && ! image_user_texture_fallback.textures.empty() &&
        ! image_wpmat.textures.empty() && image_user_texture_fallback.textures[0] == "util/white" &&
        image_wpmat.textures[0] != image_user_texture_fallback.textures[0];
    const bool solid_color_media_slot =
        wpimgobj.solid_layer && ! image_user_texture_fallback.textures.empty() &&
        image_user_texture_fallback.textures[0] == "util/white" &&
        image_user_texture_fallback.usertextures.len() > 0 &&
        IsSystemMediaTextureBinding(image_user_texture_fallback.usertextures[0]);
    const std::array<float, 3> layer_color =
        replaced_solid_color ? std::array<float, 3> { 1.0f, 1.0f, 1.0f } : wpimgobj.color;
    spImgNode->SetBaseColor(Vector3f(layer_color.data()), wpimgobj.alpha);
    {
        svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
        svData.propagatedParallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
        if (! hasEffect) {
            svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
            if (puppet && has_bones) {
                WPMdlParser::AddPuppetShaderInfo(shaderInfo, *puppet);
            }
        }

        baseConstSvs[std::string(G_COLOR4)] = std::array<float, 4> {
            layer_color[0], layer_color[1], layer_color[2], wpimgobj.alpha
        };
        baseConstSvs[std::string(G_COLOR)] =
            std::array<float, 3> { layer_color[0], layer_color[1], layer_color[2] };
        baseConstSvs[std::string(G_ALPHA)]      = wpimgobj.alpha;
        baseConstSvs[std::string(G_USERALPHA)]  = wpimgobj.alpha;
        baseConstSvs[std::string(G_BRIGHTNESS)] = wpimgobj.brightness;

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
        LoadConstvalue(context, material, image_wpmat, shaderInfo);
    }

    if (! material.textures.empty()) {
        if (auto control = context.scene->VideoControl(material.textures.front())) {
            spImgNode->SetVideoControl(std::move(control));
        }
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

    // mesh
    SceneMesh                  effct_final_mesh {};
    auto                       spMesh        = std::make_shared<SceneMesh>();
    auto&                      mesh          = *spMesh;
    const std::array<float, 2> mapRate       = Texture0UvScale(material, wpimgobj.nopadding);
    const Vector3f source_alignment_offset   = hasEffect ? Vector3f::Zero() : alignment_offset;
    auto           add_puppet_mask_submeshes = [&](SceneMesh& target, uint32_t first_mask_slot) {
        if (! puppet_has_masks) return;
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : puppet->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
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
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                target.Submeshes().emplace_back();
                auto& pre_sm = target.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b, mapRate);
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a, mapRate);
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet) {
        if (hasEffect) {
            GenCardMesh(
                mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
            if (primary_puppet_mesh != nullptr) {
                effct_final_mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(
                    effct_final_mesh.Submeshes().back(), *primary_puppet_mesh, mapRate);
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, 1);

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat = image_wpmat.clone();
                puppet_mat.textures[0]          = "";
                WPMdlParser::AddPuppetMatInfo(puppet_mat, *puppet);
                if (color_blend_uses_layer_material) {
                    color_blend_attachment_override =
                        ApplyImageColorBlend(puppet_mat, wpimgobj);
                }
                puppet_effect.materials.push_back(std::move(puppet_mat));
                wpimgobj.effects.push_back(std::move(puppet_effect));
            }
        } else {
            mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            if (primary_puppet_mesh != nullptr) {
                mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(mesh.Submeshes().back(), *primary_puppet_mesh, mapRate);
            }
        }
    }
    if (! puppet) {
        GenCardMesh(mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
        if (parse_geometry.final_mesh != nullptr) {
            effct_final_mesh.ChangeMeshDataFrom(*parse_geometry.final_mesh);
        } else {
            GenCardMesh(effct_final_mesh,
                        { geometry_size[0], geometry_size[1] },
                        { 1.0f, 1.0f },
                        Vector3f::Zero());
        }
    }
    if (hasEffect) {
        effct_final_mesh.SetGeometryTransform(
            effct_final_mesh.GeometryTransform() *
            Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
    }
    // material blendmode for last step to use
    auto finalMaterialState = material;
    std::shared_ptr<SceneImageEffectLayer> image_effect_layer;
    std::shared_ptr<SceneNodeArcHold>      effect_camera_anchor;
    if (color_blend_attachment_override.has_value()) {
        finalMaterialState.blenmode = *color_blend_attachment_override;
    }
    // disable img material blend, as it's the first effect node now
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }
    mesh.AddMaterial(std::move(material));
    track_image_property_material(mesh.MaterialSlots().back());
    RegisterShaderUserVarIndex(
        context, spImgNode.as_ptr(), mesh.MaterialSlots().back(), image_wpmat, shaderInfo);
    SolidColorNeutralizationSource solid_color_neutralization {};
    if (solid_color_media_slot) {
        solid_color_neutralization.node  = &spImgNode;
        solid_color_neutralization.slot  = 0;
        solid_color_neutralization.color = layer_color;
    }
    RegisterMaterialUserTextureIndex(context.scene.get(),
                                     mesh.MaterialSlots().back(),
                                     image_user_texture_fallback,
                                     solid_color_neutralization);

    // Later puppet meshes can carry their own materials (for example a
    // texture-channel animation overlay). Render them in the source pass so
    // effects consume the complete puppet image rather than only mesh 0.
    for (const auto* supplemental_mesh : supplemental_puppet_meshes) {
        if (supplemental_mesh->mat_json_files.empty()) continue;
        const auto& material_ref = supplemental_mesh->mat_json_files.front();
        auto supplemental_wpmat  = WPMdlParser::ParseMaterial(material_ref, vfs);
        if (! supplemental_wpmat) continue;

        supplemental_wpmat->combos[std::string(WE_CB_SCENE_ORTHO)] =
            wpimgobj.perspective ? 0 : 1;
        supplemental_wpmat->combos[std::string(OWE_CB_IMAGE_LAYER)] = 1;

        auto supplemental_user_texture_fallback = supplemental_wpmat->clone();
        ApplyUserTextureBindings(context, *supplemental_wpmat);

        SceneMaterial        supplemental_material;
        SceneUniformNodeData supplemental_sv_data;
        WPShaderInfo         supplemental_shader_info;
        supplemental_shader_info.baseConstSvs = baseConstSvs;
        if (! LoadMaterial(vfs,
                           *supplemental_wpmat,
                           context.scene.get(),
                           spImgNode.as_ptr(),
                           &supplemental_material,
                           &supplemental_sv_data,
                           &supplemental_shader_info)) {
            rstd_warn("load puppet material '{}' failed for '{}'", material_ref, wpimgobj.name);
            continue;
        }
        LoadConstvalue(
            context, supplemental_material, *supplemental_wpmat, supplemental_shader_info);
        svData.renderTargets.insert(svData.renderTargets.end(),
                                    supplemental_sv_data.renderTargets.begin(),
                                    supplemental_sv_data.renderTargets.end());

        const auto supplemental_uv_scale = Texture0UvScale(supplemental_material);
        const auto supplemental_slot = static_cast<uint32_t>(mesh.MaterialSlots().size());
        mesh.AddMaterial(std::move(supplemental_material));
        track_image_property_material(mesh.MaterialSlots().back());
        RegisterShaderUserVarIndex(context,
                                   spImgNode.as_ptr(),
                                   mesh.MaterialSlots().back(),
                                   *supplemental_wpmat,
                                   supplemental_shader_info);
        RegisterMaterialUserTextureIndex(context.scene.get(),
                                         mesh.MaterialSlots().back(),
                                         supplemental_user_texture_fallback);

        mesh.Submeshes().emplace_back();
        auto& supplemental_submesh = mesh.Submeshes().back();
        WPMdlParser::GenMeshFromMdl(
            supplemental_submesh, *supplemental_mesh, supplemental_uv_scale);
        supplemental_submesh.material_slot   = supplemental_slot;
        supplemental_submesh.preserve_output = true;
    }

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
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        // Rebuild main submeshes' draw_ranges: drop any part whose position
        // index is in `part_ids_a` of any mask block.
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
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
            if (&pmesh != primary_puppet_mesh) continue;
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
                WPMdlParser::AddPuppetMatInfo(mask_wpmat, *puppet);

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
                track_image_property_material(mesh.MaterialSlots().back());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b, mapRate);
                pre_sm.material_slot   = pre_slot;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                // (2) clipped-main submesh: main material + CLIPPINGTARGET
                wpscene::Material clip_wpmat        = image_wpmat.clone();
                clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                WPMdlParser::AddPuppetMatInfo(clip_wpmat, *puppet);

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
                LoadConstvalue(context, clip_scene_mat, clip_wpmat, clip_shaderInfo);
                uint32_t clip_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(clip_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a, mapRate);
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
        if (composite_render_path && ! wpimgobj.fullscreen) {
            auto anchor = rstd::sync::Arc<SceneNode>::make(
                alignment_offset,
                Vector3f::Ones(),
                Vector3f::Zero(),
                nodeAddr + "_effect_camera_anchor");
            anchor->SetParentAnchor(spImgNode.as_ptr());
            effect_camera_anchor = std::make_shared<SceneNodeArcHold>(anchor.clone());
            scene.transform_updaters.push_back(
                [effect_camera_anchor](double) { (void)effect_camera_anchor; });
        }
        // set camera to attatch effect
        if (isPassthrough) {
            scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(SceneCamera::MakeOrthographic(
                scene.activeCamera->Width(), scene.activeCamera->Height(), -1.0, 1.0));
            auto attached = scene.activeCamera->GetAttachedNode();
            if (attached.is_some()) scene.cameras.at(nodeAddr)->AttatchNode(attached.unwrap());
            if (scene.linkedCameras.count("global") == 0) scene.linkedCameras["global"] = {};
            scene.linkedCameras.at("global").push_back(nodeAddr);
        } else {
            // applly scale to crop
            const auto effect_extent =
                NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
            i32 w                   = effect_extent[0];
            i32 h                   = effect_extent[1];
            scene.cameras[nodeAddr] =
                std::make_shared<SceneCamera>(SceneCamera::MakeOrthographic(w, h, -1.0, 1.0));
            scene.cameras.at(nodeAddr)->AttatchNode(effect_camera_anchor
                                                        ? effect_camera_anchor->get()
                                                        : spImgNode.as_ptr());
        }
        if (composite_render_path) {
            const std::string group_camera = nodeAddr + "_group";
            const auto        group_extent =
                NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
            scene.cameras[group_camera] = std::make_shared<SceneCamera>(
                SceneCamera::MakeOrthographic(group_extent[0], group_extent[1], -1.0, 1.0));
            scene.cameras.at(group_camera)->AttatchNode(effect_camera_anchor
                                                            ? effect_camera_anchor->get()
                                                            : spImgNode.as_ptr());
            scene.RegisterRenderGroup(WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) },
                                      group_camera);
        }
        spImgNode->SetCamera(nodeAddr);
        std::string effect_ppong_a, effect_ppong_b;
        effect_ppong_a = SR_EFFECT_PPONG_PREFIX_A.data() + nodeAddr;
        effect_ppong_b = SR_EFFECT_PPONG_PREFIX_B.data() + nodeAddr;
        // set image effect
        const auto effect_extent =
            NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
        auto imgEffectLayer =
            std::make_shared<SceneImageEffectLayer>(spImgNode.as_ptr(),
                                                    static_cast<float>(effect_extent[0]),
                                                    static_cast<float>(effect_extent[1]),
                                                    effect_ppong_a,
                                                    effect_ppong_b);
        image_effect_layer = imgEffectLayer;
        {
            imgEffectLayer->SetRequiresSourceDraw(parse_geometry.requires_source_draw);
            imgEffectLayer->SetFullscreen(wpimgobj.fullscreen);
            imgEffectLayer->SetFinalMaterialState(finalMaterialState);
            imgEffectLayer->SetSkipWhenNoRuntimeEffect(wpimgobj.fullscreen || isPassthrough);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
        }
        // set renderTarget for ping-pong operate
        {
            scene.renderTargets[effect_ppong_a] = {
                .width                = effect_extent[0],
                .height               = effect_extent[1],
                .allowReuse           = true,
                .force_clear          = ! wpimgobj.fullscreen,
                .clear_on_first_write = true,
                .preserve_on_write    = composite_render_path,
            };
            if (wpimgobj.fullscreen) {
                scene.renderTargets[effect_ppong_a].bind = { .enable = true, .screen = true };
            } else if (composite_render_path) {
                scene.renderTargets[effect_ppong_a].bind = {
                    .enable = true,
                    .name   = nodeAddr + "_group",
                    .screen = true,
                };
            }
            // Point-art images (noInterpolation) must stay point-sampled through
            // the whole effect chain.
            if (point_source) {
                auto& s     = scene.renderTargets[effect_ppong_a].sample;
                s.magFilter = s.minFilter = TextureFilter::NEAREST;
            }
            scene.renderTargets[effect_ppong_b] = scene.renderTargets.at(effect_ppong_a);
        }

        int32_t    i_eff = -1;
        bool       last_effect_can_composite_final { false };
        const bool allow_transparent_previous_final = ! solid_scene_context;
        const bool passthrough_can_composite_final =
            isPassthrough || ! parse_geometry.requires_source_draw;
        for (const auto& wpeffobj : wpimgobj.effects) {
            i_eff++;
            if (! wpeffobj.visible && wpeffobj.visible_user.empty() &&
                ! wpeffobj.field_bindings.scripts.contains("visible") &&
                ! context.scene_accesses_effects) {
                i_eff--;
                continue;
            }
            std::shared_ptr<SceneImageEffect> imgEffect = std::make_shared<SceneImageEffect>();
            imgEffect->name                             = wpeffobj.name;
            imgEffect->runtime_visible                  = wpeffobj.visible;
            if (! wpeffobj.visible_user.empty()) {
                imgEffect->visible_user_binding =
                    ToSceneUserVisibilityBinding(wpeffobj.visible_user);
            }

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
                        scene.renderTargets[rtname] = {
                            .width                = 2,
                            .height               = 2,
                            .allowReuse           = ! wpfbo.unique,
                            .clear_on_first_write = ! wpfbo.unique,
                        };
                        ApplyEffectRenderTargetFormat(
                            scene.renderTargets[rtname], wpfbo.format, scene.hdr_render_targets);
                        scene.renderTargets[rtname].bind = {
                            .enable = true,
                            .screen = true,
                            .scale  = 1.0 / wpfbo.scale,
                        };
                    } else {
                        auto fbo_size = [&]() -> std::array<uint16_t, 2> {
                            if (wpfbo.fit > 0) {
                                const float max_size =
                                    std::max(effect_target_size[0], effect_target_size[1]);
                                if (max_size > 0.0f) {
                                    const float fit_scale =
                                        static_cast<float>(wpfbo.fit) / max_size;
                                    const auto fit_extent = NonZeroRenderTargetExtent(
                                        std::round(effect_target_size[0] * fit_scale),
                                        std::round(effect_target_size[1] * fit_scale));
                                    return { static_cast<uint16_t>(fit_extent[0]),
                                             static_cast<uint16_t>(fit_extent[1]) };
                                }
                            }
                            const auto scaled_extent = NonZeroRenderTargetExtent(
                                effect_target_size[0] / static_cast<float>(wpfbo.scale),
                                effect_target_size[1] / static_cast<float>(wpfbo.scale));
                            return { static_cast<uint16_t>(scaled_extent[0]),
                                     static_cast<uint16_t>(scaled_extent[1]) };
                        }();
                        scene.renderTargets[rtname] = {
                            .width                = fbo_size[0],
                            .height               = fbo_size[1],
                            .allowReuse           = ! wpfbo.unique,
                            .clear_on_first_write = ! wpfbo.unique,
                        };
                        ApplyEffectRenderTargetFormat(
                            scene.renderTargets[rtname], wpfbo.format, scene.hdr_render_targets);
                        if (composite_render_path && wpfbo.fit == 0) {
                            scene.renderTargets[rtname].bind = {
                                .enable = true,
                                .name   = effect_ppong_a,
                                .scale  = 1.0 / static_cast<double>(std::max(1u, wpfbo.scale)),
                            };
                        }
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
                wpscene::Material                wpmat = wpeffobj.materials.at(i_mat).clone();
                std::string                      matOutRT { SR_EFFECT_PPONG_PREFIX_B };
                std::optional<wpscene::Material> user_texture_fallback;
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    ApplyTextureBinds(wpmat, std::span(wppass.bind), fboMap);
                    user_texture_fallback = wpmat.clone();
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
                wpEffShaderInfo.baseConstSvs[std::string(G_ETVP)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs[std::string(G_ETVPI)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                SceneMaterial        material;
                SceneUniformNodeData svData;
                svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
                sr::Map<std::string, SceneShaderValueAnimation> final_quad_shader_values;
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
                LoadConstvalue(
                    context, material, wpmat, wpEffShaderInfo, &final_quad_shader_values);
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.propagatedParallaxDepth = { wpimgobj.parallaxDepth[0],
                                                       wpimgobj.parallaxDepth[1] };
                    svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
                    svData.effect_projection_node = spImgNode.as_ptr();
                    svData.effect_projection_size = { static_cast<float>(effect_extent[0]),
                                                      static_cast<float>(effect_extent[1]) };
                    if (puppet && wpmat.use_puppet) {
                        svData.puppet_layer = image_puppet_layer;
                        RegisterPuppetLayer(context, spEffNode.as_ptr(), svData.puppet_layer);
                    }
                }
                spMesh->AddMaterial(std::move(material));
                track_image_property_material(spMesh->MaterialSlots().back());
                RegisterShaderUserVarIndex(
                    context, spImgNode.as_ptr(), spMesh->MaterialSlots().back(), wpmat,
                    wpEffShaderInfo);
                if (user_texture_fallback.has_value()) {
                    RegisterMaterialUserTextureIndex(
                        context.scene.get(), spMesh->MaterialSlots().back(), *user_texture_fallback);
                }
                auto add_puppet_mask_materials = [&]() -> bool {
                    if (! (puppet && wpmat.use_puppet && puppet_has_masks)) return true;
                    const std::string source_tex =
                        wpmat.textures.empty() ? std::string {} : wpmat.textures[0];
                    for (const auto& pmesh : puppet->meshes) {
                        if (&pmesh != primary_puppet_mesh) continue;
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
                            WPMdlParser::AddPuppetMatInfo(mask_wpmat, *puppet);

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
                            LoadConstvalue(context, mask_material, mask_wpmat, mask_shaderInfo);
                            spMesh->AddMaterial(std::move(mask_material));
                            track_image_property_material(spMesh->MaterialSlots().back());

                            wpscene::Material clip_wpmat        = wpmat.clone();
                            clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                            clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                            if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                            clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                            WPMdlParser::AddPuppetMatInfo(clip_wpmat, *puppet);

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
                            LoadConstvalue(context, clip_material, clip_wpmat, clip_shaderInfo);
                            spMesh->AddMaterial(std::move(clip_material));
                            track_image_property_material(spMesh->MaterialSlots().back());
                        }
                    }
                    return true;
                };
                if (! add_puppet_mask_materials()) {
                    eff_mat_ok = false;
                    break;
                }
                if (auto* mat = spMesh->Material(); mat != nullptr) {
                    last_effect_can_composite_final = CanCompositeFinalEffectMaterial(
                        mat->name, wpEffShaderInfo, allow_transparent_previous_final);
                }
                spEffNode->AddMesh(spMesh);

                context.shader_updater->SetNodeData(spEffNode.as_ptr(), svData);
                imgEffect->nodes.push_back(SceneImageEffectNode {
                    .output                   = matOutRT,
                    .sceneNode                = spEffNode.clone(),
                    .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                    .final_quad_shader_values = std::move(final_quad_shader_values),
                });
            }

            if (eff_mat_ok) {
                imgEffectLayer->AddEffect(imgEffect);
                RegisterImageEffectVisibilityScript(
                    context, spImgNode.as_ptr(), imgEffectLayer, imgEffect,
                    wpeffobj.field_bindings);
            } else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        if (! wpimgobj.fullscreen && ! wpimgobj.copybackground &&
            ! passthrough_can_composite_final &&
            ! last_effect_can_composite_final) {
            wpscene::Material passthrough_mat;
            auto              parsed = sr::ParseJson(
                fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"));
            if (parsed.is_err()) {
                rstd_error("parse effectpassthrough.json failed for '{}': {}",
                           wpimgobj.name,
                           parsed.unwrap_err());
            } else {
                auto json = parsed.unwrap();
                if (! passthrough_mat.FromJson(json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                } else {
                    if (passthrough_mat.textures.empty())
                        passthrough_mat.textures.push_back(effect_ppong_a);
                    else
                        passthrough_mat.textures[0] = effect_ppong_a;

                    auto finalEffect = std::make_shared<SceneImageEffect>();
                    auto spFinalNode = rstd::sync::Arc<SceneNode>::make();

                    WPShaderInfo wpFinalShaderInfo;
                    wpFinalShaderInfo.baseConstSvs = NeutralColorUniforms(baseConstSvs);
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
                        LoadConstvalue(
                            context, finalMaterial, passthrough_mat, wpFinalShaderInfo);
                        auto spFinalMesh = std::make_shared<SceneMesh>();
                        spFinalMesh->AddMaterial(std::move(finalMaterial));
                        RegisterShaderUserVarIndex(context,
                                                   spFinalNode.as_ptr(),
                                                   spFinalMesh->MaterialSlots().back(),
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
    }
    const Matrix4d source_alignment_base_transform = spImgNode->GeometryTransform();
    const Matrix4d final_alignment_base_transform =
        image_effect_layer ? image_effect_layer->FinalMesh().GeometryTransform()
                           : spImgNode->GeometryTransform();
    RegisterImageAlignmentBinding(
        context,
        spImgNode.as_ptr(),
        wpimgobj.alignment,
        [image_effect_layer,
         effect_camera_anchor,
         source_alignment_base_transform,
         final_alignment_base_transform,
         alignment_offset,
         geometry_size](SceneNode* node, std::string_view alignment) {
            const Vector3f offset =
                AlignmentOffset(alignment, { geometry_size[0], geometry_size[1] });
            const Vector3f delta = offset - alignment_offset;
            const Matrix4d translation =
                Affine3d(Translation3d(delta.cast<double>())).matrix();
            if (node) node->SetHitCenter({ offset.x(), offset.y() });
            if (image_effect_layer) {
                if (node && effect_camera_anchor) {
                    node->SetGeometryTransform(source_alignment_base_transform * translation);
                }
                image_effect_layer->FinalMesh().SetGeometryTransform(
                    final_alignment_base_transform * translation);
                if (effect_camera_anchor) effect_camera_anchor->get()->SetTranslate(offset);
            } else if (node) {
                node->SetGeometryTransform(final_alignment_base_transform * translation);
            }
        });

    AssignNodeFieldAnimations(*spImgNode.as_ptr(), wpimgobj.field_bindings);
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (! wpimgobj.color_user_key.empty()) {
        context.scene->image_color_user_index[wpimgobj.color_user_key].push_back(
            { spImgNode.clone(), image_property_materials });
    }
    if (! wpimgobj.alpha_user_key.empty()) {
        context.scene->image_alpha_user_index[wpimgobj.alpha_user_key].push_back(
            { spImgNode.clone(), image_property_materials });
    }
    if (! wpimgobj.scale_user_key.empty()) {
        context.scene->node_scale_user_index[wpimgobj.scale_user_key].push_back({
            spImgNode.clone(),
            Vector3f(wpimgobj.scale.data()),
        });
    }
    context.node_id_map[wpimgobj.id] = {
        wpimgobj.parent,
        rstd::Some(spImgNode.clone()),
        (puppet && puppet->puppet) ? puppet->puppet : nullptr,
        wpimgobj.attachment,
        image_puppet_layer,
    };
}

// Upper bound for a single emitter's particle count. Scene JSON comes from
// untrusted packages and these counts end up in reserve()/vertex buffer
// sizes, so every count (parent and child) is clamped to this on ingest.
constexpr u32 kMaxParticleCount = 20000u;
// Aggregate capacity of one particle mesh: per-emitter count times the
// cumulative child instance multiplier (times trail segments for ropes).
// Both factors are individually bounded by kMaxParticleCount, but their
// product still isn't, so cap it here as well. The geometry generators are
// bounds-checked against the vertex array (WallpaperParticleGeometry.cpp),
// so this only truncates absurd scenes instead of allocating gigabytes.
constexpr u32 kMaxParticleMeshCount = 1000000u;

// JSON counts are signed and unvalidated; negatives must not wrap to ~4e9.
u32 ClampParticleCount(i32 count) {
    if (count <= 0) return 0u;
    return std::min(static_cast<u32>(count), kMaxParticleCount);
}

// Saturating product: overflow, or any result past `bound`, yields `bound`.
u32 MulParticleCountClamped(u32 lhs, u32 rhs, u32 bound) {
    u32 product { 0 };
    if (__builtin_mul_overflow(lhs, rhs, &product)) return bound;
    return std::min(product, bound);
}

struct ParticleChildPtr {
    wpscene::ParticleChild* child { nullptr };
    SceneNode*              node_parent { nullptr };
    ParticleSubSystem*      particle_parent { nullptr };
    std::shared_ptr<ParticlePlaybackState> playback;
    std::shared_ptr<wpscene::ParticleInstanceoverride> instance_override;

    // Effective world scale at node_parent. Particle child origins are
    // pre-divided by this so the shader's MVP scale recovers the authored
    // parent-relative world-pixel offset.
    Eigen::Vector3f world_scale { 1.f, 1.f, 1.f };
};

std::vector<float> ReadParticleOverride(const wpscene::ParticleInstanceoverride& state,
                                        std::string_view field) {
    auto scalar = [](float value) { return std::vector<float> { value }; };
    if (field == "alpha") return scalar(state.alpha);
    if (field == "size") return scalar(state.size);
    if (field == "lifetime") return scalar(state.lifetime);
    if (field == "rate") return scalar(state.rate);
    if (field == "speed") return scalar(state.speed);
    if (field == "count") return scalar(state.count);
    if (field == "brightness") return scalar(state.brightness);
    if (field == "color")
        return { state.color[0] / 255.0f, state.color[1] / 255.0f, state.color[2] / 255.0f };
    if (field == "colorn") return { state.colorn[0], state.colorn[1], state.colorn[2] };
    return {};
}

void ApplyParticleOverride(wpscene::ParticleInstanceoverride& state, std::string_view field,
                           std::span<const float> values) {
    auto scalar = [&](float& destination) {
        if (! values.empty()) destination = values[0];
    };
    auto vec3 = [&](std::array<float, 3>& destination, float scale) {
        if (values.size() < 3) return false;
        destination = { values[0] * scale, values[1] * scale, values[2] * scale };
        return true;
    };
    if (field == "alpha") scalar(state.alpha);
    else if (field == "size") scalar(state.size);
    else if (field == "lifetime") scalar(state.lifetime);
    else if (field == "rate") scalar(state.rate);
    else if (field == "speed") scalar(state.speed);
    else if (field == "count") scalar(state.count);
    else if (field == "brightness") scalar(state.brightness);
    else if (field == "color") {
        if (vec3(state.color, 255.0f)) state.overColor = true;
    } else if (field == "colorn") {
        if (vec3(state.colorn, 1.0f)) state.overColorn = true;
    }
}

void ParseShapeObj(ParseContext& context, wpscene::ShapeObject& shape_obj) {
    if (shape_obj.shape != "quad") {
        rstd_error("unsupported shape '{}' for '{}'", shape_obj.shape, shape_obj.name);
        return;
    }

    const wpscene::ImageEffect* first_effect { nullptr };
    const wpscene::ImageEffect* last_effect { nullptr };
    for (const auto& effect : shape_obj.effects) {
        if (! effect.visible && effect.visible_user.empty()) continue;
        if (first_effect == nullptr) first_effect = &effect;
        last_effect = &effect;
    }
    if (first_effect == nullptr || first_effect->materials.empty() ||
        first_effect->passes.empty() || last_effect == nullptr || last_effect->materials.empty() ||
        last_effect->passes.empty()) {
        rstd_error("shape '{}' has no renderable effect", shape_obj.name);
        return;
    }

    auto direct_draw_material = first_effect->materials.front().clone();
    direct_draw_material.MergePass(first_effect->passes.front());
    auto direct_draw = direct_draw_material.combos.find("DIRECTDRAW");
    if (direct_draw == direct_draw_material.combos.end() || direct_draw->second == 0) {
        rstd_error("shape '{}' first effect is not direct draw", shape_obj.name);
        return;
    }
    auto points = ReadDirectDrawQuad(direct_draw_material);
    if (! points) {
        rstd_error("shape '{}' has invalid direct draw points", shape_obj.name);
        return;
    }

    const auto edge = static_cast<float>(context.ortho_h);
    SceneMesh  direct_draw_mesh;
    GenDirectDrawQuadMesh(direct_draw_mesh, edge, *points);

    wpscene::ImageObject image;
    image.id       = shape_obj.id;
    image.name     = std::move(shape_obj.name);
    image.origin   = shape_obj.origin;
    image.scale    = shape_obj.scale;
    image.angles   = shape_obj.angles;
    image.size     = { edge, edge };
    image.visible  = shape_obj.visible;
    image.material = last_effect->materials.back().clone();
    image.material.MergePass(last_effect->passes.back());
    image.material.blending  = "additive";
    image.effects            = std::move(shape_obj.effects);
    image.nopadding          = true;
    image.locktransforms     = shape_obj.locktransforms;
    image.muteineditor       = shape_obj.muteineditor;
    image.nointerpolation    = shape_obj.nointerpolation;
    image.reflected          = shape_obj.reflected;
    image.castshadow         = shape_obj.castshadow;
    image.disablepropagation = shape_obj.disablepropagation;
    image.parent             = shape_obj.parent;
    image.attachment         = std::move(shape_obj.attachment);
    image.dependencies       = std::move(shape_obj.dependencies);
    image.field_bindings     = std::move(shape_obj.field_bindings);
    image.visible_user       = std::move(shape_obj.visible_user);
    image.visible_user_key   = std::move(shape_obj.visible_user_key);
    ParseImageObj(context,
                  image,
                  ImageParseGeometry {
                      .requires_source_draw = false,
                      .final_mesh           = &direct_draw_mesh,
                  });
}

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
        std::optional<i32> controlpointstartindex;
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
        // ParticleChild::maxcount is raw JSON. It is handed to
        // ParticleSubSystem as the u32 instance cap (which reserve()s it) and
        // it multiplies cumulatively down the child chain, so clamp it on
        // ingest with the same bound the parent's maxcount uses.
        child_data.maxcount = static_cast<i32>(ClampParticleCount(child_data.maxcount));

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNodeOpt = rstd::Some(rstd::sync::Arc<SceneNode>::make(Vector3f(wppartobj.origin.data()),
                                                                Vector3f(wppartobj.scale.data()),
                                                                Vector3f(wppartobj.angles.data()),
                                                                wppartobj.name));
        auto& spNode = *spNodeOpt;
        spNode->ID() = wppartobj.id;
        if (! wppartobj.visible) {
            spNode->SetVisible(false);
            context.scene->MarkLayerVisibilityElidable(
                WallpaperLayerId { .value = static_cast<i32>(wppartobj.id) });
        }
        if (! wppartobj.visible_user.empty())
            spNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wppartobj.visible_user));
    }
    auto& spNode = *spNodeOpt;
    spNode->SetReflected(wppartobj.reflected);

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    auto override_state = is_child && child_ptr.instance_override
                              ? child_ptr.instance_override
                              : std::make_shared<wpscene::ParticleInstanceoverride>(
                                    wppartobj.instanceoverride);
    auto playback_state = is_child && child_ptr.playback
                              ? child_ptr.playback
                              : std::make_shared<ParticlePlaybackState>();

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;
    ParticleInstanceModifiers modifiers(override_state, particle_obj.flags, ! is_child);

    auto wppartRenderer    = particle_obj.renderers.at(0);
    auto render_desc       = DescribeParticleRender(wppartRenderer);
    bool render_rope       = render_desc.rope;
    bool render_rope_trail = render_desc.rope_trail;
    bool rope_shader       = render_rope || render_rope_trail;
    bool hastrail          = render_desc.trail;

    if (rope_shader) particle_obj.material.shader = "genericropeparticle";

    // Only the stock genericparticle shader receives the instanced layout;
    // other/custom materials retain the original CPU-expanded layout.
    const bool use_instanced_particles = ! rope_shader &&
                                         particle_obj.material.shader == "genericparticle";

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
    svData.vertices_in_world_space =
        particle_obj.flags[wpscene::Particle::FlagEnum::wordspace];

    WPShaderInfo shaderInfo;
    if (use_instanced_particles) shaderInfo.combos["PARTICLEINSTANCED"] = "1";
    shaderInfo.baseConstSvs                                    = context.global_base_uniforms;
    shaderInfo.baseConstSvs[std::string(G_ORIENTATIONUP)]      = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[std::string(G_ORIENTATIONRIGHT)]   = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[std::string(G_ORIENTATIONFORWARD)] = std::array { 0.0f, 0.0f, 1.0f };
    shaderInfo.baseConstSvs[std::string(G_VIEWUP)]             = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[std::string(G_VIEWRIGHT)]          = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[std::string(G_EYEPOSITION)]        = std::array {
        static_cast<float>(context.ortho_w) / 2.0f,
        static_cast<float>(context.ortho_h) / 2.0f,
        1000.0f,
    };

    u32 maxcount = particle_obj.maxcount;
    maxcount     = std::min(maxcount, kMaxParticleCount);

    if (hastrail) {
        double in_SegmentUVTimeOffset                      = 0.0;
        double in_SegmentMaxCount                          = maxcount - 1.0;
        shaderInfo.baseConstSvs[std::string(G_RENDERVAR0)] = std::array {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.combos[std::string(WE_CB_TRAILRENDERER)] = "1";
        if (! render_rope_trail)
            shaderInfo.combos[std::string(WE_CB_THICK_FORMAT)] = "1";
    }
    u32 rope_subdivision = 0;
    if (rope_shader && std::isfinite(wppartRenderer.subdivision) &&
        wppartRenderer.subdivision > 0.0f) {
        const double rounded = std::round(static_cast<double>(wppartRenderer.subdivision));
        rope_subdivision = static_cast<u32>(
            std::min(rounded, static_cast<double>(kMaxParticleMeshCount - 1u)));
    }
    if (rope_shader)
        shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(rope_subdivision);

    auto animationmode = ToAnimMode(particle_obj.animationmode);
    if (animationmode == ParticleAnimationMode::SEQUENCE &&
        ! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
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
    LoadConstvalue(context, material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool thick_format = material.hasSprite || (hastrail && ! render_rope_trail);
    u32 trail_length = 0;
    if (render_rope_trail) {
        i32 seg = wppartRenderer.segments;
        if (seg < 1) seg = 1;
        if (seg > 256) seg = 256;
        trail_length = (u32)seg;
    }
    ParticleFollowAnchor follow_anchor;
    if (hastrail && ! render_rope_trail) {
        follow_anchor.trail_renderer = true;
        follow_anchor.length         = wppartRenderer.length;
        follow_anchor.max_length     = wppartRenderer.maxlength;
        follow_anchor.texture_ratio  = ParticleTextureRatio(material);
    }
    auto spawn_type = ParseSpawnType(child_data.type);
    if (is_child && spawn_type == ParticleSubSystem::SpawnType::STATIC &&
        child_data.controlpointstartindex.has_value())
        spawn_type = ParticleSubSystem::SpawnType::STATIC_CONTROLPOINT;
    {
        // Rope mesh capacity = maxcount * (trail_length-1) since each live
        // particle produces (trail_length-1) GS-input segments. Non-rope path
        // is unchanged: per-particle quad fan-out.
        u32 mesh_maxcount = MulParticleCountClamped(
            maxcount,
            ParticleSubSystem::EffectiveInstanceCapacity(
                static_cast<u32>(std::max(child_data.maxcount, 0)), spawn_type),
            kMaxParticleMeshCount);
        if (rope_shader) {
            if (render_rope_trail)
                mesh_maxcount = MulParticleCountClamped(
                    mesh_maxcount, trail_length, kMaxParticleMeshCount);
            if (! use_geometry_shader)
                mesh_maxcount = MulParticleCountClamped(
                    mesh_maxcount, rope_subdivision + 1u, kMaxParticleMeshCount);
            SetRopeParticleMesh(mesh,
                                particle_obj,
                                mesh_maxcount,
                                thick_format,
                                render_rope_trail,
                                use_geometry_shader);
        } else {
            SetParticleMesh(mesh,
                            particle_obj,
                            mesh_maxcount,
                            thick_format,
                            use_geometry_shader,
                            use_instanced_particles);
        }
    }

    auto particleSub = std::make_unique<ParticleSubSystem>(
        *context.scene->paritileSys,
        spMesh,
        maxcount,
        modifiers.Rate(),
        child_data.maxcount,
        child_data.probability,
        spawn_type,
        [=](const Particle& p, const ParticleRawGenSpec& spec) {
            auto& lifetime = *(spec.lifetime);
            if (lifetime <= 0.0f) {
                lifetime = 0.0f;
                return;
            }
            switch (animationmode) {
            case ParticleAnimationMode::RANDOMONE:
                lifetime = std::clamp(p.random, 0.0f, std::nextafter(1.0f, 0.0f));
                break;
            case ParticleAnimationMode::SEQUENCE:
                lifetime = (1.0f - (p.lifetime / p.init.lifetime)) * sequencemultiplier;
                break;
            }
        },
        follow_anchor,
        trail_length,
        render_rope_trail ? static_cast<double>(wppartRenderer.length) : 0.0,
        static_cast<double>(particle_obj.starttime),
        particle_obj.flags[wpscene::Particle::FlagEnum::wordspace]);

    particleSub->SetOwnerNode(spNode.as_ptr());
    particleSub->SetPlaybackState(playback_state);
    particleSub->SetRopeSubdivision(rope_subdivision);
    for (const auto& emitter : particle_obj.emitters) {
        if (emitter.audioprocessingmode != 0) {
            context.scene->uses_audio_spectrum = true;
            break;
        }
    }
    if (child_data.controlpointstartindex.has_value())
        particleSub->SetParentControlpointStartIndex(*child_data.controlpointstartindex);
    LoadEmitter(*particleSub, particle_obj, modifiers);
    LoadInitializer(*particleSub, particle_obj, modifiers.Clone());
    LoadOperator(*particleSub, particle_obj, modifiers.Clone());
    LoadControlPoint(*particleSub, particle_obj, modifiers.Clone());
    particleSub->SetRateSource([modifiers]() { return modifiers.Rate(); });

    // Register every {user:"<key>", value:...} binding on instanceoverride
    // so RenderSetUserProperty can mutate the shared state at runtime.
    if (! is_child) {
        for (const auto& [field, key] : override_state->bindings) {
            context.scene->particle_user_var_index[key].push_back({ override_state, field });
        }
    }

    mesh.AddMaterial(std::move(material));
    RegisterShaderUserVarIndex(
        context, spNode.as_ptr(), mesh.MaterialSlots().back(), particle_obj.material, shaderInfo);
    spNode->AddMesh(spMesh);
    context.shader_updater->SetNodeData(spNode.as_ptr(), svData);

    for (auto& child : particle_obj.children) {
        ParseParticleObj(context,
                         wppartobj,
                         {
                             .child             = &child,
                             .node_parent       = spNode.as_ptr(),
                             .particle_parent = particleSub.get(),
                             .playback        = playback_state,
                             .instance_override = override_state,
                             .world_scale     = node_world_scale,
                         });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(std::move(particleSub));
    else
        context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));

    if (! is_child) {
        spNode->SetLayerPropertyControl(
            [override_state](std::string_view field) {
                return ReadParticleOverride(*override_state, field);
            },
            [override_state](std::string_view field, std::span<const float> values) {
                ApplyParticleOverride(*override_state, field, values);
            });
        spNode->SetPlaybackControl(
            [playback_state]() {
                playback_state->playing.store(true, std::memory_order_release);
                playback_state->reset_sequence.fetch_add(1, std::memory_order_acq_rel);
            },
            [playback_state]() {
                playback_state->playing.store(false, std::memory_order_release);
                playback_state->reset_sequence.fetch_add(1, std::memory_order_acq_rel);
            },
            [playback_state]() {
                playback_state->playing.store(false, std::memory_order_release);
            },
            [playback_state]() {
                return playback_state->playing.load(std::memory_order_acquire);
            });
        AssignNodeFieldAnimations(*spNode.as_ptr(), wppartobj.field_bindings);
    }
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
    if (! obj.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    auto control = WPSoundParser::Parse(obj, *context.vfs, sm, context.scene.get());
    node->SetSoundControl(control);
    if (control && ! obj.volume_user_key.empty())
        context.scene->sound_volume_user_index[obj.volume_user_key].push_back(control);

    AssignNodeFieldAnimations(*node.as_ptr(), obj.field_bindings);
    WireFieldScripts(context, node, obj.field_bindings);
    context.node_id_map[obj.id] = { obj.parent, rstd::Some(node.clone()) };
}

void ParseLightObj(ParseContext& context, wpscene::LightObject& light_obj) {
    auto node = rstd::sync::Arc<SceneNode>::make(Vector3f(light_obj.origin.data()),
                                                 Vector3f(light_obj.scale.data()),
                                                 Vector3f(light_obj.angles.data()),
                                                 light_obj.name);

    SceneLight::Desc desc;
    if (light_obj.light == "spot" || light_obj.light == "lspot") {
        desc.type = SceneLightType::Spot;
    } else if (light_obj.light == "directional" || light_obj.light == "ldirectional") {
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
    // WE evaluates the authored cone angles directly.
    const float kDegToRad     = rstd::f32_::consts::PI / 180.0f;
    desc.inner_cone_cos       = std::cos(light_obj.innercone * kDegToRad);
    desc.outer_cone_cos       = std::cos(light_obj.outercone * kDegToRad);
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
    node->SetBaseColor(desc.color, 1.0f);
    if (! light_obj.visible_user.empty()) {
        light.setVisibleUserBinding(ToSceneUserVisibilityBinding(light_obj.visible_user));
    }

    AssignNodeFieldAnimations(*node.as_ptr(), light_obj.field_bindings);
    WireFieldScripts(context, node, light_obj.field_bindings);
    context.node_id_map[light_obj.id] = { light_obj.parent, rstd::Some(node.clone()) };
}

void ParseModelObj(ParseContext& context, wpscene::ModelObject& model_obj) {
    auto& vfs = *context.vfs;

    WPMdl mdl;
    if (! WPMdlParser::Parse(model_obj.model, vfs, mdl)) {
        rstd_error("parse model failed: {}", model_obj.model);
        return;
    }

    auto node  = rstd::sync::Arc<SceneNode>::make(Vector3f(model_obj.origin.data()),
                                                  Vector3f(model_obj.scale.data()),
                                                  Vector3f(model_obj.angles.data()),
                                                  model_obj.name);
    node->ID() = model_obj.id;
    node->SetPerspective(model_obj.perspective);
    node->SetReflected(model_obj.reflected);
    if (! model_obj.visible) {
        node->SetVisible(false);
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(model_obj.id) });
    }
    MarkHiddenLinkSource(context, model_obj.id);
    if (! model_obj.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(model_obj.visible_user));

    auto mesh = std::make_shared<SceneMesh>();

    SceneUniformNodeData svData;
    svData.parallaxDepth           = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.propagatedParallaxDepth = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.use_camera_eye_position = true;
    if (context.orthographic_scene) {
        svData.eye_position_override = std::array<float, 3> {
            static_cast<float>(context.ortho_w) * 0.5f,
            static_cast<float>(context.ortho_h) * 0.5f,
            2000.0f,
        };
    }
    if (mdl.puppet && ! mdl.puppet->bones.empty()) {
        svData.puppet_layer = MakePuppetLayer(
            mdl.puppet, std::span<WPPuppetLayer::AnimationLayer>(model_obj.puppet_layers));
        RegisterPuppetLayer(context, node.as_ptr(), svData.puppet_layer);
    }

    for (const auto& mdl_mesh : mdl.meshes) {
        if (mdl_mesh.positions.empty()) continue;

        if (mdl_mesh.mat_json_files.empty()) continue;
        std::size_t skin_index = model_obj.skin;
        if (skin_index >= mdl_mesh.mat_json_files.size()) {
            rstd_error("model '{}' skin {} exceeds {} material variants; using skin 0",
                       model_obj.name,
                       model_obj.skin,
                       mdl_mesh.mat_json_files.size());
            skin_index = 0;
        }
        const auto& material_ref = mdl_mesh.mat_json_files[skin_index];

        auto wpmat = WPMdlParser::ParseMaterial(material_ref, vfs);
        if (! wpmat) continue;
        if (mdl.puppet && ! mdl.puppet->bones.empty()) WPMdlParser::AddPuppetMatInfo(*wpmat, mdl);

        SceneMaterial scene_mat;
        WPShaderInfo  shader_info;
        shader_info.baseConstSvs            = context.global_base_uniforms;
        shader_info.normalize_tangent_space = true;
        if (mdl.puppet && ! mdl.puppet->bones.empty()) {
            WPMdlParser::AddPuppetShaderInfo(shader_info, mdl);
        }

        if (! LoadMaterial(vfs,
                           *wpmat,
                           context.scene.get(),
                           node.as_ptr(),
                           &scene_mat,
                           &svData,
                           &shader_info)) {
            rstd_error(
                "load model material '{}' failed for '{}'", material_ref, model_obj.name);
            continue;
        }
        LoadConstvalue(context, scene_mat, *wpmat, shader_info);

        const uint32_t material_slot  = static_cast<uint32_t>(mesh->MaterialSlots().size());
        const auto     texcoord_scale = Texture0UvScale(scene_mat);
        mesh->AddMaterial(std::move(scene_mat));
        RegisterShaderUserVarIndex(
            context, node.as_ptr(), mesh->MaterialSlots().back(), *wpmat, shader_info);

        mesh->Submeshes().emplace_back();
        auto& submesh = mesh->Submeshes().back();
        WPMdlParser::GenMeshFromMdl(submesh, mdl_mesh, texcoord_scale);
        submesh.material_slot = material_slot;
    }

    if (mesh->Submeshes().empty()) {
        rstd_error("model '{}' has no renderable mesh", model_obj.model);
        return;
    }

    node->AddMesh(mesh);
    context.shader_updater->SetNodeData(node.as_ptr(), svData);
    AssignNodeFieldAnimations(*node.as_ptr(), model_obj.field_bindings);
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
    bool Contains(const std::string& name) const override {
        return m_synth.contains(name) || (m_inner && m_inner->Contains(name));
    }
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

bool EnsureTextAtlas(Scene& scene, text::FontFace& face) {
    const std::string& atlas_url = face.AtlasUrl();
    if (scene.textures.contains(atlas_url)) return true;
    auto atlas_img = text::BuildAtlasImage(face, atlas_url);
    if (! atlas_img) return false;
    EnsureTextImageParser(scene).Register(atlas_url, atlas_img);
    SceneTexture stex;
    stex.url                  = atlas_url;
    stex.sample               = atlas_img->header.sample;
    scene.textures[atlas_url] = stex;
    face.ClearDirtyRects();
    return true;
}

auto UserPropertyValue(rstd::Option<rstd::ref<rstd::json::Map>> user_props, std::string_view key)
    -> rstd::Option<rstd::ref<Json>> {
    if (key.empty()) return rstd::None();
    auto        props   = rstd_try(user_props);
    auto        value   = rstd_try(props->get(rstd::cppstd::as_str(key)));
    const auto& payload = SceneUserPropertyPayload(*value);
    return rstd::Some(rstd::ref<Json>::from_raw_parts(rstd::addressof(payload)));
}

void ParseTextObj(ParseContext& context, wpscene::TextObject& obj) {
    if (! obj.visible) {
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(obj.id) });
    }
    MarkHiddenLinkSource(context, obj.id);

    // --- determine initial text + whether a runtime binding will rewrite it
    auto text_binding_it      = obj.field_bindings.scripts.find("text");
    bool has_text_script      = (text_binding_it != obj.field_bindings.scripts.end());
    auto pointsize_binding_it = obj.field_bindings.scripts.find("pointsize");
    bool has_pointsize_script = (pointsize_binding_it != obj.field_bindings.scripts.end());
    const bool has_alpha_animation = obj.field_bindings.animations.count("alpha") != 0;
    // Scripts can also drive `text` indirectly: a script attached to any
    // other field (commonly `visible`) writes `thisLayer.text = "..."` from
    // its update() side-effect (e.g. workshop 2283810443's clock). Transform
    // scripts alone should not force large dynamic text RTs.
    bool has_indirect_text_script = false;
    if (! has_text_script) {
        for (const auto& [_, sb] : obj.field_bindings.scripts) {
            if (sb.source.find(".text") != std::string::npos ||
                sb.source.find("[\"text\"]") != std::string::npos ||
                sb.source.find("['text']") != std::string::npos) {
                has_indirect_text_script = true;
                break;
            }
        }
    }
    const bool has_text_user = ! obj.text_user.empty();
    bool wants_dynamic_text = has_text_script || has_indirect_text_script || has_pointsize_script ||
                              context.scene_layer_text_writes ||
                              ! obj.text_user_key.empty() || ! obj.pointsize_user_key.empty();
    bool has_text_effect    = false;
    for (const auto& effect : obj.effects) {
        if (effect.visible || ! effect.visible_user.empty() ||
            effect.field_bindings.scripts.contains("visible")) {
            has_text_effect = true;
            break;
        }
    }
    const auto text_render_mode = ResolveTextRenderMode(TextSurfaceRequirements {
        .has_effect        = has_text_effect,
        .copy_background   = obj.copybackground,
        .opaque_background = obj.opaquebackground,
        .linked_source     = context.IsLinkedSource(static_cast<std::int32_t>(obj.id)),
    });
    const bool direct_text = text_render_mode == TextRenderMode::Direct;
    const bool copy_background_seed = has_text_effect || obj.copybackground;

    std::string s_text;
    if (obj.text.is_string()) {
        s_text = rstd::cppstd::to_string(*obj.text.as_str());
    } else if (obj.text.is_object()) {
        auto value = obj.text.get("value");
        if (value.is_none()) value = obj.text.get("text");
        if (value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) s_text = rstd::cppstd::to_string(*string);
        }
    }
    if (has_text_user) {
        auto value = UserPropertyValue(context.user_properties, obj.text_user.name);
        if (value.is_some()) {
            auto text = SceneJsonScalarString(**value);
            if (text.is_some()) s_text = std::move(*text);
        }
    }
    if (s_text.empty() && ! wants_dynamic_text) {
        // Empty text objects still participate in the transform hierarchy;
        // authored child layers may use them as invisible layout parents.
        auto node = rstd::sync::Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                                      Vector3f(obj.scale.data()),
                                                      Vector3f(obj.angles.data()),
                                                      obj.name);
        node->ID() = obj.id;
        node->SetSize({ obj.size[0], obj.size[1] });
        node->SetReflected(obj.reflected);
        AssignNodeFieldAnimations(*node.as_ptr(), obj.field_bindings);
        WireFieldScripts(context, node, obj.field_bindings);
        if (! obj.visible) node->SetVisible(false);
        if (! obj.visible_user.empty())
            node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

        SceneUniformNodeData sv;
        sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        context.shader_updater->SetNodeData(node.as_ptr(), sv);
        context.node_id_map[obj.id] = {
            obj.parent,
            rstd::Some(node.clone()),
            nullptr,
            obj.attachment,
        };
        return;
    }

    std::string font_name;
    if (obj.font.is_string()) {
        font_name = rstd::cppstd::to_string(*obj.font.as_str());
    } else if (obj.font.is_object()) {
        if (auto value = obj.font.get("value"); value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) font_name = rstd::cppstd::to_string(*string);
        }
    }

    auto resolved = text::FontCache::ResolveFont(*context.vfs, font_name);
    if (! resolved.bytes) {
        rstd_error("text '{}': could not resolve font '{}'", obj.name, font_name);
        return;
    }

    std::uint32_t px = TextPointSizeToPx(obj.pointsize);

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
    auto copy_background_shader =
        copy_background_seed ? text::GetTextCopyBackgroundSceneShader() : nullptr;
    if (copy_background_seed && ! copy_background_shader) {
        rstd_error("text '{}': copy-background shader compile failed", obj.name);
        return;
    }

    // Populate the seed text's glyphs up front so the first SetText has the
    // initial layout's bbox. Runtime SetText calls (from the script actuator)
    // do their own Populate of the latest string each tick.
    {
        auto seed = text::DecodeUtf8(s_text);
        face->Populate(seed);
    }

    // --- atlas-texture registration. We snapshot the per-face CPU atlas
    // (seed glyphs + the white cell) and register it with the imageParser.
    // TextureCache::CreateTex will pick this up on first material bind.
    // Subsequent glyph adds emit dirty rects which the renderer re-uploads
    // each frame via TextureCache::PumpFontAtlases.
    const std::string& atlas_url = face->AtlasUrl();
    if (! EnsureTextAtlas(*context.scene, *face)) {
        rstd_error("text '{}': atlas snapshot failed", obj.name);
        return;
    }

    // --- mesh capacity. Static text exactly fits its initial layout;
    //     dynamic text reserves headroom so SetText can grow
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
        const float obj_w        = obj.size[0] > 0.0f ? obj.size[0] : 1024.0f;
        const float obj_h        = obj.size[1] > 0.0f ? obj.size[1] : 256.0f;
        const float rt_w         = std::max(1024.0f, obj_w * 3.0f);
        const float rt_h         = std::max(256.0f, obj_h * 2.0f);
        const std::size_t cols   = static_cast<std::size_t>(std::ceil(rt_w / adv));
        const std::size_t rows   = static_cast<std::size_t>(std::ceil(rt_h / std::max(1.0f, lh)));
        const std::size_t rt_cap = std::clamp<std::size_t>(cols * rows, 64, 16384);
        peak_quads               = std::max<std::size_t>(initial_codepoints * 4, rt_cap);
        if (has_bg) ++peak_quads;
    } else {
        peak_quads = initial_codepoints + (has_bg ? 1u : 0u);
        if (peak_quads == 0) return;
    }

    const bool supports_runtime_text_write = wants_dynamic_text || context.scene_has_scripts;
    auto       sp_mesh = std::make_shared<SceneMesh>(/*dynamic=*/supports_runtime_text_write);
    {
        SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord, VAttr::Color }),
                                peak_quads * 4);
        sp_mesh->AddVertexArray(std::move(vertex));
        sp_mesh->AddIndexArray(SceneIndexArray(peak_quads * 6));
    }
    {
        SceneMaterial material;
        material.name     = "text";
        material.textures = { atlas_url };
        material.defines  = { "g_Texture0" };
        material.blenmode = direct_text || copy_background_seed ? BlendMode::Translucent
                                                                 : BlendMode::Normal;
        material.customShader.shader = shader;
        sp_mesh->AddMaterial(std::move(material));
    }

    // --- layouter owns the cache (FontFace lifetime) + mesh ref + style.
    text::TextLayoutStyle style;
    style.color                 = { obj.color[0], obj.color[1], obj.color[2] };
    style.alpha                 = has_alpha_animation ? 1.0f : obj.alpha;
    style.brightness            = obj.brightness;
    style.opaquebackground      = has_bg;
    style.background_color      = { obj.backgroundcolor[0],
                                    obj.backgroundcolor[1],
                                    obj.backgroundcolor[2] };
    style.background_brightness = obj.backgroundbrightness;
    style.halign                = obj.horizontalalign.empty() ? obj.alignment : obj.horizontalalign;
    style.padding               = static_cast<float>(obj.padding);
    style.center_source         = ! copy_background_seed && ! has_bg;
    style.limit_width           = obj.limitwidth && obj.maxwidth > 0.0f;
    style.max_width             = obj.maxwidth;
    style.limit_rows            = obj.limitrows && obj.maxrows > 0;
    style.max_rows              = obj.maxrows;
    style.use_ellipsis          = obj.limituseellipsis;

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
    auto current_text       = std::make_shared<std::string>(s_text);
    auto current_point_size = std::make_shared<double>(obj.pointsize);
    auto raster_px          = std::make_shared<std::uint32_t>(px);
    auto current_font_blob  = std::make_shared<std::shared_ptr<std::vector<std::byte>>>(
        resolved.bytes);
    auto current_font_name = std::make_shared<std::string>(font_name);

    auto  initial_metrics = layouter->Metrics();
    float text_w          = initial_metrics.text_width;
    float text_h          = initial_metrics.text_height;
    float text_source_w   = initial_metrics.source_width;
    float text_source_h   = initial_metrics.source_height;
    if (text_w <= 0.0f || text_h <= 0.0f) {
        // Empty seed (scripted-only text). Fake a 1×1 bbox so SceneNode /
        // parallax setup still works; the runtime actuator scales the
        // compose node to actual text dims each tick.
        initial_metrics.text_width  = 1.0f;
        initial_metrics.text_height = 1.0f;
        text_w                      = initial_metrics.text_width;
        text_h                      = initial_metrics.text_height;
    }
    if (text_source_w <= 0.0f) initial_metrics.source_width = text_w;
    if (text_source_h <= 0.0f) initial_metrics.source_height = text_h;
    text_source_w = initial_metrics.source_width;
    text_source_h = initial_metrics.source_height;

    auto sp_node = rstd::sync::Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                                    Vector3f(obj.scale.data()),
                                                    Vector3f(obj.angles.data()),
                                                    direct_text ? obj.name : std::string {});
    sp_node->ID()                  = obj.id;
    const float text_bbox_w        = text_w + 2.0f * style.padding;
    const float text_bbox_h        = text_h + 2.0f * style.padding;
    const float text_source_bbox_w = text_source_w + 2.0f * style.padding;
    const float text_source_bbox_h = text_source_h + 2.0f * style.padding;
    sp_node->SetSize({ text_bbox_w, text_bbox_h });
    sp_node->AddMesh(sp_mesh);

    // sp_node renders into the layer's private ortho RT. Parallax must NOT
    // apply at this stage — the world-space mouse vector would shift glyphs
    // inside ppong_a, but the compose pass samples a fixed UV window, so the
    // shift would manifest as the text appearing to drift in the wrong frame
    // of reference. Parallax goes on compose_node below (world-space quad).
    SceneUniformNodeData svData;
    if (direct_text) {
        svData.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        svData.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
    }
    context.shader_updater->SetNodeData(sp_node.as_ptr(), svData);

    // --- per-layer compose -------------------------------------------------
    // Render the glyphs into a private bbox-sized RT via an ortho camera
    // that maps text-mesh pixel coords 1:1 onto the RT, then composite that
    // RT onto _rt_default with a Translucent fullscreen-quad pass. The glyph
    // pass writes straight RGBA into ppong_a; composing applies alpha once.
    //
    // Glyphs render immediately before compose_node. Attaching the layer
    // camera to sp_node cancels parent transforms inside the private RT.
    struct TextAnchorState {
        std::string horizontal;
        std::string vertical;
        Vector3f    origin;
        float       width { 1.0f };
        float       height { 1.0f };
        bool        authored_width { false };
        bool        authored_height { false };
        float       line_box_width { 1.0f };
        float       line_box_height { 1.0f };
    };

    // `size` is the logical text-layer frame used by WE for alignment and
    // child placement. It must remain independent of the current glyph ink
    // crop: e.g. a bottom-aligned clock still anchors by its declared 58 px
    // frame even when the digits themselves occupy only 35 px.
    const bool  has_authored_width  = obj.size[0] > 0.0f;
    const bool  has_authored_height = obj.size[1] > 0.0f;
    const float object_w            = has_authored_width ? obj.size[0] : text_bbox_w;
    const float object_h            = has_authored_height ? obj.size[1] : text_bbox_h;
    auto anchor_state = std::make_shared<TextAnchorState>(TextAnchorState {
        .horizontal      = initial_halign,
        .vertical        = initial_valign,
        .origin          = Vector3f(obj.origin.data()),
        .width           = object_w,
        .height          = object_h,
        .authored_width  = has_authored_width,
        .authored_height = has_authored_height,
    });

    auto compose_node =
        direct_text ? sp_node.clone()
                    : rstd::sync::Arc<SceneNode>::make(
                          Vector3f::Zero(), Vector3f::Ones(), Vector3f::Zero(), obj.name);
    compose_node->SetReflected(obj.reflected);
    // Layer RT must cover the source glyph bounds, not the main canvas.
    // Clock/date scripts often render a large text source and shrink it with
    // the scene transform when composing into the world.
    const text::TextGeometryPolicy geometry_policy {
        .frame_width        = object_w,
        .frame_height       = object_h,
        .dynamic            = wants_dynamic_text,
        .has_effect         = has_text_effect,
        .preserve_text_bbox = has_bg || obj.copybackground,
    };
    const auto initial_geometry = text::ResolveTextGeometry(geometry_policy, layouter->Metrics());
    const auto initial_render_scale = TextRenderScale(initial_geometry, has_text_effect);
    const auto [initial_logical_w, initial_logical_h] = TextLayerExtent(initial_geometry);
    const auto [initial_layer_w, initial_layer_h] =
        TextLayerExtent(initial_geometry, initial_render_scale);
    auto runtime_targets                          = std::make_shared<TextRuntimeTargets>();
    if (! direct_text) {
        auto&             scene   = *context.scene;
        const std::string addr    = getAddr(sp_node.as_ptr());
        const std::string ppong_a = std::string(SR_EFFECT_PPONG_PREFIX_A) + addr;
        const std::string ppong_b = std::string(SR_EFFECT_PPONG_PREFIX_B) + addr;
        const std::string effect_final =
            std::string(SR_EFFECT_PPONG_PREFIX_A) + "text_final_" + addr;
        runtime_targets->scene          = &scene;
        runtime_targets->shader_updater = context.shader_updater;
        runtime_targets->camera_key     = addr;
        runtime_targets->ppong_a        = ppong_a;
        runtime_targets->ppong_b        = ppong_b;
        runtime_targets->effect_final   = effect_final;
        runtime_targets->has_effect     = has_text_effect;
        runtime_targets->render_scale   = initial_render_scale;
        runtime_targets->layer_w        = initial_layer_w;
        runtime_targets->layer_h        = initial_layer_h;
        runtime_targets->logical_w      = initial_logical_w;
        runtime_targets->logical_h      = initial_logical_h;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        scene.cameras[addr] = std::make_shared<SceneCamera>(
            SceneCamera::MakeOrthographic(initial_logical_w, initial_logical_h, -1.0, 1.0));
        scene.cameras.at(addr)->AttatchNode(sp_node.as_ptr());

        scene.renderTargets[ppong_a] = {
            .width                = initial_layer_w,
            .height               = initial_layer_h,
            .allowReuse           = true,
            .force_clear          = ! copy_background_seed,
            .clear_on_first_write = false,
            .preserve_on_write    = copy_background_seed,
        };
        if (has_text_effect) scene.renderTargets[ppong_b] = scene.renderTargets.at(ppong_a);
        if (has_text_effect) scene.renderTargets[effect_final] = scene.renderTargets.at(ppong_a);

        compose_node->CopyTrans(*sp_node.as_ptr());
        compose_node->ID() = obj.id;
        compose_node->SetSize({ object_w, object_h });

        auto layer = std::make_shared<SceneImageEffectLayer>(has_text_effect ? compose_node.as_ptr()
                                                                             : sp_node.as_ptr(),
                                                             static_cast<float>(initial_logical_w),
                                                             static_cast<float>(initial_logical_h),
                                                             ppong_a,
                                                             has_text_effect ? ppong_b : ppong_a);
        scene.cameras.at(addr)->AttatchImgEffect(layer);

        if (copy_background_seed) {
            auto bg_node = rstd::sync::Arc<SceneNode>::make();
            bg_node->SetCamera("effect");
            auto bg_mesh = std::make_shared<SceneMesh>();
            bg_mesh->ChangeMeshDataFrom(scene.default_effect_mesh);
            SceneMaterial bg_material;
            bg_material.name                = "text_copybackground";
            bg_material.textures            = { std::string(SpecTex_Default) };
            bg_material.defines             = { "g_Texture0" };
            bg_material.blenmode            = BlendMode::Normal;
            bg_material.customShader.shader = copy_background_shader;
            bg_mesh->AddMaterial(std::move(bg_material));
            bg_node->AddMesh(bg_mesh);

            SceneUniformNodeData bg_sv;
            bg_sv.effect_projection_node = compose_node.as_ptr();
            bg_sv.effect_projection_size = { initial_geometry.effect_frame_width,
                                             initial_geometry.effect_frame_height };
            context.shader_updater->SetNodeData(bg_node.as_ptr(), bg_sv);
            runtime_targets->effect_nodes.push_back(TextRuntimeEffectNode {
                .node = bg_node.as_ptr(),
                .data = bg_sv,
            });
            layer->AddPrefillNode(SceneImageEffectNode {
                .output    = ppong_a,
                .sceneNode = bg_node.clone(),
            });
        }

        SceneUniformNodeData compose_sv;
        compose_sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };

        ShaderValueMap effect_base = NeutralColorUniforms(context.global_base_uniforms);

        struct LoadedTextMaterial {
            wpscene::Material    source;
            SceneMaterial        material;
            SceneUniformNodeData sv;
            WPShaderInfo         shader_info;
        };
        auto load_passthrough_material =
            [&](SceneNode* owner, std::string_view input) -> std::optional<LoadedTextMaterial> {
            auto parsed = sr::ParseJson(
                fs::GetFileContent(*context.vfs, "/assets/materials/util/effectpassthrough.json"));
            if (parsed.is_err()) {
                rstd_error("text '{}': parse effectpassthrough.json failed: {}",
                           obj.name,
                           parsed.unwrap_err());
                return std::nullopt;
            }
            auto              pt_json = parsed.unwrap();
            wpscene::Material pt_mat;
            if (! pt_mat.FromJson(pt_json)) {
                rstd_error("text '{}': Material::FromJson failed", obj.name);
                return std::nullopt;
            }
            if (pt_mat.textures.empty())
                pt_mat.textures.push_back(std::string(input));
            else
                pt_mat.textures[0] = std::string(input);

            SceneMaterial        mat;
            SceneUniformNodeData sv;
            WPShaderInfo         si;
            si.baseConstSvs = effect_base;
            if (! LoadMaterial(*context.vfs, pt_mat, &scene, owner, &mat, &sv, &si)) {
                rstd_error("text '{}': compose LoadMaterial failed", obj.name);
                return std::nullopt;
            }
            LoadConstvalue(context, mat, pt_mat, si);
            mat.blenmode = BlendMode::Translucent;
            return LoadedTextMaterial {
                .source      = std::move(pt_mat),
                .material    = std::move(mat),
                .sv          = std::move(sv),
                .shader_info = std::move(si),
            };
        };

        if (has_text_effect) {
            SceneMaterial final_state;
            final_state.blenmode    = BlendMode::Normal;
            final_state.depth_test  = false;
            final_state.depth_write = false;
            layer->SetFullscreen(true);
            layer->SetFinalTarget(effect_final);
            layer->SetFinalMaterialState(final_state);

            for (const auto& wpeffobj : obj.effects) {
                if (! wpeffobj.visible && wpeffobj.visible_user.empty() &&
                    ! wpeffobj.field_bindings.scripts.contains("visible"))
                    continue;

                auto effect             = std::make_shared<SceneImageEffect>();
                effect->name            = wpeffobj.name;
                effect->runtime_visible = wpeffobj.visible;
                if (! wpeffobj.visible_user.empty()) {
                    effect->visible_user_binding =
                        ToSceneUserVisibilityBinding(wpeffobj.visible_user);
                }

                const std::string                            inRT { ppong_a };
                std::unordered_map<std::string, std::string> fboMap;
                fboMap["previous"] = inRT;

                const std::string effaddr = getAddr(layer.get());
                for (const auto& wpfbo : wpeffobj.fbos) {
                    const std::string rtname =
                        sstart_with(wpfbo.name, WE_SPEC_PREFIX)
                            ? wpfbo.name + "_" + effaddr
                            : std::string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
                    auto fbo_size = TextEffectFboExtent(initial_geometry, wpfbo.scale, wpfbo.fit);
                    scene.renderTargets[rtname] = { .width                = fbo_size[0],
                                                    .height               = fbo_size[1],
                                                    .allowReuse           = ! wpfbo.unique,
                                                    .clear_on_first_write = true };
                    fboMap[wpfbo.name]          = rtname;
                    runtime_targets->fbos.push_back(TextRuntimeFbo {
                        .name  = rtname,
                        .scale = wpfbo.scale,
                        .fit   = wpfbo.fit,
                    });
                }

                for (const auto& cmd : wpeffobj.commands) {
                    if (cmd.command != "copy") {
                        rstd_error("Unknown effect command: {}", cmd.command);
                        continue;
                    }
                    if (fboMap.count(cmd.target) + fboMap.count(cmd.source) < 2) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", cmd.target, cmd.source);
                        continue;
                    }
                    effect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                 .dst      = fboMap[cmd.target],
                                                 .src      = fboMap[cmd.source],
                                                 .afterpos = cmd.afterpos });
                }

                bool effect_ok = true;
                for (usize i_mat = 0; i_mat < wpeffobj.materials.size(); ++i_mat) {
                    wpscene::Material                wpmat = wpeffobj.materials.at(i_mat).clone();
                    std::string                      matOutRT { SR_EFFECT_PPONG_PREFIX_B };
                    std::optional<wpscene::Material> user_texture_fallback;
                    if (wpeffobj.passes.size() > i_mat) {
                        const auto& pass = wpeffobj.passes.at(i_mat);
                        wpmat.MergePass(pass);
                        ApplyTextureBinds(wpmat, std::span(pass.bind), fboMap);
                        user_texture_fallback = wpmat.clone();
                        ApplyUserTextureBindings(context, wpmat);
                        if (! pass.target.empty()) {
                            if (fboMap.count(pass.target) == 0)
                                rstd_error("fbo {} not found", pass.target);
                            else
                                matOutRT = fboMap.at(pass.target);
                        }
                    }
                    for (auto& tex : wpmat.textures) {
                        if (ParseImageLayerCompositeId(tex) == static_cast<std::uint32_t>(obj.id))
                            tex = ppong_a;
                    }
                    if (wpmat.textures.empty()) wpmat.textures.resize(1);
                    if (wpmat.textures.at(0).empty()) wpmat.textures[0] = inRT;

                    auto         effect_node = rstd::sync::Arc<SceneNode>::make();
                    WPShaderInfo shader_info;
                    shader_info.baseConstSvs = effect_base;
                    shader_info.baseConstSvs[std::string(G_ETVP)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                    shader_info.baseConstSvs[std::string(G_ETVPI)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());

                    SceneMaterial        mat;
                    SceneUniformNodeData sv;
                    sv.propagate_parallax_to_children = true;
                    sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
                    sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
                    sv.effect_projection_node  = compose_node.as_ptr();
                    sv.effect_projection_size  = { initial_geometry.effect_frame_width,
                                                   initial_geometry.effect_frame_height };
                    sr::Map<std::string, SceneShaderValueAnimation> final_quad_shader_values;
                    if (! LoadMaterial(*context.vfs,
                                       wpmat,
                                       &scene,
                                       effect_node.as_ptr(),
                                       &mat,
                                       &sv,
                                       &shader_info)) {
                        effect_ok = false;
                        break;
                    }
                    LoadConstvalue(
                        context, mat, wpmat, shader_info, &final_quad_shader_values);

                    auto mesh = std::make_shared<SceneMesh>();
                    mesh->AddMaterial(std::move(mat));
                    RegisterShaderUserVarIndex(
                        context, compose_node.as_ptr(), mesh->MaterialSlots().back(), wpmat,
                        shader_info);
                    if (user_texture_fallback.has_value()) {
                        RegisterMaterialUserTextureIndex(
                            &scene, mesh->MaterialSlots().back(), *user_texture_fallback);
                    }
                    effect_node->AddMesh(mesh);
                    context.shader_updater->SetNodeData(effect_node.as_ptr(), sv);
                    runtime_targets->effect_nodes.push_back(TextRuntimeEffectNode {
                        .node = effect_node.as_ptr(),
                        .data = sv,
                    });
                    effect->nodes.push_back(SceneImageEffectNode {
                        .output                   = matOutRT,
                        .sceneNode                = effect_node.clone(),
                        .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                        .final_quad_shader_values = std::move(final_quad_shader_values),
                    });
                }

                if (effect_ok) {
                    layer->AddEffect(effect);
                    RegisterImageEffectVisibilityScript(
                        context, compose_node.as_ptr(), layer, effect,
                        wpeffobj.field_bindings);
                } else
                    rstd_error("effect '{}' failed to load", wpeffobj.name);
            }
            auto resolve_node = rstd::sync::Arc<SceneNode>::make();
            auto resolved     = load_passthrough_material(resolve_node.as_ptr(), ppong_a);
            if (! resolved.has_value()) return;
            auto resolve_mesh = std::make_shared<SceneMesh>();
            resolve_mesh->AddMaterial(std::move(resolved->material));
            resolve_node->AddMesh(std::move(resolve_mesh));
            context.shader_updater->SetNodeData(resolve_node.as_ptr(), resolved->sv);
            runtime_targets->effect_nodes.push_back(TextRuntimeEffectNode {
                .node = resolve_node.as_ptr(),
                .data = resolved->sv,
            });
            auto resolve_effect  = std::make_shared<SceneImageEffect>();
            resolve_effect->name = "text_resolve";
            resolve_effect->nodes.push_back(SceneImageEffectNode {
                .output    = ppong_b,
                .sceneNode = resolve_node.clone(),
            });
            layer->SetFinalResolveEffect(std::move(resolve_effect));
        }

        auto compose_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
        GenCardMesh(*compose_mesh,
                    { static_cast<float>(runtime_targets->logical_w),
                      static_cast<float>(runtime_targets->logical_h) });
        auto loaded = load_passthrough_material(compose_node.as_ptr(),
                                                has_text_effect ? effect_final : ppong_a);
        if (! loaded.has_value()) return;
        compose_sv                         = std::move(loaded->sv);
        compose_sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_mesh->AddMaterial(std::move(loaded->material));
        RegisterShaderUserVarIndex(context,
                                   compose_node.as_ptr(),
                                   compose_mesh->MaterialSlots().back(),
                                   loaded->source,
                                   loaded->shader_info);
        compose_node->AddMesh(compose_mesh);
        context.shader_updater->SetNodeData(compose_node.as_ptr(), compose_sv);

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
    }

    RegisterHiddenTextEffectScripts(context, compose_node.as_ptr(), obj.effects);

    auto compose_hold      = SceneNodeArcHold(compose_node.clone());
    auto apply_text_anchor = [compose_hold, anchor_state]() {
        auto* compose_ptr = compose_hold.get();
        const auto& scale = compose_ptr->Scale();
        const auto anchored = text::ResolveTextAnchorPosition(anchor_state->horizontal,
                                                              anchor_state->vertical,
                                                              anchor_state->origin.x(),
                                                              anchor_state->origin.y(),
                                                              anchor_state->width,
                                                              anchor_state->height,
                                                              scale.x(),
                                                              scale.y(),
                                                              anchor_state->line_box_width,
                                                              anchor_state->line_box_height);
        Vector3f pos = anchor_state->origin;
        pos.x()      = anchored[0];
        pos.y()      = anchored[1];
        compose_ptr->SetTranslate(pos);
    };


    // Per-frame compose-quad rebuild: world card sized to current visible
    // source bbox. The quad offset keeps glyphs at their logical text-box
    // position after the private RT path centers them for UV cropping.

    auto rebuild_compose = [compose_hold,
                            anchor_state,
                            apply_text_anchor,
                            runtime_targets,
                            sp_mesh,
                            geometry_policy,
                            direct_text,
                            text_padding = style.padding](text::TextLayoutMetrics metrics) {
        auto* compose_ptr = compose_hold.get();
        metrics.padding   = text_padding;
        if (! anchor_state->authored_width)
            anchor_state->width = std::max(1.0f, metrics.text_width + 2.0f * text_padding);
        if (! anchor_state->authored_height)
            anchor_state->height = std::max(1.0f, metrics.text_height + 2.0f * text_padding);
        compose_ptr->SetSize({ anchor_state->width, anchor_state->height });
        anchor_state->line_box_width  = std::max(1.0f, metrics.text_width);
        anchor_state->line_box_height = std::max(1.0f, metrics.text_height);
        apply_text_anchor();
        if (direct_text) return;

        const auto geometry       = text::ResolveTextGeometry(geometry_policy, metrics);
        const bool target_changed = runtime_targets->Apply(geometry);
        const float                 hx = geometry.draw_width * 0.5f;
        const float                 hy = geometry.draw_height * 0.5f;
        const float                 cx = geometry.draw_offset_x;
        const float                 cy = geometry.draw_offset_y;
        const std::array<float, 12> pos {
            cx - hx, cy - hy, 0.0f, cx - hx, cy + hy, 0.0f,
            cx + hx, cy - hy, 0.0f, cx + hx, cy + hy, 0.0f,
        };
        const float u_half =
            0.5f *
            std::min(1.0f, geometry.uv_source_width / float(runtime_targets->logical_w));
        const float v_half =
            0.5f *
            std::min(1.0f, geometry.uv_source_height / float(runtime_targets->logical_h));
        const float                u_l = 0.5f - u_half;
        const float                u_r = 0.5f + u_half;
        const float                v_t = 0.5f - v_half;
        const float                v_b = 0.5f + v_half;
        const std::array<float, 8> uv {
            u_l, v_b, u_l, v_t, u_r, v_b, u_r, v_t,
        };
        auto* mesh = compose_ptr->Mesh();
        if (mesh == nullptr) return;
        auto& v = mesh->GetVertexArray(0);
        v.SetVertex(WE_IN_POSITION, pos);
        v.SetVertex(WE_IN_TEXCOORD, uv);
        mesh->SetDirty();
        if (target_changed) {
            mesh->SetLayoutDirty();
            if (sp_mesh) sp_mesh->SetLayoutDirty();
        }
    };
       rebuild_compose(initial_metrics);

    auto apply_text_origin = [anchor_state, apply_text_anchor](const script::ScriptValue& value) {
        Vector3f current = anchor_state->origin;
        auto     next    = ScriptValueAsVec3(value, current);
        if (! next) return;
        anchor_state->origin = *next;
        apply_text_anchor();
    };
    // Same effect as apply_text_origin but from a concrete Vec3, for the
    // scripting origin setter hook (thisLayer.origin = ...).
    auto apply_text_origin_vec = [anchor_state, apply_text_anchor](const Vector3f& next) {
        anchor_state->origin = next;
        apply_text_anchor();
    };
    auto apply_text_scale = [compose_hold, apply_text_anchor](const script::ScriptValue& value) {
        auto*    compose_ptr = compose_hold.get();
        Vector3f current     = compose_ptr->Scale();
        auto     next        = ScriptValueAsVec3(value, current);
        if (! next) return;
        compose_ptr->SetScale(*next);
        apply_text_anchor();
    };
    auto apply_text_alpha = [layouter, rebuild_compose](const script::ScriptValue& value) {
        auto next = ScriptValueAsFloat(value);
        if (! next) return;
        layouter->SetAlpha(*next);
        rebuild_compose(layouter->Metrics());
    };

    auto set_halign = [layouter, rebuild_compose, anchor_state](std::string_view align) {
        anchor_state->horizontal = std::string(align);
        layouter->SetHorizontalAlign(align);
        rebuild_compose(layouter->Metrics());
    };
    auto set_valign = [anchor_state, apply_text_anchor](std::string_view align) {
        anchor_state->vertical = std::string(align);
        apply_text_anchor();
    };
    auto set_pointsize = [scene          = context.scene.get(),
                          font_cache_ptr = &font_cache,
                          current_font_blob,
                          sp_mesh,
                          layouter,
                          rebuild_compose,
                          current_text,
                          current_point_size,
                          raster_px](double next_point_size) {
        if (scene == nullptr || font_cache_ptr == nullptr || ! std::isfinite(next_point_size) ||
            next_point_size <= 0.0) {
            return;
        }
        const std::uint32_t want_px = TextPointSizeToPx(static_cast<float>(next_point_size));
        std::uint32_t       use_px  = *raster_px;
        if (want_px > use_px) {
            use_px = std::clamp<std::uint32_t>(std::max(want_px, use_px * 2u), 1u, 1024u);
        }
        if (use_px != *raster_px) {
            auto* next_face = font_cache_ptr->GetFace(*current_font_blob, use_px);
            if (next_face == nullptr) return;
            next_face->Populate(text::DecodeUtf8(*current_text));
            if (! EnsureTextAtlas(*scene, *next_face)) return;
            *raster_px = use_px;
            if (auto* mat = sp_mesh->Material()) {
                auto mutation = scene->SetMaterialTextureSlot(*mat, 0, next_face->AtlasUrl());
                // The slot swap alone doesn't rebind the GPU descriptor. Queue the
                // changed material so the per-frame drain rebinds the new atlas;
                // without this the mesh gets new-layout UVs while the GPU still
                // samples the old atlas → glyphs shatter on size change.
                if (mutation.changed && mutation.material.has_value()) {
                    scene->QueueTextTextureRefresh(*mutation.material);
                }
            }
            layouter->SetFace(next_face);
        }
        *current_point_size = next_point_size;
        layouter->SetLayoutScale(static_cast<float>(want_px) / static_cast<float>(*raster_px));
        rebuild_compose(layouter->Metrics());
    };
    auto set_font = [scene          = context.scene.get(),
                     font_cache_ptr = &font_cache,
                     current_font_blob,
                     current_font_name,
                     sp_mesh,
                     layouter,
                     rebuild_compose,
                     current_text,
                     raster_px](std::string_view next_font) {
        if (scene == nullptr || font_cache_ptr == nullptr || next_font.empty()) return;
        if (next_font == *current_font_name) return;

        text::FontCache::ResolvedBlob resolved_next;
        if (auto* vfs = static_cast<fs::VFS*>(scene->vfs.get()); vfs != nullptr) {
            resolved_next = text::FontCache::ResolveFont(*vfs, next_font);
        } else {
            resolved_next = text::FontCache::ResolveSystemFont(next_font);
        }
        if (! resolved_next.bytes) {
            rstd_error("layer.font: could not resolve font '{}'", next_font);
            return;
        }

        auto* next_face = font_cache_ptr->GetFace(resolved_next.bytes, *raster_px);
        if (next_face == nullptr) {
            rstd_error("layer.font: FreeType failed to open '{}'", resolved_next.source);
            return;
        }
        next_face->Populate(text::DecodeUtf8(*current_text));
        if (! EnsureTextAtlas(*scene, *next_face)) return;

        if (auto* mat = sp_mesh->Material()) {
            auto mutation = scene->SetMaterialTextureSlot(*mat, 0, next_face->AtlasUrl());
            if (mutation.changed && mutation.material.has_value()) {
                scene->QueueTextTextureRefresh(*mutation.material);
            }
        }
        layouter->SetFace(next_face);
        *current_font_blob = resolved_next.bytes;
        *current_font_name = std::string(next_font);
        layouter->SetText(*current_text);
        rebuild_compose(layouter->Metrics());
    };

    // Must go through EnsureScriptScene: it is the only place that seeds
    // engine.userProperties and the bone resolvers into a fresh JS runtime.
    EnsureScriptScene(context);
    context.script_scene->runtime().RegisterTextFontSetter(
        compose_node.as_ptr(),
        [current_font_name]() {
            return *current_font_name;
        },
        set_font);
    context.script_scene->runtime().RegisterTextAlignSetters(
        compose_node.as_ptr(),
        anchor_state->horizontal,
        anchor_state->vertical,
        obj.pointsize,
        set_halign,
        set_valign,
        [current_point_size]() {
            return *current_point_size;
        },
        set_pointsize);
    // Route `thisLayer.origin` reads/writes (e.g. drag scripts on the origin
    // field) through the logical text origin + re-anchor, so they survive the
    // per-frame compose rebuild that reapplies apply_text_anchor's translate.
    context.script_scene->runtime().RegisterTextOriginHooks(
        compose_node.as_ptr(),
        [anchor_state]() -> Vector3f { return anchor_state->origin; },
        apply_text_origin_vec);
    // Transform-style script bindings (origin/scale/angles) animate the
    // composite quad in world space, not the layer-space glyph node.
    AssignNodeFieldAnimations(*compose_node.as_ptr(), obj.field_bindings);
    WireFieldScripts(
        context,
        compose_node,
        obj.field_bindings,
        apply_text_origin,
        apply_text_scale,
        apply_text_alpha);
    if (! obj.visible) compose_node->SetVisible(false);
    if (! obj.visible_user.empty())
        compose_node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    // --- text-content actuator. Captures the layouter + a closure that
    // re-rasterises new codepoints, lays them out, and rebuilds the
    // compose quad to the new text dims. Runs on the render thread, which
    // is also the JS thread — no synchronization needed.
    auto set_text = [layouter, rebuild_compose, current_text](std::string_view s) {
        *current_text = std::string(s);
        if (auto* active_face = layouter->Face()) active_face->Populate(text::DecodeUtf8(s));
        layouter->SetText(s);
        rebuild_compose(layouter->Metrics());
    };
    if (has_text_script) {
        const auto& sb  = text_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::String,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
            RegisterFieldScriptMetadata(context, compose_node.as_ptr(), fs);
            ss.AddActuator({
                fs,
                [set_text](const script::ScriptValue& v) {
                    if (auto* p = std::get_if<script::StringValue>(&v)) set_text(p->s);
                },
            });
        }
    }
    if (has_pointsize_script) {
        const auto& sb  = pointsize_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::Scalar,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
            RegisterFieldScriptMetadata(context, compose_node.as_ptr(), fs);
            ss.AddActuator({
                fs,
                [set_pointsize](const script::ScriptValue& v) {
                    auto scalar = ScriptValueAsFloat(v);
                    if (scalar) set_pointsize(*scalar);
                },
            });
        }
    }
    // Scripts attached to non-text fields can mutate `thisLayer.text`
    // directly. Register the setter so NodeSetText routes those writes
    // back into the layouter. compose_node is the SceneNode every
    // field-bound script's `thisLayer` resolves to (WireFieldScripts at
    // line above).
    if (supports_runtime_text_write) {
        EnsureScriptScene(context);
        context.script_scene->runtime().RegisterTextSetter(compose_node.as_ptr(),
                                                           [set_text](std::string_view s) {
                                                               set_text(s);
                                                           });
    }

    // Direct user-property bindings on `text` / `pointsize` (authored as
    // `{user:"<key>"}`, no script). Register the same setter closures under
    // the scene's user index so RenderSetUserProperty can drive live sidebar
    // edits. Runs on the render thread, matching the setters' owning thread.
    if (! obj.text_user_key.empty()) {
        context.scene->text_user_index[obj.text_user_key].push_back(
            [set_text](const std::string& s) { set_text(s); });
    }
    if (! obj.pointsize_user_key.empty()) {
        context.scene->pointsize_user_index[obj.pointsize_user_key].push_back(
            [set_pointsize](double v) { set_pointsize(v); });
    }
    // Text color / alpha user bindings. Unlike image layers (node uniforms),
    // text color/alpha live in the glyph vertex colors, so drive the layouter
    // directly and rebuild the compose quad for the refreshed layout.
    if (! obj.color_user_key.empty()) {
        context.scene->text_color_user_index[obj.color_user_key].push_back(
            [layouter, rebuild_compose](float r, float g, float b) {
                layouter->SetColor(r, g, b);
                rebuild_compose(layouter->Metrics());
            });
    }
    if (! obj.alpha_user_key.empty()) {
        context.scene->text_alpha_user_index[obj.alpha_user_key].push_back(
            [layouter, rebuild_compose](float a) {
                layouter->SetAlpha(a);
                rebuild_compose(layouter->Metrics());
            });
    }
    if (! obj.maxwidth_user_key.empty() && obj.limitwidth) {
        context.scene->text_maxwidth_user_index[obj.maxwidth_user_key].push_back(
            [layouter, rebuild_compose](float w) {
                layouter->SetMaxWidth(w);
                rebuild_compose(layouter->Metrics());
            });
    }
    if (! obj.scale_user_key.empty()) {
        context.scene->node_scale_user_index[obj.scale_user_key].push_back({
            compose_node.clone(),
            Vector3f(obj.scale.data()),
            apply_text_anchor,
        });
    }

    std::vector<rstd::sync::Arc<SceneNode>> text_before_nodes;
    if (! direct_text) text_before_nodes.push_back(sp_node.clone());
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
        std::move(text_before_nodes),
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

bool ResolveVisibleUserBinding(bool& visible, const wpscene::VisibleUserBinding& binding,
                               rstd::Option<rstd::ref<rstd::json::Map>> user_props) {
    if (binding.empty()) return false;
    auto value = UserPropertyValue(user_props, binding.name);
    if (value.is_some()) {
        if (auto resolved =
                ResolveSceneUserVisibilityBinding(ToSceneUserVisibilityBinding(binding), **value))
            visible = *resolved;
    }
    return true;
}

struct ObjectVisibilityInfo {
    std::uint32_t parent { 0 };
    bool          visible { true };
    bool          user_bound { false };
};

ObjectVisibilityInfo ResolveObjectVisibility(const Json&                              json_obj,
                                             rstd::Option<rstd::ref<rstd::json::Map>> user_props) {
    ObjectVisibilityInfo info;
    sr::GetJsonValue(json_obj, "parent", info.parent, false);
    wpscene::VisibleUserBinding binding;
    wpscene::ReadVisibleProperty(json_obj, info.visible, binding);
    info.user_bound = ! binding.empty();
    ResolveVisibleUserBinding(info.visible, binding, user_props);
    return info;
}

std::unordered_map<std::int32_t, ObjectVisibilityInfo>
BuildObjectVisibilityInfo(const Json& json, rstd::Option<rstd::ref<rstd::json::Map>> user_props) {
    std::unordered_map<std::int32_t, ObjectVisibilityInfo> out;
    auto                                                   objects = json.get("objects");
    if (objects.is_none()) return out;
    auto array = (*objects)->as_array();
    if (array.is_none()) return out;
    for (const auto& obj : **array) {
        if (! obj.is_object()) continue;
        std::int32_t id {};
        if (! sr::GetJsonValue(obj, "id", id, false)) continue;
        out[id] = ResolveObjectVisibility(obj, user_props);
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
CollectHiddenLinkedSourceIds(const Json& json, const Set<std::int32_t>& linked_source_ids,
                             rstd::Option<rstd::ref<rstd::json::Map>> user_props) {
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
void AddSceneObject(std::vector<SceneObjectVar>& objs, const Json& json_obj, fs::VFS& vfs,
                    wpscene::SceneVersion v, rstd::Option<rstd::ref<rstd::json::Map>> user_props,
                    const Set<std::int32_t>* linked_source_ids, bool force_invisible) {
    T scene_obj;
    if (! scene_obj.FromJson(json_obj, vfs, v)) {
        rstd_error("parse scene object failed, name: {}", scene_obj.name);
        return;
    }
    ResolveVisibleUserBinding(scene_obj.visible, scene_obj.visible_user, user_props);
    if constexpr (std::is_same_v<T, wpscene::ImageObject> ||
                  std::is_same_v<T, wpscene::ShapeObject> ||
                  std::is_same_v<T, wpscene::TextObject>) {
        for (auto& effect : scene_obj.effects)
            ResolveVisibleUserBinding(effect.visible, effect.visible_user, user_props);
    }
    if (force_invisible) scene_obj.visible = false;
    const bool preserve_hidden_link_source =
        ! scene_obj.visible && linked_source_ids != nullptr &&
        linked_source_ids->count(static_cast<std::int32_t>(scene_obj.id)) != 0;
    const bool preserve_hidden_user_bound = ! scene_obj.visible && ! scene_obj.visible_user.empty();
    const bool preserve_hidden_visible_script =
        ! scene_obj.visible && scene_obj.field_bindings.scripts.count("visible") != 0;
    constexpr bool preserve_hidden_text = std::is_same_v<T, wpscene::TextObject>;
    // Image objects keep going even when visible=false: another layer's
    // material may reference them via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder later decides whether to actually emit passes.
    if constexpr (! std::is_same_v<T, wpscene::ImageObject> &&
                  ! std::is_same_v<T, wpscene::ShapeObject>) {
        constexpr bool preserve_user_visibility = ! std::is_same_v<T, wpscene::SoundObject>;
        if (! scene_obj.visible && ! preserve_hidden_link_source && ! preserve_hidden_text &&
            ! (preserve_user_visibility &&
               (preserve_hidden_user_bound || preserve_hidden_visible_script)))
            return;
        if (preserve_hidden_link_source) scene_obj.visible = true;
    }
    objs.push_back(std::move(scene_obj));
}
} // namespace

namespace sr
{

std::vector<SceneObjectVar> ExpandObjects(const Json& json, fs::VFS& vfs, wpscene::SceneVersion v,
                                          rstd::Option<rstd::ref<rstd::json::Map>> user_props,
                                          const Set<std::int32_t>* linked_source_ids) {
    std::vector<SceneObjectVar> scene_objs;
    auto                        objects = json.get("objects");
    if (objects.is_none()) return scene_objs;
    auto array = (*objects)->as_array();
    if (array.is_none()) return scene_objs;
    auto visibility_info = BuildObjectVisibilityInfo(json, user_props);
    for (const auto& obj : **array) {
        bool                       force_invisible = false;
        rstd::Option<std::int32_t> id;
        if (obj.is_object()) {
            std::int32_t value {};
            if (sr::GetJsonValue(obj, "id", value, false)) id = rstd::Some(value);
        }
        if (id.is_some()) {
            force_invisible =
                HasHiddenUserAncestor(static_cast<std::uint32_t>(*id), visibility_info);
        }
        // Order matters: text/model/camera kinds coexist with null
        // image/particle/sound/light fields, so the renderer-supported
        // kinds get first pick. Falls through to the parsing-only kinds
        // (no rendering yet) so the data stays absorbed.
        if (auto value = obj.get("image"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ImageObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("shape"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ShapeObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("particle"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ParticleObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("sound"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::SoundObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("light"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::LightObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("text"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::TextObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("model"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ModelObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("camera"); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::CameraObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        }
    }
    return scene_objs;
}

std::array<i32, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata&   sc,
                                                std::span<const SceneObjectVar> scene_objs) {
    i32 w = sc.general.orthogonalprojection.width;
    i32 h = sc.general.orthogonalprojection.height;
    if (! sc.general.orthogonalprojection.auto_) return { w, h };
    w = 0;
    h = 0;
    for (const auto& obj : scene_objs) {
        const auto* img = std::get_if<wpscene::ImageObject>(&obj);
        if (img == nullptr) continue;
        i32 size = (i32)(img->size.at(0) * img->size.at(1));
        if (size > w * h) {
            w = (i32)img->size.at(0);
            h = (i32)img->size.at(1);
        }
    }
    return { w, h };
}

ParseContext BuildContext(fs::VFS& vfs, std::string_view scene_id, const wpscene::SceneMetadata& sc,
                          std::array<i32, 2>                       ortho_extent,
                          rstd::Option<rstd::ref<rstd::json::Map>> user_properties,
                          std::string script_persistence_path) {
    ParseContext context;
    InitContext(context, vfs, sc, ortho_extent);
    ParseCamera(context, sc);
    context.user_properties = user_properties;
    context.pkg_version     = sc.pkg_version;
    context.script_persistence_path = std::move(script_persistence_path);

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

std::optional<rstd::sync::Arc<SceneNode>> ParseRegisteredAsset(ParseContext& context,
                                                               std::string_view asset) {
    const std::int32_t id = context.next_dynamic_layer_id--;
    if (sstart_with(asset, "models/") && asset.ends_with(".json")) {
        auto size = ResolveImageAssetSize(context, asset);
        if (! size) return std::nullopt;
        wpscene::ImageObject image;
        image.id = id;
        if (! image.FromAsset(asset, *size, *context.vfs, context.pkg_version))
            return std::nullopt;
        ParseImageObj(context, image);
    } else if (asset.ends_with(".mdl")) {
        wpscene::ModelObject model;
        model.id    = id;
        model.name  = asset;
        model.model = asset;
        ParseModelObj(context, model);
    } else if (sstart_with(asset, "particles/") && asset.ends_with(".json")) {
        wpscene::ParticleObject particle;
        particle.id      = id;
        particle.name    = asset;
        particle.visible = true;
        if (! particle.FromAsset(asset, *context.vfs)) return std::nullopt;
        ParseParticleObj(context, particle);
    } else if (asset.ends_with(".ogg") && context.sound_manager != nullptr) {
        wpscene::SoundObject sound;
        sound.id          = id;
        sound.name        = asset;
        sound.startsilent = false;
        sound.sound.push_back(std::string(asset));
        ParseSoundObj(context, sound, *context.sound_manager);
    } else {
        return std::nullopt;
    }

    auto it = context.node_id_map.find(id);
    if (it == context.node_id_map.end() || it->second.node.is_none()) return std::nullopt;
    auto node = (*it->second.node).clone();
    context.node_id_map.erase(it);
    return node;
}

std::optional<std::string> WorkshopAssetPath(const script::LayerAssetReference& reference) {
    if (! reference.workshop_id || reference.path.empty() || reference.path.front() == '/')
        return std::nullopt;
    const auto slash = reference.path.find('/');
    if (slash == std::string_view::npos || slash + 1 >= reference.path.size())
        return std::nullopt;
    const auto relative = reference.path.substr(slash + 1);
    if (relative.starts_with("workshop/")) return std::nullopt;
    if (reference.workshop_id->empty() ||
        ! std::all_of(reference.workshop_id->begin(), reference.workshop_id->end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        return std::nullopt;
    return std::string(reference.path.substr(0, slash)) + "/workshop/" +
           std::string(*reference.workshop_id) + "/" + std::string(relative);
}

std::optional<std::string> ResolveLayerAssetPath(
    ParseContext& context, const script::LayerAssetReference& reference) {
    if (context.vfs->Contains(fs::ResolveAssetPath(reference.path)))
        return std::string(reference.path);
    auto workshop = WorkshopAssetPath(reference);
    if (workshop && context.vfs->Contains(fs::ResolveAssetPath(*workshop))) return workshop;
    return std::nullopt;
}

std::unordered_map<std::string, std::vector<sr::SceneNode*>> SpawnCreateLayerAssetClones(
    ParseContext& context, std::int32_t owner_id, std::span<const std::string> assets,
    std::optional<std::string_view> workshop_id) {
    constexpr unsigned                                           pool_size = 8;
    std::unordered_map<std::string, std::vector<sr::SceneNode*>> out;

    auto owner_it = context.node_id_map.find(owner_id);
    if (owner_it == context.node_id_map.end() || owner_it->second.node.is_none()) return out;

    for (const auto& asset : assets) {
        auto& nodes = out[asset];
        nodes.reserve(pool_size);
        auto resolved = ResolveLayerAssetPath(
            context,
            script::LayerAssetReference { .path = asset, .workshop_id = workshop_id });
        if (! resolved) {
            out.erase(asset);
            continue;
        }
        for (unsigned i = 0; i < pool_size; ++i) {
            auto node = ParseRegisteredAsset(context, *resolved);
            if (! node) break;
            (*node)->SetVisible(false);
            nodes.push_back(node->as_ptr());
            context.layer_clones[owner_id].push_back(std::move(*node));
        }
        if (nodes.empty()) out.erase(asset);
    }
    return out;
}

void ResolveCreateLayerAssetRequests(ParseContext& context) {
    for (auto& req : context.create_layer_asset_requests) {
        if (! req.script) continue;
        auto queues = SpawnCreateLayerAssetClones(context,
                                                  req.owner_id,
                                                  req.script->RegisteredAssets(),
                                                  req.script->WorkshopId());
        for (auto& [asset, nodes] : queues) {
            req.script->AddAssetCloneQueue(std::move(asset), std::move(nodes));
        }
    }
    context.create_layer_asset_requests.clear();
}

std::optional<rstd::sync::Arc<SceneNode>> AttachCreatedLayer(
    ParseContext& context, SceneNode* owner, rstd::sync::Arc<SceneNode> node) {
    SceneNode* parent =
        owner != nullptr && owner->Parent() != nullptr ? owner->Parent() : context.scene->sceneGraph.as_ptr();
    context.scene->AttachRuntimeNode(*parent, node.clone());
    return node;
}

std::optional<rstd::sync::Arc<SceneNode>> InstantiateRegisteredAsset(
    ParseContext& context, SceneNode* owner, const script::LayerAssetReference& reference) {
    auto asset = ResolveLayerAssetPath(context, reference);
    if (! asset) return std::nullopt;
    auto node = ParseRegisteredAsset(context, *asset);
    if (! node) return std::nullopt;
    return AttachCreatedLayer(context, owner, std::move(*node));
}

std::optional<rstd::sync::Arc<SceneNode>> InstantiateLayerConfiguration(
    ParseContext& context, SceneNode* owner, const Json& config) {
    const std::int32_t id = context.next_dynamic_layer_id--;

    if (config.get("text").is_some()) {
        wpscene::TextObject text;
        if (! text.FromJson(config, *context.vfs, context.pkg_version)) return std::nullopt;
        text.id      = id;
        text.parent  = 0;
        ParseTextObj(context, text);
    } else if (config.get("image").is_some()) {
        wpscene::ImageObject image;
        if (! image.FromJson(config, *context.vfs, context.pkg_version)) return std::nullopt;
        image.id      = id;
        image.parent  = 0;
        if (config.get("size").is_none()) {
            if (auto size = ResolveImageAssetSize(context, image.image)) image.size = *size;
        }
        ParseImageObj(context, image);
    } else {
        std::vector<float> requested_size;
        sr::GetJsonValue(config, "size", requested_size, false);
        std::array<float, 2> size { 2.0f, 2.0f };
        if (requested_size.size() >= 2) size = { requested_size[0], requested_size[1] };

        wpscene::ImageObject image;
        image.id = id;
        if (! image.FromAsset(
                "models/util/solidlayer.json", size, *context.vfs, context.pkg_version))
            return std::nullopt;
        image.name    = "__createLayer";
        image.size    = size;
        image.solid   = true;
        image.parent  = 0;
        wpscene::VisibleUserBinding visible_user;
        wpscene::ReadVisibleProperty(config, image.visible, visible_user);
        sr::GetJsonValue(config, "origin", image.origin, false);
        sr::GetJsonValue(config, "angles", image.angles, false);
        sr::GetJsonValue(config, "scale", image.scale, false);
        sr::GetJsonValue(config, "color", image.color, false);
        sr::GetJsonValue(config, "alpha", image.alpha, false);
        sr::GetJsonValue(config, "brightness", image.brightness, false);
        sr::GetJsonValue(config, "alignment", image.alignment, false);
        sr::GetJsonValue(config, "perspective", image.perspective, false);
        if (image.alpha > 1.0f) image.alpha /= 100.0f;
        image.alpha = std::clamp(image.alpha, 0.0f, 1.0f);
        context.solid_layer_ids.insert(id);
        ParseImageObj(context, image);
    }

    auto parsed = context.node_id_map.find(id);
    if (parsed == context.node_id_map.end() || parsed->second.node.is_none()) return std::nullopt;
    auto node = (*parsed->second.node).clone();
    context.node_id_map.erase(parsed);
    bool                        visible = true;
    wpscene::VisibleUserBinding visible_user;
    wpscene::ReadVisibleProperty(config, visible, visible_user);
    node->SetVisible(visible);
    if (config.get("solid").is_some()) {
        bool solid = true;
        if (sr::GetJsonValue(config, "solid", solid, false)) node->SetSolid(solid);
    }
    return AttachCreatedLayer(context, owner, std::move(node));
}

struct DynamicImageRuntimeState {
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    bool                 visible { true };
    bool                 perspective { false };
    bool                 reflected { true };
    std::string          alignment { "center" };
};

DynamicImageRuntimeState ReadDynamicImageRuntimeState(const Json& config) {
    DynamicImageRuntimeState state;
    sr::GetJsonValue(config, "origin", state.origin, false);
    sr::GetJsonValue(config, "scale", state.scale, false);
    sr::GetJsonValue(config, "angles", state.angles, false);
    sr::GetJsonValue(config, "color", state.color, false);
    sr::GetJsonValue(config, "alpha", state.alpha, false);
    sr::GetJsonValue(config, "brightness", state.brightness, false);
    sr::GetJsonValue(config, "perspective", state.perspective, false);
    sr::GetJsonValue(config, "reflected", state.reflected, false);
    sr::GetJsonValue(config, "alignment", state.alignment, false);
    wpscene::VisibleUserBinding visible_user;
    wpscene::ReadVisibleProperty(config, state.visible, visible_user);
    if (state.alpha > 1.0f) state.alpha /= 100.0f;
    state.alpha = std::clamp(state.alpha, 0.0f, 1.0f);
    return state;
}

std::optional<std::string> DynamicImageConfigurationKey(const Json& config) {
    std::string image;
    if (! sr::GetJsonValue(config, "image", image, false) || image.empty()) return std::nullopt;
    if (auto effects = config.get("effects"); effects.is_some()) {
        auto array = (*effects)->as_array();
        if (array.is_none() || (*array)->len() != 0) return std::nullopt;
    }
    constexpr std::array<std::string_view, 22> structural_fields {
        "image",          "size",             "name",             "parallaxDepth",
        "colorBlendMode", "fullscreen",       "nopadding",        "instance",
        "effects",        "locktransforms",   "muteineditor",     "nointerpolation",
        "dependencies",   "perspective",      "copybackground",   "solid",
        "opaquebackground", "clampuvs",       "castshadow",       "disablepropagation",
        "depthtest",      "backgroundbrightness",
    };
    std::string key;
    for (auto field : structural_fields) {
        auto value = config.get(field);
        if (value.is_none()) continue;
        key.append(field);
        key.push_back('=');
        key.append(sr::Dump(**value));
        key.push_back(';');
    }
    return key;
}

class DynamicLayerService {
public:
    DynamicLayerService(ParseContext&& context, script::JsRuntime& runtime, Scene* scene)
        : m_context(std::move(context)), m_runtime(runtime), m_scene(scene) {
        m_context.scene = std::shared_ptr<Scene>(scene, [](Scene*) {});
        m_context.script_scene.reset();
    }

    std::optional<rstd::sync::Arc<SceneNode>> InstantiateAsset(SceneNode* owner,
                                                                script::LayerAssetReference asset) {
        if (! CanAllocate(1, asset.path)) return std::nullopt;
        WPShaderParser::InitGlslang();
        auto node = InstantiateRegisteredAsset(m_context, owner, asset);
        WPShaderParser::FinalGlslang();
        if (! node) return std::nullopt;
        ++m_allocated;
        m_scene->MarkDynamicTopologyDirty();
        return node;
    }

    std::optional<rstd::sync::Arc<SceneNode>> InstantiateConfiguration(SceneNode* owner,
                                                                        Json config) {
        auto key = DynamicImageConfigurationKey(config);
        if (! key) {
            if (! CanAllocate(1, "configuration")) return std::nullopt;
            WPShaderParser::InitGlslang();
            auto node = InstantiateLayerConfiguration(m_context, owner, config);
            WPShaderParser::FinalGlslang();
            if (! node) return std::nullopt;
            ++m_allocated;
            m_scene->MarkDynamicTopologyDirty();
            return node;
        }

        const auto state = ReadDynamicImageRuntimeState(config);
        auto node = AcquireImageNode(*key, owner, config);
        if (! node) return std::nullopt;
        ApplyState(node->as_ptr(), state);
        return node;
    }

private:
    static constexpr std::size_t kMaxDynamicLayers = 4096;

    struct DynamicImagePool {
        rstd::sync::Arc<SceneNode>              tmpl;
        std::vector<rstd::sync::Arc<SceneNode>> available;
        std::size_t                             capacity { 0 };

        explicit DynamicImagePool(rstd::sync::Arc<SceneNode> node): tmpl(std::move(node)) {}
    };

    bool CanAllocate(std::size_t count, std::string_view request) {
        if (count <= kMaxDynamicLayers - m_allocated) return true;
        if (! m_limit_logged) {
            rstd_error("dynamic layer limit {} reached while creating '{}'",
                       kMaxDynamicLayers,
                       request);
            m_limit_logged = true;
        }
        return false;
    }

    std::optional<rstd::sync::Arc<SceneNode>> CompileImageTemplate(const Json& config) {
        wpscene::ImageObject image;
        if (! image.FromJson(config, *m_context.vfs, m_context.pkg_version)) return {};
        if (! image.effects.empty() || ! image.puppet.empty()) return {};
        if (config.get("size").is_none()) {
            auto size = ResolveImageAssetSize(m_context, image.image);
            if (! size) return {};
            image.size = *size;
        }
        image.id     = m_context.next_dynamic_layer_id--;
        image.parent = 0;
        WPShaderParser::InitGlslang();
        ParseImageObj(m_context, image);
        WPShaderParser::FinalGlslang();

        auto parsed = m_context.node_id_map.find(image.id);
        if (parsed == m_context.node_id_map.end() || parsed->second.node.is_none()) return {};
        auto node = (*parsed->second.node).clone();
        m_context.node_id_map.erase(parsed);
        if (config.get("solid").is_some()) {
            bool solid = true;
            if (sr::GetJsonValue(config, "solid", solid, false)) node->SetSolid(solid);
        }
        InstallAlignmentBinding(node.as_ptr(), image.alignment);
        return node;
    }

    std::optional<rstd::sync::Arc<SceneNode>> AcquireImageNode(const std::string& key,
                                                               SceneNode* owner,
                                                               const Json& config) {
        SceneNode* parent = owner != nullptr && owner->Parent() != nullptr
                                ? owner->Parent()
                                : m_scene->sceneGraph.as_ptr();
        std::string pool_key = key + "@" + getAddr(parent);
        auto        pool_it  = m_image_pools.find(pool_key);
        if (pool_it == m_image_pools.end()) {
            auto tmpl = CompileImageTemplate(config);
            if (! tmpl) return std::nullopt;
            pool_it = m_image_pools
                          .emplace(std::move(pool_key), DynamicImagePool(tmpl->clone()))
                          .first;
        }
        auto& pool = pool_it->second;
        if (pool.available.empty() && ! GrowImagePool(pool, owner)) return std::nullopt;
        auto node = std::move(pool.available.back());
        pool.available.pop_back();
        return node;
    }

    bool GrowImagePool(DynamicImagePool& pool, SceneNode* owner) {
        const std::size_t requested = pool.capacity == 0 ? 8 : pool.capacity;
        const std::size_t available = kMaxDynamicLayers - m_allocated;
        if (available == 0) {
            CanAllocate(1, "image configuration pool");
            return false;
        }
        const std::size_t count = std::min(requested, available);
        if (! CanAllocate(count, "image configuration pool")) return false;

        pool.available.reserve(pool.available.size() + count);
        for (std::size_t i = 0; i < count; ++i) {
            auto node = pool.capacity == 0 && i == 0 ? pool.tmpl.clone()
                                                     : CloneImageNode(pool.tmpl.as_ptr());
            node->SetVisible(false);
            auto attached = AttachCreatedLayer(m_context, owner, node.clone());
            if (! attached) return false;
            pool.available.push_back(std::move(node));
        }
        pool.capacity += count;
        m_allocated += count;
        m_scene->MarkDynamicTopologyDirty();
        return true;
    }

    rstd::sync::Arc<SceneNode> CloneImageNode(SceneNode* tmpl) {
        auto clone = rstd::sync::Arc<SceneNode>::make(
            tmpl->Translate(), tmpl->Scale(), tmpl->Rotation(), tmpl->Name());
        clone->SetLocalFrame(tmpl->LocalFrame());
        clone->SetSize(tmpl->Size());
        if (tmpl->HasHitCenter()) clone->SetHitCenter(tmpl->HitCenter());
        clone->SetGeometryTransform(tmpl->GeometryTransform());
        clone->SetPerspective(tmpl->Perspective());
        clone->SetReflected(tmpl->Reflected());
        clone->SetSolid(tmpl->Solid());
        clone->SetBaseColor(tmpl->BaseColor(), tmpl->BaseAlpha());
        if (! tmpl->Camera().empty()) clone->SetCamera(tmpl->Camera());
        if (auto control = tmpl->VideoControlHandle()) clone->SetVideoControl(std::move(control));
        clone->AddMesh(tmpl->MeshShared());
        clone->ID() = m_context.next_dynamic_layer_id--;
        m_context.shader_updater->CopyNodeData(tmpl, clone.as_ptr());
        CloneImageAlignmentBinding(m_context, tmpl, clone.as_ptr());
        InstallAlignmentBinding(clone.as_ptr(), "center");
        return clone;
    }

    void InstallAlignmentBinding(SceneNode* node, std::string_view alignment) {
        for (auto it = m_context.image_alignment_bindings.rbegin();
             it != m_context.image_alignment_bindings.rend(); ++it) {
            if (it->node != node) continue;
            InstallImageAlignmentBinding(m_runtime, node, alignment, it->setter);
            return;
        }
    }

    void ApplyState(SceneNode* node, const DynamicImageRuntimeState& state) {
        node->SetTranslate(Vector3f(state.origin.data()));
        node->SetScale(Vector3f(state.scale.data()));
        node->SetRotation(Vector3f(state.angles.data()));
        node->SetBaseColor(Vector3f(state.color.data()), state.alpha);
        node->SetColor(Vector3f(state.color.data()));
        node->SetUserAlpha(state.alpha);
        node->SetBrightness(state.brightness);
        node->SetPerspective(state.perspective);
        node->SetReflected(state.reflected);
        m_scene->SetNodeVisible(*node, state.visible);
        for (auto it = m_context.image_alignment_bindings.rbegin();
             it != m_context.image_alignment_bindings.rend(); ++it) {
            if (it->node != node) continue;
            if (it->setter) it->setter(node, state.alignment);
            m_runtime.RegisterImageAlignmentSetter(
                node,
                state.alignment,
                [node, setter = it->setter](std::string_view value) {
                    if (setter) setter(node, value);
                });
            break;
        }
    }

    ParseContext m_context;
    script::JsRuntime& m_runtime;
    Scene* m_scene { nullptr };
    std::unordered_map<std::string, DynamicImagePool> m_image_pools;
    std::size_t m_allocated { 0 };
    bool m_limit_logged { false };
};

void ProcessObjects(ParseContext& context, std::span<SceneObjectVar> scene_objs,
                    wavsen::audio::SoundManager* sm, ProcessOpts opts) {
    WPShaderParser::InitGlslang();
    context.sound_manager = sm;
    IndexSystemMediaImageFallbacks(context, scene_objs);

    for (SceneObjectVar& obj : scene_objs) {
        std::visit(visitor::overload {
                       [&context, opts](wpscene::ImageObject& obj) {
                           if (opts.kinds & ProcessOpts::Image) ParseImageObj(context, obj);
                       },
                       [&context, opts](wpscene::ShapeObject& obj) {
                           if (opts.kinds & ProcessOpts::Image) ParseShapeObj(context, obj);
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

    ResolveCreateLayerAssetRequests(context);
    WPShaderParser::FinalGlslang();
}

std::shared_ptr<Scene> FinalizeScene(ParseContext& context) {
    auto scene = context.scene;
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
        if (auto config = context.initial_layer_configs.find(id);
            config != context.initial_layer_configs.end() && config->second.get("solid").is_some()) {
            bool solid = true;
            if (sr::GetJsonValue(config->second, "solid", solid, false)) {
                (*ref.node)->SetSolid(solid);
            }
        }
        context.scene->RegisterNode(
            **ref.node,
            id >= 0 ? std::optional<WallpaperLayerId>(WallpaperLayerId { .value = id })
                    : std::nullopt);
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
        // Named MDAT anchors provide the child's full local frame in the
        // parent puppet's bind space, including rotation and scale.
        if (! ref.attachment.empty() && parent_ref && parent_ref->puppet) {
            const auto& puppet           = *parent_ref->puppet;
            auto        attachment_index = puppet.attachmentIndex(ref.attachment);
            if (attachment_index.has_value()) {
                auto apply_bind_offset = [&]() {
                    auto anchor = puppet.attachmentBindTransform(*attachment_index);
                    if (! anchor) return;
                    if (ref.apply_attachment_offset) {
                        ref.apply_attachment_offset(anchor->translation());
                    } else {
                        (*ref.node)->SetLocalFrame(anchor->matrix().cast<double>() *
                                                  (*ref.node)->LocalFrame());
                    }
                };
                if (! ref.apply_attachment_offset && parent_ref->puppet_layer) {
                    SceneNode* node        = (*ref.node).as_ptr();
                    auto       layer       = parent_ref->puppet_layer;
                    auto       local_base  = node->LocalFrame();
                    auto       update      = [node, layer, attachment_index = *attachment_index,
                                         local_base](double time) {
                        auto anchor = layer->attachmentTransform(attachment_index, time);
                        if (! anchor) return;
                        node->SetLocalFrame(anchor->matrix().cast<double>() * local_base);
                    };
                    update(context.scene->elapsingTime);
                    context.scene->transform_updaters.push_back(std::move(update));
                } else {
                    apply_bind_offset();
                }
            }
        }
        for (auto& before_node : ref.ordered_before_nodes) {
            context.scene->RegisterNode(*before_node);
            parent_node->AppendChild(before_node.clone());
        }
        parent_node->AppendChild((*ref.node).clone());
        attached++;

        // Attach this layer's fanout clones (audio bars) right after it, so
        // all bars sit at the template's z-position in the parent child list.
        if (auto cit = context.layer_clones.find(id); cit != context.layer_clones.end()) {
            for (auto& clone : cit->second) {
                context.scene->RegisterNode(*clone);
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
        auto scripts = std::move(context.script_scene);
        auto& runtime = scripts->runtime();
        for (auto id : context.node_id_order) {
            auto node   = context.node_id_map.find(id);
            auto config = context.initial_layer_configs.find(id);
            if (node == context.node_id_map.end() || node->second.node.is_none() ||
                config == context.initial_layer_configs.end())
                continue;
            runtime.RegisterInitialLayerConfig((*node->second.node).as_ptr(), config->second.clone());
        }
        runtime.SetScene(scene.get());
        auto dynamic_layers =
            std::make_shared<DynamicLayerService>(std::move(context), runtime, scene.get());
        runtime.SetLayerFactory(
            [dynamic_layers](SceneNode* owner, script::LayerAssetReference asset) {
                auto node = dynamic_layers->InstantiateAsset(owner, asset);
                if (! node)
                    rstd_error("registered layer asset '{}' is unsupported or unavailable",
                               asset.path);
                return node;
            });
        runtime.SetLayerConfigFactory([dynamic_layers](SceneNode* owner, Json config) {
            auto node = dynamic_layers->InstantiateConfiguration(owner, std::move(config));
            if (! node) rstd_error("layer configuration is unsupported or unavailable");
            return node;
        });
        runtime.SetSceneRoot(scene->sceneGraph.as_ptr());
        scene->CommitDynamicTopology();
        sr::script::InstallScriptScene(*scene, std::move(scripts));
    }
    if (scene->hdr_render_targets) {
        for (auto& [key, rt] : scene->renderTargets) {
            if (rt.inherit_scene_format) rt.hdr_format = true;
        }
    }
    return scene;
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
    pp->enabled = g.bloom;

    auto add_pass = [&](const char* mat_relpath,
                        std::vector<wpscene::MaterialPassBindItem>
                                                                binds,
                        std::string                             output_rt,
                        std::function<void(wpscene::Material&)> mutate         = nullptr,
                        std::function<void(WPShaderInfo&)>      configure_info = nullptr) -> bool {
        auto parsed =
            sr::ParseJson(fs::GetFileContent(vfs, std::string("/assets/") + mat_relpath));
        if (parsed.is_err()) {
            rstd_error(
                "bloom: parse material json failed {}: {}", mat_relpath, parsed.unwrap_err());
            return false;
        }
        auto              jMat = parsed.unwrap();
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
        LoadConstvalue(context, material, wpmat, wpShaderInfo);

        auto pp_mesh = std::make_shared<SceneMesh>();
        pp_mesh->ChangeMeshDataFrom(scene.default_effect_mesh);
        pp_mesh->AddMaterial(std::move(material));
        RegisterShaderUserVarIndex(
            context, pp_node.as_ptr(), pp_mesh->MaterialSlots().back(), wpmat, wpShaderInfo);
        pp_node->AddMesh(pp_mesh);

        // Camera name drives CustomShaderPass color-write mask: empty or
        // "global" cameras strip the A bit for direct local display. Keep
        // post-process bloom on the existing "effect" cam (2x2 ortho,
        // identity for NDC fullscreen quads) so A=1.0 from the shader survives.
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
            info.baseConstSvs[std::string(G_RENDERVAR0)] = value;
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

    if (! g.user_bindings.empty()) {
        auto bloom_key = g.user_bindings.find("bloom");
        if (bloom_key != g.user_bindings.end()) {
            context.scene->post_process_enable_user_index[bloom_key->second].push_back(pp);
        }

        auto register_bloom_uniform = [&](const char* field) {
            auto it = g.user_bindings.find(field);
            if (it == g.user_bindings.end()) return;
            const std::string& key = it->second;
            for (auto& step : pp->steps) {
                if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                    if (auto* mesh = sp->node->Mesh()) {
                        for (const auto& mat : mesh->MaterialSlots()) {
                            context.scene->shader_user_var_index[key].push_back({ mat, field });
                        }
                    }
                }
            }
        };

        if (g.hdr) {
            register_bloom_uniform("bloomstrength");
            register_bloom_uniform("bloomtint");
        } else {
            register_bloom_uniform("bloomstrength");
            register_bloom_uniform("bloomthreshold");
            register_bloom_uniform("bloomtint");
        }
    }

    scene.post_processes.push_back(std::move(pp));
}

} // namespace sr

std::shared_ptr<Scene> WPSceneParser::Parse(std::string_view scene_id, const std::string& buf,
                                            fs::VFS& vfs, wavsen::audio::SoundManager& sm,
                                            wpscene::SceneVersion pkg_version) {
    auto doc = wpscene::ParseSceneDocumentJson(buf, pkg_version);
    if (! doc) return nullptr;
    return Parse(scene_id, *doc, vfs, sm);
}

std::shared_ptr<Scene> WPSceneParser::Parse(std::string_view              scene_id,
                                            const wpscene::SceneDocument& doc, fs::VFS& vfs,
                                            wavsen::audio::SoundManager& sm) {
    const auto& json = doc.root_json;
    const auto& sc   = doc.metadata;
    rstd_info("scene: pkg_version={} scene_json_version={}",
              static_cast<unsigned>(sc.pkg_version),
              static_cast<unsigned>(sc.scene_json_version));

    auto linked_source_ids = CollectLinkedSourceIdsFromJson(json);
    auto scene_objs =
        ExpandObjects(json, vfs, sc.pkg_version, m_user_properties, &linked_source_ids);
    const auto ortho_extent = ResolveOrthoProjectionExtent(sc, scene_objs);
    auto       context      = BuildContext(vfs,
                                            scene_id,
                                            sc,
                                            ortho_extent,
                                            m_user_properties,
                                            m_script_persistence_path);
    context.scene_has_scripts       = SceneHasScripts(json, scene_objs);
    context.scene_accesses_effects  = SceneAccessesEffects(json, scene_objs);
    context.scene_layer_text_writes = SceneWritesLayerText(json, scene_objs);
    context.scene->uses_audio_spectrum = SceneUsesAudioScripts(json, scene_objs);
    context.hidden_link_source_ids =
        CollectHiddenLinkedSourceIds(json, linked_source_ids, m_user_properties);
    context.linked_source_ids = std::move(linked_source_ids);

    // Single JSON-order walk:
    // - record every object's id (and parent_id) in declaration order so the
    //   final attach phase can rebuild the scene tree with matching child
    //   ordering — z-order in WE is JSON declaration order.
    // - for transform-only "container" layers (no image/particle/sound/light/
    //   text/model/camera field, e.g. workshop 3327063360's "组件"), create the
    //   bare SceneNode here so ParseImageObj children can find their parent.
    //   Their `visible:false` form is preserved as a parent anchor.
    if (auto objects = json.get("objects"); objects.is_some()) {
        auto object_array = (*objects)->as_array();
        if (object_array.is_none()) return context.scene;
        auto visibility_info = BuildObjectVisibilityInfo(json, m_user_properties);
        auto has_kind        = [](const Json& o) {
            for (const char* k :
                 { "image", "particle", "sound", "light", "shape", "text", "model", "camera" }) {
                if (auto value = o.get(k); value.is_some() && ! (*value)->is_null()) return true;
            }
            return false;
        };
        auto read_vec3 = [](const Json& o, const char* key, std::array<float, 3>& out) {
            sr::GetJsonValue(o, key, out, false);
        };
        for (const auto& o : **object_array) {
            if (! o.is_object()) continue;
            std::int32_t id {};
            if (! sr::GetJsonValue(o, "id", id, false)) continue;
            context.initial_layer_configs.emplace(id, o.clone());
            context.script_initialization_orders[id] = context.node_id_order.size();
            context.node_id_order.push_back(id);
            std::uint32_t parent = 0;
            sr::GetJsonValue(o, "parent", parent, false);
            context.object_parent_ids[id] = parent;
            context.scene->RegisterAuthoredLayer(
                WallpaperLayerId { .value = id }, static_cast<i32>(parent));
            bool solid                    = false;
            sr::GetJsonValue(o, "solid", solid, false);
            if (solid) context.solid_layer_ids.insert(id);

            if (has_kind(o)) continue;
            std::string name;
            sr::GetJsonValue(o, "name", name, false);
            std::array<float, 3> origin { 0, 0, 0 }, scale { 1, 1, 1 }, angles { 0, 0, 0 };
            read_vec3(o, "origin", origin);
            read_vec3(o, "scale", scale);
            read_vec3(o, "angles", angles);
            auto node = rstd::sync::Arc<SceneNode>::make(
                Vector3f(origin.data()), Vector3f(scale.data()), Vector3f(angles.data()), name);
            node->ID() = id;
            std::array<float, 2> parallax_depth { 0.0f, 0.0f };
            bool                 disable_propagation = false;
            sr::GetJsonValue(o, "parallaxDepth", parallax_depth, false);
            sr::GetJsonValue(o, "disablepropagation", disable_propagation, false);
            if (parallax_depth[0] != 0.0f || parallax_depth[1] != 0.0f || disable_propagation) {
                SceneUniformNodeData sv_data;
                sv_data.propagate_parallax_to_children = ! disable_propagation;
                sv_data.parallaxDepth                  = { parallax_depth[0], parallax_depth[1] };
                sv_data.propagatedParallaxDepth        = { parallax_depth[0], parallax_depth[1] };
                context.shader_updater->SetNodeData(node.as_ptr(), sv_data);
            }
            auto vit = visibility_info.find(id);
            if (vit != visibility_info.end()) {
                const bool hidden_ancestor =
                    HasHiddenUserAncestor(static_cast<std::uint32_t>(id), visibility_info);
                if (! vit->second.visible || hidden_ancestor) {
                    node->SetVisible(false);
                    // A container owns no mesh, so the render graph can only see
                    // this hide through the elision set; SceneNode::Visible() is
                    // never consulted during graph build. Without the mark the
                    // whole subtree keeps emitting passes.
                    if (vit->second.user_bound || hidden_ancestor) {
                        context.scene->MarkLayerVisibilityElidable(
                            WallpaperLayerId { .value = id });
                    }
                }
            }
            wpscene::VisibleUserBinding visible_user;
            wpscene::ReadVisibleUserBinding(o, visible_user);
            if (! visible_user.empty())
                node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(visible_user));
            wpscene::UserValueBinding scale_user;
            wpscene::ReadUserValueBinding(o, "scale", scale_user);
            if (! scale_user.name.empty()) {
                context.scene->node_scale_user_index[scale_user.name].push_back({
                    node.clone(),
                    Vector3f(scale.data()),
                });
            }
            wpscene::FieldBindings fb;
            wpscene::AbsorbAllFieldBindings(o, fb);
            WireFieldScripts(context, node, fb);
            std::string attachment;
            sr::GetJsonValue(o, "attachment", attachment, false);
            context.node_id_map[id] = {
                parent, rstd::Some(node.clone()), nullptr, std::move(attachment), nullptr
            };
        }
    }

    ProcessObjects(context, scene_objs, &sm);

    const bool bloom_user_bound =
        sc.general.user_bindings.find("bloom") != sc.general.user_bindings.end();
    if (sc.general.bloom || bloom_user_bound) {
        BuildBloomPostProcess(context, vfs, sc.general);
    }
    return FinalizeScene(context);
}
