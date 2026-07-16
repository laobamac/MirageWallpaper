module;

#include <rstd/macro.hpp>

module sr.pkg.scene_obj;
import sr.core;
import sr.fs;
import sr.json;
import rstd.log;
import rstd.cppstd;

using namespace sr::wpscene;

bool ParticleChild::FromJson(const sr::Json& json, fs::VFS& vfs) {
    sr::GetJsonValue(json, "name", name);
    sr::GetJsonValue(json, "type", type);

    if (name.empty()) {
        return false;
    }

    auto parsed_particle = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + name));
    if (parsed_particle.is_err()) {
        rstd_error("Can't parse particle json {}: {}", name, parsed_particle.unwrap_err());
        return false;
    }
    auto jParticle = parsed_particle.unwrap();

    if (! obj.FromJson(jParticle, vfs)) return false;

    sr::GetJsonValue(json, "maxcount", maxcount, false);
    sr::GetJsonValue(json, "controlpointstartindex", controlpointstartindex, false);
    sr::GetJsonValue(json, "probability", probability, false);
    sr::GetJsonValue(json, "origin", origin, false);
    sr::GetJsonValue(json, "scale", scale, false);
    sr::GetJsonValue(json, "angles", angles, false);
    return true;
}

bool ParticleControlpoint::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "id", id);

    uint32_t _raw_flags { 0 };
    sr::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    sr::GetJsonValue(json, "offset", offset, false);
    return true;
};

bool ParticleRender::FromJson(const sr::Json& json) {
    sr::GetJsonValue(json, "name", name);

    if (sstart_with(name, "rope")) {
        sr::GetJsonValue(json, "subdivision", subdivision, false);
    }
    if (name.compare("spritetrail") == 0 || name.compare("ropetrail") == 0) {
        sr::GetJsonValue(json, "length", length, false);
        sr::GetJsonValue(json, "maxlength", maxlength, false);
        sr::GetJsonValue(json, "segments", segments, false);
    }
    return true;
}

bool Emitter::FromJson(const sr::Json& json) {
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

bool ParticleInstanceoverride::FromJosn(const sr::Json& json) {
    enabled = true;

    // {"user":"<key>","value":...} indirection -> record the key for the
    // live user-property pipeline. The value still parses normally via
    // GetJsonValue (which already looks through the `value` wrapper).
    auto bind = [&](const char* field) {
        auto sub = json.get(field);
        if (sub.is_none() || ! (*sub)->is_object()) return;
        auto user = (*sub)->get("user");
        if (user.is_none()) return;
        auto string = (*user)->as_str();
        if (string.is_some()) bindings[field] = rstd::cppstd::to_string(*string);
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
    if (auto value = json.get("color"); value.is_some()) {
        sr::GetJsonValue(json, "color", color);
        overColor = true;
        bind("color");
    } else if (auto value = json.get("colorn"); value.is_some()) {
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

bool Particle::FromJson(const sr::Json& json, fs::VFS& vfs) {
    auto emitter_values = json.get("emitter");
    if (emitter_values.is_none()) {
        rstd_error("particle no emitter");
        return false;
    }
    auto emitter_array = (*emitter_values)->as_array();
    if (emitter_array.is_none()) {
        rstd_error("particle emitter is not an array");
        return false;
    }
    for (const auto& el : **emitter_array) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push_back(std::move(emi));
    }
    if (auto values = json.get("renderer"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleRender pr;
                pr.FromJson(el);
                renderers.push_back(std::move(pr));
            }
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
    }
    if (auto values = json.get("initializer"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) initializers.push(el.clone());
    }
    if (auto values = json.get("operator"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) operators.push(el.clone());
    }
    if (auto values = json.get("controlpoint"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleControlpoint pc;
                pc.FromJson(el);
                controlpoints.push_back(std::move(pc));
            }
        }
    }

    if (auto values = json.get("children"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleChild child;
                if (child.FromJson(el, vfs)) children.push_back(std::move(child));
            }
        }
    }
    if (json.get("material").is_some()) {
        std::string matPath;
        sr::GetJsonValue(json, "material", matPath);
        auto parsed_material = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath));
        if (parsed_material.is_err()) {
            rstd_error("Can't parse material json {}: {}", matPath, parsed_material.unwrap_err());
            return false;
        }
        auto jMat = parsed_material.unwrap();
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

bool ParticleObject::FromJson(const sr::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ParticleObject::FromJson(const sr::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    sr::GetJsonValue(json, "particle", particle);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;

    sr::GetJsonValue(json, "name", name, false);
    sr::GetJsonValue(json, "id", id, false);
    sr::GetJsonValue(json, "origin", origin);
    sr::GetJsonValue(json, "angles", angles);
    sr::GetJsonValue(json, "scale", scale);
    sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);

    if (auto value = json.get("instanceoverride"); value.is_some() && ! (*value)->is_null()) {
        instanceoverride.FromJosn(**value);
    }

    sr::GetJsonValue(json, "locktransforms", locktransforms, false);
    sr::GetJsonValue(json, "muteineditor", muteineditor, false);
    sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    sr::GetJsonValue(json, "parent", parent, false);
    sr::GetJsonValue(json, "attachment", attachment, false);
    sr::GetJsonValue(json, "dependencies", dependencies, false);
    sr::GetJsonValue(json, "controlpoint", controlpoint, false);
    if (auto value = json.get("instance"); value.is_some()) instance = (*value)->clone();
    if (auto value = json.get("particlesrc"); value.is_some()) particlesrc = (*value)->clone();

    AbsorbAllFieldBindings(json, field_bindings);

    auto parsed_particle = sr::ParseJson(fs::GetFileContent(vfs, "/assets/" + particle));
    if (parsed_particle.is_err()) {
        rstd_error("Can't parse particle json {}: {}", particle, parsed_particle.unwrap_err());
        return false;
    }
    auto jParticle = parsed_particle.unwrap();
    if (! particleObj.FromJson(jParticle, vfs)) return false;
    return true;
}
