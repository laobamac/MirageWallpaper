module;

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
        const std::string_view result { SpecTex_Default };
        const VkImageLayout    layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

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

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace sr::vulkan
