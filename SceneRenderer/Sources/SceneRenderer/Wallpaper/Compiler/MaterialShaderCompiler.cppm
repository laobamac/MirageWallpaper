module;

export module sr.pkg.parse:wp_shader_parser;
import nlohmann.json;
import sr.core;
import sr.types;
import rstd.cppstd;
import sr.shader_compile;
import sr.scene;
import sr.fs;

export import :wp_uniform;

export namespace sr

{
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<i32, std::string>>;

// Staged direct-route u_* uniforms (shader annotation's `material` field
// equals the wallpaper-level project.json key). LoadMaterial fills this
// during compile; the caller registers it into `Scene::shader_user_var_index`
// AFTER `AddMaterial` so the stored pointer references the shared_ptr-owned
// SceneMaterial, not the stack-local one being moved.
struct UserVarRecord {
    std::string    material;      // project.json key (== shader annotation's material)
    std::string    name;          // GLSL identifier (e.g. "u_Brightness")
    nlohmann::json default_value; // raw default from annotation; may be null
};

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;
    bool             normalize_tangent_space { false };

    // Full annotation metadata. Renderer reads `combos / svs / defTexs /
    // alias` on the hot path; the editor / material UI and the user-property
    // bridge for `u_*` uniforms read `combo_defs / scalar_uniforms`.
    std::vector<wpscene::WPCombo>      combo_defs;
    std::vector<wpscene::WPUniformVar> scalar_uniforms;

    // Filled by LoadMaterial for the direct-binding u_* route. The
    // scene-instance-level user-binding route (effect-key → wallpaper-key)
    // is registered separately from `Material::constantshadervalues_user`.
    std::vector<UserVarRecord> user_var_staging;
};

struct WPPreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;

    // `uniform TYPE NAME;` declarations for non-sampler types. Captured
    // per-stage so Finalprocessor can build a cross-stage union and emit
    // a single shared cbuffer (matching what glslang's iomapper used to
    // produce). Without this, DXC's per-stage $Globals cbuffers desync
    // and FS-only uniforms read as zero.
    Map<std::string, std::string> uniforms; // name -> "TYPE"

    Set<unsigned> active_tex_slots;
};

struct WPShaderTexInfo {
    bool                enabled { false };
    std::array<bool, 3> composEnabled { false, false, false };
};

struct WPShaderUnit {
    ShaderType         stage;
    std::string        src;
    WPPreprocessorInfo preprocess_info;
};

// Output of CompileMaterialShader. On ok=true, spvs holds one SPIR-V
// blob per stage (currently always vertex+fragment in that order).
// On ok=false, error carries a short diagnostic.
struct CompileMaterialShaderResult {
    bool                         ok { false };
    std::vector<ShaderCode>      spvs;
    WPShaderInfo                 info;
    std::vector<WPShaderTexInfo> tex_info;
    std::string                  error;
    std::string                  shader_name;
};

// Per-stage shader-annotation parser. Implementation lives in
// MaterialProgramCompiler backend; declaration here so the rest of the parse
// module sees it. Not exported — internal helper.
void ParseWPShader(const std::string& src, WPShaderInfo* info,
                   const std::vector<WPShaderTexInfo>& texinfos);

class MaterialProgramCompiler {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static void InitGlslang();
    static void FinalGlslang();

    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, fs::VFS&, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>);

    // Lightweight entry point: compile the vert+frag shader pair for one
    // material directly, without instantiating a Scene or running the
    // full SceneParser pipeline.
    //
    // Inputs come from the material JSON (parsed via Material::FromJson)
    // plus the VFS that resolves /assets/shaders/<material.shader>.{vert,frag}
    // and #include directives. combos_override entries win over the
    // material's own combos. BLENDMODE=0 and BONECOUNT=1 are seeded if
    // absent.
    //
    // Caveat: combos that ParseImageObj derives from object-level state
    // (color-blend mode, sprite-sheet flags, puppet bone count beyond
    // default, etc.) are NOT injected. Materials that hard-require them
    // will fail compile here; supply the right values via combos_override.
    static CompileMaterialShaderResult CompileMaterialShader(const nlohmann::json& material_json,
                                                             fs::VFS&              vfs,
                                                             std::string_view scene_id = "test",
                                                             const Combos&    combos_override = {});
};
} // namespace sr
