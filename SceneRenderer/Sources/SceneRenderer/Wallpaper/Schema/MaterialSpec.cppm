module;

#include <nlohmann/json.hpp>

export module sr.pkg.scene_obj:material;
import rstd.cppstd;
import sr.fs;
import :scene_document;

export namespace sr

{
namespace wpscene
{

class MaterialPassBindItem {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    int32_t     index;
};

class MaterialPass {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                Update(const MaterialPass&);
    std::uint32_t                                       id { 0 }; // pass id (PKGV0001+)
    std::vector<std::string>                            textures;
    std::vector<nlohmann::json>                         usertextures; // PKGV0018+; polymorphic
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    // scene.json instance-level user binding:
    //   "constantshadervalues": { "Opacity": {"user":"luzopacidad","value":1} }
    // Maps effect-internal material key → wallpaper-level project.json key.
    // The fallback `value` is already extracted into `constantshadervalues`
    // by GetJsonValue's auto-unwrap.
    std::unordered_map<std::string, std::string> constantshadervalues_user;
    // Legacy `usershadervalues`: project.json key -> shader material key.
    std::unordered_map<std::string, std::string> user_shader_values;
    std::string                                  target;
    std::vector<MaterialPassBindItem>            bind;
};

class Material {
public:
    bool                        FromJson(const nlohmann::json&);               // legacy
    bool                        FromJson(const nlohmann::json&, SceneVersion); // canonical
    void                        MergePass(const MaterialPass&);
    std::string                 blending { "translucent" };
    std::string                 cullmode { "nocull" };
    std::string                 shader;
    std::string                 depthtest { "disabled" };
    std::string                 depthwrite { "disabled" };
    std::vector<std::string>    textures;
    std::vector<nlohmann::json> usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::unordered_map<std::string, std::string>        constantshadervalues_user;
    std::unordered_map<std::string, std::string>        user_shader_values;

    bool use_puppet { false };
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MaterialPassBindItem, name, index);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MaterialPass, bind, target, textures, combos,
                                   constantshadervalues);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Material, blending, shader, textures, combos,
                                   constantshadervalues);
} // namespace wpscene
} // namespace sr
