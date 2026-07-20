module;

#include <vulkan/vulkan_core.h>

export module sr.vulkan_render:resource_key;
import rstd.cppstd;
import sr.types;
import sr.vulkan;

export namespace sr::vulkan
{

struct PipelineResourceRequest {
    std::vector<DescriptorSetInfo>                 descriptor_sets;
    std::vector<VkVertexInputBindingDescription>   vertex_bindings;
    std::vector<VkVertexInputAttributeDescription> vertex_attrs;
    std::vector<Uni_ShaderSpv>                     shader_stages;
    VkPipelineColorBlendAttachmentState            color_blend {};
    VkPipelineColorBlendStateCreateFlags           color_blend_flags { 0 };
    std::array<float, 4>                           blend_constants { 0.0f, 0.0f, 0.0f, 0.0f };
    VkPipelineDepthStencilStateCreateInfo          depth {};
    VkPipelineRasterizationStateCreateInfo         raster {};
    VkPipelineMultisampleStateCreateInfo           multisample {};
    VkPipelineCreateFlags                          create_flags { 0 };
    VkPrimitiveTopology         topology { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    uint32_t                    subpass { 0 };
    bool                        primitive_restart_enable { false };
    uint32_t                    viewport_count { 1 };
    uint32_t                    scissor_count { 1 };
    bool                        logic_op_enable { false };
    VkLogicOp                   logic_op { VK_LOGIC_OP_COPY };
    std::vector<VkDynamicState> dynamic_states { VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR };
    VkFormat                    color_format { VK_FORMAT_R8G8B8A8_UNORM };
    VkImageLayout               color_final_layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentLoadOp          color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentLoadOp          depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    bool                        has_depth_attachment { false };
};

struct PipelineCacheKey {
    std::size_t            value { 0 };
    std::vector<std::byte> bytes;
};

struct RenderPassCacheKey {
    std::size_t            value { 0 };
    std::vector<std::byte> bytes;
};

struct FramebufferCacheKey {
    std::size_t            value { 0 };
    std::vector<std::byte> bytes;
};

struct FramebufferAttachmentIdentity {
    std::size_t            value { 0 };
    std::vector<std::byte> bytes;
};

struct FramebufferAttachmentDesc {
    VkImageView                   view { VK_NULL_HANDLE };
    FramebufferAttachmentIdentity identity;
};

struct FramebufferResourceRequest {
    VkRenderPass                           render_pass { VK_NULL_HANDLE };
    RenderPassCacheKey                     render_pass_key;
    std::vector<FramebufferAttachmentDesc> attachments;
    VkExtent2D                             extent { 0, 0 };
    uint32_t                               layers { 1 };
};

struct RenderPassResourceDesc {
    VkFormat              color_format { VK_FORMAT_R8G8B8A8_UNORM };
    VkFormat              depth_format { VK_FORMAT_D32_SFLOAT };
    VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };
    VkImageLayout         color_initial_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageLayout         color_final_layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentLoadOp    color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   color_store_op { VK_ATTACHMENT_STORE_OP_STORE };
    VkAttachmentLoadOp    color_stencil_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   color_stencil_store_op { VK_ATTACHMENT_STORE_OP_DONT_CARE };
    VkImageLayout         color_attachment_layout { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkImageLayout         resolve_initial_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageLayout         resolve_final_layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentLoadOp    resolve_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   resolve_store_op { VK_ATTACHMENT_STORE_OP_STORE };
    VkAttachmentLoadOp    resolve_stencil_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   resolve_stencil_store_op { VK_ATTACHMENT_STORE_OP_DONT_CARE };
    VkImageLayout         resolve_attachment_layout { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkImageLayout         depth_initial_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageLayout         depth_final_layout { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentLoadOp    depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   depth_store_op { VK_ATTACHMENT_STORE_OP_STORE };
    VkAttachmentLoadOp    depth_stencil_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp   depth_stencil_store_op { VK_ATTACHMENT_STORE_OP_DONT_CARE };
    VkImageLayout depth_attachment_layout { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    bool          has_depth_attachment { false };
};

struct PipelineResourceDesc {
    std::vector<DescriptorSetInfo>                 descriptor_sets;
    std::vector<VkVertexInputBindingDescription>   vertex_bindings;
    std::vector<VkVertexInputAttributeDescription> vertex_attrs;
    std::vector<ShaderSpv>                         shader_stages;
    VkPipelineColorBlendAttachmentState            color_blend {};
    VkPipelineColorBlendStateCreateFlags           color_blend_flags { 0 };
    std::array<float, 4>                           blend_constants { 0.0f, 0.0f, 0.0f, 0.0f };
    VkPipelineDepthStencilStateCreateInfo          depth {};
    VkPipelineRasterizationStateCreateInfo         raster {};
    VkPipelineMultisampleStateCreateInfo           multisample {};
    VkPipelineCreateFlags                          create_flags { 0 };
    VkPrimitiveTopology         topology { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    uint32_t                    subpass { 0 };
    bool                        primitive_restart_enable { false };
    uint32_t                    viewport_count { 1 };
    uint32_t                    scissor_count { 1 };
    bool                        logic_op_enable { false };
    VkLogicOp                   logic_op { VK_LOGIC_OP_COPY };
    std::vector<VkDynamicState> dynamic_states { VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR };
    RenderPassResourceDesc      render_pass;
};

struct FramebufferResourceDesc {
    VkRenderPass                           render_pass { VK_NULL_HANDLE };
    RenderPassCacheKey                     render_pass_key;
    std::vector<FramebufferAttachmentDesc> attachments;
    VkExtent2D                             extent { 0, 0 };
    uint32_t                               layers { 1 };
};

struct PipelineCacheProbe {
    PipelineCacheKey key;
    bool             hit { false };
    uint64_t         observed_count { 0 };
};

inline bool SamePipelineCacheKey(const PipelineCacheKey& lhs, const PipelineCacheKey& rhs) {
    return lhs.value == rhs.value && lhs.bytes.data() == rhs.bytes.data();
}

inline bool SameRenderPassCacheKey(const RenderPassCacheKey& lhs, const RenderPassCacheKey& rhs) {
    return lhs.value == rhs.value && lhs.bytes.data() == rhs.bytes.data();
}

inline bool SameFramebufferCacheKey(const FramebufferCacheKey& lhs,
                                    const FramebufferCacheKey& rhs) {
    return lhs.value == rhs.value && lhs.bytes.data() == rhs.bytes.data();
}

struct CanonicalCacheKeyData {
    std::size_t            value { 0 };
    std::vector<std::byte> bytes;
};

class PipelineKeyWriter {
public:
    void writeArraySize(std::size_t value) {
        writeType(ValueType::ArraySize);
        writeRawU64(static_cast<std::uint64_t>(value));
    }

    void writeBool(bool value) {
        writeType(ValueType::Bool);
        writeRawU8(static_cast<std::uint8_t>(value ? 1u : 0u));
    }

    void writeU32(std::uint32_t value) {
        writeType(ValueType::U32);
        writeRawU32(value);
    }

    void writeU64(std::uint64_t value) {
        writeType(ValueType::U64);
        writeRawU64(value);
    }

    void writeF32(float value) {
        writeType(ValueType::F32);
        writeRawU32(std::bit_cast<std::uint32_t>(value));
    }

    void writeString(std::string_view value) {
        writeType(ValueType::String);
        writeRawU64(static_cast<std::uint64_t>(value.size()));
        for (unsigned char ch : value) writeRawU8(static_cast<std::uint8_t>(ch));
    }

    void writeBytes(std::span<const std::byte> value) {
        writeType(ValueType::Bytes);
        writeRawU64(static_cast<std::uint64_t>(value.size()));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    }

    CanonicalCacheKeyData finish() && {
        return CanonicalCacheKeyData {
            .value = HashCanonicalBytes(std::span<const std::byte>(m_bytes.data(), m_bytes.size())),
            .bytes = std::move(m_bytes),
        };
    }

private:
    enum class ValueType : std::uint8_t
    {
        U32       = 1,
        U64       = 2,
        Bool      = 3,
        F32       = 4,
        String    = 5,
        ArraySize = 6,
        Bytes     = 7,
    };

    static std::size_t HashCanonicalBytes(std::span<const std::byte> bytes) {
        std::uint64_t hash { 1469598103934665603ull };
        for (auto byte : bytes) {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }

    void writeType(ValueType type) { writeRawU8(static_cast<std::uint8_t>(type)); }

    void writeRawU8(std::uint8_t value) { m_bytes.push_back(static_cast<std::byte>(value)); }

    void writeRawU32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            writeRawU8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void writeRawU64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            writeRawU8(static_cast<std::uint8_t>((value >> shift) & 0xffull));
        }
    }

    std::vector<std::byte> m_bytes;
};

struct CanonicalCacheKeyHash {
    template<typename T>
    std::size_t operator()(const T& key) const {
        return key.value;
    }
};

struct PipelineCacheKeyEqual {
    bool operator()(const PipelineCacheKey& lhs, const PipelineCacheKey& rhs) const {
        return SamePipelineCacheKey(lhs, rhs);
    }
};

struct RenderPassCacheKeyEqual {
    bool operator()(const RenderPassCacheKey& lhs, const RenderPassCacheKey& rhs) const {
        return SameRenderPassCacheKey(lhs, rhs);
    }
};

struct FramebufferCacheKeyEqual {
    bool operator()(const FramebufferCacheKey& lhs, const FramebufferCacheKey& rhs) const {
        return SameFramebufferCacheKey(lhs, rhs);
    }
};

template<typename T>
inline void WritePipelineScalar(PipelineKeyWriter& writer, T value) {
    writer.writeU64(static_cast<std::uint64_t>(value));
}

inline void WriteCacheKey(PipelineKeyWriter& writer, const RenderPassCacheKey& key) {
    writer.writeU64(static_cast<std::uint64_t>(key.value));
    writer.writeBytes(std::span<const std::byte>(key.bytes.data(), key.bytes.size()));
}

inline void WritePipelineShaderStages(PipelineKeyWriter&         writer,
                                      std::span<const ShaderSpv> stages) {
    struct StageRecord {
        sr::ShaderType                  stage;
        std::string_view                 entry_point;
        const std::vector<unsigned int>* spirv { nullptr };
    };

    std::vector<StageRecord> records;
    records.reserve(stages.size());
    for (const auto& stage : stages) {
        records.push_back(StageRecord {
            .stage       = stage.stage,
            .entry_point = stage.entry_point,
            .spirv       = &stage.spirv,
        });
    }
    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        const auto lhs_stage = static_cast<std::uint32_t>(lhs.stage);
        const auto rhs_stage = static_cast<std::uint32_t>(rhs.stage);
        if (lhs.stage != rhs.stage) {
            return static_cast<int>(lhs.stage) < static_cast<int>(rhs.stage);
        }
        return lhs.entry_point.compare(rhs.entry_point) < 0;
    });

    writer.writeArraySize(records.size());
    for (const auto& record : records) {
        WritePipelineScalar(writer, record.stage);
        writer.writeString(record.entry_point);
        writer.writeArraySize(record.spirv != nullptr ? record.spirv->size() : std::size_t { 0 });
        if (record.spirv != nullptr) {
            for (auto word : *record.spirv) writer.writeU32(static_cast<std::uint32_t>(word));
        }
    }
}

inline void WritePipelineDescriptorSets(PipelineKeyWriter&                 writer,
                                        std::span<const DescriptorSetInfo> sets) {
    writer.writeArraySize(sets.size());
    for (const auto& set : sets) {
        writer.writeBool(set.push_descriptor);
        auto bindings = set.bindings;
        std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.binding < rhs.binding;
        });
        writer.writeArraySize(bindings.size());
        for (const auto& binding : bindings) {
            writer.writeU32(binding.binding);
            WritePipelineScalar(writer, binding.descriptorType);
            writer.writeU32(binding.descriptorCount);
            writer.writeU32(binding.stageFlags);
            writer.writeArraySize(binding.pImmutableSamplers != nullptr
                                      ? static_cast<std::size_t>(binding.descriptorCount)
                                      : std::size_t { 0 });
            if (binding.pImmutableSamplers != nullptr) {
                for (uint32_t i = 0; i < binding.descriptorCount; ++i) {
                    writer.writeU64(static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(binding.pImmutableSamplers[i])));
                }
            }
        }
    }
}

