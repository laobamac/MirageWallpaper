module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;

using namespace sr::wpscene;

namespace
{

void LoadUserShaderValues(const nlohmann::json&                         json,
                          std::unordered_map<std::string, std::string>& out) {
    auto it = json.find("usershadervalues");
    if (it == json.end() || ! it->is_object()) return;

    for (const auto& el : it->items()) {
        if (! el.value().is_string()) continue;
        out[el.key()] = el.value().get<std::string>();
    }
}

void MergeUserTextures(const std::vector<nlohmann::json>& src, std::vector<nlohmann::json>& dst) {
    if (src.size() > dst.size()) dst.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (! src[i].is_null()) dst[i] = src[i];
    }
}

} // namespace

bool MaterialPassBindItem::FromJson(const nlohmann::json& json) {
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
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

void Material::MergePass(const MaterialPass& p) {
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
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

bool MaterialPass::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "id", id, false);
    if (json.contains("textures")) {
        for (const auto& jT : json.at("textures")) {
            std::string tex;
            if (! jT.is_null()) sr::GetJsonValue(jT, tex);
            textures.push_back(tex);
        }
    }
    if (json.contains("usertextures") && json.at("usertextures").is_array()) {
        for (const auto& jU : json.at("usertextures")) {
            usertextures.push_back(jU);
        }
    }
    if (json.contains("constantshadervalues")) {
        for (const auto& jC : json.at("constantshadervalues").items()) {
            std::string        name;
            std::vector<float> value;
            sr::GetJsonValue(jC.key(), name);
            sr::GetJsonValue(jC.value(), value);
            constantshadervalues[name] = value;
            // Capture scene.json instance-level user binding so the parser can
            // bridge effect-internal keys ("Opacity") to wallpaper-level
            // project.json keys ("luzopacidad").
            if (jC.value().is_object() && jC.value().contains("user") &&
                jC.value().at("user").is_string()) {
                constantshadervalues_user[name] = jC.value().at("user").get<std::string>();
            }
        }
    }
    LoadUserShaderValues(json, user_shader_values);
    if (json.contains("combos")) {
        for (const auto& jC : json.at("combos").items()) {
            std::string name;
            int32_t     value;
            sr::GetJsonValue(jC.key(), name);
            sr::GetJsonValue(jC.value(), value);
            combos[name] = value;
        }
    }
    sr::GetJsonValue(json, "target", target, false);
    if (json.contains("bind")) {
        for (const auto& jB : json.at("bind")) {
            MaterialPassBindItem bindItem;
            bindItem.FromJson(jB);
            bind.push_back(bindItem);
        }
    }
    return true;
}

bool Material::FromJson(const nlohmann::json& json) { return FromJson(json, kSceneVersionUnknown); }

bool Material::FromJson(const nlohmann::json& json, SceneVersion /*v*/) {
    if (! json.contains("passes") || json.at("passes").size() == 0) {
        rstd_error("material no data");
        return false;
    }
    const auto jContent = json.at("passes").at(0);
    if (! jContent.contains("shader")) {
        rstd_error("material no shader");
        return false;
    }
    sr::GetJsonValue(jContent, "blending", blending);
    sr::GetJsonValue(jContent, "cullmode", cullmode);
    sr::GetJsonValue(jContent, "depthtest", depthtest);
    sr::GetJsonValue(jContent, "depthwrite", depthwrite);
    sr::GetJsonValue(jContent, "shader", shader);
    if (jContent.contains("textures")) {
        for (const auto& jT : jContent.at("textures")) {
            std::string tex;
            if (! jT.is_null()) sr::GetJsonValue(jT, tex);
            textures.push_back(tex);
        }
    }
    if (jContent.contains("usertextures") && jContent.at("usertextures").is_array()) {
        for (const auto& jU : jContent.at("usertextures")) {
            usertextures.push_back(jU);
        }
    }
    if (jContent.contains("constantshadervalues")) {
        for (const auto& jC : jContent.at("constantshadervalues").items()) {
            std::string        name;
            std::vector<float> value;
            sr::GetJsonValue(jC.key(), name);
            sr::GetJsonValue(jC.value(), value);
            constantshadervalues[name] = value;
            if (jC.value().is_object() && jC.value().contains("user") &&
                jC.value().at("user").is_string()) {
                constantshadervalues_user[name] = jC.value().at("user").get<std::string>();
            }
        }
    }
    LoadUserShaderValues(jContent, user_shader_values);
    if (jContent.contains("combos")) {
        for (const auto& jC : jContent.at("combos").items()) {
            std::string name;
            int32_t     value;
            sr::GetJsonValue(jC.key(), name);
            sr::GetJsonValue(jC.value(), value);
            combos[name] = value;
        }
    }
    return true;
}
