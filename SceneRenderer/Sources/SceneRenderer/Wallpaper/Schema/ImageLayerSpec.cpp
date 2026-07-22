module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import sr.json;

using namespace sr::wpscene;

namespace
{

float NormalizeLayerAlpha(float alpha) {
    // Older WE scene JSON stores layer alpha as 0..100 percent.
    if (alpha > 1.0f) alpha /= 100.0f;
    return std::clamp(alpha, 0.0f, 1.0f);
}

constexpr std::string_view kFoliageSwayEffect = "effects/foliagesway/effect.json";
constexpr SceneVersion     kNormalizedFoliageSwayStrengthVersion = 9;

void ScaleAnimCurve(AnimCurve& curve, float scale) {
    auto scale_axis = [scale](std::vector<AnimKeyframe>& keys) {
        for (auto& key : keys) {
            key.value *= scale;
            key.front.y *= scale;
            key.back.y *= scale;
        }
    };
    scale_axis(curve.c0);
    scale_axis(curve.c1);
    scale_axis(curve.c2);
}

void NormalizeLegacyFoliageSwayStrength(MaterialPass& pass) {
    constexpr float scale = 0.01f;
    auto            value = pass.constantshadervalues.find("strength");
    if (value != pass.constantshadervalues.end()) {
        for (float& component : value->second) component *= scale;
    }
    auto animation = pass.constantshadervalues_animations.find("strength");
    if (animation != pass.constantshadervalues_animations.end())
        ScaleAnimCurve(animation->second, scale);
}

} // namespace

bool EffectCommand::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "command", command);
    sr::GetJsonValue(json, "target", target);
    sr::GetJsonValue(json, "source", source);
    return true;
}

bool ObjectInstance::FromJson(const sr::Json& json) {
    present = true;
    sr::GetJsonValue(json, "id", id, false);
    if (auto values = json.get("combos"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                if (sr::GetJsonValue(*entry_value, value))
                    combos.emplace(rstd::cppstd::to_string(entry_key->as_str()), value);
            });
    }
    if (auto values = json.get("textures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) {
                auto texture = value.as_str();
                textures.push_back(texture.is_some() ? rstd::cppstd::to_string(*texture)
                                                       : std::string {});
            }
        }
    }
    if (auto values = json.get("usertextures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) usertextures.push(value.clone());
        }
    }
    return true;
}

