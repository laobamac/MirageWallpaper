module;

#if defined(__linux__)
#include <string>
#endif

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import sr.json;
import sr.pkg_fs;

using namespace sr::wpscene;

namespace sr::wpscene
{

SceneVersion ParsePkgVersionStamp(std::string_view stamp) {
    constexpr std::string_view kPrefix = "PKGV";
    if (stamp.size() < kPrefix.size() + 1) return kSceneVersionUnknown;
    if (stamp.substr(0, kPrefix.size()).data() != kPrefix.data()) return kSceneVersionUnknown;
    SceneVersion v       = 0;
    const char*  first   = stamp.data() + kPrefix.size();
    const char*  last    = stamp.data() + stamp.size();
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec != std::errc {} || end != last) return kSceneVersionUnknown;
    return v;
}

SceneJsonVersion DetectSceneJsonVersion(const sr::Json& root) {
    auto version = root.get("version");
    if (version.is_none()) return kSceneJsonVersionDefault;
    auto value = (*version)->as_u64();
    if (value.is_some() && *value <= std::numeric_limits<SceneJsonVersion>::max())
        return static_cast<SceneJsonVersion>(*value);
    return kSceneJsonVersionDefault;
}

} // namespace sr::wpscene

bool Orthogonalprojection::FromJson(const sr::Json& json) {
    if (json.is_null()) return false;
    if (json.get("auto").is_some()) {
        sr::GetJsonValue(json, "auto", auto_);
    } else {
        sr::GetJsonValue(json, "width", width);
        sr::GetJsonValue(json, "height", height);
    }
    return true;
}

bool SceneCamera::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "center", center);
    sr::GetJsonValue(json, "eye", eye);
    sr::GetJsonValue(json, "up", up);
    if (auto raw_paths = json.get("paths"); raw_paths.is_some()) {
        auto array = (*raw_paths)->as_array();
        if (array.is_none()) return true;
        for (const auto& path : **array) {
            auto value = path.as_str();
            if (value.is_some()) paths.push_back(rstd::cppstd::to_string(*value));
        }
    }
    return true;
}

bool SceneLightConfig::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "directional", directional, false);
    sr::GetJsonValue(json, "directionalshadow", directionalshadow, false);
    sr::GetJsonValue(json, "point", point, false);
    sr::GetJsonValue(json, "pointshadow", pointshadow, false);
    sr::GetJsonValue(json, "spot", spot, false);
    sr::GetJsonValue(json, "spotshadow", spotshadow, false);
    return true;
}

namespace
{

// A single SceneVersion is the sole gate for "should we attempt to read
// fields introduced in PKGVxxxx". An unknown version (loose dir mount,
// dump.cpp legacy entry) falls through every gate so behaviour matches
// the pre-refactor "try everything" path.
constexpr bool wants(SceneVersion v, SceneVersion gate) {
    return v == kSceneVersionUnknown || v >= gate;
}

void capture_user_bindings(SceneGeneral& g, const sr::Json& json) {
    auto object = json.as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto& field             = *entry_value;
        if (! field.is_object()) return;
        auto user = field.get("user");
        if (user.is_none() || ! (*user)->is_string()) return;
        g.user_bindings[rstd::cppstd::to_string(entry_key->as_str())] =
            rstd::cppstd::to_string(*(*user)->as_str());
    });
}

void parse_baseline(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "ambientcolor", g.ambientcolor);
    sr::GetJsonValue(json, "skylightcolor", g.skylightcolor);
    sr::GetJsonValue(json, "clearcolor", g.clearcolor);
    sr::GetJsonValue(json, "clearenabled", g.clearenabled, false);
    sr::GetJsonValue(json, "camerafade", g.camerafade, false);
    sr::GetJsonValue(json, "camerapreview", g.camerapreview, false);
    sr::GetJsonValue(json, "cameraparallax", g.cameraparallax);
    sr::GetJsonValue(json, "cameraparallaxamount", g.cameraparallaxamount);
    sr::GetJsonValue(json, "cameraparallaxdelay", g.cameraparallaxdelay);
    sr::GetJsonValue(json, "cameraparallaxmouseinfluence", g.cameraparallaxmouseinfluence);
    sr::GetJsonValue(json, "zoom", g.zoom, false);
    sr::GetJsonValue(json, "fov", g.fov, false);
    sr::GetJsonValue(json, "nearz", g.nearz, false);
    sr::GetJsonValue(json, "farz", g.farz, false);
    sr::GetJsonValue(json, "bloom", g.bloom, false);
    sr::GetJsonValue(json, "bloomstrength", g.bloomstrength, false);
    sr::GetJsonValue(json, "bloomthreshold", g.bloomthreshold, false);
    sr::GetJsonValue(json, "camerashake", g.camerashake, false);
    sr::GetJsonValue(json, "camerashakeamplitude", g.camerashakeamplitude, false);
    sr::GetJsonValue(json, "camerashakespeed", g.camerashakespeed, false);
    sr::GetJsonValue(json, "camerashakeroughness", g.camerashakeroughness, false);
    g.isOrtho = false;
    if (auto ortho = json.get("orthogonalprojection"); ortho.is_some()) {
        if ((*ortho)->is_null())
            g.isOrtho = false;
        else {
            g.isOrtho = true;
            g.orthogonalprojection.FromJson(**ortho);
        }
    }
}

