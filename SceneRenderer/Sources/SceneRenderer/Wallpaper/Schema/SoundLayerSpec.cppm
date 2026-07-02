module;

export module sr.pkg.scene_obj:sound_object;
import rstd.cppstd;
import wavsen.audio;
import sr.fs;

import sr.json;
export import :field_binding;
import :visibility_binding;
import :scene_document;

export namespace sr

{

namespace wpscene
{

struct SoundObject {
    std::int32_t             id { 0 };
    std::string              playbackmode { "loop" };
    std::array<float, 3>     origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     scale { 1.0f, 1.0f, 1.0f };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    std::string              name;
    std::vector<std::string> sound;

    // Common cross-kind metadata.
    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json            instance;
    FieldBindings             field_bindings;

    // Sound-kind specifics.
    bool        startsilent { false };    // PKGV0002+
    bool        blockalign { false };     // PKGV0018+
    bool        spatialization { false }; // PKGV0023+
    std::string queuemode;                // PKGV0020+

    VisibleUserBinding visible_user;
    std::string        visible_user_key;
    std::string        volume_user_key;

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }

    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        sr::GetJsonValue(json, "volume", volume);
        if (json.contains("volume") && json.at("volume").is_object()) {
            const auto& jv = json.at("volume");
            if (jv.contains("user") && jv.at("user").is_string())
                volume_user_key = jv.at("user").get<std::string>();
        }
        sr::GetJsonValue(json, "playbackmode", playbackmode);
        sr::GetJsonValue(json, "origin", origin, false);
        sr::GetJsonValue(json, "angles", angles, false);
        sr::GetJsonValue(json, "scale", scale, false);
        sr::GetJsonValue(json, "mintime", mintime, false);
        sr::GetJsonValue(json, "maxtime", maxtime, false);
        sr::GetJsonValue(json, "visible", visible, false);
        ReadVisibleUserBinding(json, visible_user);
        visible_user_key = visible_user.name;
        sr::GetJsonValue(json, "name", name, false);
        sr::GetJsonValue(json, "id", id, false);

        sr::GetJsonValue(json, "locktransforms", locktransforms, false);
        sr::GetJsonValue(json, "muteineditor", muteineditor, false);
        sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
        sr::GetJsonValue(json, "parent", parent, false);
        sr::GetJsonValue(json, "dependencies", dependencies, false);
        if (json.contains("instance")) instance = json.at("instance");

        sr::GetJsonValue(json, "startsilent", startsilent, false);
        sr::GetJsonValue(json, "blockalign", blockalign, false);
        sr::GetJsonValue(json, "spatialization", spatialization, false);
        sr::GetJsonValue(json, "queuemode", queuemode, false);

        if (! json.contains("sound") || ! json.at("sound").is_array()) {
            return false;
        }
        for (const auto& el : json.at("sound")) {
            std::string name;
            sr::GetJsonValue(el, name);
            if (! name.empty()) sound.push_back(name);
        }
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};
} // namespace wpscene
} // namespace sr
