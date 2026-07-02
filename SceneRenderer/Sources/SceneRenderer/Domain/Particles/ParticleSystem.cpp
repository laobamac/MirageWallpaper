module;

#include <rstd/macro.hpp>

module sr.scene;
import sr.core;
import rstd.cppstd;

using namespace sr;

void ParticleInstance::Refresh() {
    SetDeath(false);
    SetNoLiveParticle(false);
    GetBoundedData() = {};
    ParticlesVec().clear();
    TrailsVec().clear();
}

bool ParticleInstance::IsDeath() const { return m_is_death; }
void ParticleInstance::SetDeath(bool v) { m_is_death = v; };

bool ParticleInstance::IsNoLiveParticle() const { return m_no_live_particle; };
void ParticleInstance::SetNoLiveParticle(bool v) { m_no_live_particle = v; };

std::span<const Particle> ParticleInstance::Particles() const { return m_particles; };
std::vector<Particle>&    ParticleInstance::ParticlesVec() { return m_particles; };

std::span<const ParticleTrail> ParticleInstance::Trails() const { return m_trails; };
std::vector<ParticleTrail>&    ParticleInstance::TrailsVec() { return m_trails; };

ParticleInstance::BoundedData& ParticleInstance::GetBoundedData() { return m_bounded_data; }

ParticleSubSystem::ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm,
                                     uint32_t maxcount, double rate, u32 maxcount_instance,
                                     double probability, SpawnType type,
                                     ParticleRawGenSpecOp specOp,
                                     ParticleFollowAnchor follow_anchor, u32 trail_length,
                                     double start_time)
    : m_sys(p),
      m_mesh(sm),
      m_maxcount(maxcount),
      m_rate(rate),
      m_genSpecOp(specOp),
      m_follow_anchor(follow_anchor),
      m_time(0),
      m_start_time(start_time),
      m_maxcount_instance(maxcount_instance),
      m_probability(probability),
      m_spawn_type(type),
      m_trail_length(trail_length) {};

ParticleSubSystem::~ParticleSubSystem() = default;

void ParticleSubSystem::AddEmitter(ParticleEmittOp&& em) { m_emiters.emplace_back(em); }

void ParticleSubSystem::AddInitializer(ParticleInitOp&& ini) { m_initializers.emplace_back(ini); }

void ParticleSubSystem::AddOperator(ParticleOperatorOp&& op) { m_operators.emplace_back(op); }

std::span<const ParticleControlpoint> ParticleSubSystem::Controlpoints() const {
    return m_controlpoints;
}
std::span<ParticleControlpoint> ParticleSubSystem::Controlpoints() { return m_controlpoints; };

ParticleSubSystem::SpawnType ParticleSubSystem::Type() const { return m_spawn_type; }

u32 ParticleSubSystem::MaxInstanceCount() const { return m_maxcount_instance; };

Eigen::Vector3f ParticleSubSystem::FollowPosition(const Particle& p) const {
    Eigen::Vector3f pos = ParticleModify::GetPos(p);
    if (! m_follow_anchor.trail_renderer) return pos;

    Eigen::Vector3f velocity = ParticleModify::GetVelocity(p);
    float           speed    = velocity.norm();
    if (speed <= 1e-6f) return pos;

    float trail_len =
        std::max(0.0f, std::min(speed * m_follow_anchor.length, m_follow_anchor.max_length));
    if (trail_len <= 0.0f) return pos;

    float visual_half_len = (p.size * 0.5f) * m_follow_anchor.texture_ratio * trail_len * 0.5f;
    return pos + velocity.normalized() * visual_half_len;
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    m_children.emplace_back(std::move(child));
}

ParticleInstance* ParticleSubSystem::QueryNewInstance() {
    if (Random::get(0.0, 1.0) <= m_probability) {
        for (auto& inst : m_instances) {
            if (inst->IsDeath() && inst->IsNoLiveParticle()) {
                inst->Refresh();
                return inst.get();
            }
        }
        if (m_instances.size() < m_maxcount_instance) {
            m_instances.emplace_back(std::make_unique<ParticleInstance>());
            return m_instances.back().get();
        }
    }
    return nullptr;
}

void ParticleSubSystem::Emitt() { Tick(m_sys.scene.frameTime, true); }

void ParticleSubSystem::Tick(double frame_time, bool update_mesh) {
    if (! m_started) {
        m_started = true;
        Warmup();
    }
    Advance(frame_time, update_mesh);
}