inline void WritePipelineVertexInput(PipelineKeyWriter&                                 writer,
                                     std::span<const VkVertexInputBindingDescription>   bindings,
                                     std::span<const VkVertexInputAttributeDescription> attrs) {
    auto sorted_bindings =
        std::vector<VkVertexInputBindingDescription>(bindings.begin(), bindings.end());
    std::sort(sorted_bindings.begin(), sorted_bindings.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.binding < rhs.binding;
    });

    auto sorted_attrs = std::vector<VkVertexInputAttributeDescription>(attrs.begin(), attrs.end());
    std::sort(sorted_attrs.begin(), sorted_attrs.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.location != rhs.location) return lhs.location < rhs.location;
        return lhs.binding < rhs.binding;
    });

    writer.writeArraySize(sorted_bindings.size());
    for (const auto& binding : sorted_bindings) {
        writer.writeU32(binding.binding);
        writer.writeU32(binding.stride);
        WritePipelineScalar(writer, binding.inputRate);
    }
    writer.writeArraySize(sorted_attrs.size());
    for (const auto& attr : sorted_attrs) {
        writer.writeU32(attr.location);
        writer.writeU32(attr.binding);
        WritePipelineScalar(writer, attr.format);
        writer.writeU32(attr.offset);
    }
}

inline void WritePipelineColorBlend(PipelineKeyWriter&                         writer,
                                    const VkPipelineColorBlendAttachmentState& state) {
    writer.writeBool(state.blendEnable == VK_TRUE);
    WritePipelineScalar(writer, state.srcColorBlendFactor);
    WritePipelineScalar(writer, state.dstColorBlendFactor);
    WritePipelineScalar(writer, state.colorBlendOp);
    WritePipelineScalar(writer, state.srcAlphaBlendFactor);
    WritePipelineScalar(writer, state.dstAlphaBlendFactor);
    WritePipelineScalar(writer, state.alphaBlendOp);
    writer.writeU32(state.colorWriteMask);
}

