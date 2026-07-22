module;

#include <rstd/macro.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>

module sr.scene_wallpaper;
import sr.types;
import sr.utils;
import sr.scene;
import sr.spec_texs;

import eigen;
import sr.json;
import sr.user_property;
import rstd.log;
import rstd.cppstd;
import wavsen.audio;
import sr.fs;
import sr.message_loop;
import sr.timer;
import sr.pkg.parse;
import sr.pkg_fs;
import sr.rgraph;
import sr.script;
import sr.vulkan_render;

using namespace sr;

namespace sr
{

// ---- Render-thread messages -------------------------------------------------

struct RenderInit {
    std::shared_ptr<RenderInitInfo> info;
};
struct RenderSetScene {
    std::shared_ptr<Scene> scene;
};
struct RenderSetFillMode {
    FillMode mode;
};
struct RenderSetSpeed {
    float speed;
};
struct RenderSetUserProperty {
    std::string key;
    Json        property;
};
struct RenderSetMediaStatus {
    MediaStatus status;
};
struct RenderStop {
    bool stop;
};
struct RenderDraw {};
struct RenderSwapchainReady {
    bool     ready;
    uint32_t width;
    uint32_t height;
};
struct RenderRequestPreparedPassDiagnostics {
    RenderPassDiagnosticCallback cb;
};

// Wrapped in a non-std struct so the rstd channel's internal `addressof`
// calls don't fall into ADL ambiguity with std::addressof when the element
// type sits in namespace std.
struct RenderMsg {
    std::variant<RenderInit, RenderSetScene, RenderSetFillMode, RenderSetSpeed,
                 RenderSetUserProperty, RenderSetMediaStatus, RenderStop, RenderDraw,
                 RenderSwapchainReady, RenderRequestPreparedPassDiagnostics>
        v;
};

// ---- Main-thread messages ---------------------------------------------------

struct MainLoadScene {};
struct MainStop {
    bool     stop;
    uint32_t fade_ms { 0 };
    bool     scale_audio { false };
};
struct MainPauseAudio {
    uint64_t generation { 0 };
};
struct MainFirstFrame {};
struct MainConfigure {
    SceneWallpaperConfig config;
};
struct MainSetFps {
    uint32_t fps { 0 };
};
struct MainSetVolume {
    float volume { 1.0f };
};
struct MainSetVolumeScale {
    float    scale { 1.0f };
    uint32_t fade_ms { 0 };
};
struct MainSetMuted {
    bool muted { false };
};
struct MainSetFillMode {
    FillMode mode { FillMode::ASPECTCROP };
};
struct MainSetSpeed {
    float speed { 1.0f };
};
struct MainSetUserProperty {
    std::string key;
    Json        value;
};
struct MainSetFirstFrameCallback {
    FirstFrameCallback cb;
};
struct MainSetUserPropertyDiagnosticCallback {
    UserPropertyDiagnosticCallback cb;
};
struct MainUserPropertyDiagnostics {
    std::vector<SceneUserPropertyDiagnostic> diagnostics;
};
struct MainPreparedPassDiagnostics {
    RenderPassDiagnosticCallback                cb;
    std::vector<vulkan::PreparedPassDiagnostic> diagnostics;
};

struct MainMsg {
    std::variant<MainLoadScene, MainConfigure, MainSetFps, MainSetVolume, MainSetVolumeScale,
                 MainSetMuted, MainSetFillMode, MainSetSpeed, MainSetUserProperty,
                 MainSetFirstFrameCallback, MainSetUserPropertyDiagnosticCallback,
                 MainUserPropertyDiagnostics, MainPreparedPassDiagnostics, MainStop, MainPauseAudio,
                 MainFirstFrame>
        v;
};

namespace
{

Json MakeUserPropertyDescriptor(Json value) {
    if (value.get("value").is_some()) return value;
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")), rstd::move(value));
    return Json::Object(rstd::move(object));
}

Json RawUserProperty(std::string_view value) { return MakeUserPropertyWirePatch(value); }

Json InitialUserProperty(Json value) {
    if (value.is_string()) {
        auto raw = rstd::cppstd::to_string(*value.as_str());
        return RawUserProperty(raw);
    }
    return MakeUserPropertyDescriptor(std::move(value));
}

bool IsShaderGraphUserProperty(const Json& prop) {
    auto type = prop.get("type");
    if (type.is_none()) return false;
    auto string = (*type)->as_str();
    return string.is_some() && rstd::cppstd::as_string_view(*string) == "combo";
}

constexpr std::string_view kSchemeColorKey          = "schemecolor";
constexpr std::string_view kSrSchemeColorKey = "scenerenderer.scheme_color";

std::string CanonicalUserPropertyKey(std::string_view key) {
    if (key == kSrSchemeColorKey) return std::string(kSchemeColorKey);
    return std::string(key);
}

// Parse a "r g b" / "r g b a" / "x y z w ..." space-separated float string into
// a small float vector. Trailing / leading whitespace is tolerated. Returns
// false when no numbers parse — caller treats as coercion failure.
bool ParseFloatList(std::string_view s, std::vector<float>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        std::string tok(s.substr(start, i - start));
        try {
            out.push_back(std::stof(tok));
        } catch (...) {
            return false;
        }
    }
    return ! out.empty();
}

// Local wall-clock time of day as a 0..1 fraction (0 = midnight, 0.5 = noon).
// Wallpaper Engine's `engine.timeOfDay` is exactly this: scripts multiply by
// 24 to recover the hour. Uses the host's local timezone via localtime so
// day/night scenes track the user's actual clock.
float LocalTimeOfDay() {
    const std::time_t now = std::time(nullptr);
    std::tm           tm_local {};
#if defined(_WIN32)
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif
    const float seconds = static_cast<float>(tm_local.tm_hour) * 3600.0f +
                          static_cast<float>(tm_local.tm_min) * 60.0f +
                          static_cast<float>(tm_local.tm_sec);
    float frac = seconds / 86400.0f;
    if (! std::isfinite(frac) || frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac;
}

// Coerce a project.json property entry into a ShaderValue. Returns ok=false
// (with skip_reason) for combo / texture / unsupported types — the handler
// logs and skips those.
struct UserPropertyCoerceResult {
    bool        ok { false };
    ShaderValue value;
    const char* skip_reason { nullptr };
};

UserPropertyCoerceResult CoerceUserPropertyValue(const Json& prop) {
    UserPropertyCoerceResult r;

    // Pull the explicit type if present; project.json properties always have
    // one, but inline {"value": ...} descriptors don't.
    std::string type;
    if (auto member = prop.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }

    // Combo / texture / file paths can't write a uniform.
    if (type == "combo") {
        r.skip_reason = "shader graph mutation is not a uniform update";
        return r;
    }
    if (type == "texture" || type == "replacetexture" || type == "file" || type == "textinput") {
        r.skip_reason = "non-uniform property type";
        return r;
    }

    // Find the raw value.
    auto        value = prop.get("value");
    const Json& v     = value.is_some() ? **value : prop;

    if (type == "color") {
        std::vector<float> nums;
        auto               value = v.as_str();
        if (value.is_some() && ParseFloatList(rstd::cppstd::as_string_view(*value), nums) &&
            nums.size() >= 3) {
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "color value not a 'r g b[ a]' float string";
        return r;
    }

    // Fallback inference when type is missing.
    if (v.is_boolean()) {
        r.ok    = true;
        float f = *v.as_bool() ? 1.0f : 0.0f;
        r.value = ShaderValue(f);
        return r;
    }
    if (v.is_number()) {
        auto number = v.as_f64();
        r.ok        = number.is_some() && *number >= std::numeric_limits<float>::lowest() &&
                      *number <= std::numeric_limits<float>::max();
        if (r.ok) r.value = ShaderValue(static_cast<float>(*number));
        return r;
    }
    if (v.is_string()) {
        std::vector<float> nums;
        auto               value = rstd::cppstd::as_string_view(*v.as_str());
        if (ParseFloatList(value, nums)) {
            if (nums.size() == 1) {
                r.ok    = true;
                r.value = ShaderValue(nums[0]);
                return r;
            }
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "string value isn't parseable as float list";
        return r;
    }
    r.skip_reason = "unsupported JSON value shape";
    return r;
}

void ApplyUserPropertyToClear(Scene& scene, const std::string& key, const Json& prop) {
    if (scene.clearColorUserKey.empty()) return;
    if (CanonicalUserPropertyKey(scene.clearColorUserKey) != key) return;
    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 3) return;
    auto clamp01 = [](float n) {
        return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
    };
    scene.clearColor = {
        clamp01(coerced.value[0]),
        clamp01(coerced.value[1]),
        clamp01(coerced.value[2]),
    };
}

// Push a user-property value to every material whose shader declared a
// `u_*` uniform with this material-key.
void ApplyUserPropertyToShaderUniforms(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.shader_user_var_index.find(key);
    if (it == scene.shader_user_var_index.end()) return;

    if (IsShaderGraphUserProperty(prop)) {
        rstd_warn("user property '{}' skipped: shader graph mutation is not a uniform update", key);
        return;
    }

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok) {
        rstd_warn("user property '{}' skipped: {}",
                  key,
                  coerced.skip_reason ? coerced.skip_reason : "unknown");
        return;
    }
    for (auto& [material, uniform_name] : it->second) {
        if (! material) continue;
        scene.SetMaterialShaderValue(*material, uniform_name, coerced.value);
    }
}

std::optional<std::string> ResolveRuntimeSceneTextureProperty(const Json& prop) {
    if (prop.is_string()) {
        return rstd::cppstd::to_string(*prop.as_str());
    }
    if (! prop.is_object()) return std::nullopt;

    std::string type;
    if (auto member = prop.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return std::nullopt;
    auto value = prop.get("value");
    if (value.is_none()) return std::nullopt;
    auto string = (*value)->as_str();
    return string.is_some() ? std::optional<std::string>(rstd::cppstd::to_string(*string))
                            : std::nullopt;
}

bool SameSceneMaterialId(SceneMaterialId lhs, SceneMaterialId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueMaterialId(std::vector<SceneMaterialId>& materials, SceneMaterialId id) {
    auto it = std::find_if(materials.begin(), materials.end(), [id](auto existing) {
        return SameSceneMaterialId(existing, id);
    });
    if (it == materials.end()) materials.push_back(id);
}

vulkan::PassInvalidationFlags MaterialDirtyToPassInvalidationFlags(SceneMaterialDirtyFlags flags) {
    vulkan::PassInvalidationFlags out = vulkan::PassInvalidationNone;
    if ((flags & SceneMaterialDirtyResources) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources);
    }
    if ((flags & SceneMaterialDirtyPipeline) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Pipeline) |
               vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Framebuffer);
    }
    return out;
}

std::vector<SceneMaterialId>
ApplyUserPropertyToMaterialTextures(Scene& scene, const std::string& key, const Json& prop) {
    std::vector<SceneMaterialId> changed_materials;
    auto                         it = scene.material_texture_user_index.find(key);
    if (it == scene.material_texture_user_index.end()) return changed_materials;

    auto texture_value = ResolveRuntimeSceneTextureProperty(prop);
    if (! texture_value.has_value()) return changed_materials;

    for (const auto& binding : it->second) {
        if (binding.material == nullptr) continue;
        std::string next     = texture_value->empty() ? binding.fallback : *texture_value;
        auto        mutation = scene.SetMaterialTextureSlot(*binding.material, binding.slot, next);
        if (mutation.changed && mutation.material.has_value()) {
            PushUniqueMaterialId(changed_materials, *mutation.material);
        }
    }

    return changed_materials;
}

Json RuntimeTextureProperty(std::string value) {
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("type")),
                  JsonFromStd("scenetexture"));
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")), JsonFromStd(value));
    return Json::Object(rstd::move(object));
}

