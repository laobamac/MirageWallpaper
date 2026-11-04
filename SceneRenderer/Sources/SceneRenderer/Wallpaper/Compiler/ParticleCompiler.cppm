module;
#include <optional>

export module sr.pkg.parse:wp_particle_parser;
import rstd.cppstd;
import sr.json;
import sr.scene;
import sr.fs;

export import sr.pkg.scene_obj;

export namespace sr

{
class ParticleInstanceModifiers {
public:
    ParticleInstanceModifiers() = default;
    ParticleInstanceModifiers(std::shared_ptr<wpscene::ParticleInstanceoverride> state,
                              wpscene::Particle::EFlags flags, bool controlpoints)
        : m_state(std::move(state)), m_flags(flags), m_controlpoints(controlpoints) {}
    ParticleInstanceModifiers Clone() const { return *this; }
    bool Enabled() const noexcept { return m_state && m_state->enabled; }
    float Alpha() const noexcept { return m_state ? m_state->alpha : 1.0f; }
    float Count() const noexcept {
        return !m_state || m_flags[wpscene::Particle::FlagEnum::disable_count_override]
                   ? 1.0f
                   : m_state->count;
    }
    float Lifetime() const noexcept {
        return !m_state || m_flags[wpscene::Particle::FlagEnum::disable_lifetime_override]
                   ? 1.0f
                   : m_state->lifetime;
    }
    float Rate() const noexcept { return m_state ? m_state->rate : 1.0f; }
    float Size() const noexcept {
        return !m_state || m_flags[wpscene::Particle::FlagEnum::disable_size_override]
                   ? 1.0f
                   : m_state->size;
    }
    float Speed() const noexcept {
        return !m_state || m_flags[wpscene::Particle::FlagEnum::disable_speed_override]
                   ? 1.0f
                   : m_state->speed;
    }
    bool HasColorOverride() const noexcept {
        return m_state && !m_flags[wpscene::Particle::FlagEnum::disable_color_override] &&
               (m_state->overColor || m_state->overColorn);
    }
    bool UsesLegacyColor() const noexcept { return m_state && m_state->overColor; }
    const std::array<float, 3>& Color() const noexcept {
        static const std::array<float, 3> kWhite { 1.0f, 1.0f, 1.0f };
        return m_state ? (m_state->overColor ? m_state->color : m_state->colorn) : kWhite;
    }
    bool ControlpointsEnabled() const noexcept { return m_state && m_controlpoints; }
    const std::optional<std::array<float, 3>>& Controlpoint(std::size_t index) const noexcept {
        return m_state->controlpoint[index];
    }
    const std::array<float, 3>& ControlpointAngle(std::size_t index) const noexcept {
        return m_state->controlpointangle[index];
    }
private:
    std::shared_ptr<wpscene::ParticleInstanceoverride> m_state;
    wpscene::Particle::EFlags                         m_flags { 0 };
    bool                                              m_controlpoints { false };
};

class WPParticleParser {
public:
    static ParticleInitOp genParticleInitOp(const Json&);
    static ParticleOperatorOp
    genParticleOperatorOp(const Json&, ParticleInstanceModifiers);
    static ParticleEmittOp genParticleEmittOp(const wpscene::Emitter&, bool sort = false);
    static ParticleInitOp
        genOverrideInitOp(ParticleInstanceModifiers);
};
} // namespace sr
