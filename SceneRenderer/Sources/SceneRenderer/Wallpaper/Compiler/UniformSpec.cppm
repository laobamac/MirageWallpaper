module;
#include <unordered_map>
#include <utility>

export module sr.pkg.parse:wp_uniform;
import rstd.cppstd;
import sr.fs;

import sr.json;

export namespace sr

{
namespace wpscene
{

// Texture-uniform metadata. WE's per-sampler `// {json}` annotation is parsed
// into this struct. Fields not used by the renderer (label / group / ...) are
// stored anyway so future editor / material UI consumers can read them back.
struct WPUniformTex {
    struct Component {
        std::string label;
        std::string combo;
    };
    std::string                          material; // unique key for material override
    std::string                          label;    // editor display name
    std::string                          default_; // default texture path or `_rt_*`
    std::string                          mode;     // opacitymask / rgbmask / flowmask
    std::string                          combo;    // bound? `#define <IDENT> 1` else 0
    std::array<float, 4>                 paintdefaultcolor { 0, 0, 0, 1.0f };
    std::vector<Component>               components;
    bool                                 requireany { false };
    std::unordered_map<std::string, int> require;

    // Corpus-observed extras (parsed, not yet consumed).
    bool        hidden { false };
    bool        nonremovable { false };
    std::string group;
    bool        linked { false };
    std::string format; // "normalmap" etc.
    bool        formatcombo { false };
    bool        direction { false };
    std::string conversion; // "startdelta" etc.
    int         order { 0 };

    bool FromJson(const Json& json) {
        sr::GetJsonValue(json, "material", material, false);
        sr::GetJsonValue(json, "label", label, false);
        sr::GetJsonValue(json, "default", default_, false);
        sr::GetJsonValue(json, "mode", mode, false);
        sr::GetJsonValue(json, "combo", combo, false);
        if (auto values = json.get("components"); values.is_some()) {
            auto array = (*values)->as_array();
            if (array.is_some())
                for (const auto& el : **array) {
                    Component c;
                    sr::GetJsonValue(el, "label", c.label);
                    sr::GetJsonValue(el, "combo", c.combo, false);
                    components.push_back(std::move(c));
                }
        }
        sr::GetJsonValue(json, "requireany", requireany, false);
        if (auto values = json.get("require"); values.is_some()) {
            auto object = (*values)->as_object();
            if (object.is_some())
                (*object)->iter().for_each([&](auto entry) {
                    auto [entry_key, entry_value] = entry;
                    int value { false };
                    sr::GetJsonValue(*entry_value, value);
                    require[rstd::cppstd::to_string(entry_key->as_str())] = value;
                });
        }

        sr::GetJsonValue(json, "hidden", hidden, false);
        sr::GetJsonValue(json, "nonremovable", nonremovable, false);
        sr::GetJsonValue(json, "group", group, false);
        sr::GetJsonValue(json, "linked", linked, false);
        sr::GetJsonValue(json, "format", format, false);
        sr::GetJsonValue(json, "formatcombo", formatcombo, false);
        sr::GetJsonValue(json, "direction", direction, false);
        sr::GetJsonValue(json, "conversion", conversion, false);
        sr::GetJsonValue(json, "order", order, false);
        return true;
    }
};

// Scalar / vec / color / UV uniform metadata. Covers `g_*` user-controlled
// scalars (e.g. `g_Brightness`) and `u_*` user-variable convention.
struct WPUniformVar {
    std::string          name;     // GLSL identifier (e.g. "g_Brightness")
    std::string          material; // UI key for editor / project bindings
    std::string          label;
    std::string          group;
    std::string          type; // "color" | "" (UV picker uses position:true)
    bool                 position { false };
    bool                 linked { false };
    bool                 nobindings { false };
    bool                 is_user { false }; // true iff name starts with "u_"
    std::array<float, 2> range { 0.0f, 1.0f };
    bool                 has_range { false };

    // Default value as raw JSON. Host coerces to float / vec2 / vec3 / vec4
    // depending on uniform type at upload time.
    Json default_value;

    bool FromJson(const Json& json, std::string uniform_name) {
        name    = std::move(uniform_name);
        is_user = name.size() >= 2 && name[0] == 'u' && name[1] == '_';
        sr::GetJsonValue(json, "material", material, false);
        sr::GetJsonValue(json, "label", label, false);
        sr::GetJsonValue(json, "group", group, false);
        sr::GetJsonValue(json, "type", type, false);
        sr::GetJsonValue(json, "position", position, false);
        sr::GetJsonValue(json, "linked", linked, false);
        sr::GetJsonValue(json, "nobindings", nobindings, false);
        if (auto values = json.get("range"); values.is_some()) {
            auto array = (*values)->as_array();
            if (array.is_some() && (*array)->len() >= 2)
                has_range = sr::GetJsonValue((**array)[0], range[0]) &&
                            sr::GetJsonValue((**array)[1], range[1]);
        }
        if (auto value = json.get("default"); value.is_some()) default_value = (*value)->clone();
        return true;
    }
};

// [COMBO] preprocessor switch declaration. `combo` is the IDENT that gets
// `#define`'d in the GLSL prologue, with value `default_` (or whichever
// option the user picked in the editor).
struct WPCombo {
    std::string                          material; // editor display name
    std::string                          combo;    // IDENT injected as #define
    std::string                          type;     // "options" | "" (checkbox)
    int                                  default_ { 0 };
    std::unordered_map<std::string, int> options; // label → value (combo box mode)
    std::unordered_map<std::string, int> require; // gating combos

    bool FromJson(const Json& json) {
        sr::GetJsonValue(json, "material", material, false);
        sr::GetJsonValue(json, "combo", combo, false);
        sr::GetJsonValue(json, "type", type, false);
        sr::GetJsonValue(json, "default", default_, false);
        if (auto values = json.get("options"); values.is_some()) {
            auto object = (*values)->as_object();
            if (object.is_some())
                (*object)->iter().for_each([&](auto entry) {
                    auto [entry_key, entry_value] = entry;
                    int value { 0 };
                    sr::GetJsonValue(*entry_value, value);
                    options[rstd::cppstd::to_string(entry_key->as_str())] = value;
                });
        }
        if (auto values = json.get("require"); values.is_some()) {
            auto object = (*values)->as_object();
            if (object.is_some())
                (*object)->iter().for_each([&](auto entry) {
                    auto [entry_key, entry_value] = entry;
                    int value { 0 };
                    sr::GetJsonValue(*entry_value, value);
                    require[rstd::cppstd::to_string(entry_key->as_str())] = value;
                });
        }
        return true;
    }
};

} // namespace wpscene
} // namespace sr
