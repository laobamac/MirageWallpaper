module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import nlohmann.json;
import sr.core;
import rstd.log;
import rstd.cppstd;

using namespace sr::wpscene;

bool ParticleChild::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "type", type);

    if (name.empty()) {
        return false;
    }

    nlohmann::json jParticle;
    if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + name), jParticle)) return false;

    if (! obj.FromJson(jParticle, vfs)) return false;

    sr::GetJsonValue(json, "maxcount", maxcount, false);
    sr::GetJsonValue(json, "controlpointstartindex", controlpointstartindex, false);
    sr::GetJsonValue(json, "probability", probability, false);

    sr::GetJsonValue(json, "origin", origin, false);
    sr::GetJsonValue(json, "scale", scale, false);
    sr::GetJsonValue(json, "angles", angles, false);
    return true;
}

bool ParticleControlpoint::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "id", id);

    uint32_t _raw_flags { 0 };
    sr::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    sr::GetJsonValue(json, "offset", offset, false);
    return true;
};

bool ParticleRender::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "name", name);

    if (sstart_with(name, "rope")) {
        sr::GetJsonValue(json, "subdivision", subdivision, false);
    }
    if (name == "spritetrail" || name == "ropetrail") {
        sr::GetJsonValue(json, "length", length, false);
        sr::GetJsonValue(json, "maxlength", maxlength, false);
        sr::GetJsonValue(json, "segments", segments, false);
    }
    return true;
}

bool Emitter::FromJson(const nlohmann::json& json) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "id", id);

    sr::GetJsonValue(json, "speedmin", speedmin, false);
    sr::GetJsonValue(json, "speedmax", speedmax, false);
    sr::GetJsonValue(json, "instantaneous", instantaneous, false);
    sr::GetJsonValue(json, "distancemax", distancemax, false);
    sr::GetJsonValue(json, "distancemin", distancemin, false);
    sr::GetJsonValue(json, "rate", rate, false);
    sr::GetJsonValue(json, "directions", directions, false);
    sr::GetJsonValue(json, "origin", origin, false);
    sr::GetJsonValue(json, "sign", sign, false);
    sr::GetJsonValue(json, "audioprocessingmode", audioprocessingmode, false);
    sr::GetJsonValue(json, "audioamount", audioamount, false);
    sr::GetJsonValue(json, "audioexponent", audioexponent, false);
    sr::GetJsonValue(json, "audiofrequency", audiofrequency, false);
    sr::GetJsonValue(json, "audiobounds", audiobounds, false);
    sr::GetJsonValue(json, "controlpoint", controlpoint, false);
    sr::GetJsonValue(json, "duration", duration, false);

    if (controlpoint >= 8) rstd_error("wrong controlpoint {}", controlpoint);
    controlpoint = controlpoint % 8; // limited to 0-7

    uint32_t _raw_flags { 0 };
    sr::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    std::transform(sign.begin(), sign.end(), sign.begin(), [](int32_t v) {
        if (v != 0)
            return v / std::abs(v);
        else
            return 0;
    });
    return true;
}

bool ParticleInstanceoverride::FromJosn(const nlohmann::json& json) {
    enabled = true;

    // {"user":"<key>","value":...} indirection -> record the key for the
    // live user-property pipeline. The value still parses normally via
    // GetJsonValue (which already looks through the `value` wrapper).
    auto bind = [&](const char* field) {
        if (! json.contains(field)) return;
        const auto& sub = json.at(field);
        if (! sub.is_object()) return;
        auto it = sub.find("user");
        if (it != sub.end() && it->is_string()) {
            bindings[field] = it->get<std::string>();
        }
    };

    sr::GetJsonValue(json, "alpha", alpha, false);
    bind("alpha");
    sr::GetJsonValue(json, "size", size, false);
    bind("size");
    sr::GetJsonValue(json, "lifetime", lifetime, false);
    bind("lifetime");
    sr::GetJsonValue(json, "rate", rate, false);
    bind("rate");
    sr::GetJsonValue(json, "speed", speed, false);
    bind("speed");
    sr::GetJsonValue(json, "count", count, false);
    bind("count");
    sr::GetJsonValue(json, "brightness", brightness, false);
    bind("brightness");
    sr::GetJsonValue(json, "id", id, false);
    if (json.contains("color")) {
        sr::GetJsonValue(json, "color", color);
        overColor = true;
        bind("color");
    } else if (json.contains("colorn")) {
        sr::GetJsonValue(json, "colorn", colorn);
        overColorn = true;
        bind("colorn");
    }
    {
        const char* cp_keys[]  = { "controlpoint0", "controlpoint1", "controlpoint2",
                                   "controlpoint3", "controlpoint4", "controlpoint5",
                                   "controlpoint6", "controlpoint7" };
        const char* cpa_keys[] = { "controlpointangle0", "controlpointangle1", "controlpointangle2",
                                   "controlpointangle3", "controlpointangle4", "controlpointangle5",
                                   "controlpointangle6", "controlpointangle7" };
        for (int i = 0; i < 8; ++i) {
            sr::GetJsonValue(json, cp_keys[i], controlpoint[i], false);
            bind(cp_keys[i]);
            sr::GetJsonValue(json, cpa_keys[i], controlpointangle[i], false);
            bind(cpa_keys[i]);
        }
    }
    return true;
};

