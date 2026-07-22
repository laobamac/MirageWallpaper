module;

#include <rstd/macro.hpp>
#include "vk_mem_alloc.h"
#include "vvk/macros.hpp"

module sr.vulkan;
import sr.types;
import rstd.log;
import rstd.cppstd;

using namespace sr::vulkan;

#define CHECK_REF(ref, act)                                                   \
    if (! ref) {                                                              \
        rstd_error("stage ref not available, index {}", ref.m_virtual_index); \
        {                                                                     \
            act;                                                              \
        }                                                                     \
    }

StagingBuffer::StagingBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage)
    : m_device(d), m_size_step(size), m_usage(usage) {}
StagingBuffer::~StagingBuffer() {}

namespace
{
std::optional<VmaBufferParameters> CreateGpuBuffer(VmaAllocator allocator, VkBufferUsageFlags usage,
                                                   std::size_t size) {
    do {
        VmaBufferParameters buffer;
        VkBufferCreateInfo  ci {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .size  = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
        };
        buffer.req_size                  = ci.size;
        VmaAllocationCreateInfo vma_info = {};
        vma_info.usage                   = VMA_MEMORY_USAGE_GPU_ONLY;
        VVK_CHECK_ACT(break, vvk::CreateBuffer(allocator, ci, vma_info, buffer.handle));
        return buffer;
    } while (false);
    return std::nullopt;
}

void RecordCopyBuffer(const BufferParameters& dst_buf, const BufferParameters& src_buf,
                      std::span<const StagingBuffer::DirtyRange> ranges,
                      vvk::CommandBuffer& cmd) {
    for (const auto& r : ranges) {
        if (r.size == 0) continue;
        VkBufferCopy copy {
            .srcOffset = r.offset,
            .dstOffset = r.offset,
            .size      = r.size,
        };
        cmd.CopyBuffer(src_buf.handle, dst_buf.handle, copy);
    }

    VkBufferMemoryBarrier in_bar {
        .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext         = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        // INDEX_READ for the index-buffer use of this staging buffer;
        // missing it triggered sync-validation READ_AFTER_WRITE at INDEX_INPUT.
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                         VK_ACCESS_UNIFORM_READ_BIT,
        .buffer        = dst_buf.handle,
        .offset        = 0,
        .size          = VK_WHOLE_SIZE,
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        in_bar);
}
} // namespace

void StagingBuffer::markDirty(VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) return;
    m_dirty_ranges.push_back(DirtyRange { .offset = offset, .size = size });
}

StagingBuffer::VirtualBlock* StagingBuffer::newVirtualBlock(VkDeviceSize nsize) {
    auto it = std::find_if(m_virtual_blocks.begin(), m_virtual_blocks.end(), [nsize](auto& b) {
        return ! b.enabled && b.size >= nsize;
    });
    if (it == std::end(m_virtual_blocks)) {
        VkDeviceSize offset = m_virtual_blocks.empty()
                                  ? 0
                                  : m_virtual_blocks.back().offset + m_virtual_blocks.back().size;

        m_virtual_blocks.push_back({});
        it         = m_virtual_blocks.end() - 1;
        it->size   = nsize > m_size_step ? nsize : m_size_step;
        it->index  = (size_t)std::distance(m_virtual_blocks.begin(), it);
        it->offset = offset;
    }
    auto& block = *it;

    VmaVirtualBlockCreateInfo blockCreateInfo = {};
    blockCreateInfo.size                      = block.size;

    VVK_CHECK_ACT(return nullptr, vmaCreateVirtualBlock(&blockCreateInfo, &block.handle));
    block.enabled = true;

    rstd_info("new buffer block({:#x}), size: {}, index: {} / {}",
              reinterpret_cast<std::uintptr_t>(this),
              block.size,
              block.index,
              m_virtual_blocks.size());
    return &block;
}
bool StagingBuffer::increaseBuf(VkDeviceSize nsize) {
    if (m_stage_raw == nullptr) {
        VVK_CHECK_BOOL_RE(mapStageBuf());
    }
    auto                 newsize = m_stage_buf.req_size + nsize;
    std::vector<uint8_t> tmp;
    tmp.resize(newsize);
    std::memcpy(tmp.data(), m_stage_raw, m_stage_buf.req_size);

    m_stage_raw = nullptr;
    m_stage_buf.handle.UnMapMemory();
    m_stage_buf.handle = nullptr;

    if (! CreateStagingBuffer(m_device.vma_allocator(), newsize, m_stage_buf)) return false;
    VVK_CHECK_BOOL_RE(mapStageBuf());
    std::memcpy(m_stage_raw, tmp.data(), newsize);

    m_gpu_buf.handle = nullptr;
    rstd_info("increase buffer size: {}", nsize);
    return true;
}

bool StagingBuffer::allocate() {
    if (! CreateStagingBuffer(m_device.vma_allocator(), m_size_step, m_stage_buf)) return false;
    VVK_CHECK_BOOL_RE(m_stage_buf.handle.MapMemory(&m_stage_raw));
    auto* block = newVirtualBlock(m_size_step);
    return block != nullptr;
}

void StagingBuffer::destroy() {
    if (m_stage_raw != nullptr) {
        m_stage_buf.handle.UnMapMemory();
    }
    for (auto& block : m_virtual_blocks) {
        if (block.enabled) {
            vmaClearVirtualBlock(block.handle);
            vmaDestroyVirtualBlock(block.handle);
        }
    }
    m_virtual_blocks.clear();

    m_stage_buf = {};
    m_gpu_buf   = {};
    m_dirty_ranges.clear();
}