void parse_v10_plus(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "hdr", g.hdr, false);
    sr::GetJsonValue(json, "norecompile", g.norecompile, false);
    sr::GetJsonValue(json, "bloomhdrfeather", g.bloomhdrfeather, false);
    sr::GetJsonValue(json, "bloomhdriterations", g.bloomhdriterations, false);
    sr::GetJsonValue(json, "bloomhdrscatter", g.bloomhdrscatter, false);
    sr::GetJsonValue(json, "bloomhdrstrength", g.bloomhdrstrength, false);
    sr::GetJsonValue(json, "bloomhdrthreshold", g.bloomhdrthreshold, false);
}

void parse_v20_plus(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "bloomtint", g.bloomtint, false);
}

void parse_v21_plus(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "perspectiveoverridefov", g.perspectiveoverridefov, false);
    sr::GetJsonValue(json, "windenabled", g.windenabled, false);
    sr::GetJsonValue(json, "winddirection", g.winddirection, false);
    sr::GetJsonValue(json, "windstrength", g.windstrength, false);
    sr::GetJsonValue(json, "gravitydirection", g.gravitydirection, false);
    sr::GetJsonValue(json, "gravitystrength", g.gravitystrength, false);
}

void parse_v22_plus(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "transparentsorting", g.transparentsorting, false);
    sr::GetJsonValue(json, "fogdistance", g.fogdistance, false);
    sr::GetJsonValue(json, "fogdistancestart", g.fogdistancestart, false);
    sr::GetJsonValue(json, "fogdistanceend", g.fogdistanceend, false);
    sr::GetJsonValue(json, "fogdistancecolor", g.fogdistancecolor, false);
    sr::GetJsonValue(json, "fogdistancestartdensity", g.fogdistancestartdensity, false);
    sr::GetJsonValue(json, "fogdistanceenddensity", g.fogdistanceenddensity, false);
}

void parse_v23_plus(SceneGeneral& g, const sr::Json& json) {
    sr::GetJsonValue(json, "fogheight", g.fogheight, false);
    sr::GetJsonValue(json, "fogheightstart", g.fogheightstart, false);
    sr::GetJsonValue(json, "fogheightend", g.fogheightend, false);
    sr::GetJsonValue(json, "fogheightcolor", g.fogheightcolor, false);
    sr::GetJsonValue(json, "fogheightstartdensity", g.fogheightstartdensity, false);
    sr::GetJsonValue(json, "fogheightenddensity", g.fogheightenddensity, false);
}

void parse_lightconfig(SceneGeneral& g, const sr::Json& json) {
    if (auto lightconfig = json.get("lightconfig");
        lightconfig.is_some() && (*lightconfig)->is_object()) {
        g.lightconfig.FromJson(**lightconfig);
    }
}

