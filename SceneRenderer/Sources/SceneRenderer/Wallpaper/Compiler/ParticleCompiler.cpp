module;

#include <rstd/macro.hpp>

module sr.pkg.parse;
import eigen;
import sr.core;
import rstd.log;
import rstd.cppstd;
import sr.utils;
import sr.scene;

using namespace sr;
using namespace Eigen;
namespace PM = ParticleModify;

namespace
{

inline void Color(Particle& p, const std::array<float, 3> min, const std::array<float, 3> max) {
    double               random = Random::get(0.0, 1.0);
    std::array<float, 3> result;
    for (int32_t i = 0; i < 3; i++) {
        result[i] = (float)algorism::lerp(random, min[i], max[i]);
    }
    PM::InitColor(p, result[0], result[1], result[2]);
}

inline Vector3d GenRandomVec3(const std::array<float, 3>& min, const std::array<float, 3>& max) {
    Vector3d result(3);
    for (int32_t i = 0; i < 3; i++) {
        result[i] = Random::get(min[i], max[i]);
    }
    return result;
}

inline float UiColorToLinear(float value) { return value * value; }

inline float UiScalarToLinear(float value) { return value; }

} // namespace

struct SingleRandom {
    float       min { 0.0f };
    float       max { 0.0f };
    float       exponent { 1.0f };
    static void ReadFromJson(const Json& j, SingleRandom& r) {
        sr::GetJsonValue(j, "min", r.min, false);
        sr::GetJsonValue(j, "max", r.max, false);
    };
};
struct VecRandom {
    std::array<float, 3> min { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> max { 0.0f, 0.0f, 0.0f };
    float                exponent { 1.0f };

    static void ReadFromJson(const Json& j, VecRandom& r) {
        sr::GetJsonValue(j, "min", r.min, false);
        sr::GetJsonValue(j, "max", r.max, false);
    };
};
struct TurbulentRandom {
    float  scale { 1.0f };
    double timescale { 1.0f };
    float  offset { 0.0f };
    float  speedmin { 100.0f };
    float  speedmax { 250.0f };
    float  phasemin { 0.0f };
    float  phasemax { 0.1f };

    std::array<float, 3> forward { 0.0f, 1.0f, 0.0f }; // x y z
    std::array<float, 3> right { 0.0f, 0.0f, 1.0f };
    std::array<float, 3> up { 1.0f, 0.0f, 0.0f };

