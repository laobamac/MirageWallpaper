module;

export module sr.pkg.parse:scene_stages;
import eigen;
import nlohmann.json;

import rstd;
import rstd.cppstd;
import wavsen.audio;
import sr.core;
import sr.fs;
import sr.scene;
import sr.script;
import sr.scene_uniform_updater;
import sr.types;
import sr.pkg.scene_obj;

import sr.pkg.puppet;

export namespace sr
{

using SceneObjectVar = std::variant<wpscene::ImageObject, wpscene::ParticleObject,
                                    wpscene::SoundObject, wpscene::LightObject, wpscene::TextObject,
                                    wpscene::ModelObject, wpscene::CameraObject>;

struct PuppetLayerRegistry {
    std::unordered_map<SceneNode*, std::shared_ptr<WPPuppetLayer>> by_node;
    std::unordered_map<SceneNode*, std::shared_ptr<WPPuppetLayer>> fallback_by_node;
};

// Per-Parse state. Built by BuildContext, mutated by ProcessObjects,
// finalized by FinalizeScene. Holding it as a public struct lets the
// CLI test driver run any subset of the pipeline.
struct ParseContext {
    std::shared_ptr<Scene>                                 scene;
    SceneUniformUpdater*                                   shader_updater { nullptr };
    i32                                                    ortho_w { 0 };
    i32                                                    ortho_h { 0 };
    fs::VFS*                                               vfs { nullptr };
    const std::unordered_map<std::string, nlohmann::json>* user_properties { nullptr };

    ShaderValueMap                           global_base_uniforms;
    rstd::Option<rstd::sync::Arc<SceneNode>> effect_camera_node;
    rstd::Option<rstd::sync::Arc<SceneNode>> global_camera_node;
    rstd::Option<rstd::sync::Arc<SceneNode>> global_perspective_camera_node;

    // Lazily allocated by WireFieldScripts as objects with script
    // bindings come in. Installed onto the Scene by FinalizeScene.
    // Stays null when no object has any script binding.
    std::unique_ptr<sr::script::ScriptScene> script_scene;
    std::shared_ptr<PuppetLayerRegistry> puppet_layers { std::make_shared<PuppetLayerRegistry>() };

    // ID → (parent_id, node) for every parseable object. Filled by each
    // ParseXObj. FinalizeScene re-parents nodes with non-zero parent_id
    // from the scene root onto their actual parent. WE wallpapers use
    // this to position child layers relative to a script-driven parent
    // (e.g. workshop 3327063360's "Audio Bars" hardcoded at (-155, 322)
    // is parent=4995, the "总组件" centre).
    struct NodeRef {
        std::uint32_t                            parent_id { 0 };
        rstd::Option<rstd::sync::Arc<SceneNode>> node;
        // Carried for cross-node wiring at FinalizeScene attach time.
        // `puppet` populated for image objects that own an MDL skeleton,
        // so a child layer with `attachment = "<name>"` can resolve the
        // matching MDAT entry on its parent's puppet (no second lookup
        // pass needed). Both nullable.
        std::shared_ptr<WPPuppet>                   puppet;
        std::string                                 attachment;
        std::shared_ptr<WPPuppetLayer>              puppet_layer;
        std::function<void(const Eigen::Vector3f&)> apply_attachment_offset;
    };
    std::unordered_map<std::int32_t, NodeRef> node_id_map;
    // Scene.json declaration order. Reparenting in this order keeps each
    // container's children in the order they appeared in scene.json (so
    // layer 28 stays the first child of layer 79). Iterating the unordered
    // map directly would scramble z-order and let the background overwrite
    // foreground layers.
    std::vector<std::int32_t> node_id_order;

    // Audio-bar fanout clones, keyed by their template layer's id. Held here
    // (not appended to the graph at spawn time) so FinalizeScene can attach
    // them right after the template node — keeping all bars at the template's
    // z-position instead of jumping to the front of the root child list.
    std::unordered_map<std::int32_t, std::vector<rstd::sync::Arc<SceneNode>>> layer_clones;

    std::unordered_map<std::int32_t, std::string> image_texture_fallbacks;
    Set<std::int32_t>                             hidden_link_source_ids;
};

struct ProcessOpts {
    enum Kind : unsigned
    {
        Image    = 1u << 0,
        Particle = 1u << 1,
        Sound    = 1u << 2,
        Light    = 1u << 3,
        Text     = 1u << 4,
        Model    = 1u << 5,
        All      = 0x3Fu,
    };
    unsigned kinds { All };
};

// Walks json["objects"] and instantiates one SceneObjectVar per recognised
// kind via FromJson. Pure JSON deserialisation plus per-object VFS
// reads (for image/material refs); no Scene / glslang touched.
// `user_props` (nullable) lets `visible:{user:"<key>"}` resolve to the
// host's current bool, so layers toggled off in the UI are pruned at
// parse time.
std::vector<SceneObjectVar>
ExpandObjects(const nlohmann::json&, fs::VFS&, wpscene::SceneVersion,
              const std::unordered_map<std::string, nlohmann::json>* user_props        = nullptr,
              const Set<std::int32_t>*                               linked_source_ids = nullptr);

// If general.orthogonalprojection.auto_, replaces width/height with the
// largest image object's size.
void AdjustAutoOrthoProjection(wpscene::SceneMetadata&, std::span<const SceneObjectVar>);

// Allocates Scene + cameras + base uniforms + the two default render
// targets (SpecTex_Default, WE_MIP_MAPPED_FRAME_BUFFER).
ParseContext BuildContext(fs::VFS&, std::string_view scene_id, wpscene::SceneMetadata&);

// Per-object dispatch. Brackets glslang init/finalize around the visit
// loop. opts.kinds masks which kinds run; default is all-kinds. Sound
// dispatch additionally requires sm non-null.
void ProcessObjects(ParseContext&, std::span<SceneObjectVar>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {});

// Installs the lazily-built ScriptScene onto the Scene (if any) and
// returns the now-frozen Scene.
std::shared_ptr<Scene> FinalizeScene(ParseContext&);

} // namespace sr
