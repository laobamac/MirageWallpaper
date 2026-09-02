module sr.vulkan_render;
import sr.core;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

namespace sr::vulkan
{

namespace
{

std::span<const uint8_t> bytesOf(const float* data, usize size) {
    return { reinterpret_cast<const uint8_t*>(data), size };
}

std::span<const uint8_t> bytesOf(const uint32_t* data, usize size) {
    return { reinterpret_cast<const uint8_t*>(data), size };
}

std::span<uint8_t> mutableBytesOf(const float* data, usize size) {
    return { const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data)), size };
}

std::span<uint8_t> mutableBytesOf(const uint32_t* data, usize size) {
    return { const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data)), size };
}

uint64_t next_dynamic_allocation_generation() {
    static std::atomic_uint64_t next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

uint64_t resolve_allocation_generation(const DrawBufferRequest& request, bool dynamic) {
    if (! dynamic) return 0;
    if (request.dynamic_allocation_generation != 0) return request.dynamic_allocation_generation;
    return next_dynamic_allocation_generation();
}

} // namespace

RenderBufferResolver::RenderBufferResolver(const Device& device, StagingBuffer& dynamic_buffer)
    : m_device(device), m_dynamic_buffer(dynamic_buffer) {}

std::vector<DrawBufferKey> BuildDrawBufferKeys(const DrawBufferRequest& request,
                                               uint64_t                 allocation_generation) {
    std::vector<DrawBufferKey> keys;
    if (request.mesh == nullptr) return keys;
    const SceneMesh& mesh = *request.mesh;
    if (request.submesh_index >= mesh.Submeshes().size()) return keys;

    const auto& submesh = mesh.Submeshes()[request.submesh_index];
    keys.reserve(submesh.vertex_arrays.size() + (submesh.index_arrays.empty() ? 0u : 1u));
    const uint64_t resolved_allocation = mesh.Dynamic() ? allocation_generation : 0;

    for (usize i = 0; i < submesh.vertex_arrays.size(); ++i) {
        const auto& vertex = submesh.vertex_arrays[i];
        keys.push_back(DrawBufferKey { .render_item           = request.render_item,
                                       .role                  = DrawBufferRole::Vertex,
                                       .submesh_index         = request.submesh_index,
                                       .stream_index          = static_cast<uint32_t>(i),
                                       .data_generation       = vertex.DataGeneration(),
                                       .allocation_generation = resolved_allocation });
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index = submesh.index_arrays[0];
        keys.push_back(DrawBufferKey { .render_item           = request.render_item,
                                       .role                  = DrawBufferRole::Index,
                                       .submesh_index         = request.submesh_index,
                                       .stream_index          = 0,
                                       .data_generation       = index.DataGeneration(),
                                       .allocation_generation = resolved_allocation });
    }

    return keys;
}

std::optional<DrawBufferRefs>
RenderBufferResolver::prepareDrawBuffers(const DrawBufferRequest& request) {
    if (request.mesh == nullptr) return std::nullopt;
    SceneMesh& mesh          = *request.mesh;
    const auto submesh_index = request.submesh_index;
    if (mesh.Submeshes().empty() || submesh_index >= mesh.Submeshes().size()) return std::nullopt;

    const auto& submesh = mesh.Submeshes()[submesh_index];

    DrawBufferRefs out;
    out.render_item           = request.render_item;
    out.dynamic               = false;
    out.allocation_generation = resolve_allocation_generation(request, mesh.Dynamic());
    out.instance_count        = mesh.ParticleInstanced() ? mesh.ParticleInstanceCount() : 1;
    auto keys                 = BuildDrawBufferKeys(request, out.allocation_generation);
    out.vertex_keys.reserve(submesh.vertex_arrays.size());
    out.dynamic_vertices.resize(submesh.vertex_arrays.size());
    out.static_vertices.resize(submesh.vertex_arrays.size());

    auto& mesh_cache = m_device.mesh_cache();
    for (usize i = 0; i < submesh.vertex_arrays.size(); i++) {
        const auto& vertex = submesh.vertex_arrays[i];
        out.vertex_keys.push_back(keys[i]);
        out.draw_count += static_cast<u32>(vertex.DataSize() / vertex.OneSize());

        const bool vertex_dynamic = mesh.Dynamic() && ! vertex.StaticData();
        if (vertex_dynamic) {
            if (! m_dynamic_buffer.allocateSubRef(vertex.CapacitySizeOf(), out.dynamic_vertices[i]))
                return std::nullopt;
            if (const auto size = vertex.DataSizeOf(); size > 0) {
                if (! m_dynamic_buffer.writeToBuf(out.dynamic_vertices[i],
                                                  mutableBytesOf(vertex.Data(), size)))
                    return std::nullopt;
            }
            out.dynamic = true;
        } else {
            auto ref = mesh_cache.QueryOrUpload({ &vertex, vertex.DataGeneration() },
                                                bytesOf(vertex.Data(), vertex.CapacitySizeOf()));
            if (! ref) return std::nullopt;
            out.static_vertices[i] = std::move(*ref);
        }
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index = submesh.index_arrays[0];
        out.draw_count    = static_cast<u32>(index.DataCount());
        out.index_key     = keys.back();

        if (mesh.Dynamic() && ! index.StaticTopology()) {
            if (! m_dynamic_buffer.allocateSubRef(index.CapacitySizeof(), out.dynamic_index))
                return std::nullopt;
            if (const auto size = index.DataSizeOf(); size > 0) {
                if (! m_dynamic_buffer.writeToBuf(out.dynamic_index,
                                                  mutableBytesOf(index.Data(), size)))
                    return std::nullopt;
            }
            out.dynamic = true;
        } else {
            auto ref = mesh_cache.QueryOrUpload({ &index, index.DataGeneration() },
                                                bytesOf(index.Data(), index.CapacitySizeof()));
            if (! ref) return std::nullopt;
            out.static_index = std::move(*ref);
        }
    }

    return out;
}

