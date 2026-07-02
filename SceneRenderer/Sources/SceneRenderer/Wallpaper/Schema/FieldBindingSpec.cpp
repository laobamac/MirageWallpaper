module;

module sr.pkg.scene_obj;
import nlohmann.json;
import rstd.cppstd;

namespace sr::wpscene
{

bool ParseAnimKeyframeTangent(const nlohmann::json& json, AnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "enabled", out.enabled, false);
    sr::GetJsonValue(json, "x", out.x, false);
    sr::GetJsonValue(json, "y", out.y, false);
    sr::GetJsonValue(json, "magic", out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const nlohmann::json& json, AnimKeyframe& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "frame", out.frame, false);
    sr::GetJsonValue(json, "value", out.value, false);
    sr::GetJsonValue(json, "lockangle", out.lockangle, false);
    sr::GetJsonValue(json, "locklength", out.locklength, false);
    if (json.contains("front")) ParseAnimKeyframeTangent(json.at("front"), out.front);
    if (json.contains("back")) ParseAnimKeyframeTangent(json.at("back"), out.back);
    return true;
}

bool ParseAnimAxis(const nlohmann::json& json, std::vector<AnimKeyframe>& out) {
    if (! json.is_array()) return false;
    out.reserve(json.size());
    for (const auto& jK : json) {
        AnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push_back(std::move(k));
    }
    return true;
}

bool ParseAnimOptions(const nlohmann::json& json, AnimOptions& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "fps", out.fps, false);
    sr::GetJsonValue(json, "length", out.length, false);
    sr::GetJsonValue(json, "mode", out.mode, false);
    sr::GetJsonValue(json, "name", out.name, false);
    sr::GetJsonValue(json, "startpaused", out.startpaused, false);
    sr::GetJsonValue(json, "wraploop", out.wraploop, false);
    if (json.contains("smoothing")) out.smoothing = json.at("smoothing");
    if (json.contains("children")) out.children = json.at("children");
    if (json.contains("events")) out.events = json.at("events");
    if (json.contains("parent")) out.parent = json.at("parent");
    return true;
}

bool ParseAnimCurve(const nlohmann::json& json, AnimCurve& out) {
    if (! json.is_object()) return false;
    if (json.contains("c0")) ParseAnimAxis(json.at("c0"), out.c0);
    if (json.contains("c1")) ParseAnimAxis(json.at("c1"), out.c1);
    if (json.contains("c2")) ParseAnimAxis(json.at("c2"), out.c2);
    if (json.contains("options")) ParseAnimOptions(json.at("options"), out.options);
    sr::GetJsonValue(json, "relative", out.relative, false);
    return true;
}

std::size_t AbsorbAllFieldBindings(const nlohmann::json& obj_json, FieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n = 0;
    for (const auto& el : obj_json.items()) {
        const auto& field_value = el.value();
        if (! field_value.is_object()) continue;
        if (field_value.contains("animation")) {
            AnimCurve curve;
            if (ParseAnimCurve(field_value.at("animation"), curve)) {
                out.animations[el.key()] = std::move(curve);
                ++n;
            }
        }
        if (field_value.contains("scriptproperties")) {
            out.scriptproperties[el.key()] = field_value.at("scriptproperties");
            ++n;
        }
        if (field_value.contains("script") && field_value.at("script").is_string()) {
            ScriptBinding sb;
            sb.source = field_value.at("script").get<std::string>();
            if (field_value.contains("scriptproperties"))
                sb.properties = field_value.at("scriptproperties");
            if (field_value.contains("value")) sb.initial_value = field_value.at("value");
            if (field_value.contains("user") && field_value.at("user").is_string())
                sb.user = field_value.at("user").get<std::string>();
            out.scripts[el.key()] = std::move(sb);
            ++n;
        }
    }
    return n;
}

} // namespace sr::wpscene
