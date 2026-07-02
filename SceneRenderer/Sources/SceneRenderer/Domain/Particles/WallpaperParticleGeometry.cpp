module;

#include <rstd/macro.hpp>

module sr.scene;
import eigen;
import sr.spec_texs;
import sr.core;
import rstd.log;
import rstd.cppstd;

using namespace sr;
using namespace Eigen;

struct WPGOption {
    bool thick_format { false };
};

namespace
{
inline void AssignVertexTimes(std::span<float> dst, std::span<const float> src,
                              unsigned num) noexcept {
    const unsigned dst_one_size = dst.size() / num;
    for (unsigned i = 0; i < num; i++) {
        std::copy(src.begin(), src.end(), dst.begin() + i * dst_one_size);
    }
}

inline void AssignVertex(std::span<float> dst, std::span<const float> src, unsigned num) noexcept {
    const unsigned dst_one_size = dst.size() / num;
    const unsigned src_one_size = src.size() / num;
    for (unsigned i = 0; i < num; i++) {
        std::copy_n(src.begin() + i * src_one_size, src_one_size, dst.begin() + i * dst_one_size);
    }
}

inline usize GenParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                             const ParticleRawGenSpecOp& specOp, WPGOption opt,
                             SceneVertexArray& sv) noexcept {
    std::array<float, 32 * 4> storage;

    float* data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = 4 * one_size;
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;

            usize offset = 0;

            // pos
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { pos[0], pos[1], pos[2] }, 4);
            offset += 4;
            // TexCoordVec4
            float      rz = p.rotation[2];
            std::array t { 0.0f, 1.0f, rz, size, 1.0f, 1.0f, rz, size,
                           1.0f, 0.0f, rz, size, 0.0f, 0.0f, rz, size };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;

            // color
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha },
                              4);
            offset += 4;

            if (opt.thick_format) {
                AssignVertexTimes(
                    { data + offset, totle_size },
                    std::array { p.velocity[0], p.velocity[1], p.velocity[2], lifetime },
                    4);
                offset += 4;
            }
            // TexCoordC2
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { p.rotation[0], p.rotation[1] }, 4);

            sv.SetVertexs((i++) * 4, { data, totle_size });
        }
    }
    return i;
}

struct AttrSlot {
    usize offset { 0 };
    bool  enabled { false };
};

inline AttrSlot
FindAttrSlot(const sr::Map<std::string, SceneVertexArray::SceneVertexAttributeOffset>& attrs,
             std::string_view name) noexcept {
    auto it = attrs.find(std::string(name));
    if (it == attrs.end()) return {};
    return { it->second.offset / sizeof(float), true };
}

inline usize GenParticlePointData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv) noexcept {
    const auto            one_size = sv.OneSize();
    const auto            attrs    = sv.GetAttrOffsetMap();
    const AttrSlot        position = FindAttrSlot(attrs, WE_IN_POSITION);
    const AttrSlot        texcoord = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4);
    const AttrSlot        color    = FindAttrSlot(attrs, WE_IN_COLOR);
    const AttrSlot        velocity = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4C1);
    std::array<float, 16> v {};
    auto                  write3 = [&](AttrSlot slot, float x, float y, float z) noexcept {
        if (! slot.enabled) return;
        v[slot.offset + 0] = x;
        v[slot.offset + 1] = y;
        v[slot.offset + 2] = z;
    };
    auto write4 = [&](AttrSlot slot, float x, float y, float z, float w) noexcept {
        if (! slot.enabled) return;
        v[slot.offset + 0] = x;
        v[slot.offset + 1] = y;
        v[slot.offset + 2] = z;
        v[slot.offset + 3] = w;
    };

    usize i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) continue;

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;

            std::fill(v.begin(), v.begin() + (isize)one_size, 0.0f);
            write3(position, pos[0], pos[1], pos[2]);
            write4(texcoord, p.rotation[0], p.rotation[1], p.rotation[2], size);
            write4(color, p.color[0], p.color[1], p.color[2], p.alpha);

            if (opt.thick_format) {
                write4(velocity, p.velocity[0], p.velocity[1], p.velocity[2], lifetime);
            }

            sv.SetVertexs(i++, { v.data(), one_size });
        }
    }
    return i;
}