sr::script::MediaStatus ToScriptMediaStatus(const MediaStatus& status) {
    return sr::script::MediaStatus { .state            = status.state,
                                      .title            = status.title,
                                      .artist           = status.artist,
                                      .album            = status.album,
                                      .album_artist     = status.album_artist,
                                      .art_url          = status.art_url,
                                      .previous_art_url = status.previous_art_url };
}

std::vector<SceneUserPropertyDiagnostic> CollectUserPropertyDiagnostics(const Scene&     scene,
                                                                        std::string_view key) {
    std::vector<SceneUserPropertyDiagnostic> out;
    for (const auto& diagnostic : scene.UserPropertyDiagnostics()) {
        if (diagnostic.key == key) out.push_back(diagnostic);
    }
    return out;
}

std::optional<std::string>
ResolveRuntimeShaderComboValue(const Json& prop, const Scene::ShaderComboUserBinding& binding) {
    auto        member = prop.get("value");
    const auto& value  = member.is_some() ? **member : prop;

    if (value.is_null()) return binding.fallback;
    if (value.is_boolean()) return *value.as_bool() ? "1" : "0";
    if (value.is_number()) {
        auto number = value.as_f64();
        if (number.is_some() && *number >= std::numeric_limits<int>::min() &&
            *number <= std::numeric_limits<int>::max())
            return std::to_string(static_cast<int>(*number));
        return std::nullopt;
    }
    if (! value.is_string()) return std::nullopt;

    auto text = rstd::cppstd::to_string(*value.as_str());
    if (text.empty()) return binding.fallback;
    if (auto it = binding.options.find(text); it != binding.options.end()) return it->second;
    if (text == "true") return "1";
    if (text == "false") return "0";

    try {
        std::size_t parsed = 0;
        int         number = std::stoi(text, &parsed);
        if (parsed == text.size()) return std::to_string(number);
    } catch (...) {
    }
    return std::nullopt;
}

void RecordShaderComboDiagnostic(Scene& scene, std::string key,
                                 SceneUserPropertyDiagnosticCode code, std::string material,
                                 std::string combo, std::string message) {
    scene.AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic {
        .key      = std::move(key),
        .code     = code,
        .material = std::move(material),
        .combo    = std::move(combo),
        .message  = std::move(message),
    });
}

bool ApplyUserPropertyToShaderCombos(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.shader_combo_user_index.find(key);
    if (it == scene.shader_combo_user_index.end()) return false;

    scene.ClearUserPropertyDiagnostics(key);

    auto* vfs = static_cast<fs::VFS*>(scene.vfs.get());
    if (! vfs) {
        rstd_warn("user property '{}' skipped: scene VFS is not available", key);
        RecordShaderComboDiagnostic(scene,
                                    key,
                                    SceneUserPropertyDiagnosticCode::SceneVfsUnavailable,
                                    {},
                                    {},
                                    "scene VFS is not available");
        return false;
    }

    bool requires_graph_rebuild = false;
    for (const auto& binding : it->second) {
        if (! binding.material) continue;
        auto next = ResolveRuntimeShaderComboValue(prop, binding);
        if (! next.has_value()) {
            rstd_warn(
                "user property '{}' skipped: combo '{}' value is unsupported", key, binding.combo);
            RecordShaderComboDiagnostic(
                scene,
                key,
                SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue,
                binding.material ? binding.material->name : std::string {},
                binding.combo,
                "shader combo value is unsupported");
            continue;
        }
        auto& material = *binding.material;
        if (! material.customShader.variant.has_value()) {
            rstd_warn("user property '{}' skipped: material '{}' has no shader variant descriptor",
                      key,
                      material.name);
            RecordShaderComboDiagnostic(
                scene,
                key,
                SceneUserPropertyDiagnosticCode::MissingShaderVariantDescriptor,
                material.name,
                binding.combo,
                "material has no shader variant descriptor");
            continue;
        }
        const auto& current_variant = *material.customShader.variant;
        if (auto current = current_variant.resolved_combos.find(binding.combo);
            current != current_variant.resolved_combos.end() && current->second == *next) {
            continue;
        }

        auto compiled = WPShaderParser::CompileSceneShaderVariant(
            current_variant, *vfs, { { binding.combo, *next } });
        if (! compiled.ok || ! compiled.shader) {
            rstd_warn("user property '{}' skipped: shader combo '{}' compile failed: {}",
                      key,
                      binding.combo,
                      compiled.error);
            RecordShaderComboDiagnostic(scene,
                                        key,
                                        SceneUserPropertyDiagnosticCode::ShaderComboCompileFailed,
                                        material.name,
                                        binding.combo,
                                        compiled.error);
            continue;
        }
        auto mutation = scene.SetMaterialShaderVariant(material,
                                                       SceneShaderVariantMutation {
                                                           .shader  = std::move(compiled.shader),
                                                           .variant = std::move(compiled.variant),
                                                       });
        if (mutation.changed && (material.DirtyFlags() & SceneMaterialDirtyGraph) != 0) {
            requires_graph_rebuild = true;
        }
    }
    return requires_graph_rebuild;
}

float CurrentImagePropertyAlpha(SceneNode* node) {
    if (! node) return 1.0f;
    return node->IsAlphaOverridden() ? node->EffectiveAlpha() : node->BaseAlpha();
}

Eigen::Vector3f CurrentImagePropertyColor(SceneNode* node) {
    if (! node) return { 1.0f, 1.0f, 1.0f };
    return node->IsColorOverridden() ? node->Color() : node->BaseColor();
}

bool MaterialHasShaderUniform(const SceneMaterial& material, std::string_view uniform_name) {
    const std::string name(uniform_name);
    if (material.customShader.constValues.contains(name)) return true;
    if (material.customShader.shader &&
        material.customShader.shader->default_uniforms.contains(name))
        return true;
    if (material.customShader.variant &&
        material.customShader.variant->default_uniforms.contains(name))
        return true;
    return false;
}

