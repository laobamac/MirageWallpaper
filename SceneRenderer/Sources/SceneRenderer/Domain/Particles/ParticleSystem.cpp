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
    m_emitter_states.clear();
    m_warmup_pending = true;
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
                                     double trail_duration, double start_time, bool world_space)
    : m_sys(p),
      m_mesh(sm),
      m_maxcount(maxcount),
      m_rate(rate),
      m_genSpecOp(specOp),
      m_follow_anchor(follow_anchor),
      m_time(0),
      m_start_time(start_time),
      m_world_space(world_space),
      m_maxcount_instance(EffectiveInstanceCapacity(maxcount_instance, type)),
      m_probability(probability),
      m_spawn_type(type),
      m_trail_length(trail_length),
      m_trail_sample_interval(trail_length == 0
                                  ? 0.0
                                  : trail_duration / static_cast<double>(trail_length)) {
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
                child->Type() == SpawnType::EVENT_SPAWN ||
                child->Type() == SpawnType::STATIC_CONTROLPOINT)
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
void ParticleSubSystem::AddContextInitializer(ParticleContextInitOp&& ini) {
    m_context_initializers.emplace_back(std::move(ini));
}

void ParticleSubSystem::AddOperator(ParticleOperatorOp&& op) { m_operators.emplace_back(op); }

std::span<const ParticleControlpoint> ParticleSubSystem::Controlpoints() const {
    return m_controlpoints;
}
std::span<ParticleControlpoint> ParticleSubSystem::Controlpoints() { return m_controlpoints; };

ParticleSubSystem::SpawnType ParticleSubSystem::Type() const { return m_spawn_type; }

u32 ParticleSubSystem::MaxInstanceCount() const { return m_maxcount_instance; };

Eigen::Vector3f ParticleSubSystem::RenderPosition(const ParticleInstance& instance,
                                                  const Particle& p) const {
    if (m_world_space) return ParticleModify::GetPos(p);
    return instance.GetBoundedData().pos + ParticleModify::GetPos(p);
}

Eigen::Vector3f ParticleSubSystem::OwnerLocalToWorld(const Eigen::Vector3f& position) const {
    if (m_owner_node == nullptr) return position;
    m_owner_node->UpdateTrans();
    const Eigen::Vector4d world =
        m_owner_node->ModelTrans() * Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0);
    return world.head<3>().cast<float>();
}

Eigen::Vector3f ParticleSubSystem::OwnerWorldToLocal(const Eigen::Vector3f& position) const {
    if (m_owner_node == nullptr) return position;
    m_owner_node->UpdateTrans();
    const Eigen::Vector4d local = m_owner_node->ModelTrans().inverse() *
                                  Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0);
    return local.head<3>().cast<float>();
}

Eigen::Vector3f ParticleSubSystem::FollowWorldPosition(const ParticleInstance& instance,
                                                       const Particle& p) const {
    Eigen::Vector3f pos = RenderPosition(instance, p);
    auto to_world = [this](const Eigen::Vector3f& value) {
        return m_world_space ? value : OwnerLocalToWorld(value);
    };
    if (! m_follow_anchor.trail_renderer) return to_world(pos);

    Eigen::Vector3f velocity = ParticleModify::GetVelocity(p);
    float           speed    = velocity.norm();
    if (speed <= 1e-6f) return to_world(pos);

    float trail_len =
        std::max(0.0f, std::min(speed * m_follow_anchor.length, m_follow_anchor.max_length));
    if (trail_len <= 0.0f) return to_world(pos);

    float visual_half_len = (p.size * 0.5f) * m_follow_anchor.texture_ratio * trail_len * 0.5f;
    pos += velocity.normalized() * visual_half_len;
    return to_world(pos);
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    m_children.emplace_back(std::move(child));
}

