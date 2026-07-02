module;

export module sr.pkg.parse:wp_particle_parser;
import nlohmann.json;
import rstd.cppstd;
import sr.scene;
import sr.fs;

export import sr.pkg.scene_obj;

export namespace sr

{
class ParticleProgramCompiler {
public:
    static ParticleInitOp genParticleInitOp(const nlohmann::json&);
    static ParticleOperatorOp
    genParticleOperatorOp(const nlohmann::json&,
                          std::shared_ptr<const wpscene::ParticleInstanceoverride>);
    static ParticleEmittOp genParticleEmittOp(const wpscene::Emitter&, bool sort = false);
    static ParticleInitOp
        genOverrideInitOp(std::shared_ptr<const wpscene::ParticleInstanceoverride>);
};
} // namespace sr
