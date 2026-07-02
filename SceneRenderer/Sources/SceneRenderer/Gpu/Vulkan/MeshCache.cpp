module;

#include <rstd/macro.hpp>

module sr.vulkan;

import rstd.log;

namespace sr::vulkan
{

// ---------- MeshBufferRef ----------

MeshBufferRef::MeshBufferRef(MeshCache* owner, MeshCacheKey key, VkDeviceSize offset,
                             VkDeviceSize size)
    : m_owner(owner), m_key(key), m_offset(offset), m_size(size) {}

MeshBufferRef::~MeshBufferRef() {
    if (m_owner) m_owner->release(m_key);
}

MeshBufferRef::MeshBufferRef(MeshBufferRef&& o) noexcept
    : m_owner(o.m_owner), m_key(o.m_key), m_offset(o.m_offset), m_size(o.m_size) {
    o.m_owner  = nullptr;
    o.m_key    = {};
    o.m_offset = 0;
    o.m_size   = 0;
}

MeshBufferRef& MeshBufferRef::operator=(MeshBufferRef&& o) noexcept {
    if (this != &o) {
        if (m_owner) m_owner->release(m_key);
        m_owner    = o.m_owner;
        m_key      = o.m_key;
        m_offset   = o.m_offset;
        m_size     = o.m_size;
        o.m_owner  = nullptr;
        o.m_key    = {};
        o.m_offset = 0;
        o.m_size   = 0;
    }
    return *this;
}

VkBuffer MeshBufferRef::buffer() const noexcept {
    return m_owner ? m_owner->gpuBuf() : VK_NULL_HANDLE;
}

// ---------- MeshCache ----------

namespace
{
// Match StagingBuffer's default seed size; vertex/index pools rarely need more
// than a few MB for typical wallpapers.
constexpr VkDeviceSize kSeedSize = 2 * 1024 * 1024;
} // namespace

MeshCache::MeshCache(const Device& d): m_device(d) {}
MeshCache::~MeshCache() { destroy(); }

bool MeshCache::init() {
    if (m_buf) return true;
    m_buf = std::make_unique<StagingBuffer>(
        m_device, kSeedSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (! m_buf->allocate()) {
        m_buf.reset();
        return false;
    }
    return true;
}

void MeshCache::destroy() {
    m_map.clear();
    if (m_buf) {
        m_buf->destroy();
        m_buf.reset();
    }
    m_dirty = false;
}

std::optional<MeshBufferRef>
MeshCache::QueryOrUpload(MeshCacheKey key, std::span<const uint8_t> data, VkDeviceSize alignment) {
    if (! m_buf || key.array_ptr == nullptr) return std::nullopt;

    if (auto it = m_map.find(key); it != m_map.end()) {
        ++it->second.refcount;
        return MeshBufferRef(this, key, it->second.ref.offset, it->second.ref.size);
    }

    StagingBufferRef ref;
    if (! m_buf->allocateSubRef(data.size(), ref, alignment)) return std::nullopt;
    if (! m_buf->writeToBuf(ref, { const_cast<uint8_t*>(data.data()), data.size() })) {
        m_buf->unallocateSubRef(ref);
        return std::nullopt;
    }

    auto [it, _] = m_map.emplace(key, Entry { ref, 1 });
    m_dirty      = true;
    return MeshBufferRef(this, key, it->second.ref.offset, it->second.ref.size);
}

void MeshCache::release(MeshCacheKey key) {
    auto it = m_map.find(key);
    if (it == m_map.end()) return;
    if (it->second.refcount > 0) --it->second.refcount;
}

VkBuffer MeshCache::gpuBuf() const { return m_buf ? m_buf->gpuBuf() : VK_NULL_HANDLE; }

bool MeshCache::recordPendingUploads(vvk::CommandBuffer& cmd) {
    if (! m_buf || ! m_dirty) return true;
    if (! m_buf->recordUpload(cmd)) return false;
    m_dirty = false;
    return true;
}

void MeshCache::onRenderGraphCleared() {
    // refcounts get dropped naturally as CustomShaderPass::destory clears its
    // MeshBufferRef vectors. Reserved as a future fence/eviction hook.
}

void MeshCache::evictUnused() {
    if (! m_buf) return;
    size_t freed = 0;
    for (auto it = m_map.begin(); it != m_map.end();) {
        if (it->second.refcount == 0) {
            m_buf->unallocateSubRef(it->second.ref);
            it = m_map.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    if (freed > 0) rstd_info("MeshCache evict: freed {} entries", freed);
}

} // namespace sr::vulkan