bool EffectFbo::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "format", format);
    sr::GetJsonValue(json, "scale", scale);
    sr::GetJsonValue(json, "fit", fit, false);
    sr::GetJsonValue(json, "unique", unique, false);
    if (scale == 0) {
        rstd_error("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

bool ImageEffect::FromJson(const sr::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageEffect::FromJson(const sr::Json& json, fs::VFS& vfs, SceneVersion v) {
    std::string filePath;
    sr::GetJsonValue(json, "file", filePath);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "username", username, false);
    sr::GetJsonValue(json, "id", id, false);
    auto parsed_effect = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + filePath));
    if (parsed_effect.is_err()) {
        rstd_error("Can't parse effect json {}: {}", filePath, parsed_effect.unwrap_err());
        return false;
    }
    auto jEffect = parsed_effect.unwrap();
    if (! FromFileJson(jEffect, vfs)) return false;

    if (auto injected_passes = json.get("passes"); injected_passes.is_some()) {
        auto array = (*injected_passes)->as_array();
        if (array.is_none()) return true;
        if ((*array)->len() > passes.size()) {
            rstd_error("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for (const auto& jP : **array) {
            MaterialPass pass;
            pass.FromJson(jP);
            if (filePath == kFoliageSwayEffect && v != kSceneVersionUnknown &&
                v < kNormalizedFoliageSwayStrengthVersion)
                NormalizeLegacyFoliageSwayStrength(pass);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool ImageEffect::FromFileJson(const sr::Json& json, fs::VFS& vfs) {
    sr::GetJsonValue(json, "version", version, false);
    sr::GetJsonValue(json, "name", name);
    if (auto values = json.get("fbos"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jF : **array) {
                EffectFbo fbo;
                fbo.FromJson(jF);
                fbos.push_back(std::move(fbo));
            }
        }
    }
    if (auto effect_passes = json.get("passes"); effect_passes.is_some()) {
        auto array = (*effect_passes)->as_array();
        if (array.is_none()) {
            rstd_error("passes in effect file is not an array");
            return false;
        }
        bool compose { false };
        for (const auto& jP : **array) {
            if (jP.get("material").is_none()) {
                if (jP.get("command").is_some()) {
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
            auto parsed_material = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath));
            if (parsed_material.is_err()) {
                rstd_error(
                    "Can't parse material json {}: {}", matPath, parsed_material.unwrap_err());
                return false;
            }
            auto     jMat = parsed_material.unwrap();
            Material material;
            material.FromJson(jMat);
            materials.push_back(std::move(material));
            MaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if (jP.get("compose").is_some()) sr::GetJsonValue(jP, "compose", compose);
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

bool ImageObject::FromJson(const sr::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

std::optional<ImageAssetInfo> sr::wpscene::LoadImageAssetInfo(fs::VFS&         vfs,
                                                               std::string_view image) {
    auto parsed_image = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + std::string(image)));
    if (parsed_image.is_err()) {
        rstd_error("Can't parse image json {}: {}", image, parsed_image.unwrap_err());
        return std::nullopt;
    }
    auto j_image = parsed_image.unwrap();

    ImageAssetInfo info;
    sr::GetJsonValue(j_image, "solidlayer", info.solid_layer, false);
    int32_t w = 0, h = 0;
    if (j_image.get("width").is_some() && j_image.get("height").is_some()) {
        sr::GetJsonValue(j_image, "width", w, false);
        sr::GetJsonValue(j_image, "height", h, false);
        if (w > 0 && h > 0) {
            info.size = std::array { static_cast<float>(w), static_cast<float>(h) };
            return info;
        }
    }

    std::string mat_path;
    if (! sr::GetJsonValue(j_image, "material", mat_path, false)) return info;
    auto parsed_material = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + mat_path));
    if (parsed_material.is_err()) {
        rstd_error("Can't parse material json {}: {}", mat_path, parsed_material.unwrap_err());
        return info;
    }
    auto     j_mat = parsed_material.unwrap();
    Material mat;
    if (mat.FromJson(j_mat) && ! mat.textures.empty()) info.first_texture = mat.textures.front();
    return info;
}

bool ImageObject::FromJson(const sr::Json& json, fs::VFS& vfs, SceneVersion v) {
    sr::GetJsonValue(json, "image", image);
    composite_layer = image == "models/util/composelayer.json";
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    sr::GetJsonValue(json, "alignment", alignment, false);
    auto parsed_image = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + image));
    if (parsed_image.is_err()) {
        rstd_error("Can't parse image json {}: {}", image, parsed_image.unwrap_err());
        return false;
    }
    auto jImage = parsed_image.unwrap();
    sr::GetJsonValue(jImage, "fullscreen", fullscreen, false);
    sr::GetJsonValue(jImage, "passthrough", config.passthrough, false);
    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "id", id, false);
    sr::GetJsonValue(json, "colorBlendMode", colorBlendMode, false);
    if (! fullscreen) {
        sr::GetJsonValue(json, "origin", origin);
        sr::GetJsonValue(json, "angles", angles);
        sr::GetJsonValue(json, "scale", scale);
        if (! sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false) && composite_layer) {
            // WE gives composite containers the regular layer depth when the field is omitted.
            parallaxDepth = { 1.0f, 1.0f };
        }
        sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        if (jImage.get("width").is_some()) {
            int32_t w, h;
            sr::GetJsonValue(jImage, "width", w);
            sr::GetJsonValue(jImage, "height", h);
            size = { (float)w, (float)h };
        } else if (json.get("size").is_some()) {
            sr::GetJsonValue(json, "size", size);
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    sr::GetJsonValue(jImage, "nopadding", nopadding, false);
    sr::GetJsonValue(jImage, "solidlayer", solid_layer, false);
    sr::GetJsonValue(json, "color", color, false);
    ReadUserValueBinding(json, "color", color_user);
    color_user_key = color_user.name;
    sr::GetJsonValue(json, "alpha", alpha, false);
    alpha = NormalizeLayerAlpha(alpha);
    ReadUserValueBinding(json, "alpha", alpha_user);
    alpha_user_key = alpha_user.name;
    sr::GetJsonValue(json, "brightness", brightness, false);

    sr::GetJsonValue(jImage, "puppet", puppet, false);
    bool copy_background_value { true };
    bool explicit_no_copy_background =
        sr::GetJsonValue(json, "copybackground", copy_background_value, false) &&
        ! copy_background_value;

    if (jImage.get("material").is_some()) {
        std::string matPath;
        sr::GetJsonValue(jImage, "material", matPath);
        auto parsed_material = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath));
        if (parsed_material.is_err()) {
            rstd_error("Can't parse material json {}: {}", matPath, parsed_material.unwrap_err());
            return false;
        }
        auto jMat = parsed_material.unwrap();
        material.FromJson(jMat, v);
        if (image == "models/util/composelayer.json" && explicit_no_copy_background) {
            material.combos["CLEARALPHA"] = 1;
        }
    } else {
        rstd_info("image object no material");
        return false;
    }
    if (auto values = json.get("effects"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jE : **array) {
                ImageEffect wpeff;
                wpeff.FromJson(jE, vfs, v);
                effects.push_back(std::move(wpeff));
            }
        }
    }
    ReadPuppetAnimationLayers(json, puppet_layers);
    if (auto config_json = json.get("config"); config_json.is_some()) {
        sr::GetJsonValue(**config_json, "passthrough", config.passthrough, false);
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
    if (auto instance_json = json.get("instance");
        instance_json.is_some() && (*instance_json)->is_object()) {
        instance.FromJson(**instance_json);
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