inline void WritePipelineColorBlendState(PipelineKeyWriter&          writer,
                                         const PipelineResourceDesc& desc) {
    writer.writeU32(desc.color_blend_flags);
    writer.writeBool(desc.logic_op_enable);
    WritePipelineScalar(writer, desc.logic_op);
    for (auto value : desc.blend_constants) writer.writeF32(value);
    writer.writeArraySize(1u);
    WritePipelineColorBlend(writer, desc.color_blend);
}

inline void WritePipelineInputAssembly(PipelineKeyWriter&          writer,
                                       const PipelineResourceDesc& desc) {
    WritePipelineScalar(writer, desc.topology);
    writer.writeBool(desc.primitive_restart_enable);
}

inline void WritePipelineViewportState(PipelineKeyWriter&          writer,
                                       const PipelineResourceDesc& desc) {
    writer.writeU32(desc.viewport_count);
    writer.writeU32(desc.scissor_count);
}

inline void WritePipelineDynamicStates(PipelineKeyWriter&              writer,
                                       std::span<const VkDynamicState> states) {
    auto sorted_states = std::vector<VkDynamicState>(states.begin(), states.end());
    std::sort(sorted_states.begin(), sorted_states.end());
    writer.writeArraySize(sorted_states.size());
    for (auto state : sorted_states) WritePipelineScalar(writer, state);
}