bool RenderBufferResolver::updateDynamicDrawBuffers(const DrawBufferRequest& request,
                                                    DrawBufferRefs&          buffers) {
    if (! buffers.dynamic) return true;
    if (request.mesh == nullptr) return false;
    SceneMesh& mesh          = *request.mesh;
    const auto submesh_index = request.submesh_index;
    if (submesh_index >= mesh.Submeshes().size()) return true;

    const auto& submesh = mesh.Submeshes()[submesh_index];
    if (! submesh.index_arrays.empty()) {
        buffers.draw_count = static_cast<u32>(submesh.index_arrays[0].RenderDataCount());
    } else if (! submesh.vertex_arrays.empty()) {
        buffers.draw_count = static_cast<u32>(submesh.vertex_arrays[0].VertexCount());
    } else {
        buffers.draw_count = 0;
    }
    if ((mesh.DirtyFlags() & SceneMeshDirtyData) == 0) return true;

    auto require_reprepare = [&mesh] {
        mesh.SetLayoutDirty();
        return false;
    };
    if (buffers.dynamic_vertices.size() != submesh.vertex_arrays.size() ||
        buffers.static_vertices.size() != submesh.vertex_arrays.size())
        return require_reprepare();
    if (buffers.vertex_keys.size() != submesh.vertex_arrays.size()) return require_reprepare();
    if (! submesh.index_arrays.empty()) {
        const bool needs_dynamic_index = ! submesh.index_arrays[0].StaticTopology();
        if (static_cast<bool>(buffers.dynamic_index) != needs_dynamic_index)
            return require_reprepare();
        if (! needs_dynamic_index && ! buffers.static_index) return require_reprepare();
    } else if (buffers.dynamic_index || buffers.static_index) {
        return require_reprepare();
    }

    for (usize i = 0; i < submesh.vertex_arrays.size(); i++) {
        const auto& vertex                     = submesh.vertex_arrays[i];
        buffers.vertex_keys[i].data_generation = vertex.DataGeneration();
        if (vertex.StaticData()) {
            if (! buffers.static_vertices[i]) return require_reprepare();
            continue;
        }
        if (! buffers.dynamic_vertices[i] || vertex.DataSizeOf() > buffers.dynamic_vertices[i].size)
            return require_reprepare();
        if (! m_dynamic_buffer.writeToBuf(buffers.dynamic_vertices[i],
                                          mutableBytesOf(vertex.Data(), vertex.DataSizeOf())))
            return false;
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index = submesh.index_arrays[0];
        buffers.instance_count = mesh.ParticleInstanced() ? mesh.ParticleInstanceCount() : 1;
        if (buffers.index_key) buffers.index_key->data_generation = index.DataGeneration();
        if (! index.StaticTopology()) {
            if (! buffers.dynamic_index || index.DataSizeOf() > buffers.dynamic_index.size)
                return require_reprepare();
            if (! m_dynamic_buffer.writeToBuf(buffers.dynamic_index,
                                              mutableBytesOf(index.Data(), index.DataSizeOf())))
                return false;
        }
    } else if (! submesh.vertex_arrays.empty()) {
        buffers.instance_count = 1;
    }

    return true;
}

void RenderBufferResolver::releaseDynamicDrawBuffers(DrawBufferRefs& buffers) {
    for (auto& ref : buffers.dynamic_vertices) {
        if (ref) m_dynamic_buffer.unallocateSubRef(ref);
    }
    buffers.dynamic_vertices.clear();
    buffers.static_vertices.clear();
    if (buffers.dynamic_index) m_dynamic_buffer.unallocateSubRef(buffers.dynamic_index);
    buffers.dynamic_index = {};
    buffers.vertex_keys.clear();
    buffers.index_key = std::nullopt;
}

} // namespace sr::vulkan
