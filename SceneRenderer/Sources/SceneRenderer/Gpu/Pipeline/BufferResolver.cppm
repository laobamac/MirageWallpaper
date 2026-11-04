module;
#include <optional>

export module sr.vulkan_render:buffer_resolver;
import sr.core;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

export namespace sr::vulkan
{

enum class DrawBufferRole
{
    Vertex,
    Index,
};

struct DrawBufferKey {
    RenderItemId   render_item;
    DrawBufferRole role { DrawBufferRole::Vertex };
    uint32_t       submesh_index { 0 };
    uint32_t       stream_index { 0 };
    uint64_t       data_generation { 0 };
    uint64_t       allocation_generation { 0 };
};

struct DrawBufferRefs {
    RenderItemId render_item;
    uint64_t     allocation_generation { 0 };
    bool         dynamic { false };
    u32          draw_count { 0 };
    u32          instance_count { 1 };

    std::vector<DrawBufferKey>   vertex_keys;
    std::optional<DrawBufferKey> index_key;

    std::vector<MeshBufferRef> static_vertices;
    MeshBufferRef              static_index;

    std::vector<StagingBufferRef> dynamic_vertices;
    StagingBufferRef              dynamic_index;

    DrawBufferRefs()                                     = default;
    DrawBufferRefs(const DrawBufferRefs&)                = delete;
    DrawBufferRefs& operator=(const DrawBufferRefs&)     = delete;
    DrawBufferRefs(DrawBufferRefs&&) noexcept            = default;
    DrawBufferRefs& operator=(DrawBufferRefs&&) noexcept = default;

    bool hasIndex() const {
        return static_cast<bool>(dynamic_index) || static_cast<bool>(static_index);
    }
};

struct DrawBufferRequest {
    RenderItemId render_item;
    SceneMesh*   mesh { nullptr };
    uint32_t     submesh_index { 0 };
    uint64_t     dynamic_allocation_generation { 0 };
};

std::vector<DrawBufferKey> BuildDrawBufferKeys(const DrawBufferRequest&,
                                               uint64_t allocation_generation = 0);

class RenderBufferResolver {
public:
    RenderBufferResolver(const Device&, StagingBuffer&);

    std::optional<DrawBufferRefs> prepareDrawBuffers(const DrawBufferRequest&);
    bool updateDynamicDrawBuffers(const DrawBufferRequest&, DrawBufferRefs&);
    void releaseDynamicDrawBuffers(DrawBufferRefs&);

private:
    const Device&  m_device;
    StagingBuffer& m_dynamic_buffer;
};

} // namespace sr::vulkan
