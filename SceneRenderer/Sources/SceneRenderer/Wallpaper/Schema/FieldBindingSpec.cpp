module;

module sr.pkg.scene_obj;
import rstd.cppstd;
import sr.json;

namespace sr::wpscene
{

bool ParseAnimKeyframeTangent(const sr::Json& json, AnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "enabled", out.enabled, false);
    sr::GetJsonValue(json, "x", out.x, false);
    sr::GetJsonValue(json, "y", out.y, false);
    sr::GetJsonValue(json, "magic", out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const sr::Json& json, AnimKeyframe& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "frame", out.frame, false);
    sr::GetJsonValue(json, "value", out.value, false);
    sr::GetJsonValue(json, "lockangle", out.lockangle, false);
    sr::GetJsonValue(json, "locklength", out.locklength, false);
    if (auto front = json.get("front"); front.is_some())
        ParseAnimKeyframeTangent(**front, out.front);
    if (auto back = json.get("back"); back.is_some()) ParseAnimKeyframeTangent(**back, out.back);
    return true;
}

bool ParseAnimAxis(const sr::Json& json, std::vector<AnimKeyframe>& out) {
    if (! json.is_array()) return false;
    auto array = json.as_array();
    out.reserve((*array)->len());
    for (const auto& jK : **array) {
        AnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push_back(std::move(k));
    }
    return true;
}

bool ParseAnimOptions(const sr::Json& json, AnimOptions& out) {
    if (! json.is_object()) return false;
    sr::GetJsonValue(json, "fps", out.fps, false);
    sr::GetJsonValue(json, "length", out.length, false);
    sr::GetJsonValue(json, "mode", out.mode, false);
    sr::GetJsonValue(json, "name", out.name, false);
    sr::GetJsonValue(json, "startpaused", out.startpaused, false);
    sr::GetJsonValue(json, "wraploop", out.wraploop, false);
    if (auto value = json.get("smoothing"); value.is_some()) out.smoothing = (*value)->clone();
    if (auto value = json.get("children"); value.is_some()) out.children = (*value)->clone();
    if (auto value = json.get("events"); value.is_some()) out.events = (*value)->clone();
    if (auto value = json.get("parent"); value.is_some()) out.parent = (*value)->clone();
    return true;
}

bool ParseAnimCurve(const sr::Json& json, AnimCurve& out) {
    if (! json.is_object()) return false;
    if (auto value = json.get("c0"); value.is_some()) ParseAnimAxis(**value, out.c0);
    if (auto value = json.get("c1"); value.is_some()) ParseAnimAxis(**value, out.c1);
    if (auto value = json.get("c2"); value.is_some()) ParseAnimAxis(**value, out.c2);
    if (auto value = json.get("options"); value.is_some()) ParseAnimOptions(**value, out.options);
    sr::GetJsonValue(json, "relative", out.relative, false);
    return true;
}

auto AnimCurve::clone() const -> AnimCurve {
    AnimCurve result;
    result.c0                  = c0;
    result.c1                  = c1;
    result.c2                  = c2;
    result.options.fps         = options.fps;
    result.options.length      = options.length;
    result.options.mode        = options.mode;
    result.options.name        = options.name;
    result.options.startpaused = options.startpaused;
    result.options.wraploop    = options.wraploop;
    result.options.smoothing   = options.smoothing.clone();
    result.options.children    = options.children.clone();
    result.options.events      = options.events.clone();
    result.options.parent      = options.parent.clone();
    result.relative            = relative;
    return result;
}

std::size_t AbsorbAllFieldBindings(const sr::Json& obj_json, FieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n      = 0;
    auto        object = obj_json.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  field             = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& field_value       = *entry_value;
        if (! field_value.is_object()) return;
        if (auto animation = field_value.get("animation"); animation.is_some()) {
            AnimCurve curve;
            if (ParseAnimCurve(**animation, curve)) {
                out.animations[std::string(field)] = std::move(curve);
                ++n;
            }
        }
        if (auto properties = field_value.get("scriptproperties"); properties.is_some()) {
            out.scriptproperties.insert(::alloc::string::String::make(rstd::cppstd::as_str(field)),
                                        (*properties)->clone());
            ++n;
        }
        auto script = field_value.get("script");
        if (script.is_some() && (*script)->is_string()) {
            ScriptBinding sb;
            sb.source = rstd::cppstd::to_string(*(*script)->as_str());
            if (auto properties = field_value.get("scriptproperties"); properties.is_some())
                sb.properties = (*properties)->clone();
            if (auto value = field_value.get("value"); value.is_some())
                sb.initial_value = (*value)->clone();
            if (auto user = field_value.get("user"); user.is_some()) {
                auto string = (*user)->as_str();
                if (string.is_some()) sb.user = rstd::cppstd::to_string(*string);
            }
            out.scripts[std::string(field)] = std::move(sb);
            ++n;
        }
    });
    return n;
}

} // namespace sr::wpscene
