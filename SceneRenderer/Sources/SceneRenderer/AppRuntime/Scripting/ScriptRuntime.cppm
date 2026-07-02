module;

export module sr.script;
import nlohmann.json;
import sr.core;
import rstd;
import rstd.cppstd;
import sr.scene;

export namespace sr::script
{

// --- shared value variant ----------------------------------------------------

// Result of a script's update() call, after coercion. The variant is a
// snapshot of what the JS code returned this frame; the actuator (in the
// renderer) reads it and writes into the bound C++ field.
struct ScalarValue {
    double v { 0.0 };
};
struct Vec2Value {
    double x { 0.0 }, y { 0.0 };
};
struct Vec3Value {
    double x { 0.0 }, y { 0.0 }, z { 0.0 };
};
struct ColorValue {
    double r { 0.0 }, g { 0.0 }, b { 0.0 };
};
struct StringValue {
    std::string s;
};
struct BoolValue {
    bool v { false };
};

using ScriptValue = std::variant<std::monostate, ScalarValue, BoolValue, Vec2Value, Vec3Value,
                                 ColorValue, StringValue>;

struct BoneTranslation {
    float x { 0.0f }, y { 0.0f }, z { 0.0f };
};

// What kind of value a FieldScript is expected to produce. Set at parse
// time based on the field name's well-known type — see the per-field-kind
// table in the API doc.
enum class FieldKind
{
    Unknown,
    Scalar,
    Bool,
    Vec2,
    Vec3,
    Color,
    String
};

// --- frame inputs -----------------------------------------------------------

// One snapshot of host-supplied per-frame state, fed by the renderer into
// `JsRuntime::TickFieldScripts` once per frame. Mirrors the engine.* fields
// the audio-response cluster (and the parallax cluster) actually read.
struct FrameInputs {
    float frametime { 0.0f };   // seconds since last frame
    float runtime { 0.0f };     // seconds since wallpaper start
    float time_of_day { 0.0f }; // 0..1, 0=midnight, 0.5=noon
    float canvas_w { 1920.0f };
    float canvas_h { 1080.0f };
    float screen_w { 1920.0f };
    float screen_h { 1080.0f };
    // 64-bin audio buffers populated by the audio chain. The script bridge
    // resamples these to the requested registerAudioBuffers() resolution.
    std::array<float, 64> audio_left {};
    std::array<float, 64> audio_right {};
    std::array<float, 64> audio_average {};
    // Cursor state. (cursor_x, cursor_y) is normalised canvas coords:
    // x ∈ [0,1] left-to-right, y ∈ [0,1] top-to-bottom. button bits use
    // GLFW numbering (left=0, right=1, middle=2). down is held-state,
    // pressed/released are edge events for this frame only.
    float    cursor_x { 0.0f }, cursor_y { 0.0f };
    bool     cursor_in_window { false };
    uint32_t mouse_buttons_down { 0 };
    uint32_t mouse_buttons_pressed { 0 };
    uint32_t mouse_buttons_released { 0 };
};

// --- script properties (configuration) --------------------------------------

// One descriptor produced by createScriptProperties().addX() calls inside
// the JS module. Captured at module-load time and then merged with the
// per-binding `scriptproperties` config from scene.json before exposing
// the resulting `scriptProperties.<name>` accessors back to the script.
struct PropDescriptor {
    enum class Kind
    {
        Slider,
        Checkbox,
        Text,
        Combo,
        Color,
        Delimiter,
        Other
    };
    Kind           kind { Kind::Other };
    std::string    name;
    std::string    label;
    nlohmann::json default_value; // captured verbatim
    double         min { 0.0 };
    double         max { 1.0 };
    bool           integer { false };
};

// --- runtime ----------------------------------------------------------------

class FieldScript;

// One JsRuntime per Scene. Owns one JSRuntime and one JSContext. Compiled
// modules are deduped by sha so duplicated sources across many bound fields
// only allocate once. The runtime is not thread-safe; the renderer's frame
// tick is the single owner.
class JsRuntime : NoCopy, NoMove {
public:
    JsRuntime();
    ~JsRuntime();

    // Returns nullptr on hard compile/init failure (logs once).
    // `node` (nullable) is the SceneNode the script will see as `thisLayer`
    // inside init/update. When null, `thisLayer` falls back to a generic
    // stub (the JS-side default created at bootstrap).
    FieldScript* MakeFieldScript(std::string_view source, std::string_view script_sha,
                                 FieldKind field_kind, const nlohmann::json& properties_config,
                                 const nlohmann::json&        initial_value,
                                 sr::SceneNode*              node   = nullptr,
                                 std::vector<sr::SceneNode*> clones = {});

    // Install the Scene root that backs `thisScene`. `thisScene.getLayer(name)`
    // searches from this node. Call once per scene after parsing finishes.
    void SetSceneRoot(sr::SceneNode* root);

    // Wire localStorage to a JSON file. Existing keys load synchronously;
    // subsequent script writes flush back to the file. Pass an empty
    // string to revert to in-memory-only behaviour.
    void SetPersistence(std::string path);

    // Push one frame's worth of host state into the runtime. The next
    // FieldScript::Update call will see these values via `engine.*`.
    void SetFrameInputs(const FrameInputs& fi);

