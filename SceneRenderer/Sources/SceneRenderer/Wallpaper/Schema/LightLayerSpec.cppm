module;

export module sr.pkg.scene_obj:light_object;
import nlohmann.json;
import rstd.cppstd;
import sr.fs;

export import :field_binding;
import :visibility_binding;
export import sr.pkg.puppet;
import :scene_document;

export namespace sr

{

namespace wpscene
{

class LightObject {
public:
    bool                 FromJson(const nlohmann::json&, fs::VFS&);               // legacy
    bool                 FromJson(const nlohmann::json&, fs::VFS&, SceneVersion); // canonical
    int32_t              id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::string          light; // "point" / "spot" / "directional" / ...
    std::string          shape; // PKGV0021+
    float                radius { 1000.0f };
    float                intensity { 1.0f };
    bool                 visible { true };
    VisibleUserBinding   visible_user;
    std::string          visible_user_key;

    // Common cross-kind metadata.
    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json            instance;
    FieldBindings             field_bindings;

    // Light-kind specifics.
    bool  ledsource { false };          // PKGV0006+
    bool  castshadow { false };         // PKGV0019+
    bool  castvolumetrics { false };    // PKGV0019+
    float outercone { 0.0f };           // PKGV0019+
    float innercone { 0.0f };           // PKGV0019+
    float attenuation { 0.0f };         // PKGV0023+
    float exponent { 1.0f };            // PKGV0021+
    float density { 1.0f };             // PKGV0021+
    float volumetricsexponent { 1.0f }; // PKGV0021+
    float lightsourcesize { 0.0f };     // PKGV0022+
    float mindistance { 0.0f };         // PKGV0023+
    float cascadedistance0 { 0.0f };    // PKGV0021+
    float cascadedistance1 { 0.0f };
    float cascadedistance2 { 0.0f };
};

} // namespace wpscene
} // namespace sr
/*
        {
            "angles" : "0.00000 0.00000 0.00000",
            "color" : "1.00000 0.95686 0.87451",
            "id" : 237,
            "intensity" : 0.5,
            "light" : "point",
            "locktransforms" : false,
            "name" : "",
            "origin" : "611.30676 302.13736 2000.00000",
            "parallaxDepth" : "0.00000 0.00000",
            "radius" : 3000.0,
            "scale" : "1.00000 1.00000 1.00000",
            "visible" : true
        },
*/
