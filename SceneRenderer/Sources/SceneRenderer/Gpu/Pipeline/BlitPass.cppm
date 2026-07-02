module;

export module sr.vulkan_render:copy_pass;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :vulkan_pass;
import :resource;

export namespace sr::vulkan
{

class CopyPass : public VulkanPass {
public:
    struct Desc {
        std::string src;
        std::string dst;

        ImageParameters vk_src;
        ImageParameters vk_dst;
    };

    CopyPass(const Desc&);
    virtual ~CopyPass();

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace sr::vulkan
