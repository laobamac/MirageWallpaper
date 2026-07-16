export module sr.scene:visibility;
import rstd.cppstd;
import sr.json;

export namespace sr
{

struct SceneUserVisibilityBinding {
    std::string key;
    Json        condition;
    bool        has_condition { false };

    bool empty() const { return key.empty(); }
};

inline const Json& SceneUserPropertyPayload(const Json& property) {
    if (auto value = property.get("value"); value.is_some()) return **value;
    return property;
}

inline auto SceneJsonScalarString(const Json& value) -> rstd::Option<std::string> {
    if (value.is_string()) return rstd::Some(rstd::cppstd::to_string(*value.as_str()));
    if (value.is_boolean()) return rstd::Some(std::string { *value.as_bool() ? "true" : "false" });
    if (value.is_number()) return rstd::Some(Dump(value));
    return rstd::None();
}

inline bool SceneJsonScalarEquals(const Json& a, const Json& b) {
    if (a == b) return true;
    auto as = SceneJsonScalarString(a);
    auto bs = SceneJsonScalarString(b);
    if (! as || ! bs) return false;
    if (as->compare(*bs) == 0) return true;
    if (a.is_boolean() && b.is_string()) {
        auto s = rstd::cppstd::as_string_view(*b.as_str());
        return (*a.as_bool() && s.compare("1") == 0) || (! *a.as_bool() && s.compare("0") == 0);
    }
    if (a.is_string() && b.is_boolean()) {
        auto s = rstd::cppstd::as_string_view(*a.as_str());
        return (*b.as_bool() && s.compare("1") == 0) || (! *b.as_bool() && s.compare("0") == 0);
    }
    return false;
}

inline rstd::Option<bool>
ResolveSceneUserVisibilityBinding(const SceneUserVisibilityBinding& binding, const Json& property) {
    if (binding.empty()) return rstd::None();
    const auto& value = SceneUserPropertyPayload(property);
    if (binding.has_condition) return rstd::Some(SceneJsonScalarEquals(value, binding.condition));
    return value.as_bool();
}

inline rstd::Option<bool>
ResolveSceneUserVisibilityBinding(const SceneUserVisibilityBinding& binding, std::string_view key,
                                  const Json& property) {
    if (binding.key.compare(key) == 0) return rstd::None();
    return ResolveSceneUserVisibilityBinding(binding, property);
}

} // namespace sr
