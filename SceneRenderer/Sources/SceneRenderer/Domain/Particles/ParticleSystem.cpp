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
                                     double start_time, bool world_space)
    : m_sys(p),
      m_mesh(sm),
      m_maxcount(maxcount),
      m_rate(rate),
      m_genSpecOp(specOp),
      m_follow_anchor(follow_anchor),
      m_time(0),
      m_start_time(start_time),
      m_world_space(world_space),
      m_maxcount_instance(maxcount_instance),
      m_probability(probability),
      m_spawn_type(type),
      m_trail_length(trail_length) {
    m_instances.reserve(m_maxcount_instance);
}

ParticleSubSystem::~ParticleSubSystem() = default;

u32 ParticleSubSystem::AcquireParticleSlotId() {
    if (! m_free_particle_slot_ids.empty()) {
        const u32 id = m_free_particle_slot_ids.back();
        m_free_particle_slot_ids.pop_back();
        return id;
    }
    return m_next_particle_slot_id++;
}

void ParticleSubSystem::ReleaseParticleSlotId(Particle& particle) {
    if (particle.slot_id != std::numeric_limits<u32>::max()) {
        m_free_particle_slot_ids.push_back(particle.slot_id);
        particle.slot_id = std::numeric_limits<u32>::max();
    }
}

void ParticleSubSystem::RebindOrKillChildParticles(ParticleInstance& parent, isize old_index,
                                                    isize new_index) {
    for (auto& child : m_children) {
        for (auto& child_instance : child->m_instances) {
            auto& bound = child_instance->GetBoundedData();
            if (bound.parent != &parent || bound.parent_subsystem != this ||
                bound.particle_idx != old_index)
                continue;

            if (new_index >= 0) {
                bound.particle_idx = new_index;
                continue;
            }

            // eventfollow/eventspawn children are attached to this exact
            // parent particle. Compacting a dead parent must preserve the
            // same death transition the old sparse vector delivered.
            if (child->Type() == SpawnType::EVENT_FOLLOW ||
                child->Type() == SpawnType::EVENT_SPAWN)
                child_instance->SetDeath(true);
            bound.particle_idx = -1;
        }
    }
}

void ParticleSubSystem::CompactInstance(ParticleInstance& instance) {
    auto& particles = instance.ParticlesVec();
    auto& trails    = instance.TrailsVec();
    usize write     = 0;

    for (usize read = 0; read < particles.size(); ++read) {
        if (! ParticleModify::LifetimeOk(particles[read])) {
            RebindOrKillChildParticles(instance, static_cast<isize>(read), -1);
            ReleaseParticleSlotId(particles[read]);
            continue;
        }

        if (write != read) {
            particles[write] = std::move(particles[read]);
            if (read < trails.size()) {
                if (write >= trails.size()) trails.resize(write + 1);
                trails[write] = std::move(trails[read]);
            }
            RebindOrKillChildParticles(instance,
                                       static_cast<isize>(read),
                                       static_cast<isize>(write));
        }
        ++write;
    }

    particles.resize(write);
    if (trails.size() > write) trails.resize(write);
}

void ParticleSubSystem::ClearInstanceParticles(ParticleInstance& instance) {
    auto& particles = instance.ParticlesVec();
    for (usize i = 0; i < particles.size(); ++i) {
        RebindOrKillChildParticles(instance, static_cast<isize>(i), -1);
        ReleaseParticleSlotId(particles[i]);
    }
    particles.clear();
    instance.TrailsVec().clear();
}

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