bool Particle::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    if (! json.contains("emitter")) {
        rstd_error("particle no emitter");
        return false;
    }
    for (const auto& el : json.at("emitter")) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push_back(emi);
    }
    if (json.contains("renderer")) {
        for (const auto& el : json.at("renderer")) {
            ParticleRender pr;
            pr.FromJson(el);
            renderers.push_back(pr);
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
    }
    if (json.contains("initializer")) {
        for (const auto& el : json.at("initializer")) {
            initializers.push_back(el);
        }
    }
    if (json.contains("operator")) {
        for (const auto& el : json.at("operator")) {
            operators.push_back(el);
        }
    }
    if (json.contains("controlpoint")) {
        for (const auto& el : json.at("controlpoint")) {
            ParticleControlpoint pc;
            pc.FromJson(el);
            controlpoints.push_back(pc);
        }
    }

    if (json.contains("children")) {
        for (const auto& el : json.at("children")) {
            ParticleChild child;
            if (child.FromJson(el, vfs)) {
                children.push_back(child);
            }
        }
    }
    if (json.contains("material")) {
        std::string matPath;
        sr::GetJsonValue(json, "material", matPath);
        nlohmann::json jMat;
        if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
        material.FromJson(jMat);
    } else {
        rstd_error("particle object no material");
        return false;
    }

    sr::GetJsonValue(json, "animationmode", animationmode, false);
    sr::GetJsonValue(json, "sequencemultiplier", sequencemultiplier, false);
    sr::GetJsonValue(json, "maxcount", maxcount);
    sr::GetJsonValue(json, "starttime", starttime);

    uint32_t rawflags { 0 };
    sr::GetJsonValue(json, "flags", rawflags, false);
    flags = EFlags(rawflags);

    return true;
}

bool ParticleObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ParticleObject::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    sr::GetJsonValue(json, "particle", particle);
    sr::GetJsonValue(json, "visible", visible, false);
    ReadVisibleUserBinding(json, visible_user);
    visible_user_key = visible_user.name;

    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "id", id, false);
    sr::GetJsonValue(json, "origin", origin);
    sr::GetJsonValue(json, "angles", angles);
    sr::GetJsonValue(json, "scale", scale);
    sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);

    if (json.contains("instanceoverride") && ! json.at("instanceoverride").is_null()) {
        instanceoverride.FromJosn(json.at("instanceoverride"));
    }

    sr::GetJsonValue(json, "locktransforms", locktransforms, false);
    sr::GetJsonValue(json, "muteineditor", muteineditor, false);
    sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    sr::GetJsonValue(json, "parent", parent, false);
    sr::GetJsonValue(json, "attachment", attachment, false);
    sr::GetJsonValue(json, "dependencies", dependencies, false);
    sr::GetJsonValue(json, "controlpoint", controlpoint, false);
    if (json.contains("instance")) instance = json.at("instance");
    if (json.contains("particlesrc")) particlesrc = json.at("particlesrc");

    AbsorbAllFieldBindings(json, field_bindings);

    nlohmann::json jParticle;
    if (! sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + particle), jParticle)) return false;
    if (! particleObj.FromJson(jParticle, vfs)) return false;
    return true;
}