    static void ReadFromJson(const Json& j, TurbulentRandom& r) {
        sr::GetJsonValue(j, "scale", r.scale, false);
        sr::GetJsonValue(j, "timescale", r.timescale, false);
        sr::GetJsonValue(j, "offset", r.offset, false);
        sr::GetJsonValue(j, "speedmin", r.speedmin, false);
        sr::GetJsonValue(j, "speedmax", r.speedmax, false);
        sr::GetJsonValue(j, "phasemin", r.phasemin, false);
        sr::GetJsonValue(j, "phasemax", r.phasemax, false);
        sr::GetJsonValue(j, "forward", r.forward, false);
        sr::GetJsonValue(j, "right", r.right, false);
        sr::GetJsonValue(j, "up", r.up, false);
    };
};
template<std::size_t N>
std::array<float, N> mapVertex(const std::array<float, N>& v, float (*oper)(float)) {
    std::array<float, N> result;
    std::transform(v.begin(), v.end(), result.begin(), oper);
    return result;
};

ParticleInitOp WPParticleParser::genParticleInitOp(const Json& wpj) {
    using namespace std::placeholders;
    do {
        if (wpj.get("name").is_none()) break;
        std::string name;
        sr::GetJsonValue(wpj, "name", name);

        if (name.compare("colorrandom") == 0) {
            VecRandom r;
            r.min = { 0.0f, 0.0f, 0.0f };
            r.max = { 255.0f, 255.0f, 255.0f };
            VecRandom::ReadFromJson(wpj, r);

            return [=](Particle& p, double) {
                Color(p,
                      mapVertex(r.min,
                                [](float x) {
                                    return x / 255.0f;
                                }),
                      mapVertex(r.max, [](float x) {
                          return x / 255.0f;
                      }));
            };
        } else if (name.compare("lifetimerandom") == 0) {
            SingleRandom r = { 0.0f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitLifetime(p, Random::get(r.min, r.max));
            };
        } else if (name.compare("sizerandom") == 0) {
            SingleRandom r = { 0.0f, 20.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitSize(p, Random::get(r.min, r.max));
            };
        } else if (name.compare("alpharandom") == 0) {
            SingleRandom r = { 0.05f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitAlpha(p, Random::get(r.min, r.max));
            };
        } else if (name.compare("velocityrandom") == 0) {
            VecRandom r;
            r.min[0] = r.min[1] = -32.0f;
            r.max[0] = r.max[1] = 32.0f;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
        } else if (name.compare("rotationrandom") == 0) {
            VecRandom r;
            r.max[2] = rstd::f32_::consts::TAU;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeRotation(p, result[0], result[1], result[2]);
            };
        } else if (name.compare("angularvelocityrandom") == 0) {
            VecRandom r;
            r.min[2] = -5.0f;
            r.max[2] = 5.0f;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeAngularVelocity(p, result[0], result[1], result[2]);
            };
        } else if (name.compare("turbulentvelocityrandom") == 0) {
            // to do
            TurbulentRandom r;
            TurbulentRandom::ReadFromJson(wpj, r);
            Vector3f forward(r.forward.data());
            Vector3f right(r.right.data());
            Vector3f pos = GenRandomVec3({ 0, 0, 0 }, { 10.0f, 10.0f, 10.0f }).cast<float>();
            return [=](Particle& p, double duration) mutable {
                float speed = Random::get(r.speedmin, r.speedmax);
                if (duration > 10.0f) {
                    pos[0] += speed;
                    duration = 0.0f;
                }
                Vector3f result;
                do {
                    result = algorism::CurlNoise(pos.cast<double>()).cast<float>().normalized();
                    pos += result * 0.005f / r.timescale;
                    duration -= 0.01f;
                } while (duration > 0.01f);
                // limit direction
                {
                    double c     = result.dot(forward) / (result.norm() * forward.norm());
                    float  a     = std::acos(c) / rstd::f32_::consts::PI;
                    float  scale = r.scale / 2.0f;
                    if (a > scale) {
                        auto axis = result.cross(forward).normalized();
                        result =
                            AngleAxisf((a - a * scale) * rstd::f32_::consts::PI, axis) * result;
                    }
                }
                // offset
                result = AngleAxisf(r.offset, right) * result;
                result *= speed;
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
        }
    } while (false);
    return [](Particle&, double) {
    };
}

ParticleInitOp
WPParticleParser::genOverrideInitOp(std::shared_ptr<const wpscene::ParticleInstanceoverride> over) {
    return [over = std::move(over)](Particle& p, double) {
        PM::MutiplyInitLifeTime(p, over->lifetime);
        PM::MutiplyInitAlpha(p, UiScalarToLinear(over->alpha));
        PM::MutiplyInitSize(p, over->size);
        PM::MutiplyVelocity(p, over->speed);
        if (over->overColor) {
            PM::InitColor(p,
                          UiColorToLinear(over->color[0] / 255.0f),
                          UiColorToLinear(over->color[1] / 255.0f),
                          UiColorToLinear(over->color[2] / 255.0f));
        } else if (over->overColorn) {
            // `colorn` = "color (normalized)" -> absolute 0..1 RGB override
            // (matches WE editor behaviour: picking red in the UI yields a
            // red trail regardless of the base colorrandom initializer).
            PM::InitColor(p,
                          UiColorToLinear(over->colorn[0]),
                          UiColorToLinear(over->colorn[1]),
                          UiColorToLinear(over->colorn[2]));
        }
    };
}
double FadeValueChange(float life, float start, float end, float startValue,
                       float endValue) noexcept {
    if (life <= start)
        return startValue;
    else if (life > end)
        return endValue;
    else {
        double pass = (life - start) / (end - start);
        return algorism::lerp(pass, startValue, endValue);
    }
}

struct ValueChange {
    float starttime { 0 };
    float endtime { 1.0f };
    float startvalue { 1.0f };
    float endvalue { 0.0f };

    static auto ReadFromJson(const Json& j) {
        ValueChange v;
        sr::GetJsonValue(j, "starttime", v.starttime, false);
        sr::GetJsonValue(j, "endtime", v.endtime, false);
        sr::GetJsonValue(j, "startvalue", v.startvalue, false);
        sr::GetJsonValue(j, "endvalue", v.endvalue, false);
        return v;
    }
};
double FadeValueChange(float life, const ValueChange& v) noexcept {
    return FadeValueChange(life, v.starttime, v.endtime, v.startvalue, v.endvalue);
}