void ApplyUserPropertyToImageColor(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.image_color_user_index.find(key);
    if (it == scene.image_color_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 3) return;

    Eigen::Vector3f color { coerced.value[0], coerced.value[1], coerced.value[2] };
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetColor(color);

        std::array<float, 3> color3 { color.x(), color.y(), color.z() };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool           has_user_alpha = MaterialHasShaderUniform(*material, G_USERALPHA);
            const float          alpha          = has_user_alpha && binding.node
                                                      ? binding.node->BaseAlpha()
                                                      : CurrentImagePropertyAlpha(binding.node);
            std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
            if (MaterialHasShaderUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
            if (MaterialHasShaderUniform(*material, G_COLOR))
                scene.SetMaterialShaderValue(*material, G_COLOR, color3);
        }
    }
}

void ApplyUserPropertyToImageAlpha(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.image_alpha_user_index.find(key);
    if (it == scene.image_alpha_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;

    const float alpha = std::clamp(coerced.value[0], 0.0f, 1.0f);
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetUserAlpha(alpha);

        Eigen::Vector3f      color = CurrentImagePropertyColor(binding.node);
        std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool has_user_alpha = MaterialHasShaderUniform(*material, G_USERALPHA);
            if (has_user_alpha) scene.SetMaterialShaderValue(*material, G_USERALPHA, alpha);
            if (MaterialHasShaderUniform(*material, G_ALPHA))
                scene.SetMaterialShaderValue(*material, G_ALPHA, alpha);
            if (! has_user_alpha && MaterialHasShaderUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
        }
    }
}

// Drive text layers whose `text` field was authored as `{user:"<key>"}`.
// The string value is taken verbatim (no float coercion) so custom text —
// including CJK / emoji — reaches the layouter unchanged. Runs on the render
// thread; the registered closures rebuild the glyph atlas + compose quad.
void ApplyUserPropertyToText(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.text_user_index.find(key);
    if (it == scene.text_user_index.end()) return;

    auto        value = prop.get("value");
    const Json& v     = value.is_some() ? **value : prop;
    auto        text   = v.as_str();
    if (text.is_none()) return;
    for (const auto& setter : it->second) {
        if (setter) setter(rstd::cppstd::to_string(*text));
    }
}

// Drive text layers whose `pointsize` field was authored as `{user:"<key>"}`.
// Coerces the scalar the same way sliders do, then re-rasterises the glyph
// atlas at the new size via the registered setter.
void ApplyUserPropertyToPointSize(Scene& scene, const std::string& key,
                                  const Json& prop) {
    auto it = scene.pointsize_user_index.find(key);
    if (it == scene.pointsize_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;
    const double point_size = static_cast<double>(coerced.value[0]);
    if (! (point_size > 0.0)) return;
    for (const auto& setter : it->second) {
        if (setter) setter(point_size);
    }
}

// Drive text-layer `color` user bindings. Text color is baked into glyph
// vertex colors, so this re-runs the layout with the new RGB.
void ApplyUserPropertyToTextColor(Scene& scene, const std::string& key,
                                  const Json& prop) {
    auto it = scene.text_color_user_index.find(key);
    if (it == scene.text_color_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 3) return;
    const float r = coerced.value[0];
    const float g = coerced.value[1];
    const float b = coerced.value[2];
    for (const auto& setter : it->second) {
        if (setter) setter(r, g, b);
    }
}

// Drive text-layer `alpha` user bindings.
void ApplyUserPropertyToTextAlpha(Scene& scene, const std::string& key,
                                  const Json& prop) {
    auto it = scene.text_alpha_user_index.find(key);
    if (it == scene.text_alpha_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;
    const float alpha = std::clamp(coerced.value[0], 0.0f, 1.0f);
    for (const auto& setter : it->second) {
        if (setter) setter(alpha);
    }
}

// Push a user-property value into every particle subsystem whose
// instanceoverride was authored as `{user:"<key>", value:...}` for one of
// its fields. The override sits behind a shared_ptr; mutating it through the
// scene-wide binding index is observed by every initializer / operator
// closure on next emission.
void ApplyUserPropertyToParticles(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.particle_user_var_index.find(key);
    if (it == scene.particle_user_var_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok) return;

    auto write_scalar = [&](float& dst) {
        if (coerced.value.size() >= 1) dst = coerced.value[0];
    };
    auto write_vec3 = [&](std::array<float, 3>& dst, float scale) {
        if (coerced.value.size() < 3) return;
        dst = { coerced.value[0] * scale, coerced.value[1] * scale, coerced.value[2] * scale };
    };

    for (auto& b : it->second) {
        if (! b.state) continue;
        auto*              st = static_cast<sr::wpscene::ParticleInstanceoverride*>(b.state.get());
        const std::string& f  = b.field;
        if (f == "alpha")
            write_scalar(st->alpha);
        else if (f == "size")
            write_scalar(st->size);
        else if (f == "lifetime")
            write_scalar(st->lifetime);
        else if (f == "rate")
            write_scalar(st->rate);
        else if (f == "speed")
            write_scalar(st->speed);
        else if (f == "count")
            write_scalar(st->count);
        else if (f == "brightness")
            write_scalar(st->brightness);
        else if (f == "color") {
            // `color` is 0..255 in the JSON; the init op divides by 255.
            write_vec3(st->color, 255.0f);
            st->overColor = true;
        } else if (f == "colorn") {
            write_vec3(st->colorn, 1.0f);
            st->overColorn = true;
        } else if (f.starts_with("controlpoint") && ! f.starts_with("controlpointangle")) {
            int idx = -1;
            try {
                idx = std::stoi(f.substr(std::string_view("controlpoint").size()));
            } catch (...) {
            }
            if (idx >= 0 && idx < 8) write_vec3(st->controlpoint[idx], 1.0f);
        } else if (f.starts_with("controlpointangle")) {
            int idx = -1;
            try {
                idx = std::stoi(f.substr(std::string_view("controlpointangle").size()));
            } catch (...) {
            }
            if (idx >= 0 && idx < 8) write_vec3(st->controlpointangle[idx], 1.0f);
        }
    }
}

void ApplyUserPropertyToSoundVolume(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.sound_volume_user_index.find(key);
    if (it == scene.sound_volume_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;
    const float volume = std::clamp(coerced.value[0], 0.0f, 1.0f);
    for (auto& control : it->second) {
        if (control) control->SetVolume(volume);
    }
}

void ApplyUserPropertyToCameraParallax(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.camera_parallax_user_var_index.find(key);
    if (it == scene.camera_parallax_user_var_index.end() || ! scene.shaderValueUpdater) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;

    float value = coerced.value[0];
    for (const auto& field : it->second) {
        if (field == "cameraparallaxmouseinfluence")
            scene.shaderValueUpdater->SetCameraParallaxMouseInfluence(value);
    }
}

void ApplyUserPropertyToCameraShake(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.camera_shake_user_var_index.find(key);
    if (it == scene.camera_shake_user_var_index.end() || ! scene.shaderValueUpdater) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < 1) return;

    float value = coerced.value[0];
    for (const auto& field : it->second) {
        if (field == "camerashake")
            scene.shaderValueUpdater->SetCameraShakeEnabled(value >= 0.5f);
        else if (field == "camerashakeamplitude")
            scene.shaderValueUpdater->SetCameraShakeAmplitude(value);
        else if (field == "camerashakespeed")
            scene.shaderValueUpdater->SetCameraShakeSpeed(value);
        else if (field == "camerashakeroughness")
            scene.shaderValueUpdater->SetCameraShakeRoughness(value);
    }
}

void ApplyUserPropertyToCameraPath(Scene& scene, const std::string& key, const Json& prop) {
    scene.ApplyUserCameraPathVisibilityBindings(key, prop);
}

bool ApplyUserPropertyToNodeVisibility(Scene& scene, const std::string& key, const Json& prop) {
    return scene.ApplyUserNodeVisibilityBindings(key, prop);
}

void ApplyUserPropertyBeforeFirstGraph(Scene& scene, const std::string& key, const Json& prop) {
    sr::script::SetSceneUserProperty(scene, key, prop);
    ApplyUserPropertyToClear(scene, key, prop);
    ApplyUserPropertyToShaderUniforms(scene, key, prop);
    (void)ApplyUserPropertyToMaterialTextures(scene, key, prop);
    (void)ApplyUserPropertyToShaderCombos(scene, key, prop);
    ApplyUserPropertyToImageColor(scene, key, prop);
    ApplyUserPropertyToImageAlpha(scene, key, prop);
    ApplyUserPropertyToText(scene, key, prop);
    ApplyUserPropertyToPointSize(scene, key, prop);
    ApplyUserPropertyToTextColor(scene, key, prop);
    ApplyUserPropertyToTextAlpha(scene, key, prop);
    ApplyUserPropertyToParticles(scene, key, prop);
    ApplyUserPropertyToSoundVolume(scene, key, prop);
    ApplyUserPropertyToCameraParallax(scene, key, prop);
    ApplyUserPropertyToCameraShake(scene, key, prop);
    ApplyUserPropertyToCameraPath(scene, key, prop);
    (void)ApplyUserPropertyToNodeVisibility(scene, key, prop);
    (void)scene.ApplyUserImageEffectVisibilityBindings(key, prop);
}

void MergeProjectUserProperties(const std::filesystem::path& project_dir, rstd::json::Map& out) {
    const auto    project_path = project_dir / "project.json";
    std::ifstream is(project_path);
    if (! is) return;

    std::string source(std::istreambuf_iterator<char>(is), {});
    auto        parsed = ParseJson(source, { .allow_comments = true });
    if (parsed.is_err()) {
        rstd_warn("Can't parse {}: {}", project_path.string(), parsed.unwrap_err());
        return;
    }
    auto root    = parsed.unwrap();
    auto general = root.get("general");
    if (general.is_none()) return;
    auto properties = (*general)->get("properties");
    if (properties.is_none()) return;
    auto object = (*properties)->as_object();
    if (object.is_none()) return;

    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  raw_key           = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string key               = CanonicalUserPropertyKey(raw_key);
        auto        current           = out.get(rstd::cppstd::as_str(key));
        auto        descriptor = current.is_some() ? MergeUserPropertyDescriptor(value, **current)
                                                   : MakeUserPropertyDescriptor(value.clone());
        out.insert(::alloc::string::String::make(rstd::cppstd::as_str(key)), std::move(descriptor));
    });
}

