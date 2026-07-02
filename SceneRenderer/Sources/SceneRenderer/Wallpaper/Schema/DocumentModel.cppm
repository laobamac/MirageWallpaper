module;

#include <nlohmann/json.hpp>

export module sr.pkg.scene_obj:scene_document;
import rstd.cppstd;
import sr.fs;
import :field_binding;

export namespace sr

{

namespace wpscene
{

// pkg container version (the "PKGV00xx" stamp at the head of scene.pkg).
// Spans 1..23 in the live corpus. All scene.json schema evolution is gated
// on this axis (lightconfig/fog/hdr added in v23, etc.).
using SceneVersion = std::uint16_t;

// scene.json self-reported revision (top-level "version" int). Independent
// of SceneVersion: a single PKGV0023 pkg can contain scene.json with
// version 0/1/3/4/5. Captured for diagnostics, not used for dispatch.
using SceneJsonVersion = std::uint16_t;

constexpr SceneVersion     kSceneVersionUnknown     = 0;
constexpr SceneJsonVersion kSceneJsonVersionDefault = 0;

// Parse "PKGV0023" → 23. Returns kSceneVersionUnknown on any other shape.
SceneVersion ParsePkgVersionStamp(std::string_view stamp);

// Read top-level "version" number_unsigned; returns kSceneJsonVersionDefault
// when absent or wrong type.
SceneJsonVersion DetectSceneJsonVersion(const nlohmann::json& root);

class Orthogonalprojection {
public:
    bool    FromJson(const nlohmann::json&);
    int32_t width;
    int32_t height;
    bool    auto_ { false };
};

class SceneCamera {
public:
    bool                     FromJson(const nlohmann::json&);
    std::array<float, 3>     center { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     eye { 0.0f, 0.0f, 1.0f };
    std::array<float, 3>     up { 0.0f, 1.0f, 0.0f };
    std::vector<std::string> paths;
};

// PKGV0021+ — global maximum-light counts the runtime should be sized for
// (per WE editor configuration). All entries default to 0 if absent.
class SceneLightConfig {
public:
    bool          FromJson(const nlohmann::json&);
    std::uint32_t directional { 0 };
    std::uint32_t directionalshadow { 0 };
    std::uint32_t point { 0 };
    std::uint32_t pointshadow { 0 };
    std::uint32_t spot { 0 };
    std::uint32_t spotshadow { 0 };
};

class SceneGeneral {
public:
    bool FromJson(const nlohmann::json&);               // legacy
    bool FromJson(const nlohmann::json&, SceneVersion); // canonical

    // ---- baseline (PKGV0001+) ------------------------------------------
    std::array<float, 3> clearcolor { 0.0f, 0.0f, 0.0f };
    bool                 clearenabled { true };
    bool                 camerafade { false };
    bool                 camerapreview { false };
    bool                 cameraparallax { false };
    float                cameraparallaxamount { 0.0f };
    float                cameraparallaxdelay { 0.0f };
    float                cameraparallaxmouseinfluence { 0.0f };
    bool                 isOrtho { false };
    Orthogonalprojection orthogonalprojection { 1920, 1080 };
    float                zoom { 1.0f };
    float                fov { 50.0f };
    float                nearz { 0.01f };
    float                farz { 10000.0f };
    std::array<float, 3> ambientcolor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightcolor { 0.3f, 0.3f, 0.3f };

    // bloom / camerashake scalars exist since PKGV0001 but were never
    // unpacked into the struct before the version-aware split.
    bool                                         bloom { false };
    float                                        bloomstrength { 0.0f };
    float                                        bloomthreshold { 0.0f };
    bool                                         camerashake { false };
    float                                        camerashakeamplitude { 0.0f };
    float                                        camerashakespeed { 0.0f };
    float                                        camerashakeroughness { 0.0f };
    FieldBindings                                field_bindings;
    std::unordered_map<std::string, std::string> user_bindings;

    // ---- PKGV0010+ ------------------------------------------------------
    bool          hdr { false };
    bool          norecompile { false };
    float         bloomhdrfeather { 0.0f };
    std::uint32_t bloomhdriterations { 0 };
    float         bloomhdrscatter { 0.0f };
    float         bloomhdrstrength { 0.0f };
    float         bloomhdrthreshold { 0.0f };

    // ---- PKGV0020+ ------------------------------------------------------
    std::array<float, 3> bloomtint { 1.0f, 1.0f, 1.0f };

    // ---- PKGV0021+ ------------------------------------------------------
    float                perspectiveoverridefov { 0.0f };
    bool                 windenabled { false };
    std::array<float, 3> winddirection { 0.0f, 0.0f, 1.0f };
    float                windstrength { 0.0f };
    std::array<float, 3> gravitydirection { 0.0f, -1.0f, 0.0f };
    float                gravitystrength { 0.0f };

    // ---- PKGV0022+ ------------------------------------------------------
    bool                 transparentsorting { false };
    bool                 fogdistance { false };
    float                fogdistancestart { 0.0f };
    float                fogdistanceend { 0.0f };
    std::array<float, 3> fogdistancecolor { 1.0f, 1.0f, 1.0f };
    float                fogdistancestartdensity { 0.0f };
    float                fogdistanceenddensity { 0.0f };

    // ---- PKGV0023+ ------------------------------------------------------
    bool                 fogheight { false };
    float                fogheightstart { 0.0f };
    float                fogheightend { 0.0f };
    std::array<float, 3> fogheightcolor { 1.0f, 1.0f, 1.0f };
    float                fogheightstartdensity { 0.0f };
    float                fogheightenddensity { 0.0f };

    // PKGV0021+ — global per-kind maximum light counts.
    SceneLightConfig lightconfig;
};

class SceneMetadata {
public:
    bool             FromJson(const nlohmann::json&); // legacy: defaults to unknown version
    bool             FromJson(const nlohmann::json&, SceneVersion); // canonical entry
    SceneVersion     pkg_version { kSceneVersionUnknown };
    SceneJsonVersion scene_json_version { kSceneJsonVersionDefault };
    SceneCamera      camera;
    SceneGeneral     general;
    std::optional<std::array<uint32_t, 2>> canvas_extent;
};

enum class SceneObjectKind
{
    Unknown,
    Container,
    Image,
    Particle,
    Sound,
    Light,
    Text,
    Model,
    Camera,
};

class SceneObjectMetadata {
public:
    SceneObjectKind                     kind { SceneObjectKind::Unknown };
    std::size_t                         raw_index { 0 };
    std::int32_t                        id { 0 };
    std::string                         name;
    bool                                visible { true };
    std::uint32_t                       parent { 0 };
    std::optional<std::array<float, 2>> size;
};

class SceneDocument {
public:
    nlohmann::json                   root_json;
    SceneMetadata                    metadata;
    std::vector<SceneObjectMetadata> objects_metadata;
};

std::optional<SceneDocument> ParseSceneDocumentJson(std::string_view, SceneVersion);
std::optional<SceneDocument> LoadSceneDocumentFromVfs(fs::VFS&, std::string_view, SceneVersion);
std::optional<SceneDocument> LoadSceneDocumentFromPkg(std::string_view);
std::optional<SceneDocument> LoadSceneDocumentFromSource(std::string_view);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Orthogonalprojection, width, height);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneCamera, center, eye, up);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneGeneral, clearcolor, orthogonalprojection, zoom);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneMetadata, camera, general);
} // namespace wpscene
} // namespace sr
