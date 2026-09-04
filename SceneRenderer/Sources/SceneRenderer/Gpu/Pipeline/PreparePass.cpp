module;
module sr.vulkan_render;
import sr.types;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

using namespace sr::vulkan;

namespace
{
std::optional<vvk::RenderPass> CreateMsaaClearPass(const vvk::Device&    device,
                                                   VkSampleCountFlagBits samples,
                                                   VkFormat              color_format) {
    VkAttachmentDescription color {
        .format         = color_format,
        .samples        = samples,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref,
    };
    std::array<VkSubpassDependency, 2> deps {
        VkSubpassDependency {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
        VkSubpassDependency {
            .srcSubpass    = 0,
            .dstSubpass    = VK_SUBPASS_EXTERNAL,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
    };
    VkRenderPassCreateInfo info {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = (uint32_t)deps.size(),
        .pDependencies   = deps.data(),
    };
    vvk::RenderPass pass;
    if (device.CreateRenderPass(info, pass) != VK_SUCCESS) return std::nullopt;
    return pass;
}
} // namespace

PrePass::PrePass(const Desc& desc) {
    m_desc.result              = desc.result;
    m_desc.layout              = desc.layout;
    m_desc.result_request      = desc.result_request;
    m_desc.result_msaa_request = desc.result_msaa_request;
}
PrePass::~PrePass() {}

bool PrePass::setResultRequest(std::optional<TextureRequest> request,
                               std::optional<TextureRequest> msaa_request) {
    bool changed = SetTextureRequestIfChanged(m_desc.result_request, std::move(request));
    changed =
        SetTextureRequestIfChanged(m_desc.result_msaa_request, std::move(msaa_request)) || changed;
    return changed;
}

std::vector<PassTextureRequestDiagnostic> PrePass::textureRequestDiagnostics() const {
    std::vector<PassTextureRequestDiagnostic> out {
        PassTextureRequestDiagnostic {
            .role    = "frame-result",
            .name    = std::string(m_desc.result),
            .request = m_desc.result_request,
        },
    };
    if (m_desc.result_msaa_request.has_value()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "frame-result-msaa",
            .name    = m_desc.result_msaa_request->name,
            .request = m_desc.result_msaa_request,
        });
    }
    return out;
}

void PrePass::prepare(Scene& scene, const Device& device, RenderingResources&) {
    RenderResourceSystem resources(device);

    {
        auto tex_name = std::string(m_desc.result);
        if (scene.renderTargets.count(tex_name) == 0) return;
        auto& rt = scene.renderTargets.at(tex_name);
        auto  request =
            m_desc.result_request.value_or(MakeRenderTargetNoMipTextureRequest(tex_name, rt));
        if (auto opt = resources.EnsureTexture(request); opt.has_value()) {
            m_desc.vk_result = opt.value();
        } else
            return;
    }
    {
        auto  tex_name = std::string(m_desc.result);
        auto& rt       = scene.renderTargets.at(tex_name);
        m_desc.samples = TextureSampleCount(rt.sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            auto twin_name = MsaaTwinName(tex_name, m_desc.samples);
            auto request   = m_desc.result_msaa_request.value_or(
                MakeMsaaTextureRequest(twin_name, rt, m_desc.samples));
            auto opt = resources.EnsureTexture(request);
            if (! opt.has_value()) return;
            m_desc.vk_result_msaa = opt.value();

            auto pass = CreateMsaaClearPass(device.handle(),
                                            m_desc.samples,
                                            rt.hdr_format ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                          : VK_FORMAT_R8G8B8A8_UNORM);
            if (! pass.has_value()) return;
            m_desc.msaa_clear_pass = std::move(*pass);

            VkImageView             view = m_desc.vk_result_msaa.view;
            VkFramebufferCreateInfo info {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass      = *m_desc.msaa_clear_pass,
                .attachmentCount = 1,
                .pAttachments    = &view,
                .width           = m_desc.vk_result_msaa.extent.width,
                .height          = m_desc.vk_result_msaa.extent.height,
                .layers          = 1,
            };
            if (device.handle().CreateFramebuffer(info, m_desc.msaa_clear_fb) != VK_SUCCESS) return;
        }
    }
    {
        auto& sc           = scene.clearColor;
        m_desc.clear_value = VkClearValue { sc[0], sc[1], sc[2], 1.0f };
    }
    setPrepared();
}

void PrePass::execute(const Device&, RenderingResources& rr) {
    auto&                   cmd = rr.command;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,

    };
    {
        VkImageMemoryBarrier imb {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = m_desc.vk_result.handle,
            .subresourceRange = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
    cmd.ClearColorImage(m_desc.vk_result.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &m_desc.clear_value.color,
                        base_srang);
    VkImageMemoryBarrier imb {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext            = nullptr,
        .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout        = m_desc.layout,
        .image            = m_desc.vk_result.handle,
        .subresourceRange = base_srang,
    };

    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        imb);
    if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
        VkRenderPassBeginInfo pass_begin_info {
            .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass  = *m_desc.msaa_clear_pass,
            .framebuffer = *m_desc.msaa_clear_fb,
            .renderArea =
                VkRect2D {
                    .offset = { 0, 0 },
                    .extent = { m_desc.vk_result_msaa.extent.width,
                                m_desc.vk_result_msaa.extent.height },
                },
            .clearValueCount = 1,
            .pClearValues    = &m_desc.clear_value,
        };
        cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.EndRenderPass();
    }
}
void PrePass::destory(const Device&, RenderingResources&) {
    m_desc.msaa_clear_fb   = {};
    m_desc.msaa_clear_pass = {};
    m_desc.vk_result_msaa  = {};
    m_desc.samples         = VK_SAMPLE_COUNT_1_BIT;
    setPrepared(false);
    clearReleaseTexs();
}
