module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import sr.json;

using namespace sr::wpscene;

namespace
{

void LoadUserShaderValues(const sr::Json&                              json,
                          std::unordered_map<std::string, std::string>& out) {
    auto values = json.get("usershadervalues");
    if (values.is_none()) return;
    auto object = (*values)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto text                     = entry_value->as_str();
        if (text.is_some())
            out[rstd::cppstd::to_string(entry_key->as_str())] = rstd::cppstd::to_string(*text);
    });
}

void MergeUserTextures(const rstd::json::Array& src, rstd::json::Array& dst) {
    while (src.len() > dst.len()) dst.push(sr::Json::Null());
    for (rstd::usize i = 0; i < src.len(); ++i) {
        if (! src[i].is_null()) dst[i] = src[i].clone();
    }
}

void LoadConstantShaderValue(std::string name, const sr::Json& json,
                             std::unordered_map<std::string, std::vector<float>>& constant_values,
                             std::unordered_map<std::string, std::string>&        user_values,
                             std::unordered_map<std::string, AnimCurve>&          animations,
                             FieldBindings&                                       bindings) {
    std::vector<float> value;
    sr::GetJsonValue(json, value);
    constant_values[name] = std::move(value);
    if (! json.is_object()) return;

    (void)AbsorbFieldBinding(name, json, bindings);

    if (auto user = json.get("user"); user.is_some()) {
        auto string = (*user)->as_str();
        if (string.is_some()) user_values[name] = rstd::cppstd::to_string(*string);
    }
    if (auto animation = bindings.animations.find(name); animation != bindings.animations.end())
        animations[name] = animation->second.clone();
}

} // namespace

auto sr::wpscene::Material::clone() const -> Material {
    Material clone;
    clone.blending                  = blending;
    clone.cullmode                  = cullmode;
    clone.shader                    = shader;
    clone.depthtest                 = depthtest;
    clone.depthwrite                = depthwrite;
    clone.textures                  = textures;
    clone.combos                    = combos;
    clone.constantshadervalues      = constantshadervalues;
    clone.constantshadervalues_user = constantshadervalues_user;
    clone.user_shader_values        = user_shader_values;
    clone.use_puppet                = use_puppet;
    clone.constantshadervalues_bindings = constantshadervalues_bindings.clone();
    MergeUserTextures(usertextures, clone.usertextures);
    for (const auto& [name, curve] : constantshadervalues_animations)
        clone.constantshadervalues_animations[name] = curve.clone();
    return clone;
}

bool MaterialPassBindItem::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "index", index);
    return true;
}

void MaterialPass::Update(const MaterialPass& p) {
    int32_t i = -1;
    for (const auto& el : p.textures) {
        i++;
        if (p.textures.size() > textures.size()) textures.resize(p.textures.size());
        if (! el.empty()) {
            textures[i] = el;
        }
    }
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_animations) {
        constantshadervalues_animations[el.first] = el.second.clone();
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

void Material::MergePass(const MaterialPass& p) {
    MergeBindingOverrides(p.textures, p.usertextures, p.combos);
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_animations) {
        constantshadervalues_animations[el.first] = el.second.clone();
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
}

void Material::MergeBindingOverrides(
    const std::vector<std::string>& textures, const rstd::json::Array& usertextures,
    const std::unordered_map<std::string, int32_t>& combos) {
    if (textures.size() > this->textures.size()) this->textures.resize(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (! textures[i].empty()) this->textures[i] = textures[i];
    }
    MergeUserTextures(usertextures, this->usertextures);
    for (const auto& [key, value] : combos) this->combos[key] = value;
}

bool MaterialPass::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "id", id, false);
    if (auto values = json.get("textures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) sr::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
            }
        }
    }
    if (auto values = json.get("usertextures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = json.get("constantshadervalues"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_user,
                                        constantshadervalues_animations,
                                        constantshadervalues_bindings);
            });
    }
    LoadUserShaderValues(json, user_shader_values);
    if (auto values = json.get("combos"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                sr::GetJsonValue(*entry_value, value);
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    sr::GetJsonValue(json, "target", target, false);
    if (auto values = json.get("bind"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jB : **array) {
                MaterialPassBindItem bindItem;
                bindItem.FromJson(jB);
                bind.push_back(bindItem);
            }
        }
    }
    return true;
}

bool Material::FromJson(const sr::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool Material::FromJson(const sr::Json& json, SceneVersion /*v*/) {
    auto passes = json.get("passes");
    if (passes.is_none()) {
        rstd_error("material no data");
        return false;
    }
    auto pass_array = (*passes)->as_array();
    if (pass_array.is_none() || (*pass_array)->is_empty()) {
        rstd_error("material no data");
        return false;
    }
    const auto& jContent = (**pass_array)[0];
    if (jContent.get("shader").is_none()) {
        rstd_error("material no shader");
        return false;
    }
    sr::GetJsonValue(jContent, "blending", blending);
    sr::GetJsonValue(jContent, "cullmode", cullmode);
    sr::GetJsonValue(jContent, "depthtest", depthtest);
    sr::GetJsonValue(jContent, "depthwrite", depthwrite);
    sr::GetJsonValue(jContent, "shader", shader);
    if (auto values = jContent.get("textures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) sr::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
            }
        }
    }
    if (auto values = jContent.get("usertextures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = jContent.get("constantshadervalues"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_user,
                                        constantshadervalues_animations,
                                        constantshadervalues_bindings);
            });
    }
    LoadUserShaderValues(jContent, user_shader_values);
    if (auto values = jContent.get("combos"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                sr::GetJsonValue(*entry_value, value);
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    return true;
}
