module;

#include <nlohmann/json.hpp>

export module sr.pkg.scene_obj:visibility_binding;
import rstd.cppstd;

export namespace sr::wpscene
{

struct VisibleUserBinding {
    std::string    name;
    nlohmann::json condition;
    bool           has_condition { false };

    bool empty() const { return name.empty(); }
};

struct UserValueBinding {
    std::string    name;
    nlohmann::json condition;
    bool           has_condition { false };

    bool empty() const { return name.empty(); }
};

inline void ReadVisibleUserBinding(const nlohmann::json& json, VisibleUserBinding& out) {
    out = {};
    if (! json.contains("visible") || ! json.at("visible").is_object()) return;

    const auto& visible = json.at("visible");
    if (! visible.contains("user")) return;

    const auto& user = visible.at("user");
    if (user.is_string()) {
        out.name = user.get<std::string>();
        return;
    }

    if (! user.is_object()) return;
    if (user.contains("name") && user.at("name").is_string()) {
        out.name = user.at("name").get<std::string>();
    }
    if (user.contains("condition")) {
        out.condition     = user.at("condition");
        out.has_condition = true;
    }
}

inline void ReadUserValueBinding(const nlohmann::json& json, std::string_view field,
                                 UserValueBinding& out) {
    out     = {};
    auto it = json.find(field);
    if (it == json.end() || ! it->is_object()) return;

    const auto user_it = it->find("user");
    if (user_it == it->end()) return;

    if (user_it->is_string()) {
        out.name = user_it->get<std::string>();
        return;
    }

    if (! user_it->is_object()) return;
    if (user_it->contains("name") && user_it->at("name").is_string()) {
        out.name = user_it->at("name").get<std::string>();
    }
    if (user_it->contains("condition")) {
        out.condition     = user_it->at("condition");
        out.has_condition = true;
    }
}

} // namespace sr::wpscene
