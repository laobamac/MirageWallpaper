export module sr.pkg.scene_obj:image_object;
import sr.core;
import rstd.cppstd;
import sr.fs;

export import :animation_layer;
export import :field_binding;
import :visibility_binding;
export import :material;
import :scene_document;

export namespace sr

{

namespace wpscene
{

class EffectCommand {
public:
    bool        FromJson(const sr::Json&);
    std::string command;
    std::string target;
    std::string source;

    i32 afterpos { 0 }; // 0 for begin, start from 1
};

class EffectFbo {
public:
    bool        FromJson(const sr::Json&);
    std::string name;
    std::string format;
    uint32_t    scale { 1 };
    uint32_t    fit { 0 };
    bool        unique { false };
};

// objects[].instance — PKGV0018+ embedded material overrides.
class ObjectInstance {
public:
    bool                                          FromJson(const sr::Json&);
    bool                                          present { false };
    std::uint32_t                                 id { 0 };
    std::unordered_map<std::string, std::int32_t> combos;
    std::vector<std::string>                      textures;
    // usertextures elements are polymorphic: bare property-name strings
    // (PKGV0022+) and `{name, type}` system bindings (PKGV0018+). Stored
    // as raw json so both shapes are preserved.
    rstd::json::Array usertextures;
};

class ImageEffect {
public:
    bool                       FromJson(const sr::Json&, fs::VFS& vfs);               // legacy
    bool                       FromJson(const sr::Json&, fs::VFS& vfs, SceneVersion); // canonical
    bool                       FromFileJson(const sr::Json&, fs::VFS& vfs);
    int32_t                    id;
    std::string                name;
    std::string                username; // PKGV0001+; per-instance label override
    bool                       visible { true };
    VisibleUserBinding         visible_user;
    std::string                visible_user_key;
    int32_t                    version;
    std::vector<Material>      materials;
    std::vector<MaterialPass>  passes;
    std::vector<EffectCommand> commands;
    std::vector<EffectFbo>     fbos;
};

class ImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                     FromJson(const sr::Json&, fs::VFS&);               // legacy
    bool                     FromJson(const sr::Json&, fs::VFS&, SceneVersion); // canonical
    int32_t                  id { 0 };
    std::string              name;
    std::array<float, 3>     origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>     angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>     size { 2.0f, 2.0f };
    std::array<float, 2>     parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3>     color { 1.0f, 1.0f, 1.0f };
    int32_t                  colorBlendMode { 0 };
    float                    alpha { 1.0f };
    float                    brightness { 1.0f };
    bool                     fullscreen { false };
    bool                     composite_layer { false };
    bool                     nopadding { false };
    bool                     visible { true };
    std::string              image;
    std::string              alignment { "center" };
    Material                 material;
    std::vector<ImageEffect> effects;
    Config                   config;

    // Common cross-kind metadata (PKGV0001+ unless noted).
    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false }; // PKGV0021+
    std::uint32_t             parent { 0 };              // PKGV0019+; 0 = no parent
    std::vector<std::int32_t> dependencies;              // PKGV0001+; referenced object ids
    ObjectInstance            instance;                  // PKGV0018+; instance binding

    // Image-kind specifics (gates listed for reference; reads are unconditional via _NOWARN).
    bool                 perspective { false };    // PKGV0002+
    bool                 copybackground { false }; // PKGV0001+
    bool                 solid { false };          // PKGV0002+
    bool                 solid_layer { false };
    bool                 opaquebackground { false };           // PKGV0005+
    bool                 clampuvs { false };                   // PKGV0022+
    bool                 castshadow { false };                 // PKGV0019+
    bool                 disablepropagation { false };         // PKGV0023+
    std::string          depthtest { "enabled" };              // PKGV0020+
    std::array<float, 3> backgroundcolor { 0.0f, 0.0f, 0.0f }; // PKGV0005+
    float                backgroundbrightness { 1.0f };        // PKGV0010+

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;

    // PKGV0019+ named anchor on the parent's puppet (MDAT attachment). The
    // owning image renders at the parent puppet's bone[attachment.bone_index]
    // offset by attachment.local_xform (see WPPuppet::Attachment). Empty
    // string means no bone anchoring; the image inherits parent transform
    // directly.
    std::string attachment;

    // Per-field property-binding side channel; populated when scalar
    // fields (origin/scale/alpha/...) carry an `animation` curve or a
    // `scriptproperties` subtree. See FieldBinding.cppm.
    FieldBindings field_bindings;

    VisibleUserBinding visible_user;
    std::string        visible_user_key;
    UserValueBinding   color_user;
    std::string        color_user_key;
    UserValueBinding   alpha_user;
    std::string        alpha_user_key;
};

class ImageAssetInfo {
public:
    std::optional<std::array<float, 2>> size;
    std::string                         first_texture;
    bool                                solid_layer { false };
};

std::optional<ImageAssetInfo> LoadImageAssetInfo(fs::VFS& vfs, std::string_view image);

} // namespace wpscene
} // namespace sr