SceneObjectKind object_kind(const sr::Json& obj) {
    if (! obj.is_object()) return SceneObjectKind::Unknown;
    if (auto value = obj.get("image"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Image;
    if (auto value = obj.get("particle"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Particle;
    if (auto value = obj.get("sound"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Sound;
    if (auto value = obj.get("light"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Light;
    if (auto value = obj.get("text"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Text;
    if (auto value = obj.get("model"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Model;
    if (auto value = obj.get("camera"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Camera;
    return SceneObjectKind::Container;
}

SceneObjectMetadata parse_object_metadata(const sr::Json& obj, std::size_t raw_index) {
    SceneObjectMetadata metadata;
    metadata.raw_index = raw_index;
    metadata.kind      = object_kind(obj);
    if (! obj.is_object()) return metadata;

    sr::GetJsonValue(obj, "id", metadata.id, false);
    sr::GetJsonValue(obj, "name", metadata.name, false);
    sr::GetJsonValue(obj, "visible", metadata.visible, false);
    sr::GetJsonValue(obj, "parent", metadata.parent, false);

    std::array<float, 2> size {};
    if (sr::GetJsonValue(obj, "size", size, false) && size[0] > 0.0f && size[1] > 0.0f) {
        metadata.size = size;
    }
    return metadata;
}

std::vector<SceneObjectMetadata> parse_objects_metadata(const sr::Json& root) {
    std::vector<SceneObjectMetadata> objects;
    auto                             raw_objects = root.get("objects");
    if (raw_objects.is_none()) return objects;

    auto array = (*raw_objects)->as_array();
    if (array.is_none()) return objects;
    objects.reserve((*array)->len());
    for (std::size_t i = 0; i < (*array)->len(); ++i) {
        objects.push_back(parse_object_metadata((**array)[i], i));
    }
    return objects;
}

std::optional<std::array<uint32_t, 2>> image_extent(const SceneObjectMetadata& obj) {
    if (obj.kind != SceneObjectKind::Image || ! obj.size) return std::nullopt;
    return std::array<uint32_t, 2> { static_cast<uint32_t>((*obj.size)[0]),
                                     static_cast<uint32_t>((*obj.size)[1]) };
}

std::optional<std::array<uint32_t, 2>>
largest_image_extent(std::span<const SceneObjectMetadata> objects) {
    std::optional<std::array<uint32_t, 2>> best;
    uint64_t                               best_area = 0;
    for (const auto& obj : objects) {
        auto extent = image_extent(obj);
        if (! extent) continue;
        const uint64_t area =
            static_cast<uint64_t>((*extent)[0]) * static_cast<uint64_t>((*extent)[1]);
        if (area > best_area) {
            best      = *extent;
            best_area = area;
        }
    }
    return best;
}

std::optional<std::array<uint32_t, 2>>
scene_canvas_extent(const SceneMetadata&                 metadata,
                    std::span<const SceneObjectMetadata> objects_metadata) {
    const auto& general = metadata.general;
    if (! general.isOrtho) return std::nullopt;

    const auto& ortho = general.orthogonalprojection;
    if (! ortho.auto_) {
        if (ortho.width <= 0 || ortho.height <= 0) return std::nullopt;
        return std::array<uint32_t, 2> { static_cast<uint32_t>(ortho.width),
                                         static_cast<uint32_t>(ortho.height) };
    }
    return largest_image_extent(objects_metadata);
}

} // namespace

bool SceneGeneral::FromJson(const sr::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneGeneral::FromJson(const sr::Json& json, SceneVersion v) {
    parse_baseline(*this, json);
    if (wants(v, 10)) parse_v10_plus(*this, json);
    if (wants(v, 20)) parse_v20_plus(*this, json);
    if (wants(v, 21)) parse_v21_plus(*this, json);
    if (wants(v, 22)) parse_v22_plus(*this, json);
    if (wants(v, 23)) parse_v23_plus(*this, json);
    if (wants(v, 21)) parse_lightconfig(*this, json);
    AbsorbAllFieldBindings(json, field_bindings);
    capture_user_bindings(*this, json);
    return true;
}

bool SceneMetadata::FromJson(const sr::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneMetadata::FromJson(const sr::Json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if (auto camera_json = json.get("camera"); camera_json.is_some()) {
        // camera schema is identical across PKGV0001..PKGV0023; no version gate needed.
        camera.FromJson(**camera_json);
    } else {
        rstd_error("scene no camera");
        return false;
    }
    if (auto general_json = json.get("general"); general_json.is_some()) {
        general.FromJson(**general_json, v);
    } else {
        rstd_error("scene no genera data");
        return false;
    }
    return true;
}

namespace sr::wpscene
{

std::optional<SceneDocument> ParseSceneDocumentJson(std::string_view buf,
                                                    SceneVersion     pkg_version) {
    SceneDocument doc;
    auto          parsed = sr::ParseJson(buf);
    if (parsed.is_err()) {
        rstd_error("Can't parse scene json: {}", parsed.unwrap_err());
        return std::nullopt;
    }
    doc.root_json = parsed.unwrap();
    if (! doc.metadata.FromJson(doc.root_json, pkg_version)) return std::nullopt;
    doc.objects_metadata       = parse_objects_metadata(doc.root_json);
    doc.metadata.canvas_extent = scene_canvas_extent(doc.metadata, doc.objects_metadata);
    return doc;
}

std::optional<SceneDocument> LoadSceneDocumentFromVfs(fs::VFS& vfs, std::string_view scene_path,
                                                      SceneVersion pkg_version) {
    auto f = vfs.Open(scene_path);
    if (! f) return std::nullopt;
    return ParseSceneDocumentJson(f->ReadAllStr(), pkg_version);
}

std::optional<SceneDocument> LoadSceneDocumentFromPkg(std::string_view pkg_path) {
    if (pkg_path.empty()) return std::nullopt;
    auto pkg = fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! pkg) return std::nullopt;

    auto scene_file = pkg->Open("/scene.json");
    if (! scene_file) return std::nullopt;

    const auto pkg_version = ParsePkgVersionStamp(pkg->pkg_version_stamp());
    return ParseSceneDocumentJson(scene_file->ReadAllStr(), pkg_version);
}

std::optional<SceneDocument> LoadSceneDocumentFromSource(std::string_view source_path) {
    if (source_path.empty()) return std::nullopt;

    std::filesystem::path path { std::string(source_path) };
    auto                  ext = path.extension().string();
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }

    if (ext == ".pkg") return LoadSceneDocumentFromPkg(source_path);
    if (ext != ".json") return std::nullopt;

    auto scene_file = fs::CreateCBinaryStream(source_path);
    if (! scene_file) return std::nullopt;
    return ParseSceneDocumentJson(scene_file->ReadAllStr(), kSceneVersionUnknown);
}

} // namespace sr::wpscene
