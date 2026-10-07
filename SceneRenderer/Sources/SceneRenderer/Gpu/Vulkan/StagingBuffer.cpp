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
std::vector<StagingBuffer::DirtyRange>
MergeRanges(std::span<const StagingBuffer::DirtyRange> ranges) {
    std::vector<StagingBuffer::DirtyRange> sorted(ranges.begin(), ranges.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.offset < b.offset;
    });
    std::vector<StagingBuffer::DirtyRange> merged;
    merged.reserve(sorted.size());
    for (const auto& r : sorted) {
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
    return merged;
}

std::vector<StagingBuffer::DirtyRange>
IntersectRanges(std::span<const StagingBuffer::DirtyRange> dirty,
                std::span<const StagingBuffer::DirtyRange> live) {
    auto merged_dirty = MergeRanges(dirty);
    auto merged_live  = MergeRanges(live);
    std::vector<StagingBuffer::DirtyRange> result;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < merged_dirty.size() && j < merged_live.size()) {
        const auto dirty_end = merged_dirty[i].offset + merged_dirty[i].size;
        const auto live_end  = merged_live[j].offset + merged_live[j].size;
        const auto begin     = std::max(merged_dirty[i].offset, merged_live[j].offset);
        const auto end       = std::min(dirty_end, live_end);
        if (begin < end) result.push_back({ .offset = begin, .size = end - begin });
        if (dirty_end < live_end)
            ++i;
        else
            ++j;
    }
    return result;
}

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
    if (m_upload_active) return false;
    if (m_stage_raw == nullptr) {
        VVK_CHECK_BOOL_RE(mapStageBuf());
    }
    if (nsize > std::numeric_limits<VkDeviceSize>::max() - m_stage_buf.req_size) return false;
    auto current  = static_cast<VkDeviceSize>(m_stage_buf.req_size);
    auto required = current + nsize;
    auto growth   = std::max(current / 2, m_size_step);
    auto newsize  = growth > std::numeric_limits<VkDeviceSize>::max() - current
                        ? required
                        : std::max(required, current + growth);

    VmaBufferParameters new_stage;
    if (! CreateStagingBuffer(m_device.vma_allocator(), newsize, new_stage)) return false;
    void* new_raw = nullptr;
    if (new_stage.handle.MapMemory(&new_raw) != VK_SUCCESS) return false;
    if (new_raw == nullptr) {
        new_stage.handle.UnMapMemory();
        return false;
    }
    std::memcpy(new_raw, m_stage_raw, m_stage_buf.req_size);

    m_stage_buf.handle.UnMapMemory();
    m_stage_buf = std::move(new_stage);
    m_stage_raw = new_raw;

    m_gpu_buf.handle = nullptr;
    rstd_info("increase buffer size: {}", newsize);
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
    m_live_ranges.clear();
    cancelUpload();
}

