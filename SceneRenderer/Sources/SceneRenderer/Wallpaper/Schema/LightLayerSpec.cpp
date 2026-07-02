module;

module sr.pkg.scene_obj;
import nlohmann.json;

using namespace sr::wpscene;

bool LightObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool LightObject::FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
    sr::GetJsonValue(json, "origin", origin);
    sr::GetJsonValue(json, "angles", angles);
    sr::GetJsonValue(json, "scale", scale);
    sr::GetJsonValue(json, "color", color);
    sr::GetJsonValue(json, "light", light);
    sr::GetJsonValue(json, "radius", radius);
    sr::GetJsonValue(json, "intensity", intensity);
    sr::GetJsonValue(json, "visible", visible, false);
    ReadVisibleUserBinding(json, visible_user);
    visible_user_key = visible_user.name;
    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "id", id, false);
    sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
    sr::GetJsonValue(json, "shape", shape, false);

    sr::GetJsonValue(json, "locktransforms", locktransforms, false);
    sr::GetJsonValue(json, "muteineditor", muteineditor, false);
    sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    sr::GetJsonValue(json, "parent", parent, false);

    sr::GetJsonValue(json, "ledsource", ledsource, false);
    sr::GetJsonValue(json, "castshadow", castshadow, false);
    sr::GetJsonValue(json, "castvolumetrics", castvolumetrics, false);
    sr::GetJsonValue(json, "outercone", outercone, false);
    sr::GetJsonValue(json, "innercone", innercone, false);
    sr::GetJsonValue(json, "attenuation", attenuation, false);
    sr::GetJsonValue(json, "exponent", exponent, false);
    sr::GetJsonValue(json, "density", density, false);
    sr::GetJsonValue(json, "volumetricsexponent", volumetricsexponent, false);
    sr::GetJsonValue(json, "lightsourcesize", lightsourcesize, false);
    sr::GetJsonValue(json, "mindistance", mindistance, false);
    sr::GetJsonValue(json, "cascadedistance0", cascadedistance0, false);
    sr::GetJsonValue(json, "cascadedistance1", cascadedistance1, false);
    sr::GetJsonValue(json, "cascadedistance2", cascadedistance2, false);
    sr::GetJsonValue(json, "dependencies", dependencies, false);
    if (json.contains("instance")) instance = json.at("instance");
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