std::unique_ptr<ParticleInstance> ParticleSubSystem::MakeInstance() {
    auto instance = std::make_unique<ParticleInstance>();
    instance->SetPositionsInWorldSpace(m_world_space);
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

bool ParticleSubSystem::SyncPlayback() {
    if (! m_playback_state) return false;
    const u32 sequence = m_playback_state->reset_sequence.load(std::memory_order_acquire);
    if (sequence == m_seen_reset_sequence) return false;
    m_seen_reset_sequence          = sequence;
    m_time                         = 0.0;
    m_trail_sample_accumulator     = 0.0;
    m_next_particle_slot_id        = 0;
    m_next_spawn_sequence          = 0;
    m_free_particle_slot_ids.clear();
    for (auto& instance : m_instances) {
        instance->Refresh();
        if (m_spawn_type != SpawnType::STATIC) {
            instance->SetDeath(true);
            instance->SetNoLiveParticle(true);
        }
    }
    return true;
}

void ParticleSubSystem::Tick(double frame_time, bool update_mesh) {
    const bool reset = SyncPlayback();
    if (m_playback_state && ! m_playback_state->playing.load(std::memory_order_acquire)) {
        if (reset && update_mesh && m_mesh_has_geometry) {
            m_mesh->SetDirty();
            m_sys.gener->GenGLData(
                m_instances, *m_mesh, m_genSpecOp, m_rope_sequence_count);
            m_mesh_has_geometry = false;
        }
        for (auto& child : m_children) child->Tick(0.0, update_mesh);
        return;
    }
    Advance(frame_time, update_mesh);
}

void ParticleSubSystem::Warmup(ParticleInstance& instance, double current_time,
                               std::span<const float> audio_signal,
                               const Eigen::Matrix3d& world_from_local_dir,
                               const Eigen::Matrix3d& local_from_world_dir,
                               const Eigen::Matrix4d& world_from_spawn_space) {
    if (! instance.WarmupPending()) return;
    instance.FinishWarmup();
    if (m_start_time <= 0.0) return;

    constexpr double kTargetWarmupFrameTime = 1.0 / 60.0;
    constexpr u32    kMaxWarmupFrames       = 240;
    u32 frame_count = std::max(
        1u, static_cast<u32>(std::ceil(m_start_time / kTargetWarmupFrameTime)));
    frame_count       = std::min(frame_count, kMaxWarmupFrames);
    const double step = m_start_time / static_cast<double>(frame_count);
    const double saved_time = m_time;
    const double start      = std::max(0.0, current_time - m_start_time);
    double       trail_accumulator = 0.0;
    for (u32 frame = 0; frame < frame_count; ++frame) {
        m_time = start + step * static_cast<double>(frame + 1);
        usize  trail_steps = 0;
        double remainder   = 0.0;
        if (m_trail_length > 0) {
            if (m_trail_sample_interval > 0.0) {
                const double elapsed = trail_accumulator + step;
                trail_steps = std::min<usize>(
                    static_cast<usize>(std::floor(elapsed / m_trail_sample_interval)),
                    m_trail_length);
                remainder = std::fmod(elapsed, m_trail_sample_interval);
                trail_accumulator = remainder;
            } else {
                trail_steps = 1;
            }
        }
        SimulateInstance(instance,
                         step,
                         audio_signal,
                         world_from_local_dir,
                         local_from_world_dir,
                         world_from_spawn_space,
                         trail_steps,
                         remainder);
    }
    m_time = saved_time;
}

void ParticleSubSystem::SimulateInstance(
    ParticleInstance& inst, double simulation_time, std::span<const float> audio_signal,
    const Eigen::Matrix3d& world_from_local_dir, const Eigen::Matrix3d& local_from_world_dir,
    const Eigen::Matrix4d& world_from_spawn_space, usize trail_sample_steps,
    double trail_sample_remainder) {
    auto& bounded_data = inst.GetBoundedData();
    const bool type_has_death = m_spawn_type == SpawnType::EVENT_SPAWN ||
                                m_spawn_type == SpawnType::EVENT_FOLLOW ||
                                m_spawn_type == SpawnType::STATIC_CONTROLPOINT;

    if (bounded_data.parent != nullptr) {
        const auto particles = bounded_data.parent->Particles();
        if (bounded_data.particle_idx >= 0 &&
            static_cast<usize>(bounded_data.particle_idx) < particles.size()) {
            const auto& p = particles[static_cast<usize>(bounded_data.particle_idx)];
            if (m_spawn_type != SpawnType::STATIC_CONTROLPOINT) {
                const Eigen::Vector3f world = bounded_data.parent_subsystem
                                                  ? bounded_data.parent_subsystem
                                                        ->FollowWorldPosition(*bounded_data.parent, p)
                                                  : ParticleModify::GetPos(p);
                bounded_data.pos = OwnerWorldToLocal(world);
            }
            if (m_spawn_type == SpawnType::EVENT_DEATH) bounded_data.particle_idx = -1;
            if (! inst.IsDeath() && type_has_death) {
                const bool alive = ParticleModify::LifetimeOk(p);
                inst.SetDeath(! alive && bounded_data.pre_lifetime_ok);
                bounded_data.pre_lifetime_ok = alive;
            }
        }
        if (! inst.IsDeath() && type_has_death) inst.SetDeath(bounded_data.parent->IsDeath());
    }

    if (inst.IsDeath() && (m_spawn_type == SpawnType::EVENT_FOLLOW ||
                           m_spawn_type == SpawnType::STATIC_CONTROLPOINT))
        ClearInstanceParticles(inst);

    if (m_spawn_type == SpawnType::STATIC_CONTROLPOINT &&
        m_parent_controlpoint_start_index.has_value() && bounded_data.parent != nullptr &&
        bounded_data.parent_subsystem != nullptr) {
        std::vector<usize> ordered;
        const auto parent_particles = bounded_data.parent->Particles();
        ordered.reserve(parent_particles.size());
        for (usize index = 0; index < parent_particles.size(); ++index)
            if (ParticleModify::LifetimeOk(parent_particles[index])) ordered.push_back(index);
        std::sort(ordered.begin(), ordered.end(), [&](usize lhs, usize rhs) {
            return parent_particles[lhs].spawn_sequence < parent_particles[rhs].spawn_sequence;
        });
        usize cp_index = static_cast<usize>(std::max(*m_parent_controlpoint_start_index, 0));
        for (const usize particle_index : ordered) {
            if (cp_index >= m_controlpoints.size()) break;
            const auto world = bounded_data.parent_subsystem->FollowWorldPosition(
                *bounded_data.parent, parent_particles[particle_index]);
            m_controlpoints[cp_index].offset =
                (OwnerWorldToLocal(world) - bounded_data.pos).cast<double>();
            ++cp_index;
        }
    }

    if (! inst.IsDeath()) {
        for (usize emitter_index = 0; emitter_index < m_emiters.size(); ++emitter_index) {
            m_emiters[emitter_index](inst.EmitterState(emitter_index),
                                     inst.ParticlesVec(),
                                     m_initializers,
                                     m_maxcount,
                                     simulation_time,
                                     audio_signal,
                                     std::span<const ParticleControlpoint> { m_controlpoints });
        }
    }

    auto spawn_child = [this](ParticleInstance& parent, ParticleSubSystem& child, isize index,
                              Eigen::Vector3f world_position = Eigen::Vector3f::Zero(),
                              bool fixed = false) {
        if (child.Type() == SpawnType::STATIC_CONTROLPOINT) {
            world_position = OwnerLocalToWorld(parent.GetBoundedData().pos);
        } else if (! fixed && index >= 0 && static_cast<usize>(index) < parent.Particles().size()) {
            world_position = FollowWorldPosition(parent, parent.Particles()[static_cast<usize>(index)]);
        }
        auto* child_instance = child.QueryNewInstance();
        if (child_instance == nullptr) return;
        child_instance->GetBoundedData() = {
            .parent           = &parent,
            .parent_subsystem = this,
            .particle_idx     = fixed ? -1 : index,
            .pos              = child.OwnerWorldToLocal(world_position),
        };
    };

    if (m_trail_length > 0) {
        auto& trails = inst.TrailsVec();
        if (trails.size() < inst.Particles().size()) {
            const usize old_size = trails.size();
            trails.resize(inst.Particles().size());
            for (usize index = old_size; index < trails.size(); ++index)
                trails[index].positions.assign(m_trail_length, Eigen::Vector3f::Zero());
        }
    }

    for (usize index = 0; index < inst.ParticlesVec().size(); ++index) {
        auto& p = inst.ParticlesVec()[index];
        if (ParticleModify::IsNew(p)) {
            if (p.slot_id == std::numeric_limits<u32>::max()) p.slot_id = AcquireParticleSlotId();
            p.spawn_sequence = m_next_spawn_sequence++;
            for (auto& initializer : m_context_initializers)
                initializer(p, std::span<const ParticleControlpoint> { m_controlpoints });
            if (m_world_space) {
                const Eigen::Vector3f local = bounded_data.pos + p.position;
                const Eigen::Vector4d world =
                    world_from_spawn_space *
                    Eigen::Vector4d(local.x(), local.y(), local.z(), 1.0);
                p.position = world.head<3>().cast<float>();
                p.velocity =
                    (world_from_spawn_space.block<3, 3>(0, 0) * p.velocity.cast<double>())
                        .cast<float>();
            }
            for (auto& child : m_children) {
                if (child->Type() == SpawnType::EVENT_FOLLOW ||
                    child->Type() == SpawnType::EVENT_SPAWN ||
                    child->Type() == SpawnType::STATIC_CONTROLPOINT)
                    spawn_child(inst, *child, static_cast<isize>(index));
            }
            if (m_trail_length > 0) inst.TrailsVec()[index].Reset();
        }

        ParticleModify::MarkOld(p);
        if (! ParticleModify::LifetimeOk(p)) continue;
        ParticleModify::Reset(p);
        ParticleModify::ChangeLifetime(p, -simulation_time);
        if (! ParticleModify::LifetimeOk(p)) {
            for (auto& child : m_children) {
                if (child->Type() == SpawnType::EVENT_DEATH)
                    spawn_child(inst,
                                *child,
                                static_cast<isize>(index),
                                FollowWorldPosition(inst, p),
                                true);
            }
        }
    }

    if (m_spawn_type == SpawnType::EVENT_DEATH) inst.SetDeath(true);

    ParticleInfo info {
        .particles              = inst.ParticlesVec(),
        .controlpoints          = m_controlpoints,
        .world_from_local_dir   = world_from_local_dir,
        .local_from_world_dir   = local_from_world_dir,
        .world_from_spawn_space = world_from_spawn_space,
        .world_space            = m_world_space,
        .time                   = m_time,
        .time_pass              = simulation_time,
    };
    for (auto& operation : m_operators) operation(info);
    for (auto& p : info.particles) {
        if (! ParticleModify::LifetimeOk(p)) continue;
        ParticleModify::MoveByTime(p, simulation_time);
        ParticleModify::RotateByTime(p, simulation_time);
    }

    if (m_trail_length > 0) {
        auto& trails = inst.TrailsVec();
        for (usize index = 0; index < info.particles.size(); ++index) {
            auto& p = info.particles[index];
            if (! ParticleModify::LifetimeOk(p)) continue;
            auto& trail = trails[index];
            const Eigen::Vector3f current = p.position;
            if (! trail.has_previous_position) {
                trail.Initialize(current);
                continue;
            }
            const auto previous = trail.previous_position;
            const usize steps = std::min<usize>(trail_sample_steps, trail.positions.size());
            for (usize sample = 0; sample < steps; ++sample) {
                const double age = trail_sample_remainder +
                                   static_cast<double>(steps - sample - 1) *
                                       m_trail_sample_interval;
                const double amount = simulation_time > 0.0
                                          ? std::clamp((simulation_time - age) / simulation_time,
                                                       0.0,
                                                       1.0)
                                          : 1.0;
                trail.Push(previous + (current - previous) * static_cast<float>(amount));
            }
            trail.previous_position     = current;
            trail.has_previous_position = true;
        }
    }

    CompactInstance(inst);
    inst.SetNoLiveParticle(inst.Particles().empty());
}

void ParticleSubSystem::Advance(double frame_time, bool update_mesh) {
    const double rate = m_rate_source ? m_rate_source() : m_rate;
    const double simulation_time = frame_time * std::max(rate, 0.0);
    m_time += simulation_time;

    Eigen::Matrix3d world_from_local_dir = Eigen::Matrix3d::Identity();
    Eigen::Matrix3d local_from_world_dir = Eigen::Matrix3d::Identity();
    Eigen::Matrix4d world_from_spawn_space = Eigen::Matrix4d::Identity();
    Eigen::Matrix4d local_from_world = Eigen::Matrix4d::Identity();
    if (m_owner_node != nullptr) {
        m_owner_node->UpdateTrans();
        world_from_spawn_space = m_owner_node->ModelTrans();
        local_from_world       = world_from_spawn_space.inverse();
        if (! m_world_space) {
            world_from_local_dir = world_from_spawn_space.block<3, 3>(0, 0);
            if (std::abs(world_from_local_dir.determinant()) > 1e-9)
                local_from_world_dir = world_from_local_dir.inverse();
        }
    }

    for (auto& cp : m_controlpoints) {
        cp.offset           = cp.base_offset;
        cp.runtime_position = std::nullopt;
        cp.runtime_angles   = Eigen::Vector3d::Zero();
        cp.rotation         = Eigen::Matrix3d::Identity();
    }
    if (m_controlpoint_override_op) m_controlpoint_override_op(m_controlpoints);
    for (auto& cp : m_controlpoints) {
        if (cp.runtime_position.has_value()) {
            if (cp.worldspace) {
                const auto& position = *cp.runtime_position;
                cp.offset =
                    (local_from_world *
                     Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0))
                        .head<3>();
            } else {
                cp.offset += *cp.runtime_position;
            }
        }
        const auto& angles = cp.runtime_angles;
        cp.rotation = (Eigen::AngleAxisd(angles.z(), Eigen::Vector3d::UnitZ()) *
                       Eigen::AngleAxisd(angles.y(), Eigen::Vector3d::UnitY()) *
                       Eigen::AngleAxisd(angles.x(), Eigen::Vector3d::UnitX()))
                          .toRotationMatrix();
    }
    if (m_uses_mouse_controlpoint) {
        const auto pointer = m_sys.scene.pointerPosition;
        const Eigen::Vector3d mouse_world {
            static_cast<double>(pointer[0]) * static_cast<double>(m_sys.scene.ortho[0]),
            (1.0 - static_cast<double>(pointer[1])) * static_cast<double>(m_sys.scene.ortho[1]),
            0.0,
        };
        const Eigen::Vector4d mouse_local =
            local_from_world * Eigen::Vector4d(mouse_world.x(), mouse_world.y(), 0.0, 1.0);
        for (auto& cp : m_controlpoints)
            if (cp.link_mouse) cp.offset += mouse_local.head<3>();
    }

    std::array<float, 16> audio_average {};
    std::span<const float> audio_signal {};
    if (m_uses_audio_response) {
        for (usize index = 0; index < audio_average.size(); ++index)
            audio_average[index] =
                m_sys.scene.audioAverage[index].load(std::memory_order_relaxed);
        audio_signal = audio_average;
    }

    if (m_spawn_type == SpawnType::STATIC && m_instances.empty())
        m_instances.emplace_back(MakeInstance());

    usize  trail_steps = 0;
    double trail_remainder = 0.0;
    if (m_trail_length > 0) {
        if (m_trail_sample_interval > 0.0) {
            const double elapsed = m_trail_sample_accumulator + simulation_time;
            trail_steps = std::min<usize>(
                static_cast<usize>(std::floor(elapsed / m_trail_sample_interval)),
                m_trail_length);
            trail_remainder = std::fmod(elapsed, m_trail_sample_interval);
            m_trail_sample_accumulator = trail_remainder;
            if (auto* material = m_mesh->Material()) {
                auto value = material->customShader.constValues.find(std::string(G_RENDERVAR0));
                if (value != material->customShader.constValues.end() && value->second.size() >= 4) {
                    std::array<float, 4> render_var {
                        value->second[0],
                        value->second[1],
                        static_cast<float>(std::clamp(
                            trail_remainder / m_trail_sample_interval, 0.0, 1.0)),
                        value->second[3],
                    };
                    material->SetShaderValue(std::string(G_RENDERVAR0), render_var);
                }
            }
        } else {
            trail_steps = 1;
        }
    }

    for (auto& instance : m_instances) {
        rstd_assert(instance);
        Warmup(*instance,
               m_time,
               audio_signal,
               world_from_local_dir,
               local_from_world_dir,
               world_from_spawn_space);
        SimulateInstance(*instance,
                         simulation_time,
                         audio_signal,
                         world_from_local_dir,
                         local_from_world_dir,
                         world_from_spawn_space,
                         trail_steps,
                         trail_remainder);
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
            m_sys.gener->GenGLData(
                m_instances, *m_mesh, m_genSpecOp, m_rope_sequence_count);
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