template<typename T>
inline void WritePipelineStencil(PipelineKeyWriter& writer, const T& state) {
    WritePipelineScalar(writer, state.failOp);
    WritePipelineScalar(writer, state.passOp);
    WritePipelineScalar(writer, state.depthFailOp);
    WritePipelineScalar(writer, state.compareOp);
    writer.writeU32(state.compareMask);
    writer.writeU32(state.writeMask);
    writer.writeU32(state.reference);
}

inline void WritePipelineDepthStencil(PipelineKeyWriter&                           writer,
                                      const VkPipelineDepthStencilStateCreateInfo& state) {
    writer.writeU32(state.flags);
    writer.writeBool(state.depthTestEnable == VK_TRUE);
    writer.writeBool(state.depthWriteEnable == VK_TRUE);
    WritePipelineScalar(writer, state.depthCompareOp);
    writer.writeBool(state.depthBoundsTestEnable == VK_TRUE);
    writer.writeBool(state.stencilTestEnable == VK_TRUE);
    WritePipelineStencil(writer, state.front);
    WritePipelineStencil(writer, state.back);
    writer.writeF32(state.minDepthBounds);
    writer.writeF32(state.maxDepthBounds);
}

inline void WritePipelineRaster(PipelineKeyWriter&                            writer,
                                const VkPipelineRasterizationStateCreateInfo& state) {
    writer.writeU32(state.flags);
    writer.writeBool(state.depthClampEnable == VK_TRUE);
    writer.writeBool(state.rasterizerDiscardEnable == VK_TRUE);
    WritePipelineScalar(writer, state.polygonMode);
    WritePipelineScalar(writer, state.cullMode);
    WritePipelineScalar(writer, state.frontFace);
    writer.writeBool(state.depthBiasEnable == VK_TRUE);
    writer.writeF32(state.depthBiasConstantFactor);
    writer.writeF32(state.depthBiasClamp);
    writer.writeF32(state.depthBiasSlopeFactor);
    writer.writeF32(state.lineWidth);
}