    // Patch one Wallpaper Engine user property into engine.userProperties.
    // `property` should be the descriptor object shape used by project.json
    // (`{value: ...}` plus optional metadata).
    void SetUserProperty(std::string_view key, const nlohmann::json& property);

    using BoneIndexResolver = std::function<uint32_t(sr::SceneNode*, std::string_view)>;
    using BoneTransformResolver =
        std::function<std::optional<BoneTranslation>(sr::SceneNode*, uint32_t, double)>;
    void SetBoneResolvers(BoneIndexResolver     index_resolver,
                          BoneTransformResolver transform_resolver);

    // Drive every alive FieldScript once. Invokes their cached `update`
    // export and stores the coerced return into FieldScript::last_value().
    // Exceptions are caught and logged once per script_sha.
    void TickAll();

    // Walk every live FieldScript created by this runtime. Caller-provided
    // function gets a non-owning pointer; the renderer uses this to push
    // last_value() into per-field actuators.
    using EachFn = void (*)(FieldScript*, void*);
    void ForEachScript(EachFn fn, void* user);

    // Wire a text-content setter for a given SceneNode. When a script does
    // `thisLayer.text = "..."` on a wrapper whose opaque is `node`, the JS
    // setter dispatches into this callback. Used by text layers to receive
    // text writes from scripts bound to non-text fields (e.g. clock
    // scripts attached to `visible`).
    void RegisterTextSetter(sr::SceneNode* node, std::function<void(std::string_view)> setter);
    void RegisterTextAlignSetters(sr::SceneNode* node, std::string horizontal,
                                  std::string vertical, double point_size,
                                  std::function<void(std::string_view)> set_horizontal,
                                  std::function<void(std::string_view)> set_vertical);

    // Same exposure rule as FieldScript::Impl above: opaque outside the
    // module, but visible to peer module impl files.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class FieldScript : NoCopy, NoMove {
public:
    FieldScript();
    ~FieldScript();

    FieldKind          field_kind() const noexcept;
    const ScriptValue& last_value() const noexcept;
    bool               alive() const noexcept;
    std::string_view   script_sha() const noexcept;

    // Impl is intentionally exposed inside the sr.script module so
    // JsRuntime::Impl (in the same module) can mutate it directly. Treated
    // as opaque by every other consumer; see Script.cpp.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// --- per-Scene script runtime + actuators -----------------------------------

// Where on a SceneNode the transform-style script value should be written.
// Used by MakeNodeTransformApply to manufacture the corresponding closure.
enum class NodeTransformTarget
{
    Translate, // Vec3 → SceneNode m_translate (origin field)
    Scale,     // Vec3 → SceneNode m_scale (scale field)
    Rotation,  // Vec3 → SceneNode m_rotation (angles field)
};

// One write-back binding from script.last_value() to whatever subsystem
// owns the bound field. The closure does the type coercion + write; the
// generic ScriptScene::Tick has no idea what 'apply' does.
struct Actuator {
    FieldScript*                            script { nullptr };
    std::function<void(const ScriptValue&)> apply;
};

// Build the closure that drives a SceneNode transform field. Encapsulates
// the Vec3/Vec2/Scalar/Bool coercion table so callers stay one-liners.
// Captures `node` as Arc so actuator lifetime follows the SceneNode allocation.
std::function<void(const ScriptValue&)> MakeNodeTransformApply(rstd::sync::Arc<sr::SceneNode> node,
                                                               NodeTransformTarget target);

// Build the closure that drives a SceneNode alpha field.
std::function<void(const ScriptValue&)> MakeNodeAlphaApply(rstd::sync::Arc<sr::SceneNode> node);

// Owns one JsRuntime + the actuator list for one Scene. Constructed and
// populated by the parser, attached to the Scene as an opaque pointer
// (Scene::script_scene). Ticked once per frame by `TickSceneScripts`.
class ScriptScene : NoCopy, NoMove {
public:
    ScriptScene();
    ~ScriptScene();

    JsRuntime& runtime() noexcept;
    void       AddActuator(Actuator a);
    bool       empty() const noexcept;

    // Push the host's per-frame state, drive every FieldScript, drain
    // results into actuators. Call once per frame, before the renderer
    // begins drawing.
    void Tick(const FrameInputs& fi);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Attach a ScriptScene to a Scene via the opaque-pointer slot. Takes
// ownership; replaces any previous attachment.
void InstallScriptScene(sr::Scene& scene, std::unique_ptr<ScriptScene> ss);

// Convenience tick: looks up the ScriptScene attached to `scene` and
// drives one frame. No-op when no ScriptScene is installed (image-only
// pkgs, scenes without script bindings).
void TickSceneScripts(sr::Scene& scene, const FrameInputs& fi);

// Patch `engine.userProperties` on the ScriptScene attached to `scene`.
// No-op when the scene has no script runtime.
void SetSceneUserProperty(sr::Scene& scene, std::string_view key, const nlohmann::json& property);

// Forward `SetPersistence` to the ScriptScene attached to `scene`. No-op
// when the scene has no script runtime.
void SetScenePersistence(sr::Scene& scene, std::string path);

} // namespace sr::script