rstd::json::Map NormalizeUserProperties(const rstd::json::Map& input) {
    auto out = rstd::json::Map::make();
    input.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string canonical         = CanonicalUserPropertyKey(key);
        if (key == canonical || out.get(rstd::cppstd::as_str(canonical)).is_none()) {
            out.insert(::alloc::string::String::make(rstd::cppstd::as_str(canonical)),
                       InitialUserProperty(value.clone()));
        }
    });
    return out;
}

} // namespace

using MainSender   = msgloop::MessageLoop<MainMsg>::Sender;
using RenderSender = msgloop::MessageLoop<RenderMsg>::Sender;

class SceneRenderController;

class SceneRuntimeController {
public:
    SceneRuntimeController();
    ~SceneRuntimeController();

    bool init();
    auto renderController() const { return m_render_controller.get(); }
    bool inited() const { return m_inited; }

    MainSender   mainSender() { return m_main_loop.sender(); }
    RenderSender renderSender() { return m_render_loop.sender(); }

    void on(MainLoadScene&&);
    void on(MainConfigure&&);
    void on(MainSetFps&&);
    void on(MainSetVolume&&);
    void on(MainSetVolumeScale&&);
    void on(MainSetMuted&&);
    void on(MainSetFillMode&&);
    void on(MainSetSpeed&&);
    void on(MainSetUserProperty&&);
    void on(MainSetFirstFrameCallback&&);
    void on(MainSetUserPropertyDiagnosticCallback&&);
    void on(MainUserPropertyDiagnostics&&);
    void on(MainPreparedPassDiagnostics&&);
    void on(MainStop&&);
    void on(MainPauseAudio&&);
    void on(MainFirstFrame&&);

    bool isGenGraphviz() const { return m_config.graphviz; }

    void setOnClearColor(ClearColorCallback cb) { m_clear_color_cb = std::move(cb); }
    void setOnAudioDemand(AudioDemandCallback cb) { m_audio_demand_cb = std::move(cb); }

private:
    void loadScene();
    void publishPreparedScene();

    bool m_inited { false };

    SceneWallpaperConfig m_config;
    rstd::json::Map      m_user_properties;

    WPSceneParser                                m_scene_parser;
    std::unique_ptr<wavsen::audio::SoundManager> m_sound_manager;
    FirstFrameCallback                           m_first_frame_callback;
    UserPropertyDiagnosticCallback               m_user_property_diagnostic_cb;
    ClearColorCallback                           m_clear_color_cb;
    AudioDemandCallback                          m_audio_demand_cb;
    uint64_t                                     m_audio_pause_generation { 0 };
    uint64_t                                     m_config_generation { 0 };
    uint64_t                                     m_prepared_scene_generation { 0 };
    uint64_t                                     m_submitted_scene_generation { 0 };
    bool                                         m_render_ready { false };
    std::shared_ptr<Scene>                       m_prepared_scene;

    msgloop::MessageLoop<MainMsg>          m_main_loop;
    msgloop::MessageLoop<RenderMsg>        m_render_loop;
    std::unique_ptr<SceneRenderController> m_render_controller;
};

class SceneRenderController {
public:
    explicit SceneRenderController(SceneRuntimeController& main): m_main(main) {}
    ~SceneRenderController() {
        m_render->destroy();
        rstd_info("render handler deleted");
    }

    void on(RenderInit&&);
    void on(RenderSetScene&&);
    void on(RenderSetFillMode&&);
    void on(RenderSetSpeed&&);
    void on(RenderSetUserProperty&&);
    void on(RenderSetMediaStatus&&);
    void on(RenderStop&&);
    void on(RenderDraw&&);
    void on(RenderSwapchainReady&&);
    void on(RenderRequestPreparedPassDiagnostics&&);

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }
    vulkan::VulkanRender* render() const { return m_render.get(); }

    bool renderInited() const { return m_render->inited(); }
    bool sceneReady() const { return m_scene_ready.load(std::memory_order_acquire); }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

    // Edge-events for the cursor button stream. Each call from the input
    // thread sets/clears the held bit and records the edge so the next
    // TickSceneScripts can fire cursorDown/Up. fetch_or guards against
    // press-release-press coalescing between ticks (rare).
    void setMouseButton(int button, bool down) {
        if (button < 0 || button > 31) return;
        const uint32_t mask = 1u << button;
        if (down) {
            m_buttons_down.fetch_or(mask);
            m_buttons_pressed.fetch_or(mask);
        } else {
            m_buttons_down.fetch_and(~mask);
            m_buttons_released.fetch_or(mask);
        }
    }
    void     setMouseInWindow(bool in) { m_cursor_in_window.store(in); }
    uint32_t buttonsDown() const { return m_buttons_down.load(); }
    uint32_t consumePressed() { return m_buttons_pressed.exchange(0); }
    uint32_t consumeReleased() { return m_buttons_released.exchange(0); }
    bool     cursorInWindow() const { return m_cursor_in_window.load(); }

    void configureAudioCapture(bool enabled, bool external) {
        m_audio_enabled.store(enabled, std::memory_order_release);
        m_external_audio.store(enabled && external, std::memory_order_release);
        if (enabled) (void)m_audio_capture.init(! external);
    }

    void setExternalAudioSpectrum(std::array<float, 64> left,
                                  std::array<float, 64> right) {
        std::lock_guard lock(m_external_audio_mu);
        m_external_left  = std::move(left);
        m_external_right = std::move(right);
        m_external_publish_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    }

    void setSenders(RenderSender render_tx, MainSender main_tx) {
        m_render_tx.emplace(std::move(render_tx));
        m_main_tx.emplace(std::move(main_tx));
    }

    // Drop every Sender clone owned by this handler so the render channel
    // can disconnect at shutdown. The swapchain callback's sender is held
    // through `m_swapchain_tx` (strong) + a weak_ptr in the lambda — clearing
    // the strong ref turns the lambda into a no-op.
    void clearSenders() {
        m_swapchain_tx.reset();
        m_render_tx.reset();
        m_main_tx.reset();
    }

    FrameTimer frame_timer { [] {
    } };
    FpsCounter fps_counter;

