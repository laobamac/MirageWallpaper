export module sr.pkg.scene_obj:material;
import rstd.cppstd;
import sr.fs;
import :scene_document;
export import :field_binding;

export namespace sr

{
namespace wpscene
{

class MaterialPassBindItem {
public:
    bool        FromJson(const sr::Json&);
    std::string name;
    int32_t     index;
};

class MaterialPass {
public:
    bool                                                FromJson(const sr::Json&);
    void                                                Update(const MaterialPass&);
    std::uint32_t                                       id { 0 }; // pass id (PKGV0001+)
    std::vector<std::string>                            textures;
    rstd::json::Array                                   usertextures; // PKGV0018+; polymorphic
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    // scene.json instance-level user binding:
    //   "constantshadervalues": { "Opacity": {"user":"luzopacidad","value":1} }
    // Maps effect-internal material key → wallpaper-level project.json key.
    // The fallback `value` is already extracted into `constantshadervalues`
    // by GetJsonValue's auto-unwrap.
    std::unordered_map<std::string, std::string> constantshadervalues_user;
    std::unordered_map<std::string, AnimCurve>   constantshadervalues_animations;
    FieldBindings                                constantshadervalues_bindings;
    // Legacy `usershadervalues`: project.json key -> shader material key.
    std::unordered_map<std::string, std::string> user_shader_values;
    std::string                                  target;
    std::vector<MaterialPassBindItem>            bind;
};

class Material : public rstd::DefaultInClass<Material, rstd::clone::Clone> {
public:
    Material()                               = default;
    Material(const Material&)                = delete;
    Material& operator=(const Material&)     = delete;
    Material(Material&&) noexcept            = default;
    Material& operator=(Material&&) noexcept = default;

    bool                                     FromJson(const sr::Json&);               // legacy
    bool                                     FromJson(const sr::Json&, SceneVersion); // canonical
    auto                                     clone() const -> Material;
    void                                     MergePass(const MaterialPass&);
    void MergeBindingOverrides(const std::vector<std::string>&                 textures,
                               const rstd::json::Array&                        usertextures,
                               const std::unordered_map<std::string, int32_t>& combos);
    std::string                              blending { "translucent" };
    std::string                              cullmode { "nocull" };
    std::string                              shader;
    std::string                              depthtest { "disabled" };
    std::string                              depthwrite { "disabled" };
    std::vector<std::string>                 textures;
    rstd::json::Array                        usertextures;
    std::unordered_map<std::string, int32_t> combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::unordered_map<std::string, std::string>        constantshadervalues_user;
    std::unordered_map<std::string, AnimCurve>          constantshadervalues_animations;
    FieldBindings                                       constantshadervalues_bindings;
    std::unordered_map<std::string, std::string>        user_shader_values;

    bool use_puppet { false };
};

} // namespace wpscene
} // namespace sr