// Emit one VS-input vertex per consecutive pair of trail-history samples for a
// single live rope-head particle. Each emitted vertex carries the segment's
// endpoints + Catmull-Rom neighbour positions as splineCP0/CP1 (the vert shader
// derives the GS-side tangents from them). Returns the number of segment
// vertices emitted; vertices land at [base_index, base_index+ret).
inline size_t GenRopeParticleSegments(const Particle& p, const ParticleTrail& trail,
                                      const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                      SceneVertexArray& sv, size_t base_index) {
    const auto            one_size = sv.OneSize();
    std::array<float, 32> v {};
    size_t                emitted = 0;

    if (trail.len < 2) return 0;

    float size     = p.size / 2.0f;
    float lifetime = p.lifetime;
    specOp(p, { &lifetime });

    const float in_ParticleTrailLength = (float)trail.len;

    // trail.At(0) = oldest, At(len-1) = newest. Segments connect (j-1) -> j.
    for (uint16_t j = 1; j < trail.len; j++) {
        Vector3f pre_pos = trail.At((uint16_t)(j - 1));
        Vector3f cur_pos = trail.At(j);
        // Catmull-Rom neighbour samples for the cubic-Bezier subdivision; the
        // GS hardcoded 0.15 factor matches Catmull-Rom tension 0.5 (theoretical
        // 1/6 ≈ 0.167). At the trail endpoints fall back to the segment ends
        // so those boundary spans render flat.
        Vector3f scp = (j >= 2) ? trail.At((uint16_t)(j - 2)) : pre_pos;
        Vector3f ecp = (j + 1 < trail.len) ? trail.At((uint16_t)(j + 1)) : cur_pos;

        const float in_ParticleTrailPosition = (float)(j - 1);

        size_t off = 0;
        v[off++]   = pre_pos[0];
        v[off++]   = pre_pos[1];
        v[off++]   = pre_pos[2];
        v[off++]   = size;
        v[off++]   = cur_pos[0];
        v[off++]   = cur_pos[1];
        v[off++]   = cur_pos[2];
        v[off++]   = in_ParticleTrailLength;
        v[off++]   = scp[0];
        v[off++]   = scp[1];
        v[off++]   = scp[2];
        v[off++]   = in_ParticleTrailPosition;
        if (opt.thick_format) {
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
            v[off++] = size;
            v[off++] = p.color[0];
            v[off++] = p.color[1];
            v[off++] = p.color[2];
            v[off++] = p.alpha;
        } else {
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
            v[off++] = 0.0f;
        }
        v[off++] = p.color[0];
        v[off++] = p.color[1];
        v[off++] = p.color[2];
        v[off++] = p.alpha;

        rstd_assert(off == one_size);
        sv.SetVertexs(base_index + emitted, { v.data(), one_size });
        emitted++;
    }
    return emitted;
}

inline size_t GenRopeParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv) {
    size_t total = 0;
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;
        auto         particles = inst->Particles();
        auto         trails    = inst->Trails();
        const size_t n         = std::min(particles.size(), trails.size());
        for (size_t si = 0; si < n; si++) {
            if (! ParticleModify::LifetimeOk(particles[si])) continue;
            total += GenRopeParticleSegments(particles[si], trails[si], specOp, opt, sv, total);
        }
    }
    return total;
}