private:
    void rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention, bool evict_meshes);
    void consumeDirtyEventsCoveredByGraphRebuild();
    void refreshPreparedMeshDirtyEvents();
    void refreshPreparedMaterialDirtyEvents();

    bool snapshotExternalAudio(wavsen::audio::AudioSpectrum& out) {
        if (! m_external_audio.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(m_external_audio_mu);
        if (m_external_publish_ms == 0) return false;
        out.clear();
        out.left       = m_external_left;
        out.right      = m_external_right;
        out.publish_ms = m_external_publish_ms;
        for (std::size_t i = 0; i < out.average.size(); ++i) {
            out.average[i] = 0.5f * (out.left[i] + out.right[i]);
            out.bins[i]    = out.average[i];
        }
        return true;
    }

    SceneRuntimeController& m_main;

    std::unique_ptr<vulkan::VulkanRender> m_render { std::make_unique<vulkan::VulkanRender>() };
    std::shared_ptr<Scene>                m_scene { nullptr };
    std::atomic<bool>                     m_scene_ready { false };
    RenderSceneSnapshot                   m_render_scene;
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };
    float                                 m_speed { 1.0f };
    FillMode                              m_fillmode { FillMode::ASPECTCROP };
    bool                                  m_stopped { false };

    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };
    std::atomic<uint32_t>             m_buttons_down { 0 };
    std::atomic<uint32_t>             m_buttons_pressed { 0 };
    std::atomic<uint32_t>             m_buttons_released { 0 };
    std::atomic<bool>                 m_cursor_in_window { false };

    std::optional<RenderSender> m_render_tx;
    std::optional<MainSender>   m_main_tx;

    // Strong ref kept here, weak copy captured by the swapchain callback;
    // nulling this out at shutdown lets the callback short-circuit so the
    // render channel can actually reach Err on recv().
    std::shared_ptr<RenderSender> m_swapchain_tx;

    wavsen::audio::AudioCapture m_audio_capture;
    std::atomic<bool>            m_audio_enabled { true };
    std::atomic<bool>            m_external_audio { false };
    std::mutex                   m_external_audio_mu;
    std::array<float, 64>        m_external_left {};
    std::array<float, 64>        m_external_right {};
    std::int64_t                 m_external_publish_ms { 0 };
};

// ---- SceneRenderController message handlers ---------------------------------

void SceneRenderController::on(RenderStop&& m) {
    m_stopped = m.stop;
    if (m.stop)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::on(RenderDraw&&) {
    frame_timer.FrameBegin();
    if (m_rg) {
        m_scene->shaderValueUpdater->FrameBegin();
        {
            auto pos                 = m_mouse_pos.load();
            m_scene->pointerPosition = pos;
            m_scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);
        }
        // Drive any per-Scene scenescripts before particle emission.
        // Scripts mutate SceneNode transforms (scale/origin/angles) so
        // they need to run before the matrix-derivation in the
        // shaderValueUpdater's per-frame uniform pass — that's already
        // what FrameBegin set up; UpdateUniforms runs inside drawFrame.
        // The runtime is a no-op when no ScriptScene is installed.
        {
            sr::script::FrameInputs fi;
            fi.frametime = static_cast<float>(m_scene->frameTime * m_speed);
            fi.runtime   = static_cast<float>(m_scene->elapsingTime);
            fi.canvas_w  = static_cast<float>(m_scene->ortho[0]);
            fi.canvas_h  = static_cast<float>(m_scene->ortho[1]);
            fi.screen_w  = fi.canvas_w;
            fi.screen_h  = fi.canvas_h;
            fi.time_of_day = LocalTimeOfDay();
            {
                auto pos    = m_mouse_pos.load();
                fi.cursor_x = pos[0];
                fi.cursor_y = pos[1];
            }
            fi.cursor_in_window       = cursorInWindow();
            fi.mouse_buttons_down     = buttonsDown();
            fi.mouse_buttons_pressed  = consumePressed();
            fi.mouse_buttons_released = consumeReleased();
            wavsen::audio::AudioSpectrum spec;
            bool primed = false;
            if (m_audio_enabled.load(std::memory_order_acquire)) {
                primed = m_audio_capture.snapshot(spec);
                wavsen::audio::AudioSpectrum external;
                if (snapshotExternalAudio(external)) {
                    if (! primed) {
                        spec = external;
                    } else {
                        for (std::size_t i = 0; i < spec.average.size(); ++i) {
                            spec.left[i]    = std::max(spec.left[i], external.left[i]);
                            spec.right[i]   = std::max(spec.right[i], external.right[i]);
                            spec.average[i] = std::max(spec.average[i], external.average[i]);
                            spec.bins[i]    = spec.average[i];
                        }
                        spec.publish_ms = std::max(spec.publish_ms, external.publish_ms);
                    }
                    primed = true;
                }
            }
            // Treat as silence if wavsen hasn't published recently. Without
            // this, a disconnected sink / suspended pipeline leaves the
            // last snapshot frozen and bars stick at the last live value.
            // 250 ms grace covers worst-case backend batching (quantum
            // ~21 ms + FFT trigger half-window) by ~10×.
            constexpr std::int64_t kStaleMs = 250;
            const auto             now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count();
            const bool             stale =
                ! primed || spec.publish_ms == 0 || (now_ms - spec.publish_ms) > kStaleMs;
            if (stale) spec.clear();
            fi.audio_left    = spec.left;
            fi.audio_right   = spec.right;
            fi.audio_average = spec.average;
            // Feed system-audio loopback into Scene::audioAverage (the
            // 16-bin particle audio-response signal). WPSoundParser also
            // writes this array from a scene's own embedded BGM; the two
            // coexist (max-with-decay), so audio-reactive particles respond
            // to whichever source is active. Dropping this overlay leaves
            // audioAverage at zero for scenes without embedded sound, so
            // audio bars stop moving.
            for (std::size_t bin = 0; bin < m_scene->audioAverage.size(); ++bin) {
                const auto begin = bin * fi.audio_average.size() / m_scene->audioAverage.size();
                const auto end = (bin + 1) * fi.audio_average.size() / m_scene->audioAverage.size();
                float sum = 0.0f;
                for (std::size_t i = begin; i < end; ++i) sum += fi.audio_average[i];
                const float level = end > begin ? sum / static_cast<float>(end - begin) : 0.0f;
                auto&       slot  = m_scene->audioAverage[bin];
                const float old   = slot.load(std::memory_order_relaxed);
                slot.store(std::max(old * 0.75f, level), std::memory_order_relaxed);
            }
            m_scene->shaderValueUpdater->SetAudioSpectrum(
                std::span<const float, 64>(fi.audio_left),
                std::span<const float, 64>(fi.audio_right));
            m_scene->TickNodeFieldAnimations();
            sr::script::TickSceneScripts(*m_scene, fi);
            m_scene->CommitNodeVisibilityChanges();
            m_scene->TickCameraPaths();
            m_scene->TickMaterialShaderAnimations();
            m_scene->TickTransformUpdaters();
            if (m_scene->ConsumeRenderGraphDirty()) {
                rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
            }
        }
        m_scene->paritileSys->Emitt();
        refreshPreparedMeshDirtyEvents();
        refreshPreparedMaterialDirtyEvents();

        /* Advance video textures (no-op if none) before drawFrame so
         * the new RGBA frame is sampled by the same render pass. */
        m_render->pumpVideoTextures(frame_timer.IdeaTime() * m_speed);

        /* Upload any glyph rects the actuators added this tick. Runs after
         * TickSceneScripts (which calls FontFace::Populate) and before
         * drawFrame so newly-rasterised glyphs are visible the same frame. */
        m_render->pumpFontAtlases(*m_scene);

        m_render->drawFrame(*m_scene);

        m_scene->PassFrameTime(frame_timer.IdeaTime() * m_speed);

        m_scene->shaderValueUpdater->FrameEnd();

        if (! m_scene->first_frame_ok) {
            m_scene->first_frame_ok = true;
            if (m_main_tx) (void)m_main_tx->send(MainMsg { MainFirstFrame {} });
        }
    }
    frame_timer.FrameEnd();
}

void SceneRenderController::on(RenderSetFillMode&& m) {
    m_fillmode = m.mode;
    if (m_scene && renderInited()) {
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
}

void SceneRenderController::rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention,
                                               bool                                 evict_meshes) {
    if (! m_scene || ! renderInited()) return;
    if (m_rg) m_render->clearLastRenderGraph(retention);
    if (evict_meshes) m_render->evictUnusedMeshes();
    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    m_rg           = sceneToRenderGraph(*m_scene, m_render_scene);

    if (m_main.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
    m_render->compileRenderGraph(*m_scene, *m_rg, m_render_scene);
    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    consumeDirtyEventsCoveredByGraphRebuild();
    (void)m_scene->ConsumeRenderGraphDirty();
}

void SceneRenderController::consumeDirtyEventsCoveredByGraphRebuild() {
    if (! m_scene) return;
    (void)m_scene->ConsumePreparedMaterialDirtyEvents();
    (void)m_scene->ConsumePreparedMeshDirtyEvents();
}

void SceneRenderController::refreshPreparedMeshDirtyEvents() {
    if (! m_scene || ! renderInited() || ! m_rg) return;
    auto events = m_scene->ConsumePreparedMeshDirtyEvents();
    if (events.empty()) return;

    bool requires_graph_rebuild = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return (event.flags & SceneMeshDirtyLayout) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    for (const auto& event : events) {
        if ((event.flags & SceneMeshDirtyData) == 0) continue;
        m_render->refreshPreparedMesh(
            *m_scene,
            m_render_scene,
            event.mesh,
            vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources));
    }
}

