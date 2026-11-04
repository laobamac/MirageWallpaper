module;

#include <limits>
#include <string>
#include <string_view>

export module sr.pkg.scene_obj:visibility_binding;
import rstd.cppstd;
import sr.json;

export namespace sr::wpscene
{

struct VisibleUserBinding {
    std::string name;
    sr::Json   condition;
    bool        has_condition { false };

    bool empty() const { return name.empty(); }
};

struct UserValueBinding {
    std::string name;
    sr::Json   condition;
    bool        has_condition { false };

    bool empty() const { return name.empty(); }
};

inline void ReadVisibleUserBinding(const sr::Json& json, VisibleUserBinding& out) {
    out          = {};
    auto visible = json.get("visible");
    if (visible.is_none() || ! (*visible)->is_object()) return;

    auto user = (*visible)->get("user");
    if (user.is_none()) return;
    if ((*user)->is_string()) {
        out.name = rstd::cppstd::to_string(*(*user)->as_str());
        return;
    }

    if (! (*user)->is_object()) return;
    if (auto name = (*user)->get("name"); name.is_some()) {
        auto string = (*name)->as_str();
        if (string.is_some()) out.name = rstd::cppstd::to_string(*string);
    }
    if (auto condition = (*user)->get("condition"); condition.is_some()) {
        out.condition     = (*condition)->clone();
        out.has_condition = true;
    }
}

inline void ReadVisibleProperty(const sr::Json& json, bool& visible, VisibleUserBinding& out) {
    out        = {};
    auto value = json.get("visible");
    if (value.is_none()) return;

    if ((*value)->is_boolean()) {
        visible = *(*value)->as_bool();
        return;
    }
    if (! (*value)->is_object()) return;

    if (auto initial = (*value)->get("value"); initial.is_some()) {
        if ((*initial)->is_boolean()) {
            visible = *(*initial)->as_bool();
        } else {
            auto numeric = (*initial)->as_f64();
            if (numeric.is_some() && *numeric >= std::numeric_limits<int>::min() &&
                *numeric <= std::numeric_limits<int>::max())
                visible = static_cast<int>(*numeric) != 0;
        }
    }
    ReadVisibleUserBinding(json, out);
}

inline void ReadUserValueBinding(const sr::Json& json, std::string_view field,
                                 UserValueBinding& out) {
    out        = {};
    auto value = json.get(field);
    if (value.is_none() || ! (*value)->is_object()) return;

    auto user = (*value)->get("user");
    if (user.is_none()) return;

    if ((*user)->is_string()) {
        out.name = rstd::cppstd::to_string(*(*user)->as_str());
        return;
    }

    if (! (*user)->is_object()) return;
    if (auto name = (*user)->get("name"); name.is_some()) {
        auto string = (*name)->as_str();
        if (string.is_some()) out.name = rstd::cppstd::to_string(*string);
    }
    if (auto condition = (*user)->get("condition"); condition.is_some()) {
        out.condition     = (*condition)->clone();
        out.has_condition = true;
    }
}

} // namespace sr::wpscene