void ParticleSubSystem::Warmup() {
    if (m_start_time <= 0.0) return;

    constexpr double kTargetWarmupFrameTime = 1.0 / 60.0;
    constexpr u32    kMaxWarmupFrames       = 240;
    u32              frame_count =
        std::max(1u, static_cast<u32>(std::ceil(m_start_time / kTargetWarmupFrameTime)));
    frame_count       = std::min(frame_count, kMaxWarmupFrames);
    double frame_time = m_start_time / static_cast<double>(frame_count);
    for (u32 i = 0; i < frame_count; ++i) {
        Advance(frame_time, false);
    }
}

void ParticleSubSystem::Advance(double frame_time, bool update_mesh) {
    double particleTime = frame_time * m_rate;
    m_time += particleTime;

    // Cursor in canvas-absolute world coords. The "global" camera lives at
    // (ortho_w/2, ortho_h/2) with width=ortho_w (SceneCamera::Ortho uses
    // [-w/2, +w/2]), so the visible world spans [0, ortho_w] x [0, ortho_h]
    // with Y pointing up. Pointer is [0..1] with Y down -> flip Y.
    const auto            pointer = m_sys.scene.pointerPosition;
    const Eigen::Vector3d mouse_world {
        static_cast<double>(pointer[0]) * static_cast<double>(m_sys.scene.ortho[0]),
        (1.0 - static_cast<double>(pointer[1])) * static_cast<double>(m_sys.scene.ortho[1]),
        0.0,
    };
    // Particle a_Position is consumed by the vertex shader in local space and
    // then transformed by g_ModelMatrix (the owner node's world transform).
    // To make a link_mouse cp track the cursor in world, push the cursor
    // through the owner node's inverse model first.
    Eigen::Vector3d mouse_local          = mouse_world;
    Eigen::Matrix3d world_from_local_dir = Eigen::Matrix3d::Identity();
    Eigen::Matrix3d local_from_world_dir = Eigen::Matrix3d::Identity();
    if (auto* node = m_owner_node) {
        node->UpdateTrans();
        world_from_local_dir = node->ModelTrans().block<3, 3>(0, 0);
        if (std::abs(world_from_local_dir.determinant()) > 1e-9)
            local_from_world_dir = world_from_local_dir.inverse();

        Eigen::Matrix4d m_inv = node->ModelTrans().inverse();
        Eigen::Vector4d v     = m_inv * Eigen::Vector4d(mouse_world.x(), mouse_world.y(), 0.0, 1.0);
        mouse_local           = v.head<3>();
    }
    for (auto& cp : m_controlpoints) {
        if (cp.link_mouse) cp.offset = cp.base_offset + mouse_local;
    }

    std::array<float, 16> audio_average {};
    for (std::size_t i = 0; i < audio_average.size(); ++i) {
        audio_average[i] = m_sys.scene.audioAverage[i].load(std::memory_order_relaxed);
    }

    if (m_spawn_type == SpawnType::STATIC) {
        if (m_instances.empty()) m_instances.emplace_back(std::make_unique<ParticleInstance>());
    }

    auto spawn_inst = [this](ParticleInstance&  inst,
                             ParticleSubSystem& child,
                             isize              idx,
                             Eigen::Vector3f    pos       = Eigen::Vector3f::Zero(),
                             bool               fixed_pos = false) {
        if (! fixed_pos && idx >= 0 && static_cast<usize>(idx) < inst.Particles().size()) {
            pos = FollowPosition(inst.Particles()[static_cast<usize>(idx)]);
        }
        ParticleInstance* n_inst = child.QueryNewInstance();
        if (n_inst != nullptr) {
            n_inst->GetBoundedData() = {
                .parent           = &inst,
                .parent_subsystem = this,
                .particle_idx     = fixed_pos ? -1 : idx,
                .pos              = pos,
            };
        }
    };

    for (auto& inst : m_instances) {
        rstd_assert(inst);

        auto& bounded_data = inst->GetBoundedData();

        bool type_has_death =
            m_spawn_type == SpawnType::EVENT_SPAWN || m_spawn_type == SpawnType::EVENT_FOLLOW;

        // bouded data and death
        if (bounded_data.parent != nullptr) {
            std::span particles = bounded_data.parent->Particles();
            if (bounded_data.particle_idx != -1 && bounded_data.particle_idx < particles.size()) {
                auto& p          = particles[bounded_data.particle_idx];
                bounded_data.pos = bounded_data.parent_subsystem
                                       ? bounded_data.parent_subsystem->FollowPosition(p)
                                       : ParticleModify::GetPos(p);
                // only update pos once when event_death
                if (m_spawn_type == SpawnType::EVENT_DEATH) bounded_data.particle_idx = -1;

                // death if bounded particle death
                if (! inst->IsDeath() && type_has_death) {
                    bool cur_life_ok = ParticleModify::LifetimeOk(p);
                    inst->SetDeath(! cur_life_ok && bounded_data.pre_lifetime_ok);
                    bounded_data.pre_lifetime_ok = cur_life_ok;
                }
            }

            // death if parent death
            if (! inst->IsDeath() && type_has_death) {
                inst->SetDeath(bounded_data.parent->IsDeath());
            }
        }

        // clear when death if follow
        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW) {
            inst->ParticlesVec().clear();
        }

        if (! inst->IsDeath()) {
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(),
                        m_initializers,
                        m_maxcount,
                        particleTime,
                        std::span<const float> { audio_average.data(), audio_average.size() },
                        std::span<const ParticleControlpoint> { m_controlpoints });
            }
        }

        // event_death is always death after emitop
        if (m_spawn_type == SpawnType::EVENT_DEATH) inst->SetDeath(true);

        ParticleInfo info {
            .particles            = inst->ParticlesVec(),
            .controlpoints        = m_controlpoints,
            .world_from_local_dir = world_from_local_dir,
            .local_from_world_dir = local_from_world_dir,
            .time                 = m_time,
            .time_pass            = particleTime,
        };

        bool  has_live = false;
        isize i        = -1;
        // Keep the trail buffer parallel to the particle slot count, and
        // reset the trail any time a slot transitions to a fresh particle.
        if (m_trail_length > 0) {
            auto& trails = inst->TrailsVec();
            if (trails.size() < info.particles.size()) {
                trails.resize(info.particles.size());
            }
            for (auto& t : trails) {
                if (t.positions.size() != m_trail_length)
                    t.positions.assign(m_trail_length, Eigen::Vector3f::Zero());
            }
        }
        for (auto& p : info.particles) {
            i++;

            if (ParticleModify::IsNew(p)) {
                // new spawn
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_FOLLOW ||
                        child->Type() == SpawnType::EVENT_SPAWN)
                        spawn_inst(*inst, *child, i);
                }
                if (m_trail_length > 0) inst->TrailsVec()[i].Reset();
            }

            ParticleModify::MarkOld(p);
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }
            ParticleModify::Reset(p);
            ParticleModify::ChangeLifetime(p, -particleTime);

            if (! ParticleModify::LifetimeOk(p)) {
                // new dead
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_DEATH)
                        spawn_inst(*inst, *child, i, FollowPosition(p), true);
                }
            } else {
                has_live = true;
            }
        }

        inst->SetNoLiveParticle(! has_live);

        std::for_each(m_operators.begin(), m_operators.end(), [&info](ParticleOperatorOp& op) {
            op(info);
        });

        // Symplectic Euler position integration: every force op above has
        // updated `velocity` based on the start-of-frame position. Apply the
        // position step once with the final velocity so attract/drag/gravity
        // composition stays energy-stable across frames regardless of the
        // order operators appear in the source JSON.
        for (auto& p : info.particles) {
            if (! ParticleModify::LifetimeOk(p)) continue;
            ParticleModify::MoveByTime(p, info.time_pass);
            ParticleModify::RotateByTime(p, info.time_pass);
        }

        if (m_trail_length > 0) {
            // A link_mouse controlpoint drives the rope as a cursor trail:
            // the per-particle position is the spawn cursor and never moves
            // (no velocity initializer, drag=0), so sampling p.position
            // collapses the trail to a single point and the GS divides by
            // zero -> NaN. Record the cp's current world-local offset so
            // every frame contributes a fresh cursor sample.
            Eigen::Vector3f trail_sample;
            bool            use_cursor_trail = false;
            for (auto const& cp : m_controlpoints) {
                if (cp.link_mouse) {
                    trail_sample     = cp.offset.cast<float>();
                    use_cursor_trail = true;
                    break;
                }
            }
            auto& trails = inst->TrailsVec();
            for (usize si = 0; si < info.particles.size(); si++) {
                auto& p = info.particles[si];
                if (! ParticleModify::LifetimeOk(p)) continue;
                trails[si].Push(use_cursor_trail ? trail_sample : Eigen::Vector3f { p.position });
            }
        }
    }

    if (update_mesh) {
        m_mesh->SetDirty();
        m_sys.gener->GenGLData(m_instances, *m_mesh, m_genSpecOp);
    }

    for (auto& child : m_children) {
        child->Tick(frame_time, update_mesh);
    }
}

void ParticleSystem::Emitt() {
    for (auto& el : subsystems) {
        el->Emitt();
    }
}