std::unique_ptr<ParticleInstance> ParticleSubSystem::MakeInstance() {
    auto instance = std::make_unique<ParticleInstance>();
    // Each instance has a fixed authored upper bound. Reserve it once so a
    // high-rate emitter never reallocates/moves live particles (or trails)
    // while it is running.
    instance->ParticlesVec().reserve(m_maxcount);
    if (m_trail_length > 0) instance->TrailsVec().reserve(m_maxcount);
    return instance;
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
            m_instances.emplace_back(MakeInstance());
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
    // WE's particle instance `rate` is the simulation-rate factor, while
    // `count` is the emission-rate factor. Advance every time-based part of
    // the subsystem with the same scaled delta so emission, lifetime,
    // operators and sprite animation stay synchronized.
    double simulation_time = frame_time * m_rate;
    m_time += simulation_time;

    // Most particle systems neither consume the cursor nor transform a
    // world-space direction. Keep those systems out of all matrix work.
    Eigen::Matrix3d world_from_local_dir = Eigen::Matrix3d::Identity();
    Eigen::Matrix3d local_from_world_dir = Eigen::Matrix3d::Identity();
    if (m_uses_mouse_controlpoint || m_needs_direction_transform) {
        // Cursor in canvas-absolute world coords. The "global" camera lives
        // at (ortho_w/2, ortho_h/2) with width=ortho_w (SceneCamera::Ortho
        // uses [-w/2, +w/2]), so the visible world spans [0, ortho_w] x
        // [0, ortho_h] with Y pointing up. Pointer is [0..1] with Y down.
        const auto            pointer = m_sys.scene.pointerPosition;
        const Eigen::Vector3d mouse_world {
            static_cast<double>(pointer[0]) * static_cast<double>(m_sys.scene.ortho[0]),
            (1.0 - static_cast<double>(pointer[1])) * static_cast<double>(m_sys.scene.ortho[1]),
            0.0,
        };
        Eigen::Vector3d mouse_local = mouse_world;
        // Particle a_Position is consumed in local space before
        // g_ModelMatrix. Convert a linked cursor to that same space.
        if (auto* node = m_owner_node; node != nullptr) {
            node->UpdateTrans();
            world_from_local_dir = node->ModelTrans().block<3, 3>(0, 0);
            if (std::abs(world_from_local_dir.determinant()) > 1e-9)
                local_from_world_dir = world_from_local_dir.inverse();

            if (m_uses_mouse_controlpoint) {
                Eigen::Matrix4d m_inv = node->ModelTrans().inverse();
                Eigen::Vector4d v =
                    m_inv * Eigen::Vector4d(mouse_world.x(), mouse_world.y(), 0.0, 1.0);
                mouse_local = v.head<3>();
            }
        }
        if (m_uses_mouse_controlpoint) {
            for (auto& cp : m_controlpoints) {
                if (cp.link_mouse) cp.offset = cp.base_offset + mouse_local;
            }
        }
    }

    std::array<float, 16> audio_average {};
    std::span<const float> audio_signal {};
    if (m_uses_audio_response) {
        for (std::size_t i = 0; i < audio_average.size(); ++i)
            audio_average[i] = m_sys.scene.audioAverage[i].load(std::memory_order_relaxed);
        audio_signal = std::span<const float> { audio_average.data(), audio_average.size() };
    }

    if (m_spawn_type == SpawnType::STATIC) {
        if (m_instances.empty()) m_instances.emplace_back(MakeInstance());
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
        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW)
            ClearInstanceParticles(*inst);

        if (! inst->IsDeath()) {
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(),
                        m_initializers,
                        m_maxcount,
                        simulation_time,
                        audio_signal,
                        std::span<const ParticleControlpoint> { m_controlpoints });
            }
        }

        // Emitters create a fresh Particle value when reusing/growing a
        // slot. Give it a subsystem-stable id before any stateful operator
        // runs so compaction cannot change its random/oscillator state.
        for (auto& p : inst->ParticlesVec()) {
            if (ParticleModify::IsNew(p) && p.slot_id == std::numeric_limits<u32>::max())
                p.slot_id = AcquireParticleSlotId();
        }

        // event_death is always death after emitop
        if (m_spawn_type == SpawnType::EVENT_DEATH) inst->SetDeath(true);

        ParticleInfo info {
            .particles            = inst->ParticlesVec(),
            .controlpoints        = m_controlpoints,
            .world_from_local_dir = world_from_local_dir,
            .local_from_world_dir = local_from_world_dir,
            .world_space          = m_world_space,
            .time                 = m_time,
            .time_pass            = simulation_time,
        };

        bool  has_live = false;
        isize i        = -1;
        // Keep the trail buffer parallel to the particle slot count, and
        // reset the trail any time a slot transitions to a fresh particle.
        if (m_trail_length > 0) {
            auto& trails = inst->TrailsVec();
            if (trails.size() < info.particles.size()) {
                const usize old_size = trails.size();
                trails.resize(info.particles.size());
                for (usize ti = old_size; ti < trails.size(); ++ti)
                    trails[ti].positions.assign(m_trail_length, Eigen::Vector3f::Zero());
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
            ParticleModify::ChangeLifetime(p, -simulation_time);

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
            if (m_uses_mouse_controlpoint) {
                for (auto const& cp : m_controlpoints) {
                    if (cp.link_mouse) {
                        trail_sample     = cp.offset.cast<float>();
                        use_cursor_trail = true;
                        break;
                    }
                }
            }
            auto& trails = inst->TrailsVec();
            for (usize si = 0; si < info.particles.size(); si++) {
                auto& p = info.particles[si];
                if (! ParticleModify::LifetimeOk(p)) continue;
                trails[si].Push(use_cursor_trail ? trail_sample : Eigen::Vector3f { p.position });
            }
        }

        // Keep only live particles between frames. Operators and geometry
        // generation therefore scale with the current live set, not the
        // largest historical emission burst. Child bindings and trail rings
        // are remapped in CompactInstance before child simulation starts.
        CompactInstance(*inst);
        inst->SetNoLiveParticle(! has_live);
    }

    if (update_mesh) {
        bool has_live_geometry = false;
        for (const auto& inst : m_instances) {
            if (! inst->IsNoLiveParticle()) {
                has_live_geometry = true;
                break;
            }
        }
        // Visibility must not change simulation time (hidden WE layers may
        // later be shown again), but no draw can consume their vertex stream.
        // Defer geometry generation/upload until the node is visible again.
        const bool node_visible = m_owner_node == nullptr || m_owner_node->Visible();

        // Emit one empty update when the last particle disappears, then keep
        // the mesh untouched until a later emitter actually creates output.
        if (node_visible && (has_live_geometry || m_mesh_has_geometry)) {
            m_mesh->SetDirty();
            m_sys.gener->GenGLData(m_instances, *m_mesh, m_genSpecOp);
            m_mesh_has_geometry = has_live_geometry;
        }
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
