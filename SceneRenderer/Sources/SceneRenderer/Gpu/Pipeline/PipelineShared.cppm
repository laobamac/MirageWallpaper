module;

export module sr.vulkan_render:pass_common;
import rstd.cppstd;
import sr.types;
import sr.vulkan;
import sr.scene;

export namespace sr::vulkan
{

inline void SetBlend(BlendMode bm, VkPipelineColorBlendAttachmentState& state) {
    state.blendEnable  = true;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (bm) {
    case BlendMode::Disable: state.blendEnable = false; break;
    case BlendMode::Normal:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case BlendMode::Translucent:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
}
inline void SetAttachmentLoadOp(BlendMode bm, VkAttachmentLoadOp& load_op) {
    switch (bm) {
    case BlendMode::Disable:
    case BlendMode::Normal: load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
    case BlendMode::Additive:
    case BlendMode::Translucent: load_op = VK_ATTACHMENT_LOAD_OP_LOAD; break;
    }
}

inline bool IsDepthWritingBlendMode(BlendMode bm) {
    switch (bm) {
    case BlendMode::Disable:
    case BlendMode::Normal: return true;
    case BlendMode::Additive:
    case BlendMode::Translucent: return false;
    }
    return false;
}

inline bool EffectiveDepthWrite(const SceneMaterial& material) {
    return material.depth_write && IsDepthWritingBlendMode(material.blenmode);
}

inline bool UsesDepthAttachment(const SceneMaterial& material) {
    return material.depth_test || EffectiveDepthWrite(material);
}

inline void SetDepthState(const SceneMaterial&                   material,
                          VkPipelineDepthStencilStateCreateInfo& state) {
    state.depthTestEnable  = material.depth_test;
    state.depthWriteEnable = EffectiveDepthWrite(material);
    state.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
}

inline void SetCullMode(CullMode mode, VkPipelineRasterizationStateCreateInfo& state) {
    switch (mode) {
    case CullMode::Front: state.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    case CullMode::Back: state.cullMode = VK_CULL_MODE_BACK_BIT; break;
    case CullMode::None: state.cullMode = VK_CULL_MODE_NONE; break;
    }
}

inline TextureKey ToTexKey(sr::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = {},
        .format       = sr::TextureFormat::RGBA8,
        .sample       = rt.sample,
        .mipmap_level = rt.mipmap_level,
    };
}

inline TextureKey ToTexKeyMsaa(sr::SceneRenderTarget rt, VkSampleCountFlagBits s) {
    auto k    = ToTexKey(rt);
    k.samples = s;
    return k;
}

inline VkSampleCountFlagBits ToVkSampleCount(unsigned sample_count) {
    switch (sample_count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline TextureKey ToDepthTexKey(sr::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = TexUsage::DEPTH,
        .format       = sr::TextureFormat::D32F,
        .sample       = rt.sample,
        .mipmap_level = 1,
        .samples      = ToVkSampleCount(rt.sample_count),
    };
}

inline std::string MsaaTwinName(std::string_view tex_name, VkSampleCountFlagBits samples) {
    return std::string(tex_name) + "::msaa" + std::to_string((unsigned)samples);
}

} // namespace sr::vulkan
