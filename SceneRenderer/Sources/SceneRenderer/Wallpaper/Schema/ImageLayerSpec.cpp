module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;

using namespace sr::wpscene;

bool EffectCommand::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "command", command);
    sr::GetJsonValue(json, "target", target);
    sr::GetJsonValue(json, "source", source);
    return true;
}

bool ObjectInstance::FromJson(const nlohmann::json& json) {
    present = true;
    sr::GetJsonValue(json, "id", id, false);
    if (json.contains("combos") && json.at("combos").is_object()) {
        for (const auto& jC : json.at("combos").items()) {
            std::int32_t v { 0 };
            try {
                v = jC.value().get<std::int32_t>();
            } catch (...) {
                continue;
            }
            combos.emplace(jC.key(), v);
        }
    }
    if (json.contains("textures") && json.at("textures").is_array()) {
        for (const auto& jT : json.at("textures")) {
            if (jT.is_string()) textures.push_back(jT.get<std::string>());
        }
    }
    if (json.contains("usertextures") && json.at("usertextures").is_array()) {
        for (const auto& jU : json.at("usertextures")) {
            usertextures.push_back(jU);
        }
    }
    return true;
}

bool EffectFbo::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "format", format);

    sr::GetJsonValue(json, "scale", scale);
    sr::GetJsonValue(json, "fit", fit, false);
    if (scale == 0) {
        rstd_error("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

// Define and initialize the static property
const std::unordered_set<std::string> ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS = {
    "2799421411" // Audio Responsive Oscilloscope   --  causes vulcan deadlock
};

bool ImageEffect::IsEffectBlacklisted(const std::string& filePath) {
    std::filesystem::path path(filePath);
    // Check if the path has a parent path
    if (path.has_parent_path()) {
        path = path.parent_path();
        if (path.has_parent_path()) {
            std::string effectId   = path.parent_path().filename().string();
            std::string parentPath = path.parent_path().string();
            return ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.find(effectId) !=
                   ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.end();
        }
    }
    return false;
}

bool ImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    std::string filePath;
    sr::GetJsonValue(json, "file", filePath);
    sr::GetJsonValue(json, "visible", visible, false);
    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "username", username, false);
    if (this->IsEffectBlacklisted(filePath)) {
        // hide blacklisted effects
        visible = false;
    }
    sr::GetJsonValue(json, "id", id, false);
    nlohmann::json jEffect;
    if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + filePath), jEffect)) return false;
    if (! FromFileJson(jEffect, vfs)) return false;

    if (json.contains("passes")) {
        const auto& jPasses = json.at("passes");
        if (jPasses.size() > passes.size()) {
            rstd_error("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for (const auto& jP : jPasses) {
            MaterialPass pass;
            pass.FromJson(jP);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool ImageEffect::FromFileJson(const nlohmann::json& json, fs::VFS& vfs) {
    sr::GetJsonValue(json, "version", version, false);
    sr::GetJsonValue(json, "name", name);
    if (json.contains("fbos")) {
        for (auto& jF : json.at("fbos")) {
            EffectFbo fbo;
            fbo.FromJson(jF);
            fbos.push_back(std::move(fbo));
        }
    }
    if (json.contains("passes")) {
        const auto& jEPasses = json.at("passes");
        bool        compose { false };
        for (const auto& jP : jEPasses) {
            if (! jP.contains("material")) {
                if (jP.contains("command")) {
                    EffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = passes.size();
                    commands.push_back(cmd);
                    continue;
                }
                rstd_error("no material in effect pass");
                return false;
            }
            std::string matPath;
            sr::GetJsonValue(jP, "material", matPath);
            nlohmann::json jMat;
            if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
            Material material;
            material.FromJson(jMat);
            materials.push_back(std::move(material));
            MaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if (jP.contains("compose")) sr::GetJsonValue(jP, "compose", compose);
        }
        if (compose) {
            if (passes.size() != 2) {
                rstd_error("effect compose option error");
                return false;
            }
            EffectFbo fbo;
            {
                fbo.name  = "_rt_FullCompoBuffer1";
                fbo.scale = 1;
            }
            fbos.push_back(fbo);
            passes.at(0).bind.push_back({ "previous", 0 });
            passes.at(0).target = "_rt_FullCompoBuffer1";
            passes.at(1).bind.push_back({ "_rt_FullCompoBuffer1", 0 });
        }
    } else {
        rstd_error("no passes in effect file");
        return false;
    }
    return true;
}

bool ImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    sr::GetJsonValue(json, "image", image);
    sr::GetJsonValue(json, "visible", visible, false);
    ReadVisibleUserBinding(json, visible_user);
    visible_user_key = visible_user.name;
    sr::GetJsonValue(json, "alignment", alignment, false);
    nlohmann::json jImage;
    if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + image), jImage)) {
        rstd_error("Can't load image json: {}", image);
        return false;
    }
    sr::GetJsonValue(jImage, "fullscreen", fullscreen, false);
    sr::GetJsonValue(jImage, "passthrough", config.passthrough, false);
    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "id", id, false);
    sr::GetJsonValue(json, "colorBlendMode", colorBlendMode, false);
    if (! fullscreen) {
        sr::GetJsonValue(json, "origin", origin);
        sr::GetJsonValue(json, "angles", angles);
        sr::GetJsonValue(json, "scale", scale);
        sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        if (jImage.contains("width")) {
            int32_t w, h;
            sr::GetJsonValue(jImage, "width", w);
            sr::GetJsonValue(jImage, "height", h);
            size = { (float)w, (float)h };
        } else if (json.contains("size")) {
            sr::GetJsonValue(json, "size", size);
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    sr::GetJsonValue(jImage, "nopadding", nopadding, false);
    sr::GetJsonValue(json, "color", color, false);
    ReadUserValueBinding(json, "color", color_user);
    color_user_key = color_user.name;
    sr::GetJsonValue(json, "alpha", alpha, false);
    sr::GetJsonValue(json, "brightness", brightness, false);

    sr::GetJsonValue(jImage, "puppet", puppet, false);
    const bool explicit_no_copy_background = json.contains("copybackground") &&
                                             json.at("copybackground").is_boolean() &&
                                             ! json.at("copybackground").get<bool>();

    if (jImage.contains("material")) {
        std::string matPath;
        sr::GetJsonValue(jImage, "material", matPath);
        nlohmann::json jMat;
        if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) {
            rstd_error("Can't load material json: {}", matPath);
            return false;
        }
        material.FromJson(jMat);
        if (image == "models/util/composelayer.json" && explicit_no_copy_background) {
            material.combos["CLEARALPHA"] = 1;
        }
    } else {
        rstd_info("image object no material");
        return false;
    }
    if (json.contains("effects")) {
        for (const auto& jE : json.at("effects")) {
            ImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }
    ReadPuppetAnimationLayers(json, puppet_layers);
    if (json.contains("config")) {
        const auto& jConf = json.at("config");
        sr::GetJsonValue(jConf, "passthrough", config.passthrough, false);
    }

    sr::GetJsonValue(json, "locktransforms", locktransforms, false);
    sr::GetJsonValue(json, "muteineditor", muteineditor, false);
    sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    sr::GetJsonValue(json, "parent", parent, false);
    sr::GetJsonValue(json, "attachment", attachment, false);
    sr::GetJsonValue(json, "perspective", perspective, false);
    sr::GetJsonValue(json, "copybackground", copybackground, false);
    sr::GetJsonValue(json, "solid", solid, false);
    sr::GetJsonValue(json, "opaquebackground", opaquebackground, false);
    sr::GetJsonValue(json, "clampuvs", clampuvs, false);
    sr::GetJsonValue(json, "castshadow", castshadow, false);
    sr::GetJsonValue(json, "disablepropagation", disablepropagation, false);
    sr::GetJsonValue(json, "depthtest", depthtest, false);
    sr::GetJsonValue(json, "backgroundcolor", backgroundcolor, false);
    sr::GetJsonValue(json, "backgroundbrightness", backgroundbrightness, false);
    sr::GetJsonValue(json, "dependencies", dependencies, false);
    if (json.contains("instance") && json.at("instance").is_object()) {
        instance.FromJson(json.at("instance"));
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
