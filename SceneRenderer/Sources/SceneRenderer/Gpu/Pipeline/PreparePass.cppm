module;
#include <optional>

export module sr.vulkan_render:pre_pass;
import sr.spec_texs;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :vulkan_pass;
import :resource;

export namespace sr::vulkan
{

class PrePass : public VulkanPass {
public:
    struct Desc {
        // in
        std::string_view              result { SpecTex_Default };
        VkImageLayout                 layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        std::optional<TextureRequest> result_request;
        std::optional<TextureRequest> result_msaa_request;

        // prepared
        ImageParameters       vk_result;
        ImageParameters       vk_result_msaa;
        VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };
        vvk::RenderPass       msaa_clear_pass;
        vvk::Framebuffer      msaa_clear_fb;
        VkClearValue          clear_value;
    };

    PrePass(const Desc&);
    virtual ~PrePass();

    bool setResultRequest(std::optional<TextureRequest>,
                          std::optional<TextureRequest> msaa_request = std::nullopt);
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace sr::vulkan