struct VecChange {
    float                starttime { 0 };
    float                endtime { 1.0f };
    std::array<float, 3> startvalue { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> endvalue { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        VecChange v;
        sr::GetJsonValue(j, "starttime", v.starttime, false);
        sr::GetJsonValue(j, "endtime", v.endtime, false);
        sr::GetJsonValue(j, "startvalue", v.startvalue, false);
        sr::GetJsonValue(j, "endvalue", v.endvalue, false);
        return v;
    }
};

struct FrequencyValue {
    std::array<float, 3> mask { 1.0f, 1.0f, 0.0f };

    float frequencymin { 0.0f };
    float frequencymax { 10.0f };
    float scalemin { 0.0f };
    float scalemax { 1.0f };
    float phasemin { 0.0f };
    float phasemax { static_cast<float>(rstd::f32_::consts::TAU) };

    struct StorageRandom {
        bool  reset { true };
        float frequency { 0.0f };
        float scale { 1.0f };
        float phase { 0.0f };
    };

    std::vector<StorageRandom> storage;

    static auto ReadFromJson(const Json& j, std::string_view name) {
        FrequencyValue v;
        if (name.compare("oscillatesize") == 0) {
            v.scalemin = 0.8f;
            v.scalemax = 1.2f;
        } else if (name.compare("oscillateposition") == 0) {
            v.frequencymax = 5.0f;
        }
        sr::GetJsonValue(j, "frequencymin", v.frequencymin, false);
        sr::GetJsonValue(j, "frequencymax", v.frequencymax, false);
        if (v.frequencymax == 0.0f) v.frequencymax = v.frequencymin;
        sr::GetJsonValue(j, "scalemin", v.scalemin, false);
        sr::GetJsonValue(j, "scalemax", v.scalemax, false);
        sr::GetJsonValue(j, "phasemin", v.phasemin, false);
        sr::GetJsonValue(j, "phasemax", v.phasemax, false);
        sr::GetJsonValue(j, "mask", v.mask, false);
        return v;
    };
    inline void CheckAndResize(size_t s) {
        if (storage.size() < s) storage.resize(std::max(s, storage.size() * 2), StorageRandom {});
    }
    inline void GenFrequency(Particle& p, uint32_t slot_id) {
        CheckAndResize(static_cast<size_t>(slot_id) + 1);
        auto& st = storage.at(slot_id);
        // A compacted simulation array reuses physical vector positions. Slot
        // ids follow the particle instead, and a fresh spawn must never inherit
        // the oscillator state of the particle that previously used its slot.
        if (! PM::LifetimeOk(p) || PM::IsNew(p)) st.reset = true;
        if (st.reset) {
            st.frequency = Random::get(frequencymin, frequencymax);
            st.scale     = Random::get(scalemin, scalemax);
            st.phase     = (float)Random::get((double)phasemin, phasemax + rstd::f64_::consts::TAU);
            st.reset     = false;
        }
    }
    inline double GetScale(uint32_t slot_id, double time) {
        const auto& st = storage.at(slot_id);
        double      f  = st.frequency / (rstd::f32_::consts::TAU);
        double      w  = rstd::f32_::consts::TAU * f;
        return algorism::lerp((std::cos(w * time + st.phase) + 1.0f) * 0.5f, scalemin, scalemax);
    }
    inline double GetMove(uint32_t slot_id, double time, double timePass) {
        const auto& st = storage.at(slot_id);
        double      f  = st.frequency / (rstd::f32_::consts::TAU);
        double      w  = rstd::f32_::consts::TAU * f;
        return -1.0f * st.scale * w * std::sin(w * time + st.phase) * timePass;
    }
};

struct Turbulence {
    // the minimum time offset of the noise field for a particle.
    float phasemin { 0 };
    // the maximum time offset of the noise field for a particle.
    float phasemax { 0 };
    // the minimum velocity applied to particles.
    float speedmin { 500.0f };
    // the maximum velocity applied to particles.
    float speedmax { 1000.0f };
    // how fast the noise field changes shape.
    float timescale { 20.0f };

    float scale { 0.01f };

    std::array<int32_t, 3> mask { 1, 1, 0 };