void SceneRenderController::refreshPreparedMaterialDirtyEvents() {
    if (! m_scene || ! renderInited() || ! m_rg) return;
    auto events = m_scene->ConsumePreparedMaterialDirtyEvents();
    if (events.empty()) return;

    bool requires_graph_rebuild = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return (event.flags & SceneMaterialDirtyGraph) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    for (const auto& event : events) {
        auto flags = MaterialDirtyToPassInvalidationFlags(event.flags);
        if (flags == vulkan::PassInvalidationNone) continue;
        m_render->refreshPreparedMaterial(*m_scene, m_render_scene, event.material, flags);
    }
}

void SceneRenderController::on(RenderSetScene&& m) {
    m_scene_ready.store(false, std::memory_order_release);
    m_scene = std::move(m.scene);
    rebuildRenderGraph(vulkan::RenderGraphResourceRetention::ReleaseSceneTextures, true);
    m_scene_ready.store(m_scene != nullptr && m_render->readyToDraw(), std::memory_order_release);
}

void SceneRenderController::on(RenderSetSpeed&& m) { m_speed = m.speed; }

void SceneRenderController::on(RenderSetUserProperty&& m) {
    if (! m_scene) return;
    std::string key                      = CanonicalUserPropertyKey(m.key);
    const bool  has_shader_combo_binding = m_scene->shader_combo_user_index.contains(key);
    sr::script::SetSceneUserProperty(*m_scene, key, m.property);
    ApplyUserPropertyToClear(*m_scene, key, m.property);
    ApplyUserPropertyToShaderUniforms(*m_scene, key, m.property);
    auto texture_materials = ApplyUserPropertyToMaterialTextures(*m_scene, key, m.property);
    bool shader_combo_requires_graph = ApplyUserPropertyToShaderCombos(*m_scene, key, m.property);
    ApplyUserPropertyToImageColor(*m_scene, key, m.property);
    ApplyUserPropertyToImageAlpha(*m_scene, key, m.property);
    ApplyUserPropertyToText(*m_scene, key, m.property);
    ApplyUserPropertyToPointSize(*m_scene, key, m.property);
    ApplyUserPropertyToTextColor(*m_scene, key, m.property);
    ApplyUserPropertyToTextAlpha(*m_scene, key, m.property);
    ApplyUserPropertyToParticles(*m_scene, key, m.property);
    ApplyUserPropertyToSoundVolume(*m_scene, key, m.property);
    ApplyUserPropertyToCameraParallax(*m_scene, key, m.property);
    ApplyUserPropertyToCameraShake(*m_scene, key, m.property);
    ApplyUserPropertyToCameraPath(*m_scene, key, m.property);
    bool requires_graph_rebuild = ApplyUserPropertyToNodeVisibility(*m_scene, key, m.property);
    requires_graph_rebuild =
        m_scene->ApplyUserImageEffectVisibilityBindings(key, m.property) || requires_graph_rebuild;
    requires_graph_rebuild = requires_graph_rebuild || shader_combo_requires_graph;

    // Pointsize edits swap the text atlas (new FontFace → new atlas texture);
    // fold those materials into the texture-refresh set so the new atlas binds.
    for (auto material : m_scene->TakeTextTextureRefresh()) {
        PushUniqueMaterialId(texture_materials, material);
    }

    if (! texture_materials.empty() && renderInited() && m_rg && ! requires_graph_rebuild) {
        m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
        if (! m_render->refreshPreparedMaterialTextures(
                *m_scene, m_render_scene, texture_materials)) {
            requires_graph_rebuild = true;
        }
    }
    if (has_shader_combo_binding && m_main_tx) {
        auto diagnostics = CollectUserPropertyDiagnostics(*m_scene, key);
        (void)m_main_tx->send(
            MainMsg { MainUserPropertyDiagnostics { .diagnostics = std::move(diagnostics) } });
    }
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }
    // Text / pointsize edits resize the per-layer RT via SetLayoutDirty, which
    // queues a *mesh* dirty event (not a material one). Drain it here too, or
    // the GPU RT keeps its old size — enlarged text clips and the stale target
    // ghosts. The per-frame loop drains this for scripted text; live user-prop
    // edits need it explicitly.
    if (renderInited() && m_rg) {
        refreshPreparedMeshDirtyEvents();
        refreshPreparedMaterialDirtyEvents();
    }
}

void SceneRenderController::on(RenderSetMediaStatus&& m) {
    if (! m_scene) return;

    sr::script::SetSceneMediaStatus(*m_scene, ToScriptMediaStatus(m.status));

    std::vector<SceneMaterialId> texture_materials;
    for (auto material : ApplyUserPropertyToMaterialTextures(
             *m_scene, "$mediaThumbnail", RuntimeTextureProperty(m.status.art_url))) {
        PushUniqueMaterialId(texture_materials, material);
    }
    for (auto material :
         ApplyUserPropertyToMaterialTextures(*m_scene,
                                             "$mediaPreviousThumbnail",
                                             RuntimeTextureProperty(m.status.previous_art_url))) {
        PushUniqueMaterialId(texture_materials, material);
    }

    bool requires_graph_rebuild = false;
    if (! texture_materials.empty() && renderInited() && m_rg) {
        m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
        if (! m_render->refreshPreparedMaterialTextures(
                *m_scene, m_render_scene, texture_materials)) {
            requires_graph_rebuild = true;
        }
    }
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }
    if (renderInited() && m_rg) refreshPreparedMaterialDirtyEvents();
}

void SceneRenderController::on(RenderInit&& m) {
    const bool initialized = m_render->init(std::move(*m.info));

    // Subscribe to ExSwapchain ready/extent/format changes. The
    // callback runs on the render thread (sync for Local, from
    // drainPendingDirective for Bridge); we just relay it as a
    // RenderSwapchainReady message back to ourselves so the actual
    // handling happens through the normal loop path. Format reaches
    // VulkanRender via ExSwapchain::format() directly; no need to
    // round-trip it through this message.
    if (auto* sw = m_render->exSwapchain()) {
        if (m_render_tx) {
            m_swapchain_tx                   = std::make_shared<RenderSender>(*m_render_tx);
            std::weak_ptr<RenderSender> weak = m_swapchain_tx;
            sw->setOnReadyChanged([weak](const ExSwapchainReadyEvent& e) {
                if (auto tx = weak.lock()) {
                    (void)tx->send(
                        RenderMsg { RenderSwapchainReady { e.ready, e.width, e.height } });
                }
            });
        }
    }

    // inited, callback to load scene
    if (initialized && m_main_tx) (void)m_main_tx->send(MainMsg { MainLoadScene {} });
}

