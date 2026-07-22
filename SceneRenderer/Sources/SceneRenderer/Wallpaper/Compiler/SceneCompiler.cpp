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

std::vector<std::string> DetectRegisteredAssets(std::string_view src) {
    std::vector<std::string> out;
    auto                     seen = std::unordered_set<std::string> {};
    for (usize pos = 0; (pos = src.find("registerAsset", pos)) != std::string_view::npos;) {
        pos += std::string_view("registerAsset").size();
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) ++pos;
        if (pos >= src.size() || src[pos] != '(') continue;
        ++pos;
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) ++pos;
        if (pos >= src.size() || (src[pos] != '\'' && src[pos] != '"')) continue;
        char  quote = src[pos++];
        usize begin = pos;
        while (pos < src.size()) {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                pos += 2;
                continue;
            }
            if (src[pos] == quote) break;
            ++pos;
        }
        if (pos >= src.size()) break;
        std::string asset { src.substr(begin, pos - begin) };
        if (seen.insert(asset).second) out.push_back(std::move(asset));
    }
    return out;
}

std::optional<std::array<float, 2>> ResolveImageAssetSize(ParseContext&    context,
                                                          std::string_view image_path) {
    auto info = wpscene::LoadImageAssetInfo(*context.vfs, image_path);
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

void CollectLinkedSourceIdsFromJsonValue(const Json& value, Set<std::int32_t>& out) {
    if (value.is_string()) {
        auto s = rstd::cppstd::to_string(*value.as_str());
        if (auto id = ParseImageLayerCompositeId(s)) out.insert(static_cast<std::int32_t>(*id));
        if (IsSpecLinkTex(s)) out.insert(static_cast<std::int32_t>(ParseLinkTex(s)));
        return;
    }
    if (value.is_array()) {
        const auto values = value.as_array();
        for (const auto& el : **values) CollectLinkedSourceIdsFromJsonValue(el, out);
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
        CollectLinkedSourceIdsFromJsonValue(child, out);
    });
}

Set<std::int32_t> CollectLinkedSourceIdsFromJson(const Json& json) {
    Set<std::int32_t> out;
    if (auto objects = json.get("objects"); objects.is_some())
        CollectLinkedSourceIdsFromJsonValue(**objects, out);
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
        if (context.user_properties.is_some())
            (*context.user_properties)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto key                      = rstd::cppstd::as_string_view(entry_key->as_str());
                context.script_scene->runtime().SetUserProperty(key, *entry_value);
            });
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