    static auto ReadFromJson(const Json& j) {
        Turbulence v;
        sr::GetJsonValue(j, "phasemin", v.phasemin, false);
        sr::GetJsonValue(j, "phasemax", v.phasemax, false);
        sr::GetJsonValue(j, "speedmin", v.speedmin, false);
        sr::GetJsonValue(j, "speedmax", v.speedmax, false);
        sr::GetJsonValue(j, "timescale", v.timescale, false);
        sr::GetJsonValue(j, "mask", v.mask, false);
        sr::GetJsonValue(j, "scale", v.scale, false);
        return v;
    };
};

struct Vortex {
    enum class FlagEnum
    {
        infinit_axis = 0, // 1
    };
    using EFlags = BitFlags<FlagEnum>;

    i32 controlpoint { 0 };

    // anything below this distance receives force multiplied with speed inner.
    float distanceinner { 500.0f };
    // anything above this distance receives force multiplied with speed outer.
    float distanceouter { 650.0f };
    // amount of force applied to inner ring.
    float speedinner { 2500.0f };
    // amount of force applied to outer ring.
    float speedouter { 0 };

    EFlags flags { 0 };

    // positional offset from the center of the control point.
    std::array<float, 3> offset { 0.0f, 0.0f, 0.0f };

    // the axis to rotate around.
    std::array<float, 3> axis { 0.0f, 0.0f, 1.0f };

    static auto ReadFromJson(const Json& j) {
        Vortex v;
        sr::GetJsonValue(j, "controlpoint", v.controlpoint, false);
        if (v.controlpoint >= 8) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= 8;

        sr::GetJsonValue(j, "distanceinner", v.distanceinner, false);
        sr::GetJsonValue(j, "distanceouter", v.distanceouter, false);
        sr::GetJsonValue(j, "speedinner", v.speedinner, false);
        sr::GetJsonValue(j, "speedouter", v.speedouter, false);

        i32 _flags { 0 };
        sr::GetJsonValue(j, "flags", _flags, false);
        v.flags = EFlags(_flags);

        sr::GetJsonValue(j, "offset", v.offset, false);
        sr::GetJsonValue(j, "axis", v.axis, false);

        return v;
    };
};

struct ControlPointForce {
    i32 controlpoint { 0 };

    // how strongly the control point attracts or repels.
    float scale { 512.0f };
    // the maximum distance between particle and control point where the force takes effect.
    float threshold { 512.0f };

    // positional offset from the center of the control point.
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        ControlPointForce v;
        sr::GetJsonValue(j, "controlpoint", v.controlpoint, false);
        if (v.controlpoint >= 8) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= 8;

        sr::GetJsonValue(j, "scale", v.scale, false);
        sr::GetJsonValue(j, "threshold", v.threshold, false);

        sr::GetJsonValue(j, "origin", v.origin, false);
        return v;
    };
};