void SceneRenderController::on(RenderSwapchainReady&& m) {
    if (! m.ready) {
        frame_timer.Stop();
        return;
    }
    bool extent_changed = m_render->onSwapchainReady(m.width, m.height);
    if (extent_changed && m_scene && m_rg) {
        m_render->refreshPreparedResources(*m_scene, m_render_scene);
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
    if (m_stopped)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::on(RenderRequestPreparedPassDiagnostics&& m) {
    if (! m_main_tx) return;
    auto diagnostics = m_render->preparedPassDiagnostics();
    (void)m_main_tx->send(MainMsg { MainPreparedPassDiagnostics {
        .cb          = std::move(m.cb),
        .diagnostics = std::move(diagnostics),
    } });
}

// ---- SceneRuntimeController message handlers --------------------------------

void SceneRuntimeController::on(MainLoadScene&&) {
    // This message is emitted only after VulkanRender::init succeeds. Keep a
    // main-loop-owned readiness bit instead of reading VulkanRender::m_inited
    // across threads, then publish a scene that may already have finished CPU
    // preparation in parallel.
    m_render_ready = true;
    publishPreparedScene();
}

void SceneRuntimeController::on(MainConfigure&& m) {
    m_config          = std::move(m.config);
    m_render_controller->configureAudioCapture(
        m_config.spectrum_enabled, m_config.external_spectrum);
    m_user_properties = NormalizeUserProperties(m_config.user_properties);
    ++m_config_generation;
    // Preserve zero as the "not configured" sentinel after wraparound.
    if (m_config_generation == 0) {
        ++m_config_generation;
        m_prepared_scene_generation     = 0;
        m_submitted_scene_generation    = 0;
    }
    m_prepared_scene.reset();
    m_prepared_scene_generation = 0;
    on(MainSetFps { m_config.fps });
    on(MainSetVolume { m_config.volume });
    on(MainSetVolumeScale { 1.0f });
    on(MainSetMuted { m_config.muted });
    on(MainSetFillMode { m_config.fill_mode });
    on(MainSetSpeed { m_config.speed });

    // MainConfigure and RenderInit run on separate message loops. Start the
    // VFS/scene/image-header work immediately so it overlaps Vulkan/MoltenVK
    // initialization; publishPreparedScene is the single join point.
    loadScene();
}

void SceneRuntimeController::on(MainSetFps&& m) {
    m_config.fps = m.fps;
    if (m.fps >= 5) m_render_controller->frame_timer.SetRequiredFps(static_cast<uint8_t>(m.fps));
}

void SceneRuntimeController::on(MainSetVolume&& m) {
    m_config.volume = m.volume;
    m_sound_manager->set_volume(m.volume);
}

void SceneRuntimeController::on(MainSetVolumeScale&& m) {
    m_sound_manager->set_volume_scale(m.scale, m.fade_ms);
}

void SceneRuntimeController::on(MainSetMuted&& m) {
    m_config.muted = m.muted;
    m_sound_manager->set_muted(m.muted);
}

void SceneRuntimeController::on(MainSetFillMode&& m) {
    m_config.fill_mode = m.mode;
    (void)m_render_loop.sender().send(RenderMsg { RenderSetFillMode { m.mode } });
}

void SceneRuntimeController::on(MainSetSpeed&& m) {
    m_config.speed = m.speed;
    (void)m_render_loop.sender().send(RenderMsg { RenderSetSpeed { m.speed } });
}

void SceneRuntimeController::on(MainSetUserProperty&& m) {
    const std::string property = CanonicalUserPropertyKey(m.key);
    auto              current  = m_user_properties.get(rstd::cppstd::as_str(property));
    Json              prop     = current.is_some() ? MergeUserPropertyDescriptor(**current, m.value)
                                                   : MakeUserPropertyDescriptor(std::move(m.value));
    m_config.user_properties.insert(::alloc::string::String::make(rstd::cppstd::as_str(property)),
                                    prop.clone());
    m_user_properties.insert(::alloc::string::String::make(rstd::cppstd::as_str(property)),
                             prop.clone());
    if (m_prepared_scene && m_prepared_scene_generation == m_config_generation &&
        m_submitted_scene_generation != m_config_generation) {
        // A property arriving while Vulkan is still initializing must update
        // the private prepared Scene; sending it to the render loop now would
        // be consumed before RenderSetScene and silently lost.
        ApplyUserPropertyBeforeFirstGraph(*m_prepared_scene, property, prop);
    } else {
        (void)m_render_loop.sender().send(
            RenderMsg { RenderSetUserProperty { property, std::move(prop) } });
    }
}

void SceneRuntimeController::on(MainSetFirstFrameCallback&& m) {
    m_first_frame_callback = std::move(m.cb);
}

void SceneRuntimeController::on(MainSetUserPropertyDiagnosticCallback&& m) {
    m_user_property_diagnostic_cb = std::move(m.cb);
}

void SceneRuntimeController::on(MainUserPropertyDiagnostics&& m) {
    if (m_user_property_diagnostic_cb) m_user_property_diagnostic_cb(std::move(m.diagnostics));
}

void SceneRuntimeController::on(MainPreparedPassDiagnostics&& m) {
    if (m.cb) m.cb(std::move(m.diagnostics));
}

void SceneRuntimeController::on(MainStop&& m) {
    const uint64_t generation = ++m_audio_pause_generation;
    if (m.stop) {
        if (m.scale_audio) m_sound_manager->set_volume_scale(0.0f, m.fade_ms);
        if (m.fade_ms == 0 || ! m.scale_audio) {
            m_sound_manager->pause();
        } else {
            auto     tx    = m_main_loop.sender();
            uint32_t delay = m.fade_ms;
            std::thread([tx = std::move(tx), generation, delay]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                (void)tx.send(MainMsg { MainPauseAudio { generation } });
            }).detach();
        }
    } else {
        m_sound_manager->play();
        if (m.scale_audio) m_sound_manager->set_volume_scale(1.0f, m.fade_ms);
    }
    (void)m_render_loop.sender().send(RenderMsg { RenderStop { m.stop } });
}

void SceneRuntimeController::on(MainPauseAudio&& m) {
    if (m.generation == m_audio_pause_generation) m_sound_manager->pause();
}

void SceneRuntimeController::on(MainFirstFrame&&) {
    if (m_first_frame_callback) m_first_frame_callback();
}

void SceneRuntimeController::loadScene() {
    if (m_config.source_pkg_path.empty() || m_config.assets_dir.empty()) return;

    rstd_info("loading scene: {}", m_config.source_pkg_path);

    // Device lifecycle only here; playback is (re)started after the scene's
    // sound streams are mounted below. loadScene can run with the device
    // ALREADY inited — MainConfigure calls set_muted(false) before MainLoadScene,
    // and set_muted(false) eagerly re-inits the device. If play() were gated on
    // the `!is_inited()` branch, that pre-init makes loadScene take the else
    // branch and the AudioQueue never starts, leaving all BGM silent.
    // SoundManager::init() is a no-op when muted, so this stays mute-safe.
    if (! m_sound_manager->is_inited()) {
        m_sound_manager->init();
    } else {
        m_sound_manager->unmount_all();
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        bool sus = vfs.Mount("/assets", fs::CreatePhysicalFs(m_config.assets_dir), "assets");
        if (! sus) {
            rstd_error("Mount assets dir failed");
            return;
        }
    }
    std::filesystem::path pkgPath_fs { m_config.source_pkg_path };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();
    MergeProjectUserProperties(pkgPath_fs.parent_path(), m_user_properties);

    // load pkgfile. Read pkg version stamp before move-mounting so we can
    // pass it to the scene parser; on fallback (loose dir) we have no
    // version info and use kSceneVersionUnknown.
    wpscene::SceneVersion pkg_v = wpscene::kSceneVersionUnknown;
    auto                  wfs   = fs::WPPkgFs::CreatePkgFs(pkgPath, m_config.load_from_memory);
    if (wfs) pkg_v = wpscene::ParsePkgVersionStamp(wfs->pkg_version_stamp());
    if (! wfs || ! vfs.Mount("/assets", std::move(wfs))) {
        rstd_info("load pkg file {} failed, fallback to use dir", pkgPath);
        pkg_v = wpscene::kSceneVersionUnknown;
        // load pkg dir
        if (! vfs.Mount("/assets", fs::CreatePhysicalFs(pkgDir))) {
            rstd_error("can't load pkg directory: {}", pkgDir);
            return;
        }
    }
    if (! m_config.cache_dir.empty()) {
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(m_config.cache_dir, true), "cache")) {
            rstd_error("can't load cache folder: {}", m_config.cache_dir);
        } else {
            rstd_info("cache folder: {}", m_config.cache_dir);
        }
    }

    {
        const std::string base { "/assets/" };
        auto              scene_doc = m_config.scene_document;
        if (! scene_doc) {
            auto loaded = wpscene::LoadSceneDocumentFromVfs(vfs, base + pkgEntry, pkg_v);
            if (loaded) scene_doc = std::make_shared<wpscene::SceneDocument>(std::move(*loaded));
        }
        if (! scene_doc) {
            rstd_error("Not supported scene type");
            return;
        }
        // Hand the (already-merged project.json defaults + any host-supplied
        // overrides) user-property map to the parser so visible-binding
        // pruning sees the user's saved values, not the scene.json defaults.
        m_scene_parser.SetUserProperties(rstd::Some(
            rstd::ref<rstd::json::Map>::from_raw_parts(rstd::addressof(m_user_properties))));
        scene = m_scene_parser.Parse(scene_id, *scene_doc, vfs, *m_sound_manager);
        m_scene_parser.SetUserProperties(rstd::None());

        // Start (or resume) the output device now that this scene's sound
        // streams are mounted. Unconditional on purpose: the device may have
        // been inited earlier by set_muted(false), so gating on the init
        // branch above would skip start and silence all BGM. start() is a
        // no-op if the device isn't inited (e.g. muted) or already running.
        if (! m_config.muted) m_sound_manager->play();
        // Apply initial bindings while the parsed Scene is still private to
        // this preparation loop. The VFS must be attached first for shader
        // combo and text asset resolution. Doing this before RenderSetScene
        // keeps the initial render graph to a single build.
        scene->vfs.reset(pVfs.release());
        m_user_properties.iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            auto        key                = rstd::cppstd::to_string(entry_key->as_str());
            const auto& prop               = *entry_value;
            ApplyUserPropertyBeforeFirstGraph(*scene, key, prop);
        });
        if (! m_config.cache_dir.empty() && scene) {
            std::filesystem::path ls_dir =
                std::filesystem::path(m_config.cache_dir) / "script_localstorage";
            std::error_code ec;
            std::filesystem::create_directories(ls_dir, ec);
            std::string ls_file = (ls_dir / (scene_id + ".json")).native();
            sr::script::SetScenePersistence(*scene, std::move(ls_file));
        }
        // Surface the parsed clear color before the scene is shipped
        // off to the render thread; downstream callers use it to keep
        // letterbox/background fill aligned with the scene.
        if (m_clear_color_cb) {
            const auto& c = scene->clearColor;
            m_clear_color_cb(c[0], c[1], c[2]);
        }
        if (m_audio_demand_cb) m_audio_demand_cb(scene->uses_audio_spectrum);
    }

    m_prepared_scene            = std::move(scene);
    m_prepared_scene_generation = m_config_generation;
    publishPreparedScene();
}

