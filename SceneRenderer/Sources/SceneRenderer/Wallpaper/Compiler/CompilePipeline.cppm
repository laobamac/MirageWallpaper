module;

export module sr.pkg.parse:scene_stages;
import eigen;

import rstd;
import rstd.cppstd;
import wavsen.audio;
import sr.core;
import sr.json;
import sr.fs;
import sr.scene;
import sr.script;
import sr.scene_uniform_updater;
import sr.types;
import sr.pkg.scene_obj;

import sr.pkg.puppet;

export namespace sr
{

using SceneObjectVar = std::variant<wpscene::ImageObject, wpscene::ShapeObject,
                                    wpscene::ParticleObject, wpscene::SoundObject,
                                    wpscene::LightObject, wpscene::TextObject,
                                    wpscene::ModelObject, wpscene::CameraObject>;

struct PuppetLayerRegistry {
    std::unordered_map<SceneNode*, std::shared_ptr<WPPuppetLayer>> by_node;
    std::unordered_map<SceneNode*, std::shared_ptr<WPPuppetLayer>> fallback_by_node;
};

enum class TextRenderMode
{
    Direct,
    Offscreen,
};

struct TextSurfaceRequirements {
    bool has_effect { false };
    bool copy_background { false };
    bool opaque_background { false };
    bool linked_source { false };
};

constexpr TextRenderMode ResolveTextRenderMode(TextSurfaceRequirements requirements) {
    return requirements.has_effect || requirements.copy_background ||
                   requirements.opaque_background || requirements.linked_source
               ? TextRenderMode::Offscreen
               : TextRenderMode::Direct;
}

// Per-Parse state. Built by BuildContext, mutated by ProcessObjects,
// finalized by FinalizeScene. Holding it as a public struct lets the
// CLI test driver run any subset of the pipeline.
struct ParseContext {
    std::shared_ptr<Scene>                   scene;
    SceneUniformUpdater*                     shader_updater { nullptr };
    i32                                      ortho_w { 0 };
    i32                                      ortho_h { 0 };
    bool                                     orthographic_scene { false };
    fs::VFS*                                 vfs { nullptr };
    wpscene::SceneVersion                    pkg_version { wpscene::kSceneVersionUnknown };
    rstd::Option<rstd::ref<rstd::json::Map>> user_properties;

    ShaderValueMap                           global_base_uniforms;
    rstd::Option<rstd::sync::Arc<SceneNode>> effect_camera_node;
    rstd::Option<rstd::sync::Arc<SceneNode>> global_camera_node;
    rstd::Option<rstd::sync::Arc<SceneNode>> global_perspective_camera_node;

    // Lazily allocated by WireFieldScripts as objects with script
    // bindings come in. Installed onto the Scene by FinalizeScene.
    // Stays null when no object has any script binding.
    std::unique_ptr<sr::script::ScriptScene> script_scene;
    using ImageAlignmentSetter =
        std::function<void(SceneNode*, std::string_view)>;
    struct ImageAlignmentBinding {
        SceneNode*           node { nullptr };
        std::string          alignment;
        ImageAlignmentSetter setter;
    };
    std::vector<ImageAlignmentBinding> image_alignment_bindings;
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
        std::vector<rstd::sync::Arc<SceneNode>>     ordered_before_nodes;
    };
    std::unordered_map<std::int32_t, NodeRef>       node_id_map;
    std::unordered_map<std::int32_t, std::uint32_t> object_parent_ids;
    std::unordered_map<std::int32_t, std::uint64_t> script_initialization_orders;
    std::unordered_map<std::int32_t, Json>          initial_layer_configs;
    Set<std::int32_t>                               solid_layer_ids;
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
    std::int32_t next_dynamic_layer_id { -100000 };
    struct CreateLayerAssetRequest {
        sr::script::FieldScript* script { nullptr };
        std::int32_t              owner_id { 0 };
    };
    std::vector<CreateLayerAssetRequest> create_layer_asset_requests;
    wavsen::audio::SoundManager*          sound_manager { nullptr };

    std::unordered_map<std::int32_t, std::string> system_media_image_fallbacks;
    Set<std::int32_t>                             linked_source_ids;
    Set<std::int32_t>                             hidden_link_source_ids;
    std::string                                    script_persistence_path;
    Set<std::string>                              unresolved_shader_values;
    bool                                          scene_has_scripts { false };
    bool                                          scene_accesses_effects { false };
    bool                                          scene_layer_text_writes { false };

    bool IsLinkedSource(std::int32_t id) const { return linked_source_ids.contains(id); }
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
ExpandObjects(const Json&, fs::VFS&, wpscene::SceneVersion,
              rstd::Option<rstd::ref<rstd::json::Map>> user_props        = rstd::None(),
              const Set<std::int32_t>*                 linked_source_ids = nullptr);

// Resolves the effective width/height without mutating the parsed metadata.
std::array<i32, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata&,
                                                std::span<const SceneObjectVar>);

// Allocates Scene + cameras + base uniforms + the two default render
// targets (SpecTex_Default, WE_MIP_MAPPED_FRAME_BUFFER).
ParseContext BuildContext(fs::VFS&, std::string_view scene_id, const wpscene::SceneMetadata&,
                          std::array<i32, 2>                       ortho_extent,
                          rstd::Option<rstd::ref<rstd::json::Map>> user_properties = rstd::None(),
                          std::string script_persistence_path = {});

// Per-object dispatch. Brackets glslang init/finalize around the visit
// loop. opts.kinds masks which kinds run; default is all-kinds. Sound
// dispatch additionally requires sm non-null.
void ProcessObjects(ParseContext&, std::span<SceneObjectVar>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {});

// Installs the lazily-built ScriptScene onto the Scene (if any) and
// returns the now-frozen Scene.
std::shared_ptr<Scene> FinalizeScene(ParseContext&);

} // namespace sr