std::array<i32, 2> TextLayerExtent(const text::TextGeometry& geometry) {
    return {
        std::max<i32>(1, static_cast<i32>(std::ceil(geometry.rt_width))),
        std::max<i32>(1, static_cast<i32>(std::ceil(geometry.rt_height))),
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
    i32                                layer_w { 1 };
    i32                                layer_h { 1 };
    std::vector<TextRuntimeFbo>        fbos;
    std::vector<TextRuntimeEffectNode> effect_nodes;

    bool Apply(const text::TextGeometry& geometry) {
        if (scene == nullptr) return false;

        bool changed          = false;
        auto [next_w, next_h] = TextLayerExtent(geometry);
        changed |= ResizeRenderTarget(*scene, ppong_a, next_w, next_h);
        if (has_effect) {
            changed |= ResizeRenderTarget(*scene, ppong_b, next_w, next_h);
            changed |= ResizeRenderTarget(*scene, effect_final, next_w, next_h);
        }

        if (auto it = scene->cameras.find(camera_key); it != scene->cameras.end() && it->second) {
            auto& camera = *it->second;
            if (camera.Width() != static_cast<double>(next_w) ||
                camera.Height() != static_cast<double>(next_h)) {
                camera.SetWidth(next_w);
                camera.SetHeight(next_h);
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

void AssignCurve(SceneAnimationCurve& dst, const wpscene::FieldBindings& bindings,
                 std::string_view field) {
    auto it = bindings.animations.find(std::string(field));
    if (it != bindings.animations.end()) dst = ToSceneAnimationCurve(it->second);
}

void AssignNodeFieldAnimations(SceneNode& node, const wpscene::FieldBindings& bindings) {
    auto origin_it = bindings.animations.find("origin");
    if (origin_it != bindings.animations.end())
        node.SetOriginAnimation(ToSceneAnimationCurve(origin_it->second));
    auto scale_it = bindings.animations.find("scale");
    if (scale_it != bindings.animations.end())
        node.SetScaleAnimation(ToSceneAnimationCurve(scale_it->second));
    auto angles_it = bindings.animations.find("angles");
    if (angles_it != bindings.animations.end())
        node.SetRotationAnimation(ToSceneAnimationCurve(angles_it->second));
    auto it = bindings.animations.find("alpha");
    if (it != bindings.animations.end()) node.SetAlphaAnimation(ToSceneAnimationCurve(it->second));
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
        auto  props         = ScriptPropertiesForField(context, field, sb);
        auto  initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs =
            rt.MakeFieldScript(sb.source, sha, kind, props, initial_value, node, std::move(clones));
        if (! fs) continue;
        if (sb.source.find("createLayer") != std::string_view::npos &&
            sb.source.find("registerAsset") != std::string_view::npos) {
            context.create_layer_asset_requests.push_back(
                { fs, node->ID(), std::string(sb.source) });
        }
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

        std::string sha           = utils::genSha1(std::span<const char>(sb.source));
        auto        initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, initial_value, node);
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

std::array<float, 2> ImageEffectTargetSize(const ParseContext&         context,
                                           const wpscene::ImageObject& obj) {
    if (obj.fullscreen && context.scene && context.scene->activeCamera) {
        return { static_cast<float>(context.scene->activeCamera->Width()),
                 static_cast<float>(context.scene->activeCamera->Height()) };
    }
    return { obj.size[0], obj.size[1] };
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
    // Metal has no geometry-shader stage and MoltenVK can't lower it, so
    // rope/sprite/trail particles fall back to CPU quad expansion on macOS.
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
        if (pcs[i].link_mouse) pSys.SetUsesMouseControlpoint();
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     std::shared_ptr<wpscene::ParticleInstanceoverride> over_state) {
    for (const auto& ini : wp.initializers) {
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(ini));
    }
    if (over_state->enabled) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(over_state));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  std::shared_ptr<wpscene::ParticleInstanceoverride> over_state) {
    for (const auto& op : wp.operators) {
        pSys.AddOperator(WPParticleParser::genParticleOperatorOp(op, over_state));
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

void ApplyImageColorBlend(wpscene::Material& material, const wpscene::ImageObject& image) {
    if (image.colorBlendMode == 0) return;
    material.combos[std::string(WE_CB_BLENDMODE)] = image.colorBlendMode;
}

i32 CountVisibleImageEffects(std::span<const wpscene::ImageEffect> effects) {
    i32 count = 0;
    for (const auto& effect : effects) {
        if (effect.visible || ! effect.visible_user.empty()) ++count;
    }
    return count;
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

    shader->default_uniforms = pWPShaderInfo->svs;

    for (const auto& el : wpmat.combos) {
        pWPShaderInfo->combos[el.first] = std::to_string(el.second);
    }
    ApplyLegacyAtmosphereLightCombo(wpmat, *pWPShaderInfo);

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
            pWPShaderInfo->combos["reflection"]                  = "0";
            pWPShaderInfo->combos[std::string(WE_CB_REFLECTION)] = "0";
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
    WPShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        variant_desc, sd_units, shader->codes);

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
    material.customShader         = materialShader;
    material.customShader.variant = std::move(variant_desc);
    material.name                 = wpmat.shader;

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
    if (type_string.is_none() || value_string.is_none() ||
        rstd::cppstd::as_string_view(*type_string) != "system")
        return std::nullopt;
    auto name = rstd::cppstd::as_string_view(*value_string);
    if (name != "$mediaThumbnail" && name != "$mediaPreviousThumbnail") return std::nullopt;
    return std::string(name);
}

bool IsSystemMediaTextureBinding(const Json& binding) {
    return UserTexturePropertyKey(binding).has_value() && binding.is_object();
}

void RegisterMaterialUserTextureIndex(Scene* pScene, SceneMaterial* stable_mat,
                                      const wpscene::Material& fallback_material) {
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
        pScene->material_texture_user_index[*key].push_back(
            Scene::MaterialTextureUserBinding { .material = stable_mat,
                                                .slot     = static_cast<uint32_t>(i),
                                                .fallback = std::move(fallback) });
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
        if (wpmat.textures.size() <= (usize)el.index) wpmat.textures.resize((usize)el.index + 1);
        wpmat.textures[(usize)el.index] = fboMap.at(el.name);
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

std::string ResolveUserTextureProperty(const ParseContext& context, const Json& binding) {
    if (! binding.is_string()) return {};
    auto key = rstd::cppstd::to_string(*binding.as_str());
    return ResolveSceneTextureProperty(context, key);
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

void ApplyObjectInstanceOverrides(wpscene::Material& material,
                                  const wpscene::ObjectInstance& instance) {
    if (! instance.present) return;
    if (material.textures.size() < instance.textures.size())
        material.textures.resize(instance.textures.size());
    for (usize i = 0; i < instance.textures.size(); ++i) {
        if (! instance.textures[i].empty()) material.textures[i] = instance.textures[i];
    }
    while (material.usertextures.len() < instance.usertextures.len())
        material.usertextures.push(Json::Null());
    for (usize i = 0; i < instance.usertextures.len(); ++i) {
        if (! instance.usertextures[i].is_null())
            material.usertextures[i] = instance.usertextures[i].clone();
    }
    for (const auto& [key, value] : instance.combos) material.combos[key] = value;
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
    SceneMaterial& material, const wpscene::Material& wpmat, const WPShaderInfo& info,
    sr::Map<std::string, SceneShaderValueAnimation>* final_quad_shader_values = nullptr) {
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name   = cs.first;
        const std::vector<float>& value  = cs.second;
        std::string               glname = ResolveShaderMaterialKey(info, name);
        if (glname.empty()) {
            if (IsLegacyAtmosphereShadowValue(wpmat, name)) continue;
            rstd_error("ShaderValue: {} not found in glsl", name);
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
            material.customShader.constValues[glname] = const_value;
            if (auto it = wpmat.constantshadervalues_animations.find(name);
                it != wpmat.constantshadervalues_animations.end()) {
                auto curve =
                    std::make_shared<SceneAnimationCurve>(ToSceneAnimationCurve(it->second));
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
    context.ortho_w = scene.ortho[0];
    context.ortho_h = scene.ortho[1];

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
        gb[std::string(G_NORMALMODELMATRIX)]  = ShaderValue::fromMatrix(Matrix4f::Identity());
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

    std::unique_ptr<WPMdl> puppet;
    bool                   has_bones = false;
    bool                   has_mesh  = false;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! WPMdlParser::Parse(wpimgobj.puppet, vfs, *puppet)) {
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

    const bool has_author_effect = CountVisibleImageEffects(wpimgobj.effects) > 0;
    // A solid layer's flat material only produces its source color; a final compositor owns
    // BLENDMODE and the previous-framebuffer input.
    const bool layer_material_is_final =
        (! has_author_effect || has_bones) && ! wpimgobj.solid_layer;
    const bool color_blend_uses_layer_material =
        wpimgobj.colorBlendMode != 0 && layer_material_is_final;
    const bool append_color_blend_final_effect =
        wpimgobj.colorBlendMode != 0 && ! color_blend_uses_layer_material;
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
        ApplyImageColorBlend(colorMat, wpimgobj);
        colorEffect.materials.push_back(std::move(colorMat));
        wpimgobj.effects.push_back(std::move(colorEffect));
    }
    const bool is_hidden_link_source =
        context.hidden_link_source_ids.count(static_cast<std::int32_t>(wpimgobj.id)) != 0;
    if (! has_author_effect && (is_hidden_link_source || wpimgobj.composite_layer)) {
        AppendLayerCompositePassthroughEffect(vfs, wpimgobj);
    }

    bool hasEffect = CountVisibleImageEffects(wpimgobj.effects) > 0;

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
    const Vector3f alignment_offset =
        wpimgobj.fullscreen
            ? Vector3f::Zero()
            : AlignmentOffset(wpimgobj.alignment, { geometry_size[0], geometry_size[1] });
    const bool solid_scene_context = HasSolidCompositeContext(context, wpimgobj);
    spImgNode->SetSize({ geometry_size[0], geometry_size[1] });
    spImgNode->SetPerspective(wpimgobj.perspective || solid_scene_context);
    spImgNode->SetBaseColor(Vector3f(wpimgobj.color.data()), wpimgobj.alpha);
    spImgNode->ID() = wpimgobj.id;
    if (! wpimgobj.visible_user.empty())
        spImgNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wpimgobj.visible_user));
    std::vector<SceneMaterial*> image_property_materials;
    auto                        track_image_property_material = [&](SceneMaterial* mat) {
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
    wpscene::Material image_wpmat                 = wpimgobj.material.clone();
    ApplyObjectInstanceOverrides(image_wpmat, wpimgobj.instance);
    wpscene::Material image_user_texture_fallback = image_wpmat.clone();
    if (color_blend_uses_layer_material && ! hasEffect) ApplyImageColorBlend(image_wpmat, wpimgobj);
    ApplyUserTextureBindings(context, image_wpmat);
    const bool replaced_solid_color =
        wpimgobj.solid_layer && ! image_user_texture_fallback.textures.empty() &&
        ! image_wpmat.textures.empty() && image_user_texture_fallback.textures[0] == "util/white" &&
        image_wpmat.textures[0] != image_user_texture_fallback.textures[0];
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
    auto                       spMesh        = std::make_shared<SceneMesh>();
    auto&                      mesh          = *spMesh;
    const std::array<float, 2> mapRate       = Texture0UvScale(material, wpimgobj.nopadding);
    const Vector3f source_alignment_offset   = hasEffect ? Vector3f::Zero() : alignment_offset;
    auto           add_puppet_mask_submeshes = [&](SceneMesh& target, uint32_t first_mask_slot) {
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
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b, mapRate, alignment_offset);
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a, mapRate, alignment_offset);
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet) {
        if (hasEffect) {
            GenCardMesh(
                mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
            for (const auto& m : puppet->meshes) {
                if (m.positions.empty()) continue;
                effct_final_mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(
                    effct_final_mesh.Submeshes().back(), m, mapRate, alignment_offset);
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, 1);

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat = image_wpmat.clone();
                puppet_mat.textures[0]          = "";
                WPMdlParser::AddPuppetMatInfo(puppet_mat, *puppet);
                if (color_blend_uses_layer_material) ApplyImageColorBlend(puppet_mat, wpimgobj);
                puppet_effect.materials.push_back(std::move(puppet_mat));
                wpimgobj.effects.push_back(std::move(puppet_effect));
            }
        } else {
            for (const auto& m : puppet->meshes) {
                if (m.positions.empty()) continue;
                mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(mesh.Submeshes().back(), m, mapRate, alignment_offset);
            }
        }
    }
    if (! puppet) {
        GenCardMesh(mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
        GenCardMesh(effct_final_mesh,
                    { geometry_size[0], geometry_size[1] },
                    { 1.0f, 1.0f },
                    alignment_offset);
    }
    // material blendmode for last step to use
    auto finalMaterialState = material;
    // disable img material blend, as it's the first effect node now
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }
    mesh.AddMaterial(std::move(material));
    track_image_property_material(mesh.MaterialSlots().back().get());
    RegisterShaderUserVarIndex(context.scene.get(), mesh.Material(), image_wpmat, shaderInfo);
    RegisterMaterialUserTextureIndex(
        context.scene.get(), mesh.Material(), image_user_texture_fallback);

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
                track_image_property_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b, mapRate, alignment_offset);
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
                LoadConstvalue(clip_scene_mat, clip_wpmat, clip_shaderInfo);
                uint32_t clip_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(clip_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a, mapRate, alignment_offset);
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
            const auto effect_extent =
                NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
            i32 w                   = effect_extent[0];
            i32 h                   = effect_extent[1];
            scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(w, h, -1.0f, 1.0f);
            // Attach the per-layer effect camera to spImgNode itself so the
            // camera follows the layer through any parent-container world
            // translation. Otherwise the layer's quad ends up off-center in
            // the ping-pong RT whenever the layer is nested under a non-zero
            // container.
            scene.cameras.at(nodeAddr)->AttatchNode(spImgNode.as_ptr());
        }
        if (wpimgobj.composite_layer) {
            const std::string group_camera = nodeAddr + "_group";
            const auto        group_extent =
                NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
            scene.cameras[group_camera] =
                std::make_shared<SceneCamera>(group_extent[0], group_extent[1], -1.0f, 1.0f);
            scene.cameras.at(group_camera)->AttatchNode(spImgNode.as_ptr());
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
        {
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
                .preserve_on_write    = wpimgobj.composite_layer,
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

        int32_t    i_eff = -1;
        bool       last_effect_can_composite_final { false };
        const bool allow_transparent_previous_final = ! solid_scene_context;
        const bool passthrough_can_composite_final  = isPassthrough;
        for (const auto& wpeffobj : wpimgobj.effects) {
            i_eff++;
            if (! wpeffobj.visible && wpeffobj.visible_user.empty()) {
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
                            .width      = 2,
                            .height     = 2,
                            .allowReuse = ! wpfbo.unique,
                        };
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
                        scene.renderTargets[rtname] = { .width      = fbo_size[0],
                                                        .height     = fbo_size[1],
                                                        .allowReuse = ! wpfbo.unique };
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
                LoadConstvalue(material, wpmat, wpEffShaderInfo, &final_quad_shader_values);
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.propagatedParallaxDepth = { wpimgobj.parallaxDepth[0],
                                                       wpimgobj.parallaxDepth[1] };
                    svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
                    svData.effect_projection_node = spImgNode.as_ptr();
                    svData.effect_projection_size = { static_cast<float>(effect_extent[0]),
                                                      static_cast<float>(effect_extent[1]) };
                    if (puppet && wpmat.use_puppet) {
                        svData.puppet_layer =
                            MakePuppetLayer(puppet->puppet, wpimgobj.puppet_layers);
                        RegisterPuppetLayer(context, spEffNode.as_ptr(), svData.puppet_layer);
                    }
                }
                spMesh->AddMaterial(std::move(material));
                track_image_property_material(spMesh->MaterialSlots().back().get());
                RegisterShaderUserVarIndex(
                    context.scene.get(), spMesh->Material(), wpmat, wpEffShaderInfo);
                if (user_texture_fallback.has_value()) {
                    RegisterMaterialUserTextureIndex(
                        context.scene.get(), spMesh->Material(), *user_texture_fallback);
                }
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
                            LoadConstvalue(mask_material, mask_wpmat, mask_shaderInfo);
                            spMesh->AddMaterial(std::move(mask_material));
                            track_image_property_material(spMesh->MaterialSlots().back().get());

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
                            LoadConstvalue(clip_material, clip_wpmat, clip_shaderInfo);
                            spMesh->AddMaterial(std::move(clip_material));
                            track_image_property_material(spMesh->MaterialSlots().back().get());
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

            if (eff_mat_ok)
                imgEffectLayer->AddEffect(imgEffect);
            else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        if (! wpimgobj.fullscreen && ! passthrough_can_composite_final &&
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
                        track_image_property_material(spFinalMesh->MaterialSlots().back().get());
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
    }
    AssignNodeFieldAnimations(*spImgNode.as_ptr(), wpimgobj.field_bindings);
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (! wpimgobj.color_user_key.empty()) {
        context.scene->image_color_user_index[wpimgobj.color_user_key].push_back(
            { spImgNode.as_ptr(), image_property_materials });
    }
    if (! wpimgobj.alpha_user_key.empty()) {
        context.scene->image_alpha_user_index[wpimgobj.alpha_user_key].push_back(
            { spImgNode.as_ptr(), image_property_materials });
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

wpscene::ParticleInstanceoverride ParticleOverrideForNode(const wpscene::ParticleObject& obj,
                                                          bool                           is_child) {
    if (! is_child) return obj.instanceoverride;

    wpscene::ParticleInstanceoverride out;
    const auto&                       parent = obj.instanceoverride;
    out.enabled                              = parent.enabled;
    out.alpha                                = parent.alpha;
    out.overColor                            = parent.overColor;
    out.overColorn                           = parent.overColorn;
    out.color                                = parent.color;
    out.colorn                               = parent.colorn;
    for (std::string_view field : { "alpha", "color", "colorn" }) {
        if (auto it = parent.bindings.find(std::string(field)); it != parent.bindings.end()) {
            out.bindings.emplace(it->first, it->second);
        }
    }
    return out;
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

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    // Child presets inherit the placed object's opacity/tint but keep their
    // own size, lifetime, rate and count.
    auto override_state = std::make_shared<wpscene::ParticleInstanceoverride>(
        ParticleOverrideForNode(wppartobj, is_child));
    auto& override = *override_state;

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;

    auto wppartRenderer = particle_obj.renderers.at(0);
    auto render_desc    = DescribeParticleRender(wppartRenderer);
    bool render_rope    = render_desc.rope;
    bool hastrail       = render_desc.trail;

    if (render_rope) particle_obj.material.shader = "genericropeparticle";

    // Only the stock genericparticle shader receives the instanced layout;
    // other/custom materials retain the original CPU-expanded layout.
    const bool use_instanced_particles = ! render_rope &&
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
    maxcount     = std::min(maxcount, 20000u);

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
        // Only the authored "rope" renderer uses genericropeparticle's segment
        // layout. "*trail" renderers stay on genericparticle and need velocity
        // in TexCoordVec4C1 for ComputeParticleTrailTangents.
        if (! render_rope) shaderInfo.combos[std::string(WE_CB_THICK_FORMAT)] = "1";
    }
    if (render_rope) {
        // genericropeparticle.geom branches on TRAILSUBDIVISION when present;
        // 0 = no subdivision (straight quad per segment), positive = cubic
        // Bezier subdivided into N+1 quads.
        i32 subdiv = (i32)std::round(wppartRenderer.subdivision);
        if (subdiv < 0) subdiv = 0;
        shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(subdiv);
    }

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
    LoadConstvalue(material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
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
        static_cast<double>(particle_obj.starttime),
        particle_obj.flags[wpscene::Particle::FlagEnum::wordspace]);

    particleSub->SetOwnerNode(spNode.as_ptr());
    for (const auto& emitter : particle_obj.emitters) {
        if (emitter.audioprocessingmode != 0) {
            context.scene->uses_audio_spectrum = true;
            break;
        }
    }
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

    if (! is_child) AssignNodeFieldAnimations(*spNode.as_ptr(), wppartobj.field_bindings);
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
    if (mdl.puppet && ! mdl.puppet->bones.empty()) {
        svData.puppet_layer = MakePuppetLayer(
            mdl.puppet, std::span<WPPuppetLayer::AnimationLayer>(model_obj.puppet_layers));
        RegisterPuppetLayer(context, node.as_ptr(), svData.puppet_layer);
    }

    for (const auto& mdl_mesh : mdl.meshes) {
        if (mdl_mesh.positions.empty()) continue;

        auto wpmat = WPMdlParser::ParseMaterial(mdl_mesh.mat_json_file, vfs);
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
    if (! obj.visible && obj.visible_user.empty()) return;
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
        if (effect.visible || ! effect.visible_user.empty()) {
            has_text_effect = true;
            break;
        }
    }
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
    if (s_text.empty() && ! wants_dynamic_text) return;

    // --- font resolution: VFS first (WE shared /assets + pkg overlay),
    //     then host system font dirs.
    std::string font_name;
    if (obj.font.is_string()) {
        font_name = rstd::cppstd::to_string(*obj.font.as_str());
    } else if (obj.font.is_object()) {
        if (auto value = obj.font.get("value"); value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) font_name = rstd::cppstd::to_string(*string);
        }
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

    auto sp_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
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
        material.blenmode = copy_background_seed ? BlendMode::Translucent : BlendMode::Normal;
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
    auto current_text       = std::make_shared<std::string>(s_text);
    auto current_point_size = std::make_shared<double>(obj.pointsize);

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

    auto sp_node = rstd::sync::Arc<SceneNode>::make(
        Vector3f(obj.origin.data()), Vector3f(obj.scale.data()), Vector3f(obj.angles.data()));
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
    };
    auto anchor_state = std::make_shared<TextAnchorState>(TextAnchorState {
        .horizontal = initial_halign,
        .vertical   = initial_valign,
        .origin     = Vector3f(obj.origin.data()),
        .width      = text_bbox_w,
        .height     = text_bbox_h,
    });

    auto compose_node = rstd::sync::Arc<SceneNode>::make(
        Vector3f::Zero(), Vector3f::Ones(), Vector3f::Zero(), obj.name);
    // Layer RT must cover the source glyph bounds, not the main canvas.
    // Clock/date scripts often render a large text source and shrink it with
    // the scene transform when composing into the world.
    const float                    object_w = obj.size[0] > 0.0f ? obj.size[0] : text_bbox_w;
    const float                    object_h = obj.size[1] > 0.0f ? obj.size[1] : text_bbox_h;
    const text::TextGeometryPolicy geometry_policy {
        .frame_width        = object_w,
        .frame_height       = object_h,
        .dynamic            = wants_dynamic_text,
        .has_effect         = has_text_effect,
        .preserve_text_bbox = has_bg || obj.copybackground,
    };
    const auto initial_geometry = text::ResolveTextGeometry(geometry_policy, layouter->Metrics());
    const auto [initial_layer_w, initial_layer_h] = TextLayerExtent(initial_geometry);
    auto runtime_targets                          = std::make_shared<TextRuntimeTargets>();
    {
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
        runtime_targets->layer_w        = initial_layer_w;
        runtime_targets->layer_h        = initial_layer_h;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        scene.cameras[addr] =
            std::make_shared<SceneCamera>(initial_layer_w, initial_layer_h, -1.0f, 1.0f);
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
        compose_node->SetSize({ initial_geometry.draw_width, initial_geometry.draw_height });

        auto layer = std::make_shared<SceneImageEffectLayer>(has_text_effect ? compose_node.as_ptr()
                                                                             : sp_node.as_ptr(),
                                                             static_cast<float>(initial_layer_w),
                                                             static_cast<float>(initial_layer_h),
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

        ShaderValueMap effect_base             = context.global_base_uniforms;
        effect_base[std::string(G_COLOR4)]     = std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
        effect_base[std::string(G_COLOR)]      = std::array<float, 3> { 1.0f, 1.0f, 1.0f };
        effect_base[std::string(G_ALPHA)]      = 1.0f;
        effect_base[std::string(G_USERALPHA)]  = 1.0f;
        effect_base[std::string(G_BRIGHTNESS)] = 1.0f;

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
            LoadConstvalue(mat, pt_mat, si);
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
                if (! wpeffobj.visible && wpeffobj.visible_user.empty()) continue;

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
                    LoadConstvalue(mat, wpmat, shader_info, &final_quad_shader_values);

                    auto mesh = std::make_shared<SceneMesh>();
                    mesh->AddMaterial(std::move(mat));
                    RegisterShaderUserVarIndex(&scene, mesh->Material(), wpmat, shader_info);
                    if (user_texture_fallback.has_value()) {
                        RegisterMaterialUserTextureIndex(
                            &scene, mesh->Material(), *user_texture_fallback);
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

                if (effect_ok)
                    layer->AddEffect(effect);
                else
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
                    { static_cast<float>(runtime_targets->layer_w),
                      static_cast<float>(runtime_targets->layer_h) });
        auto loaded = load_passthrough_material(compose_node.as_ptr(),
                                                has_text_effect ? effect_final : ppong_a);
        if (! loaded.has_value()) return;
        compose_sv                         = std::move(loaded->sv);
        compose_sv.parallaxDepth           = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagatedParallaxDepth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_mesh->AddMaterial(std::move(loaded->material));
        RegisterShaderUserVarIndex(
            &scene, compose_mesh->Material(), loaded->source, loaded->shader_info);
        compose_node->AddMesh(compose_mesh);
        context.shader_updater->SetNodeData(compose_node.as_ptr(), compose_sv);

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
    }

    auto compose_hold      = SceneNodeArcHold(compose_node.clone());
    auto apply_text_anchor = [compose_hold, anchor_state]() {
        auto* compose_ptr = compose_hold.get();
        auto  contains    = [](const std::string& value, std::string_view token) {
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


    // Per-frame compose-quad rebuild: world card sized to current visible
    // source bbox. The quad offset keeps glyphs at their logical text-box
    // position after the private RT path centers them for UV cropping.

    auto rebuild_compose = [compose_hold,
                            anchor_state,
                            apply_text_anchor,
                            runtime_targets,
                            sp_mesh,
                            geometry_policy,
                            text_padding = style.padding](text::TextLayoutMetrics metrics) {
        auto* compose_ptr         = compose_hold.get();
        metrics.padding           = text_padding;
        const auto geometry       = text::ResolveTextGeometry(geometry_policy, metrics);
        const bool target_changed = runtime_targets->Apply(geometry);
        anchor_state->width       = geometry.draw_width;
        anchor_state->height      = geometry.draw_height;
        compose_ptr->SetSize({ geometry.draw_width, geometry.draw_height });
        apply_text_anchor();
        const float                 hx = geometry.draw_width * 0.5f;
        const float                 hy = geometry.draw_height * 0.5f;
        const float                 cx = geometry.draw_offset_x;
        const float                 cy = geometry.draw_offset_y;
        const std::array<float, 12> pos {
            cx - hx, cy - hy, 0.0f, cx - hx, cy + hy, 0.0f,
            cx + hx, cy - hy, 0.0f, cx + hx, cy + hy, 0.0f,
        };
        const float u_half =
            0.5f * std::min(1.0f, geometry.uv_source_width / float(runtime_targets->layer_w));
        const float v_half =
            0.5f * std::min(1.0f, geometry.uv_source_height / float(runtime_targets->layer_h));
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
                          font_blob      = resolved.bytes,
                          sp_mesh,
                          layouter,
                          rebuild_compose,
                          current_text,
                          current_point_size](double next_point_size) {
        if (scene == nullptr || font_cache_ptr == nullptr || ! std::isfinite(next_point_size) ||
            next_point_size <= 0.0) {
            return;
        }
        auto* next_face = font_cache_ptr->GetFace(
            font_blob, TextPointSizeToPx(static_cast<float>(next_point_size)));
        if (next_face == nullptr) return;
        next_face->Populate(text::DecodeUtf8(*current_text));
        if (! EnsureTextAtlas(*scene, *next_face)) return;
        *current_point_size = next_point_size;
        if (auto* mat = sp_mesh->Material()) {
            auto mutation = scene->SetMaterialTextureSlot(*mat, 0, next_face->AtlasUrl());
            // The slot swap alone doesn't rebind the GPU descriptor. Queue the
            // changed material so RenderSetUserProperty rebinds the new atlas;
            // without this the mesh gets new-layout UVs while the GPU still
            // samples the old atlas → glyphs shatter on size change.
            if (mutation.changed && mutation.material.has_value()) {
                scene->QueueTextTextureRefresh(*mutation.material);
            }
        }
        layouter->SetFace(next_face);
        rebuild_compose(layouter->Metrics());
    };

    if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
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
        context, compose_node, obj.field_bindings, apply_text_origin, apply_text_scale);
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
        const auto& sb = text_binding_it->second;
        if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
        auto&       ss  = *context.script_scene;
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::String,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
            ss.AddActuator({
                fs,
                [set_text](const script::ScriptValue& v) {
                    if (auto* p = std::get_if<script::StringValue>(&v)) set_text(p->s);
                },
            });
        }
    }
    if (has_pointsize_script) {
        const auto& sb = pointsize_binding_it->second;
        if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
        auto&       ss  = *context.script_scene;
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::Scalar,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
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
    if (wants_dynamic_text) {
        if (! context.script_scene) context.script_scene = std::make_unique<script::ScriptScene>();
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

    std::vector<rstd::sync::Arc<SceneNode>> text_before_nodes;
    text_before_nodes.push_back(sp_node.clone());
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
    if constexpr (std::is_same_v<T, wpscene::ImageObject>) {
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
    // Image objects keep going even when visible=false: another layer's
    // material may reference them via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder later decides whether to actually emit passes.
    if constexpr (! std::is_same_v<T, wpscene::ImageObject>) {
        constexpr bool preserve_user_visibility = ! std::is_same_v<T, wpscene::SoundObject>;
        if (! scene_obj.visible && ! preserve_hidden_link_source &&
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
                          rstd::Option<rstd::ref<rstd::json::Map>> user_properties) {
    ParseContext context;
    InitContext(context, vfs, sc, ortho_extent);
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

std::unordered_map<std::string, std::vector<sr::SceneNode*>>
SpawnCreateLayerAssetClones(ParseContext& context, std::int32_t owner_id, std::string_view source) {
    constexpr unsigned                                            pool_size = 8;
    std::unordered_map<std::string, std::vector<sr::SceneNode*>> out;
    if (source.find("createLayer") == std::string_view::npos) return out;

    auto owner_it = context.node_id_map.find(owner_id);
    if (owner_it == context.node_id_map.end() || owner_it->second.node.is_none()) return out;

    for (const auto& asset : DetectRegisteredAssets(source)) {
        if (! sstart_with(asset, "models/") || ! asset.ends_with(".json")) continue;
        auto size = ResolveImageAssetSize(context, asset);
        if (! size) continue;
        auto& nodes = out[asset];
        nodes.reserve(pool_size);
        for (unsigned i = 0; i < pool_size; ++i) {
            wpscene::ImageObject image;
            auto size_str = std::to_string((*size)[0]) + " " + std::to_string((*size)[1]);
            auto object   = rstd::json::Map::make();
            auto set      = [&](std::string_view key, Json value) {
                object.insert(::alloc::string::String::make(rstd::cppstd::as_str(key)),
                              rstd::move(value));
            };
            set("id", rstd::into<Json>(context.next_dynamic_layer_id--));
            set("name", JsonFromStd("__createLayer:" + asset));
            set("image", JsonFromStd(asset));
            set("origin", JsonFromStd("0 0 0"));
            set("angles", JsonFromStd("0 0 0"));
            set("scale", JsonFromStd("1 1 1"));
            set("size", JsonFromStd(size_str));
            set("visible", rstd::into<Json>(true));
            auto json = Json::Object(rstd::move(object));
            if (! image.FromJson(json, *context.vfs)) continue;
            ParseImageObj(context, image);
            auto it = context.node_id_map.find(image.id);
            if (it == context.node_id_map.end() || it->second.node.is_none()) continue;
            auto node = (*it->second.node).clone();
            node->SetVisible(false);
            nodes.push_back(node.as_ptr());
            context.layer_clones[owner_id].push_back(std::move(node));
            context.node_id_map.erase(it);
        }
        if (nodes.empty()) out.erase(asset);
    }
    return out;
}

void ResolveCreateLayerAssetRequests(ParseContext& context) {
    for (auto& req : context.create_layer_asset_requests) {
        if (! req.script) continue;
        auto queues = SpawnCreateLayerAssetClones(context, req.owner_id, req.source);
        for (auto& [asset, nodes] : queues) {
            req.script->AddAssetCloneQueue(std::move(asset), std::move(nodes));
        }
    }
    context.create_layer_asset_requests.clear();
}

void ProcessObjects(ParseContext& context, std::span<SceneObjectVar> scene_objs,
                    wavsen::audio::SoundManager* sm, ProcessOpts opts) {
    WPShaderParser::InitGlslang();
    IndexSystemMediaImageFallbacks(context, scene_objs);

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

    ResolveCreateLayerAssetRequests(context);
    WPShaderParser::FinalGlslang();
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
        for (auto& before_node : ref.ordered_before_nodes) {
            parent_node->AppendChild(before_node.clone());
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
        context.script_scene->runtime().SetScene(context.scene.get());
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
        LoadConstvalue(material, wpmat, wpShaderInfo);

        auto pp_mesh = std::make_shared<SceneMesh>();
        pp_mesh->ChangeMeshDataFrom(scene.default_effect_mesh);
        pp_mesh->AddMaterial(std::move(material));
        RegisterShaderUserVarIndex(&scene, pp_mesh->Material(), wpmat, wpShaderInfo);
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
    auto       context      = BuildContext(vfs, scene_id, sc, ortho_extent, m_user_properties);
    context.scene_layer_text_writes = SceneWritesLayerText(scene_objs);
    context.scene->uses_audio_spectrum = SceneUsesAudioScripts(scene_objs);
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
    if (auto objects = json.get("objects"); objects.is_some()) {
        auto object_array = (*objects)->as_array();
        if (object_array.is_none()) return context.scene;
        auto visibility_info = BuildObjectVisibilityInfo(json, m_user_properties);
        auto has_kind        = [](const Json& o) {
            for (const char* k :
                 { "image", "particle", "sound", "light", "text", "model", "camera" }) {
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
            context.node_id_order.push_back(id);
            std::uint32_t parent = 0;
            sr::GetJsonValue(o, "parent", parent, false);
            context.object_parent_ids[id] = parent;
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
                bool visible =
                    vit->second.visible &&
                    ! HasHiddenUserAncestor(static_cast<std::uint32_t>(id), visibility_info);
                if (! visible) node->SetVisible(false);
            }
            wpscene::VisibleUserBinding visible_user;
            wpscene::ReadVisibleUserBinding(o, visible_user);
            if (! visible_user.empty())
                node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(visible_user));
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

    if (sc.general.bloom) {
        BuildBloomPostProcess(context, vfs, sc.general);
    }
    return FinalizeScene(context);
}