void SceneRuntimeController::publishPreparedScene() {
    if (! m_render_ready || ! m_prepared_scene ||
        m_prepared_scene_generation != m_config_generation ||
        m_submitted_scene_generation == m_config_generation)
        return;

    auto scene = std::exchange(m_prepared_scene, nullptr);
    m_submitted_scene_generation = m_config_generation;

    auto rtx = m_render_loop.sender();
    (void)rtx.send(RenderMsg { RenderSetScene { std::move(scene) } });
    // draw first frame
    (void)rtx.send(RenderMsg { RenderDraw {} });
}

bool SceneRuntimeController::init() {
    if (m_inited) return true;

    // Wire render handler senders before starting the loops; otherwise an
    // early RenderInit could fire before they're set.
    m_render_controller->setSenders(m_render_loop.sender(), m_main_loop.sender());

    m_main_loop.start([this](MainMsg&& m) {
        std::visit(
            [this](auto&& v) {
                on(std::move(v));
            },
            std::move(m.v));
    });
    m_render_loop.start([this](RenderMsg&& m) {
        std::visit(
            [this](auto&& v) {
                m_render_controller->on(std::move(v));
            },
            std::move(m.v));
    });

    {
        auto& frameTimer = m_render_controller->frame_timer;
        auto  rtx        = m_render_loop.sender();
        frameTimer.SetCallback([rtx]() mutable {
            (void)rtx.send(RenderMsg { RenderDraw {} });
        });
        frameTimer.SetRequiredFps(15);
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}

SceneRuntimeController::SceneRuntimeController()
    : m_sound_manager(std::make_unique<wavsen::audio::SoundManager>()),
      m_main_loop("main"),
      m_render_loop("render"),
      m_render_controller(std::make_unique<SceneRenderController>(*this)) {}

SceneRuntimeController::~SceneRuntimeController() {
    // Orderly shutdown: drain both loops *before* SceneRenderController dies, so
    // m_render->destroy() doesn't race with an in-flight RenderDraw.
    //
    //   1. Stop the frame timer (joins its thread → no more Draw posts).
    //   2. Replace the timer callback with a no-op so the captured render
    //      Sender clone is released.
    //   3. Drop every Sender clone the render handler holds, including the
    //      strong ref the swapchain callback weak-captures.
    //   4. Stop the render loop — drops engine sender, recv() returns Err
    //      after the in-flight handler returns, thread joins.
    //   5. Same for the main loop.
    //   6. Default member destruction then runs SceneRenderController's dtor with
    //      the render thread already gone, so destroy() is single-threaded.
    if (m_render_controller) {
        m_render_controller->frame_timer.Stop();
        m_render_controller->frame_timer.SetCallback([] {
        });
        m_render_controller->clearSenders();
    }
    m_render_loop.stop();
    m_main_loop.stop();
}

} // namespace sr

SceneWallpaper::SceneWallpaper(): m_runtime(std::make_unique<SceneRuntimeController>()) {}

SceneWallpaper::~SceneWallpaper() = default;

bool SceneWallpaper::inited() const { return m_runtime->inited(); }

bool SceneWallpaper::init() { return m_runtime->init(); }

void SceneWallpaper::initVulkan(RenderInitInfo info) {
    m_offscreen = info.offscreen;
    auto sp     = std::make_shared<RenderInitInfo>(std::move(info));
    (void)m_runtime->renderSender().send(RenderMsg { RenderInit { std::move(sp) } });
}

void SceneWallpaper::play() { (void)m_runtime->mainSender().send(MainMsg { MainStop { false } }); }
void SceneWallpaper::play(uint32_t fade_ms) {
    (void)m_runtime->mainSender().send(MainMsg { MainStop { false, fade_ms, true } });
}
void SceneWallpaper::pause() { (void)m_runtime->mainSender().send(MainMsg { MainStop { true } }); }
void SceneWallpaper::pause(uint32_t fade_ms) {
    (void)m_runtime->mainSender().send(MainMsg { MainStop { true, fade_ms, true } });
}
void SceneWallpaper::requestFrame() {
    (void)m_runtime->renderSender().send(RenderMsg { RenderDraw {} });
}

void SceneWallpaper::mouseInput(double x, double y) {
    m_runtime->renderController()->setMousePos(x, y);
}

void SceneWallpaper::mouseButton(int button, bool down) {
    m_runtime->renderController()->setMouseButton(button, down);
}

void SceneWallpaper::mouseEnter(bool in_window) {
    m_runtime->renderController()->setMouseInWindow(in_window);
}

void SceneWallpaper::configure(SceneWallpaperConfig config) {
    (void)m_runtime->mainSender().send(MainMsg { MainConfigure { std::move(config) } });
}

void SceneWallpaper::setFps(uint32_t fps) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetFps { fps } });
}

void SceneWallpaper::setVolume(float volume) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetVolume { volume } });
}

void SceneWallpaper::setVolumeScale(float scale) { setVolumeScale(scale, 0); }

void SceneWallpaper::setVolumeScale(float scale, uint32_t fade_ms) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetVolumeScale { scale, fade_ms } });
}

void SceneWallpaper::setMuted(bool muted) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetMuted { muted } });
}

void SceneWallpaper::setFillMode(FillMode mode) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetFillMode { mode } });
}

void SceneWallpaper::setSpeed(float speed) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetSpeed { speed } });
}

void SceneWallpaper::setMediaStatus(MediaStatus status) {
    (void)m_runtime->renderSender().send(RenderMsg { RenderSetMediaStatus { std::move(status) } });
}

void SceneWallpaper::setAudioSpectrum(std::array<float, 64> left,
                                      std::array<float, 64> right) {
    m_runtime->renderController()->setExternalAudioSpectrum(
        std::move(left), std::move(right));
}

void SceneWallpaper::setUserPropertyRaw(std::string_view name, std::string value) {
    (void)m_runtime->mainSender().send(
        MainMsg { MainSetUserProperty { std::string(name), RawUserProperty(value) } });
}

void SceneWallpaper::setUserPropertyJson(std::string_view name, Json value) {
    (void)m_runtime->mainSender().send(
        MainMsg { MainSetUserProperty { std::string(name), std::move(value) } });
}

void SceneWallpaper::setOnClearColor(ClearColorCallback cb) {
    m_runtime->setOnClearColor(std::move(cb));
}

void SceneWallpaper::setOnAudioDemand(AudioDemandCallback cb) {
    m_runtime->setOnAudioDemand(std::move(cb));
}

void SceneWallpaper::setOnFirstFrame(FirstFrameCallback cb) {
    (void)m_runtime->mainSender().send(MainMsg { MainSetFirstFrameCallback { std::move(cb) } });
}

void SceneWallpaper::setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback cb) {
    (void)m_runtime->mainSender().send(
        MainMsg { MainSetUserPropertyDiagnosticCallback { std::move(cb) } });
}

void SceneWallpaper::requestPreparedPassDiagnostics(RenderPassDiagnosticCallback cb) {
    (void)m_runtime->renderSender().send(
        RenderMsg { RenderRequestPreparedPassDiagnostics { std::move(cb) } });
}

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_runtime->renderController()->exSwapchain();
}

bool SceneWallpaper::waitVulkanInited(uint32_t timeout_ms) {
    using clock   = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto rh       = m_runtime->renderController();
    while (clock::now() < deadline) {
        if (rh->renderInited()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rh->renderInited();
}

bool SceneWallpaper::sceneReady() const {
    return m_runtime->renderController()->sceneReady();
}

VkInstance SceneWallpaper::vkInstance() const {
    return m_runtime->renderController()->render()->vkInstance();
}
VkPhysicalDevice SceneWallpaper::vkPhysicalDevice() const {
    return m_runtime->renderController()->render()->vkPhysicalDevice();
}
VkDevice SceneWallpaper::vkDevice() const {
    return m_runtime->renderController()->render()->vkDevice();
}
VkQueue SceneWallpaper::vkGraphicsQueue() const {
    return m_runtime->renderController()->render()->vkGraphicsQueue();
}
uint32_t SceneWallpaper::vkGraphicsQueueFamily() const {
    return m_runtime->renderController()->render()->vkGraphicsQueueFamily();
}
void SceneWallpaper::deviceUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->deviceUuid(out);
}
void SceneWallpaper::driverUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->driverUuid(out);
}