inline uint32_t SampleMaskWordCount(VkSampleCountFlagBits samples) {
    auto count = static_cast<uint32_t>(samples);
    return (count + 31u) / 32u;
}

inline void WritePipelineMultisample(PipelineKeyWriter&                          writer,
                                     const VkPipelineMultisampleStateCreateInfo& state) {
    writer.writeU32(state.flags);
    WritePipelineScalar(writer, state.rasterizationSamples);
    writer.writeBool(state.sampleShadingEnable == VK_TRUE);
    writer.writeF32(state.minSampleShading);
    const auto mask_word_count = SampleMaskWordCount(state.rasterizationSamples);
    writer.writeArraySize(state.pSampleMask != nullptr ? static_cast<std::size_t>(mask_word_count)
                                                       : std::size_t { 0 });
    if (state.pSampleMask != nullptr) {
        for (uint32_t i = 0; i < mask_word_count; ++i) {
            writer.writeU32(state.pSampleMask[i]);
        }
    }
    writer.writeBool(state.alphaToCoverageEnable == VK_TRUE);
    writer.writeBool(state.alphaToOneEnable == VK_TRUE);
}

inline RenderPassResourceDesc MakeRenderPassResourceDesc(const PipelineResourceRequest& request) {
    RenderPassResourceDesc desc {
        .color_format         = request.color_format,
        .depth_format         = VK_FORMAT_D32_SFLOAT,
        .samples              = request.multisample.rasterizationSamples,
        .color_final_layout   = request.color_final_layout,
        .color_load_op        = request.color_load_op,
        .resolve_final_layout = request.color_final_layout,
        .depth_load_op        = request.depth_load_op,
        .has_depth_attachment = request.has_depth_attachment,
    };
    if (request.color_load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
        desc.color_initial_layout =
            request.multisample.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (request.depth_load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
        desc.depth_initial_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return desc;
}

inline PipelineResourceDesc MakePipelineResourceDesc(const PipelineResourceRequest& request) {
    std::vector<ShaderSpv> shader_stages;
    shader_stages.reserve(request.shader_stages.size());
    for (const auto& stage : request.shader_stages) {
        if (stage) shader_stages.push_back(*stage);
    }

    return PipelineResourceDesc {
        .descriptor_sets          = request.descriptor_sets,
        .vertex_bindings          = request.vertex_bindings,
        .vertex_attrs             = request.vertex_attrs,
        .shader_stages            = std::move(shader_stages),
        .color_blend              = request.color_blend,
        .color_blend_flags        = request.color_blend_flags,
        .blend_constants          = request.blend_constants,
        .depth                    = request.depth,
        .raster                   = request.raster,
        .multisample              = request.multisample,
        .create_flags             = request.create_flags,
        .topology                 = request.topology,
        .subpass                  = request.subpass,
        .primitive_restart_enable = request.primitive_restart_enable,
        .viewport_count           = request.viewport_count,
        .scissor_count            = request.scissor_count,
        .logic_op_enable          = request.logic_op_enable,
        .logic_op                 = request.logic_op,
        .dynamic_states           = request.dynamic_states,
        .render_pass              = MakeRenderPassResourceDesc(request),
    };
}

inline FramebufferResourceDesc
MakeFramebufferResourceDesc(const FramebufferResourceRequest& request) {
    return FramebufferResourceDesc {
        .render_pass     = request.render_pass,
        .render_pass_key = request.render_pass_key,
        .attachments     = request.attachments,
        .extent          = request.extent,
        .layers          = request.layers,
    };
}

inline void WriteRenderPassDesc(PipelineKeyWriter& writer, const RenderPassResourceDesc& desc) {
    const bool has_resolve = desc.samples != VK_SAMPLE_COUNT_1_BIT;
    WritePipelineScalar(writer, desc.color_format);
    WritePipelineScalar(writer, desc.depth_format);
    WritePipelineScalar(writer, desc.samples);
    WritePipelineScalar(writer, desc.color_initial_layout);
    WritePipelineScalar(writer, desc.color_final_layout);
    WritePipelineScalar(writer, desc.color_load_op);
    WritePipelineScalar(writer, desc.color_store_op);
    WritePipelineScalar(writer, desc.color_stencil_load_op);
    WritePipelineScalar(writer, desc.color_stencil_store_op);
    WritePipelineScalar(writer, desc.color_attachment_layout);
    writer.writeBool(has_resolve);
    if (has_resolve) {
        WritePipelineScalar(writer, desc.resolve_initial_layout);
        WritePipelineScalar(writer, desc.resolve_final_layout);
        WritePipelineScalar(writer, desc.resolve_load_op);
        WritePipelineScalar(writer, desc.resolve_store_op);
        WritePipelineScalar(writer, desc.resolve_stencil_load_op);
        WritePipelineScalar(writer, desc.resolve_stencil_store_op);
        WritePipelineScalar(writer, desc.resolve_attachment_layout);
    }
    WritePipelineScalar(writer, desc.depth_initial_layout);
    WritePipelineScalar(writer, desc.depth_final_layout);
    WritePipelineScalar(writer, desc.depth_load_op);
    WritePipelineScalar(writer, desc.depth_store_op);
    WritePipelineScalar(writer, desc.depth_stencil_load_op);
    WritePipelineScalar(writer, desc.depth_stencil_store_op);
    WritePipelineScalar(writer, desc.depth_attachment_layout);
    writer.writeBool(desc.has_depth_attachment);
}

inline PipelineCacheKey ToPipelineCacheKey(CanonicalCacheKeyData data) {
    return PipelineCacheKey { .value = data.value, .bytes = std::move(data.bytes) };
}

inline RenderPassCacheKey ToRenderPassCacheKey(CanonicalCacheKeyData data) {
    return RenderPassCacheKey { .value = data.value, .bytes = std::move(data.bytes) };
}

inline FramebufferCacheKey ToFramebufferCacheKey(CanonicalCacheKeyData data) {
    return FramebufferCacheKey { .value = data.value, .bytes = std::move(data.bytes) };
}

inline FramebufferAttachmentIdentity ToFramebufferAttachmentIdentity(CanonicalCacheKeyData data) {
    return FramebufferAttachmentIdentity { .value = data.value, .bytes = std::move(data.bytes) };
}

inline RenderPassCacheKey MakeRenderPassCacheKey(const RenderPassResourceDesc& desc);

inline PipelineCacheKey MakePipelineCacheKey(const PipelineResourceDesc& desc) {
    PipelineKeyWriter writer;
    writer.writeString("pipeline-v1");
    writer.writeU32(desc.create_flags);
    writer.writeU32(desc.subpass);
    WriteCacheKey(writer, MakeRenderPassCacheKey(desc.render_pass));
    WritePipelineShaderStages(writer, desc.shader_stages);
    WritePipelineDescriptorSets(writer, desc.descriptor_sets);
    WritePipelineVertexInput(writer, desc.vertex_bindings, desc.vertex_attrs);
    WritePipelineInputAssembly(writer, desc);
    WritePipelineViewportState(writer, desc);
    WritePipelineColorBlendState(writer, desc);
    WritePipelineDepthStencil(writer, desc.depth);
    WritePipelineRaster(writer, desc.raster);
    WritePipelineMultisample(writer, desc.multisample);
    WritePipelineDynamicStates(writer, desc.dynamic_states);
    return ToPipelineCacheKey(std::move(writer).finish());
}

inline PipelineCacheKey MakePipelineCacheKey(const PipelineResourceRequest& request) {
    return MakePipelineCacheKey(MakePipelineResourceDesc(request));
}

inline RenderPassCacheKey MakeRenderPassCacheKey(const RenderPassResourceDesc& desc) {
    PipelineKeyWriter writer;
    writer.writeString("render-pass-v1");
    WriteRenderPassDesc(writer, desc);
    return ToRenderPassCacheKey(std::move(writer).finish());
}

inline RenderPassCacheKey MakeRenderPassCacheKey(const PipelineResourceRequest& request) {
    return MakeRenderPassCacheKey(MakeRenderPassResourceDesc(request));
}

inline FramebufferCacheKey MakeFramebufferCacheKey(const FramebufferResourceDesc& desc) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-v1");
    WriteCacheKey(writer, desc.render_pass_key);
    writer.writeArraySize(desc.attachments.size());
    for (const auto& attachment : desc.attachments) {
        writer.writeU64(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(attachment.view)));
        writer.writeU64(static_cast<std::uint64_t>(attachment.identity.value));
        writer.writeBytes(std::span<const std::byte>(attachment.identity.bytes.data(),
                                                     attachment.identity.bytes.size()));
    }
    writer.writeU32(desc.extent.width);
    writer.writeU32(desc.extent.height);
    writer.writeU32(desc.layers);
    return ToFramebufferCacheKey(std::move(writer).finish());
}

inline FramebufferCacheKey MakeFramebufferCacheKey(const FramebufferResourceRequest& request) {
    return MakeFramebufferCacheKey(MakeFramebufferResourceDesc(request));
}

} // namespace sr::vulkan
