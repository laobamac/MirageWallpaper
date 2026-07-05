module;

export module sr.vulkan_render:fin_pass;
import sr.spec_texs;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

import :vulkan_pass;
import :resource;

export namespace sr::vulkan
{

// Final pass: blit the scene render target into the present buffer
// (offscreen ExSwapchain slot or surface-mode swapchain image), then
// emit the appropriate barrier so the consumer reads coherent pixels.
class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        std::string_view              result { SpecTex_Default }; // scene RT key
        std::optional<TextureRequest> result_request;

        // resolved in prepare()
        ImageParameters vk_result;

        // set per-frame via setPresent()
        ImageParameters vk_present;

        // configured once at init by VulkanRender
        VkImageLayout present_layout { VK_IMAGE_LAYOUT_UNDEFINED };
        uint32_t      present_queue_index { 0 };
        // Format of the present image. Used to pick copy vs blit; UNDEFINED
        // forces blit (the safe default for unknown formats).
        VkFormat present_format { VK_FORMAT_UNDEFINED };
        bool     present_can_transfer_src { false };
        std::function<void(void* mtl_texture, uint32_t width, uint32_t height)>
            metal_frame_callback;
    };

    FinPass(const Desc&);
    virtual ~FinPass();

    void                                      setPresent(ImageParameters);
    void                                      setPresentLayout(VkImageLayout);
    void                                      setPresentQueueIndex(uint32_t);
    void                                      setPresentFormat(VkFormat);
    void                                      setPresentCanTransferSrc(bool);
    void                                      setMetalFrameCallback(std::function<void(void*, uint32_t, uint32_t)>);
    bool                                      setResultRequest(std::optional<TextureRequest>);
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    void                                      finishFrameDump(const Device&);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    void recordFrameDump(const Device&, RenderingResources&);
    void recordPresentDump(const Device&, RenderingResources&);

    Desc m_desc;
    vvk::VmaBuffer m_dump_buffer;
    std::string    m_dump_path;
    std::size_t    m_dump_size { 0 };
    uint32_t       m_dump_width { 0 };
    uint32_t       m_dump_height { 0 };
    bool           m_dump_pending { false };
    bool           m_dump_done { false };
    bool           m_path_logged { false };
    bool           m_present_dump_warned { false };
    vvk::VmaBuffer m_present_dump_buffer;
    std::string    m_present_dump_path;
    std::size_t    m_present_dump_size { 0 };
    uint32_t       m_present_dump_width { 0 };
    uint32_t       m_present_dump_height { 0 };
    bool           m_present_dump_pending { false };
    bool           m_present_dump_done { false };
};

} // namespace sr::vulkan
