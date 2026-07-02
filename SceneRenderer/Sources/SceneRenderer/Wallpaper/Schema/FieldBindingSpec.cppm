module;

export module sr.pkg.scene_obj:field_binding;
import rstd.cppstd;
import sr.json;

// Property-binding side channel.
//
// In Wallpaper Engine any scalar field on a scene object can take three
// shapes:
//
//    1. Plain literal       → `42`, `"1 2 3"`, `true`
//    2. Property-bound       → `{"value": X, "user": "<binding-name>"}`
//                             (auto-unwrapped by sr.json GetJsonValue)
//    3. Animated / scripted  → `{"value": X, "animation": {...}}` or
//                             `{"value": X, "scriptproperties": {...}}`
//
// The renderer currently consumes only (1)/(2). The animation curve and
// scriptproperties subtrees are absorbed verbatim so the parsed data
// stays schema-complete; SceneSchema tests assert every observed leaf
// path under `*.animation.*` is captured.
//
// One curve covers a vec3 field (c0/c1/c2 axes) or a scalar (c0 only).

export namespace sr::wpscene
{

struct AnimKeyframeTangent {
    bool  enabled { false };
    float x { 0.0f };
    float y { 0.0f };
    // `magic` is a sometimes-present opaque editor value (unsigned int);
    // captured to keep the schema check honest.
    std::int32_t magic { 0 };
};

struct AnimKeyframe {
    std::int32_t        frame { 0 };
    float               value { 0.0f };
    bool                lockangle { false };
    bool                locklength { false };
    AnimKeyframeTangent front;
    AnimKeyframeTangent back;
};

struct AnimOptions {
    float        fps { 30.0f };
    std::int32_t length { 0 };
    std::string  mode;
    std::string  name;
    bool         startpaused { false };
    bool         wraploop { false };
    // `smoothing` may be null/int/float in the corpus; kept as raw json
    // until a renderer consumer needs it.
    nlohmann::json smoothing;
    nlohmann::json children; // array of nested anim refs
    nlohmann::json events;   // array of marker objects
    nlohmann::json parent;   // object describing parent anim
};

struct AnimCurve {
    std::vector<AnimKeyframe> c0;
    std::vector<AnimKeyframe> c1; // empty for scalar fields
    std::vector<AnimKeyframe> c2;
    AnimOptions               options;
    bool                      relative { false }; // only on `origin`
};

// FromJson helpers (defined in FieldBinding.cpp).
bool ParseAnimKeyframeTangent(const nlohmann::json&, AnimKeyframeTangent&);
bool ParseAnimKeyframe(const nlohmann::json&, AnimKeyframe&);
bool ParseAnimAxis(const nlohmann::json&, std::vector<AnimKeyframe>&);
bool ParseAnimOptions(const nlohmann::json&, AnimOptions&);
bool ParseAnimCurve(const nlohmann::json&, AnimCurve&);

// One captured `{value, script, scriptproperties, user}` per-field
// binding. `source` is the inline JS module text observed in scene.json's
// `"script"` key (5286 bindings, 2877 unique sources in the workshop
// corpus — see `tests/wpscriptdump`). `properties` mirrors the per-binding
// `scriptproperties` config block; `initial_value` is the binding's
// `value` field, fed to `init(value)` by the runtime. `user` carries the
// optional user-property name from `{user, value}` companion bindings.
struct ScriptBinding {
    std::string    source;
    nlohmann::json properties;
    nlohmann::json initial_value;
    std::string    user;
};

// Side-channel container attached to every parseable object kind. Only
// fields that actually carry a binding contribute entries — empty maps
// for the common case where every field is a plain literal.
struct FieldBindings {
    std::unordered_map<std::string, AnimCurve>      animations;
    std::unordered_map<std::string, nlohmann::json> scriptproperties;
    std::unordered_map<std::string, ScriptBinding>  scripts;
};

// Walks every direct child of `obj_json` and, when the child is an
// object containing `animation` and/or `scriptproperties`, captures into
// `out`. Idempotent: re-running on the same json overwrites prior
// entries. Returns the count of bindings absorbed.
std::size_t AbsorbAllFieldBindings(const nlohmann::json& obj_json, FieldBindings& out);

} // namespace sr::wpscene