inline size_t GenRopeParticleQuadSegments(const Particle& p, const ParticleTrail& trail,
                                          const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                          SceneVertexArray& sv, size_t base_index) {
    const auto            one_size = sv.OneSize();
    const auto            attrs    = sv.GetAttrOffsetMap();
    const AttrSlot        position = FindAttrSlot(attrs, WE_IN_POSITIONVEC4);
    const AttrSlot        endpoint = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4);
    const AttrSlot        cp_start = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4C1);
    const AttrSlot        cp_end4  = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4C2);
    const AttrSlot        cp_end3  = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC3C2);
    const AttrSlot        color2   = FindAttrSlot(attrs, WE_IN_TEXCOORDVEC4C3);
    const AttrSlot        uv4      = FindAttrSlot(attrs, WE_IN_TEXCOORDC4);
    const AttrSlot        uv3      = FindAttrSlot(attrs, WE_IN_TEXCOORDC3);
    const AttrSlot        color    = FindAttrSlot(attrs, WE_IN_COLOR);
    std::array<float, 64> v {};

    auto write2 = [&](AttrSlot slot, float x, float y) noexcept {
        if (! slot.enabled) return;
        v[slot.offset + 0] = x;
        v[slot.offset + 1] = y;
    };
    auto write4 = [&](AttrSlot slot, float x, float y, float z, float w) noexcept {
        if (! slot.enabled) return;
        v[slot.offset + 0] = x;
        v[slot.offset + 1] = y;
        v[slot.offset + 2] = z;
        v[slot.offset + 3] = w;
    };

    rstd_assert(one_size <= v.size());
    if (trail.len < 2) return 0;

    float size     = p.size / 2.0f;
    float lifetime = p.lifetime;
    specOp(p, { &lifetime });

    const float in_ParticleTrailLength = (float)trail.len;
    const std::array<std::array<float, 2>, 4> uvs {
        std::array { 0.0f, 0.0f },
        std::array { 0.0f, 1.0f },
        std::array { 1.0f, 1.0f },
        std::array { 1.0f, 0.0f },
    };

    size_t emitted = 0;
    for (uint16_t j = 1; j < trail.len; j++) {
        Vector3f pre_pos = trail.At((uint16_t)(j - 1));
        Vector3f cur_pos = trail.At(j);
        Vector3f scp     = (j >= 2) ? trail.At((uint16_t)(j - 2)) : pre_pos;
        Vector3f ecp     = (j + 1 < trail.len) ? trail.At((uint16_t)(j + 1)) : cur_pos;

        const float in_ParticleTrailPosition = (float)(j - 1);
        for (usize q = 0; q < uvs.size(); ++q) {
            std::fill(v.begin(), v.begin() + (isize)one_size, 0.0f);
            write4(position, pre_pos[0], pre_pos[1], pre_pos[2], size);
            write4(endpoint, cur_pos[0], cur_pos[1], cur_pos[2], in_ParticleTrailLength);
            write4(cp_start, scp[0], scp[1], scp[2], in_ParticleTrailPosition);
            if (opt.thick_format) {
                write4(cp_end4, ecp[0], ecp[1], ecp[2], size);
                write4(color2, p.color[0], p.color[1], p.color[2], p.alpha);
                write2(uv4, uvs[q][0], uvs[q][1]);
            } else {
                write4(cp_end3, ecp[0], ecp[1], ecp[2], 0.0f);
                write2(uv3, uvs[q][0], uvs[q][1]);
            }
            write4(color, p.color[0], p.color[1], p.color[2], p.alpha);
            sv.SetVertexs((base_index + emitted) * 4 + q, { v.data(), one_size });
        }
        emitted++;
    }
    return emitted;
}

inline size_t GenRopeParticleQuadData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                      const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                      SceneVertexArray& sv) {
    size_t total = 0;
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;
        auto         particles = inst->Particles();
        auto         trails    = inst->Trails();
        const size_t n         = std::min(particles.size(), trails.size());
        for (size_t si = 0; si < n; si++) {
            if (! ParticleModify::LifetimeOk(particles[si])) continue;
            total += GenRopeParticleQuadSegments(particles[si], trails[si], specOp, opt, sv, total);
        }
    }
    return total;
}

inline void updateIndexArray(uint32_t index, size_t count, SceneIndexArray& iarray) noexcept {
    constexpr size_t single_size = 6;
    uint32_t         cv          = index * 4;

    std::array<uint32_t, single_size> single;
    // 0 1 3
    // 1 2 3
    single[0] = cv;
    single[1] = cv + 1;
    single[2] = cv + 3;
    single[3] = cv + 1;
    single[4] = cv + 2;
    single[5] = cv + 3;
    // every particle
    for (uint32_t i = index; i < count; i++) {
        iarray.Assign(i * single_size, single);
        for (auto& x : single) x += 4;
    }
}
} // namespace

void ParticleGeometryBuilder::GenGLData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                   SceneMesh& mesh, ParticleRawGenSpecOp& specOp) {
    auto& sv = mesh.GetVertexArray(0);

    WPGOption opt;
    opt.thick_format = sv.GetOption(WE_CB_THICK_FORMAT);

    if (sv.GetOption(WE_PRENDER_ROPE)) {
        sv.ResetSize();
        usize segment_num = 0;
        if (mesh.Primitive() == MeshPrimitive::POINT) {
            segment_num = GenRopeParticleData(instances, specOp, opt, sv);
        } else {
            segment_num = GenRopeParticleQuadData(instances, specOp, opt, sv);
            auto& si    = mesh.GetIndexArray(0);
            u32   index_num = (u32)(si.DataCount() / 6);
            if (segment_num > index_num) {
                updateIndexArray(index_num, segment_num, si);
            }
            si.SetRenderDataCount(segment_num * 6);
        }
        return;
    }

    if (mesh.Primitive() == MeshPrimitive::POINT) {
        sv.ResetSize();
        GenParticlePointData(instances, specOp, opt, sv);
        return;
    }

    usize particle_num = GenParticleData(instances, specOp, opt, sv);

    auto& si       = mesh.GetIndexArray(0);
    u32   indexNum = (u32)(si.DataCount() / 6);
    if (particle_num > indexNum) {
        updateIndexArray(indexNum, particle_num, si);
    }
    si.SetRenderDataCount(particle_num * 6);
}
