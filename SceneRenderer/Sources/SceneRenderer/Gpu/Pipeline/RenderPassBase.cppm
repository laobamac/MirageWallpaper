module;

#include "FrameGraph/Pass.hpp"

export module sr.vulkan_render:vulkan_pass;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :resource;

export namespace sr
{

namespace vulkan
{

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                     = default;
    virtual ~VulkanPass()                                            = default;
    virtual void prepare(Scene&, const Device&, RenderingResources&) = 0;
    virtual void execute(const Device&, RenderingResources&)         = 0;
    virtual void destory(const Device&, RenderingResources&)         = 0;

    void addReleaseTexs(std::span<const std::string_view> texs) {
        m_release_texs.clear();
        std::transform(texs.begin(), texs.end(), std::back_inserter(m_release_texs), [](auto& sv) {
            return std::string(sv);
        });
    }
    bool                         prepared() const { return m_prepared; }
    std::span<const std::string> releaseTexs() const { return m_release_texs; }
    void                         clearReleaseTexs() { m_release_texs.clear(); }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }

private:
    bool                     m_prepared { false };
    std::vector<std::string> m_release_texs;
};

} // namespace vulkan
} // namespace sr