bool StagingBuffer::allocateSubRef(VkDeviceSize size, StagingBufferRef& ref,
                                   VkDeviceSize alignment) {
    if (m_upload_active) return false;
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
                m_live_ranges.push_back({ .offset = ref.offset, .size = ref.size });
                return true;
            }
        }
    }

    auto  old_block_num = m_virtual_blocks.size();
    auto* p_block       = newVirtualBlock(size);
    if (p_block == nullptr) return false;

    auto& block = *p_block;
    if (old_block_num < m_virtual_blocks.size()) {
        auto required = block.offset + block.size;
        if (required > m_stage_buf.req_size &&
            ! increaseBuf(required - m_stage_buf.req_size)) {
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
    m_live_ranges.push_back({ .offset = ref.offset, .size = ref.size });
    return true;
}
void StagingBuffer::unallocateSubRef(const StagingBufferRef& ref) {
    CHECK_REF(ref, ;);
    if (m_upload_active) return;
    if (ref.m_virtual_index < m_virtual_blocks.size()) {
        auto& block = m_virtual_blocks[ref.m_virtual_index];
        vmaVirtualFree(block.handle, ref.m_allocation);
        if (block.enabled && vmaIsVirtualBlockEmpty(block.handle)) {
            vmaDestroyVirtualBlock(block.handle);
            block.handle  = VK_NULL_HANDLE;
            block.enabled = false;
        }
        auto live = std::find_if(m_live_ranges.begin(), m_live_ranges.end(), [&ref](const auto& r) {
            return r.offset == ref.offset && r.size == ref.size;
        });
        if (live != m_live_ranges.end()) m_live_ranges.erase(live);
    } else {
        rstd_error("unallocate stagingbuffer failed: wrong index {}", ref.m_virtual_index);
    }
}

VkResult StagingBuffer::mapStageBuf() { return m_stage_buf.handle.MapMemory(&m_stage_raw); }

bool StagingBuffer::writeToBuf(const StagingBufferRef& ref, std::span<uint8_t> data, size_t offset,
                               bool skip_unchanged) {
    CHECK_REF(ref, return false);
    if (m_upload_active) return false;

    if (m_stage_raw == nullptr && (mapStageBuf() != VK_SUCCESS || m_stage_raw == nullptr))
        return false;
    // `offset` comes from shader-reflected uniform offsets, so it is only as
    // trustworthy as the reflection. Without this guard `ref.size - offset`
    // wraps around and the copy runs past the mapped staging region.
    if (offset >= ref.size) {
        rstd_error("stage write out of range: offset {} >= size {}", offset, ref.size);
        return false;
    }
    VkDeviceSize size = std::min<VkDeviceSize>(ref.size - offset, data.size());
    uint8_t*     raw  = (uint8_t*)m_stage_raw;
    if (skip_unchanged && size > 0 && size <= 256 &&
        std::memcmp(raw + ref.offset + offset, data.data(), size) == 0)
        return true;
    std::copy(data.begin(), data.begin() + size, raw + ref.offset + offset);
    markDirty(ref.offset + offset, size);
    return true;
}

bool StagingBuffer::fillBuf(const StagingBufferRef& ref, size_t offset, size_t size, uint8_t c) {
    CHECK_REF(ref, return false);
    if (m_upload_active) return false;

    if (m_stage_raw == nullptr && (mapStageBuf() != VK_SUCCESS || m_stage_raw == nullptr))
        return false;
    if (offset >= ref.size) {
        rstd_error("stage fill out of range: offset {} >= size {}", offset, ref.size);
        return false;
    }
    VkDeviceSize size_     = std::min<VkDeviceSize>(ref.size - offset, size);
    uint8_t*     raw       = (uint8_t*)m_stage_raw;
    uint8_t*     raw_begin = raw + ref.offset + offset;
    std::fill(raw_begin, raw_begin + size_, c);
    markDirty(ref.offset + offset, size_);
    return true;
}

bool StagingBuffer::recordUpload(vvk::CommandBuffer& cmd) {
    if (! beginUpload()) return false;
    while (hasUploadChunk()) {
        if (! recordUploadChunk(cmd, std::numeric_limits<VkDeviceSize>::max())) {
            cancelUpload();
            return false;
        }
    }
    finishUpload();
    completeUpload();
    return true;
}

bool StagingBuffer::beginUpload() {
    if (m_upload_active) return false;
    bool gpu_created = false;
    if (! m_gpu_buf.handle) {
        if (auto opt = CreateGpuBuffer(m_device.vma_allocator(), m_usage, m_stage_buf.req_size);
            opt.has_value()) {
            m_gpu_buf = std::move(opt.value());
            gpu_created = true;
        } else
            return false;
    }
    m_upload_ranges = gpu_created ? MergeRanges(m_live_ranges)
                                  : IntersectRanges(m_dirty_ranges, m_live_ranges);
    for (const auto& range : m_upload_ranges) {
        VVK_CHECK_ACT(
            {
                m_upload_ranges.clear();
                return false;
            },
            vmaFlushAllocation(m_device.vma_allocator(),
                               m_stage_buf.handle.Allocation(),
                               range.offset,
                               range.size));
    }
    m_upload_range_index  = 0;
    m_upload_range_offset = 0;
    m_upload_active       = true;
    return true;
}

bool StagingBuffer::hasUploadChunk() const {
    return m_upload_active && m_upload_range_index < m_upload_ranges.size();
}

bool StagingBuffer::recordUploadChunk(vvk::CommandBuffer& cmd, VkDeviceSize max_bytes) {
    if (! m_upload_active || max_bytes == 0) return false;
    std::vector<DirtyRange> chunk;
    VkDeviceSize remaining = max_bytes;
    while (remaining > 0 && m_upload_range_index < m_upload_ranges.size()) {
        const auto& range     = m_upload_ranges[m_upload_range_index];
        const auto  available = range.size - m_upload_range_offset;
        const auto  size      = std::min(available, remaining);
        chunk.push_back({ .offset = range.offset + m_upload_range_offset, .size = size });
        remaining -= size;
        m_upload_range_offset += size;
        if (m_upload_range_offset == range.size) {
            ++m_upload_range_index;
            m_upload_range_offset = 0;
        }
    }
    if (chunk.empty()) return false;
    RecordCopyBuffer(m_gpu_buf, m_stage_buf, chunk, cmd);
    return true;
}

void StagingBuffer::finishUpload() {
    if (! m_upload_active || hasUploadChunk()) return;
    m_dirty_ranges.clear();
}

void StagingBuffer::completeUpload() {
    if (! m_upload_active || hasUploadChunk()) return;
    cancelUpload();
}

void StagingBuffer::cancelUpload() {
    m_upload_ranges.clear();
    m_upload_range_index  = 0;
    m_upload_range_offset = 0;
    m_upload_active       = false;
}

VkBuffer StagingBuffer::gpuBuf() const { return *m_gpu_buf.handle; }