ParticleOperatorOp WPParticleParser::genParticleOperatorOp(
    const Json& wpj, std::shared_ptr<const wpscene::ParticleInstanceoverride> over_state) {
    do {
        if (wpj.get("name").is_none()) break;
        std::string name;
        sr::GetJsonValue(wpj, "name", name);
        if (name.compare("movement") == 0) {
            float drag { 0.0f };

            std::array<float, 3> gravity { 0, 0, 0 };
            sr::GetJsonValue(wpj, "drag", drag, false);
            sr::GetJsonValue(wpj, "gravity", gravity, false);
            Vector3d vecG = Vector3f(gravity.data()).cast<double>();
            // Symplectic (semi-implicit) Euler: every force op updates v from
            // the current position; ParticleSubSystem::Emitt applies the
            // x += v_new * dt integration once after all operators are done.
            // Keeping MoveByTime inside `movement` here would mix position
            // updates with force updates and silently fall back to explicit
            // Euler for any force op listed after `movement` in the JSON
            // (e.g. controlpointattract), which diverges in central force
            // fields and makes orbital trails wobble apart.
            return [drag, vecG, over_state](const ParticleInfo& info) {
                auto speed = over_state->speed;
                for (auto& p : info.particles) {
                    Vector3d world_velocity =
                        info.world_from_local_dir * PM::GetVelocity(p).cast<double>();
                    Vector3d acc =
                        info.local_from_world_dir * algorism::DragForce(world_velocity, drag);
                    if (info.world_space)
                        acc += info.local_from_world_dir * vecG;
                    else
                        acc += vecG;
                    PM::Accelerate(p, speed * acc, info.time_pass);
                }
            };
        } else if (name.compare("angularmovement") == 0) {
            float                drag { 0.0f };
            std::array<float, 3> force { 0, 0, 0 };
            sr::GetJsonValue(wpj, "drag", drag, false);
            sr::GetJsonValue(wpj, "force", force, false);
            Vector3d vecF = Vector3f(force.data()).cast<double>();
            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    Vector3d acc =
                        algorism::DragForce(PM::GetAngular(p).cast<double>(), drag) + vecF;
                    PM::AngularAccelerate(p, acc, info.time_pass);
                }
            };
        } else if (name.compare("sizechange") == 0) {
            auto vc = ValueChange::ReadFromJson(wpj);
            return [vc, over_state](const ParticleInfo& info) {
                auto size_over = over_state->size;
                for (auto& p : info.particles)
                    PM::MutiplySize(p, size_over * FadeValueChange(PM::LifetimePos(p), vc));
            };

        } else if (name.compare("alphafade") == 0) {
            float fadeintime { 0.5f }, fadeouttime { 0.5f };
            sr::GetJsonValue(wpj, "fadeintime", fadeintime, false);
            sr::GetJsonValue(wpj, "fadeouttime", fadeouttime, false);
            return [fadeintime, fadeouttime](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    auto life = PM::LifetimePos(p);
                    if (life <= fadeintime)
                        PM::MutiplyAlpha(p, FadeValueChange(life, 0, fadeintime, 0, 1.0f));
                    else if (life > fadeouttime)
                        PM::MutiplyAlpha(p,
                                         1.0f - FadeValueChange(life, fadeouttime, 1.0f, 0, 1.0f));
                }
            };
        } else if (name.compare("alphachange") == 0) {
            auto vc = ValueChange::ReadFromJson(wpj);
            return [vc](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    PM::MutiplyAlpha(p, FadeValueChange(PM::LifetimePos(p), vc));
                }
            };
        } else if (name.compare("colorchange") == 0) {
            auto vc = VecChange::ReadFromJson(wpj);
            return [vc](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    auto     life = PM::LifetimePos(p);
                    Vector3f result;
                    for (unsigned i = 0; i < 3; i++)
                        result[i] = FadeValueChange(
                            life, vc.starttime, vc.endtime, vc.startvalue[i], vc.endvalue[i]);
                    PM::MutiplyColor(p, result[0], result[1], result[2]);
                }
            };
        } else if (name.compare("oscillatealpha") == 0) {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            return [fv](const ParticleInfo& info) mutable {
                fv.CheckAndResize(info.particles.size());
                for (unsigned i = 0; i < info.particles.size(); i++) {
                    auto& p = info.particles[i];
                    fv.GenFrequency(p, p.slot_id);
                    PM::MutiplyAlpha(p, fv.GetScale(p.slot_id, PM::LifetimePassed(p)));
                }
            };
        } else if (name.compare("oscillatesize") == 0) {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            return [fv](const ParticleInfo& info) mutable {
                fv.CheckAndResize(info.particles.size());
                for (unsigned i = 0; i < info.particles.size(); i++) {
                    auto& p = info.particles[i];
                    fv.GenFrequency(p, p.slot_id);
                    PM::MutiplySize(p, fv.GetScale(p.slot_id, PM::LifetimePassed(p)));
                }
            };

        } else if (name.compare("oscillateposition") == 0) {
            std::vector<Vector3f>         lastMove;
            FrequencyValue                fvx = FrequencyValue::ReadFromJson(wpj, name);
            std::array<FrequencyValue, 3> fxp = { fvx, fvx, fvx };
            return [=](const ParticleInfo& info) mutable {
                for (auto& f : fxp) f.CheckAndResize(info.particles.size());
                for (unsigned i = 0; i < info.particles.size(); i++) {
                    auto&    p = info.particles[i];
                    Vector3d del { Vector3d::Zero() };
                    auto     time = PM::LifetimePassed(p);
                    for (unsigned d = 0; d < 3; d++) {
                        if (fxp[0].mask[d] < 0.01) continue;
                        fxp[d].GenFrequency(p, p.slot_id);
                        del[d] = fxp[d].GetMove(p.slot_id, time, info.time_pass);
                    }

                    PM::Move(p, del);
                }
            };
        } else if (name.compare("turbulence") == 0) {
            Turbulence tur   = Turbulence::ReadFromJson(wpj);
            double     phase = Random::get(tur.phasemin, tur.phasemax);
            double     speed = Random::get(tur.speedmin, tur.speedmax);

            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    Vector3d pos = PM::GetPos(p).cast<double>();
                    pos.x() += phase + tur.timescale * info.time;
                    Vector3d result = speed * algorism::CurlNoise(pos * tur.scale * 2).normalized();
                    for (usize i = 0; i < 3; i++) {
                        if (tur.mask[i] == 0) result[i] = 0;
                    }
                    PM::Accelerate(p, result, info.time_pass);
                }
            };
        } else if (name.compare("vortex") == 0) {
            Vortex v = Vortex::ReadFromJson(wpj);
            return [=](const ParticleInfo& info) {
                Vector3d offset  = info.controlpoints[v.controlpoint].offset +
                                   (Vector3f { v.offset.data() }).cast<double>();
                Vector3d axis    = (Vector3f { v.axis.data() }).cast<double>();
                double   dis_mid = v.distanceouter - v.distanceinner + 0.1f;

                for (auto& p : info.particles) {
                    Vector3d pos      = p.position.cast<double>();
                    Vector3d direct   = -axis.cross(pos).normalized();
                    double   distance = (pos - offset).norm();
                    if (dis_mid < 0 || distance < v.distanceinner) {
                        PM::Accelerate(p, direct * v.speedinner, info.time_pass);
                    }
                    if (distance > v.distanceouter) {
                        PM::Accelerate(p, direct * v.speedouter, info.time_pass);
                    } else if (distance > v.distanceinner) {
                        double t = (distance - v.distanceinner) / dis_mid;
                        PM::Accelerate(p,
                                       direct * algorism::lerp(t, v.speedinner, v.speedouter),
                                       info.time_pass);
                    }
                }
            };
        } else if (name.compare("controlpointattract") == 0) {
            ControlPointForce c = ControlPointForce::ReadFromJson(wpj);
            return [=](const ParticleInfo& info) {
                Vector3d offset = info.controlpoints[c.controlpoint].offset +
                                  Vector3f { c.origin.data() }.cast<double>();
                for (auto& p : info.particles) {
                    Vector3d diff     = offset - PM::GetPos(p).cast<double>();
                    double   distance = diff.norm();
                    if (distance < c.threshold) {
                        PM::Accelerate(p, diff.normalized() * c.scale, info.time_pass);
                    }
                }
            };
        }
    } while (false);
    return [](const ParticleInfo&) {
    };
}