bool StagingBuffer::allocateSubRef(VkDeviceSize size, StagingBufferRef& ref,
                                   VkDeviceSize alignment) {
    VmaVirtualAllocationCreateInfo allocCreateInfo = {};
    allocCreateInfo.size                           = size;
    allocCreateInfo.alignment                      = alignment;

    VmaVirtualAllocation allocation;
    VkDeviceSize         offset;

    auto setRef = [&offset, &allocation, size](StagingBufferRef& ref, VirtualBlock& block) {
        ref.size   = size;
        ref.offset = offset + block.offset;

        ref.m_allocation    = allocation;
        ref.m_virtual_index = block.index;
    };

    for (auto& block : m_virtual_blocks) {
        if (block.enabled && block.size >= size) {
            if (auto res = vmaVirtualAllocate(block.handle, &allocCreateInfo, &allocation, &offset);
                res == VK_SUCCESS) {
                setRef(ref, block);
                return true;
            }
        }
    }

    auto  old_block_num = m_virtual_blocks.size();
    auto* p_block       = newVirtualBlock(size);
    if (p_block == nullptr) return false;

    auto& block = *p_block;
    if (old_block_num < m_virtual_blocks.size()) {
        if (! increaseBuf(block.size)) {
            auto& block = m_virtual_blocks.back();
            vmaClearVirtualBlock(block.handle);
            vmaDestroyVirtualBlock(block.handle);
            m_virtual_blocks.pop_back();
            rstd_error("increase buf failed, pop_back block, current: {}", m_virtual_blocks.size());
            return false;
        }
    }
    VVK_CHECK_BOOL_RE(vmaVirtualAllocate(block.handle, &allocCreateInfo, &allocation, &offset));
    setRef(ref, block);
    return true;
}
void StagingBuffer::unallocateSubRef(const StagingBufferRef& ref) {
    CHECK_REF(ref, ;);
    if (ref.m_virtual_index < m_virtual_blocks.size()) {
        auto& block = m_virtual_blocks[ref.m_virtual_index];
        vmaVirtualFree(block.handle, ref.m_allocation);
        if (block.enabled && vmaIsVirtualBlockEmpty(block.handle)) {
            vmaDestroyVirtualBlock(block.handle);
            block.handle  = VK_NULL_HANDLE;
            block.enabled = false;
        }
    } else {
        rstd_error("unallocate stagingbuffer failed: wrong index {}", ref.m_virtual_index);
    }
}

VkResult StagingBuffer::mapStageBuf() { return m_stage_buf.handle.MapMemory(&m_stage_raw); }

bool StagingBuffer::writeToBuf(const StagingBufferRef& ref, std::span<uint8_t> data,
                               size_t offset) {
    CHECK_REF(ref, return false);

    if (m_stage_raw == nullptr) mapStageBuf();
    VkDeviceSize size = std::min<VkDeviceSize>(ref.size - offset, data.size());
    uint8_t*     raw  = (uint8_t*)m_stage_raw;
    std::copy(data.begin(), data.begin() + size, raw + ref.offset + offset);
    markDirty(ref.offset + offset, size);
    return true;
}

bool StagingBuffer::fillBuf(const StagingBufferRef& ref, size_t offset, size_t size, uint8_t c) {
    CHECK_REF(ref, return false);

    if (m_stage_raw == nullptr) mapStageBuf();
    VkDeviceSize size_     = std::min<VkDeviceSize>(ref.size - offset, size);
    uint8_t*     raw       = (uint8_t*)m_stage_raw;
    uint8_t*     raw_begin = raw + ref.offset + offset;
    std::fill(raw_begin, raw_begin + size_, c);
    markDirty(ref.offset + offset, size_);
    return true;
}

bool StagingBuffer::recordUpload(vvk::CommandBuffer& cmd) {
    bool gpu_created = false;
    if (! m_gpu_buf.handle) {
        if (auto opt = CreateGpuBuffer(m_device.vma_allocator(), m_usage, m_stage_buf.req_size);
            opt.has_value()) {
            m_gpu_buf = std::move(opt.value());
            gpu_created = true;
        } else
            return false;
    }
    if (gpu_created) markDirty(0, m_stage_buf.req_size);
    if (m_dirty_ranges.empty()) return true;

    std::sort(m_dirty_ranges.begin(), m_dirty_ranges.end(), [](const auto& a, const auto& b) {
        return a.offset < b.offset;
    });
    std::vector<DirtyRange> merged;
    merged.reserve(m_dirty_ranges.size());
    for (const auto& r : m_dirty_ranges) {
        if (r.size == 0) continue;
        if (merged.empty()) {
            merged.push_back(r);
            continue;
        }
        auto& last     = merged.back();
        auto  last_end = last.offset + last.size;
        if (r.offset <= last_end) {
            auto end = std::max(last_end, r.offset + r.size);
            last.size = end - last.offset;
        } else {
            merged.push_back(r);
        }
    }

    // Keep the staging allocation persistently mapped: it is HOST_VISIBLE
    // (VMA CPU_ONLY) so the pointer stays valid for the buffer's lifetime, and
    // vmaFlushAllocation makes writes visible to the GPU regardless of map
    // state. Previously this unmapped every frame and writeToBuf/fillBuf
    // remapped on the next frame — pure per-frame overhead. The map obtained in
    // allocate()/increaseBuf() is now released once, in destroy().
    VVK_CHECK_BOOL_RE(vmaFlushAllocation(
        m_device.vma_allocator(), m_stage_buf.handle.Allocation(), 0, VK_WHOLE_SIZE));
    RecordCopyBuffer(m_gpu_buf, m_stage_buf, merged, cmd);
    m_dirty_ranges.clear();
    return true;
}

VkBuffer StagingBuffer::gpuBuf() const { return *m_gpu_buf.handle; }
