export module sr.pkg.parse:wp_scene_parser;
import rstd.cppstd;
import wavsen.audio;
import sr.fs;
import sr.json;
import sr.scene;
import sr.pkg.scene_obj;

export namespace sr
{

class WPSceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;

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
    // The borrowed map must outlive the next Parse() call.
    void SetUserProperties(rstd::Option<rstd::ref<rstd::json::Map>> properties) {
        m_user_properties = properties;
    }

    void SetScriptPersistencePath(std::string path) {
        m_script_persistence_path = std::move(path);
    }

private:
    rstd::Option<rstd::ref<rstd::json::Map>> m_user_properties;
    std::string                              m_script_persistence_path;
};

} // namespace sr