ParticleEmittOp WPParticleParser::genParticleEmittOp(const wpscene::Emitter& wpe, bool sort) {
    ParticleAudioResponse audio_response {
        .enable    = wpe.audioprocessingmode != 0,
        .amount    = wpe.audioamount,
        .exponent  = wpe.audioexponent,
        .frequency = wpe.audiofrequency,
        .bounds    = wpe.audiobounds,
    };
    if (wpe.name.compare("boxrandom") == 0) {
        ParticleBoxEmitterArgs box;
        box.emitSpeed      = wpe.rate;
        box.minDistance    = wpe.distancemin;
        box.maxDistance    = wpe.distancemax;
        box.directions     = wpe.directions;
        box.orgin          = wpe.origin;
        box.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        box.instantaneous  = wpe.instantaneous;
        box.minSpeed       = wpe.speedmin;
        box.maxSpeed       = wpe.speedmax;
        box.duration       = wpe.duration;
        box.controlpoint   = wpe.controlpoint;
        box.audio_response = audio_response;
        box.sort           = sort;
        return ParticleBoxEmitterArgs::MakeEmittOp(box);
    } else if (wpe.name.compare("sphererandom") == 0) {
        ParticleSphereEmitterArgs sphere;
        sphere.emitSpeed      = wpe.rate;
        sphere.minDistance    = wpe.distancemin[0];
        sphere.maxDistance    = wpe.distancemax[0];
        sphere.directions     = wpe.directions;
        sphere.orgin          = wpe.origin;
        sphere.sign           = wpe.sign;
        sphere.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        sphere.instantaneous  = wpe.instantaneous;
        sphere.minSpeed       = wpe.speedmin;
        sphere.maxSpeed       = wpe.speedmax;
        sphere.duration       = wpe.duration;
        sphere.controlpoint   = wpe.controlpoint;
        sphere.audio_response = audio_response;
        sphere.sort           = sort;
        return ParticleSphereEmitterArgs::MakeEmittOp(sphere);
    } else
        return [](std::vector<Particle>&,
                  std::vector<ParticleInitOp>&,
                  uint32_t,
                  double,
                  std::span<const float>,
                  std::span<const ParticleControlpoint>) {
        };
}
