module;

#include <nlohmann/json.hpp>

export module sr.pkg.parse:wp_scene_parser;
import rstd.cppstd;
import wavsen.audio;
import sr.fs;
import sr.scene;
import sr.pkg.scene_obj;

export namespace sr
{

class WallpaperSceneCompiler {
public:
    WallpaperSceneCompiler()  = default;
    ~WallpaperSceneCompiler() = default;

    // Legacy entry; defaults pkg version to unknown. Routes to the 5-arg form.
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string& buf, fs::VFS& vfs,
                                 wavsen::audio::SoundManager& sm) {
        return Parse(scene_id, buf, vfs, sm, wpscene::kSceneVersionUnknown);
    }
    // Canonical entry: pkg_version is the integer parsed from the scene.pkg
    // "PKGV00xx" stamp (or kSceneVersionUnknown if the scene came from a
    // loose directory rather than a packed pkg).
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&,
                                 wavsen::audio::SoundManager&, wpscene::SceneVersion pkg_version);
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const wpscene::SceneDocument&, fs::VFS&,
                                 wavsen::audio::SoundManager&);

    // Pre-parse user-property snapshot. Lets `visible:{user:"<key>",...}` on
    // a layer resolve to the host's CURRENT bool at parse time, so a layer
    // the user has toggled off in the UI gets pruned (image kinds skip
    // render-graph emission; non-image kinds skip parse entirely). The
    // pointed-to map must outlive the next Parse() call.
    void SetUserProperties(const std::unordered_map<std::string, nlohmann::json>* p) {
        m_user_properties = p;
    }

private:
    const std::unordered_map<std::string, nlohmann::json>* m_user_properties { nullptr };
};

} // namespace sr
