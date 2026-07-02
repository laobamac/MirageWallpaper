module;

#include <nlohmann/json.hpp>

export module sr.pkg.scene_obj:particle_object;
import sr.core;
import rstd.cppstd;
import sr.utils;
import sr.fs;

export import :field_binding;
import :visibility_binding;
export import :material;
import :scene_document;

export namespace sr

{
namespace wpscene
{

class ParticleControlpoint {
public:
    enum class FlagEnum
    {
        link_mouse = 0, // 1
        // this control point will follow the mouse cursor.
        worldspace = 1, // 2
        // the control point will always be at the same position in the world, independent from the
        // position of the particle system.
    };
    using EFlags = BitFlags<FlagEnum>;

    bool                 FromJson(const nlohmann::json&);
    EFlags               flags { 0 };
    i32                  id { -1 };
    std::array<float, 3> offset { 0, 0, 0 };
    // a static offset relative to the position of the particle system.
};

class ParticleRender {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    float       length { 0.05f };
    float       maxlength { 10.0f };
    float       subdivision { 3.0f };
    // Trail history depth per rope-head particle (= number of trail nodes).
    // Only consumed by rope/trail renderers; default matches WE behaviour.
    i32 segments { 16 };
};

class Initializer {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> max { 0, 0, 0 };
    std::array<float, 3> min { 0, 0, 0 };
    std::string          name;
};

class Emitter {
public:
    enum class FlagEnum : uint32_t
    {
        one_per_frame = 1,
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool                   FromJson(const nlohmann::json&);
    std::array<float, 3>   directions { 1.0f, 1.0f, 0.0f };
    std::array<float, 3>   distancemax { 256.0f, 256.0f, 256.0f };
    std::array<float, 3>   distancemin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>   origin { 0, 0, 0 };
    std::array<int32_t, 3> sign { 0, 0, 0 };
    u32                    instantaneous { 0 };
    float                  speedmin { 0 };
    float                  speedmax { 0 };
    u32                    audioprocessingmode { 0 };
    float                  audioamount { 1.0f };
    float                  audioexponent { 1.0f };
    std::array<float, 2>   audiofrequency { 0.0f, 15.0f };
    std::array<float, 2>   audiobounds { 0.0f, 1.0f };
    i32                    controlpoint { 0 };
    i32                    id;
    EFlags                 flags;
    std::string            name;
    float                  rate { 5.0f };
    float                  duration { 0.0f };
};

class ParticleChild;
class Particle {
public:
    enum class FlagEnum
    {
        wordspace             = 0, // 1
        spritenoframeblending = 1, // 2
        perspective           = 2, // 4
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool FromJson(const nlohmann::json&, fs::VFS&);

    std::vector<Emitter>              emitters;
    std::vector<nlohmann::json>       initializers;
    std::vector<nlohmann::json>       operators;
    std::vector<ParticleRender>       renderers;
    std::vector<ParticleControlpoint> controlpoints;

    Material material;

    std::vector<ParticleChild> children;

    std::string animationmode;
    float       sequencemultiplier { 1.0f };
    uint32_t    maxcount { 1 };
    uint32_t    starttime { 0 };
    EFlags      flags { 0 };
};
class ParticleChild {
public:
    bool FromJson(const nlohmann::json&, fs::VFS&);

    // static
    // eventfollow
    // eventspawn
    // eventdeath
    std::string type { "static" };
    std::string name;
    i32         maxcount { 20 };

    // flags
    i32   controlpointstartindex { 0 };
    float probability { 1.0f };

    std::array<float, 3> angles { 0, 0, 0 };
    std::array<float, 3> origin { 0, 0, 0 };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };

    Particle obj;
};

class ParticleInstanceoverride {
public:
    bool FromJosn(const nlohmann::json&);
    bool enabled { false };
    bool overColor { false };
    bool overColorn { false };

    float                alpha { 1.0f };
    float                count { 1.0f };
    float                lifetime { 1.0f };
    float                rate { 1.0f };
    float                speed { 1.0f };
    float                size { 1.0f };
    float                brightness { 1.0f };
    std::int32_t         id { 0 };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> colorn { 1.0f, 1.0f, 1.0f };

    // controlpoint{0..7} carry "x y z" triplet strings (per-particle CP
    // overrides); controlpointangle{0..7} carry euler triplets in the same
    // string format. Captured into static arrays of array<float,3>.
    std::array<std::array<float, 3>, 8> controlpoint {};
    std::array<std::array<float, 3>, 8> controlpointangle {};

    // field name (e.g. "alpha", "size", "color", "colorn", "lifetime",
    // "rate", "speed", "count", "brightness") -> user-property key when the
    // scene.json value is wrapped in `{"user":"<key>","value":...}`. The
    // owning particle subsystem keeps the override behind a shared_ptr so
    // RenderSetUserProperty can mutate the relevant field at runtime and the
    // change is picked up by every initializer/operator captured closure.
    std::unordered_map<std::string, std::string> bindings;
};

class ParticleObject {
public:
    bool                     FromJson(const nlohmann::json&, fs::VFS&);               // legacy
    bool                     FromJson(const nlohmann::json&, fs::VFS&, SceneVersion); // canonical
    int32_t                  id;
    std::string              name;
    std::array<float, 3>     origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>     angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>     parallaxDepth { 0.0f, 0.0f };
    bool                     visible { true };
    std::string              particle;
    Particle                 particleObj;
    ParticleInstanceoverride instanceoverride;

    // Common cross-kind metadata.
    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::string               attachment;
    std::vector<std::int32_t> dependencies;
    nlohmann::json            instance;
    nlohmann::json            particlesrc;                       // PKGV0001+; always null in corpus
    std::array<float, 3>      controlpoint { 0.0f, 0.0f, 0.0f }; // PKGV0019+
    FieldBindings             field_bindings;

    VisibleUserBinding visible_user;
    std::string        visible_user_key;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Initializer, name, max, min);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Emitter, name, distancemax, distancemin, rate, directions);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Particle, initializers, operators, emitters);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ParticleObject, name, origin, angles, scale, visible, particle,
                                   particleObj);
} // namespace wpscene
} // namespace sr
