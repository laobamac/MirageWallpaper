module;

/* GMF include: the YuvToRgba class declaration mentions PFNs (e.g.
 * PFN_vkGetMemoryFdPropertiesKHR) that the wavsen::ffi::vulkan module
 * does not currently `using`-export. Including <vulkan/vulkan.h> here
 * makes them visible in this interface unit's purview without forcing
 * a sibling-module change. import vulkan; below still re-exports the
 * curated subset for downstream consumers. */
#include <vulkan/vulkan.h>

export module wavsen.video:yuv_to_rgba;

import rstd.cppstd;
import rstd;
import vulkan;
import :vk_device;        // Error
import :video_decoder;    // DrmFrameView

export namespace wavsen::video {

// Coefficients for the YUV→RGB push constant. CPU side fills this from
// the source frame's colorspace + range; the shader applies it as
// `rgb = M * (ycbcr + offset)`.
struct ColorMatrix {
    float m_r[3];   // Y, Cb, Cr scalings producing R
    float m_g[3];
    float m_b[3];
    float offset[3]; // subtracted from (Y, Cb, Cr) before matmul
};

// Mirrors FFmpeg's `enum AVColorSpace` for the cases we actually
// branch on. Keeping our own enum avoids leaking <libavutil/pixfmt.h>
// into the public surface.
enum class ColorSpace : std::uint32_t {
    Bt709 = 0,
    Bt601 = 1,
    Bt2020 = 2,
};

enum class ColorRange : std::uint32_t {
    Limited = 0,
    Full    = 1,
};

// Derive the ColorMatrix to push to the shader. Defaults to BT.709
// limited when either argument is the canonical "unknown" sentinel.
ColorMatrix make_color_matrix(ColorSpace cs, ColorRange cr);

class YuvToRgba {
public:
    ~YuvToRgba();
    YuvToRgba(const YuvToRgba&)            = delete;
    YuvToRgba& operator=(const YuvToRgba&) = delete;

    static auto create(VkInstance       instance,
                       VkPhysicalDevice phys,
                       VkDevice         device,
                       std::uint32_t    queue_family,
                       VkQueue          queue,
                       std::uint32_t    max_w,
                       std::uint32_t    max_h)
        -> rstd::Result<std::unique_ptr<YuvToRgba>, Error>;

    auto convert_nv12(VkImage             dst,
                      std::uint32_t       dst_w,
                      std::uint32_t       dst_h,
                      const std::uint8_t* nv12,
                      std::size_t         nv12_size,
                      const ColorMatrix&  cm)
        -> rstd::Result<int, Error>;

    struct VkFrameImports {
        VkImage         y_image;
        VkImage         uv_image;
        VkSemaphore     y_sem;
        VkSemaphore     uv_sem;
        std::uint64_t*  y_sem_val_in_out;
        std::uint64_t*  uv_sem_val_in_out;
        VkImageLayout*  y_layout_in_out;
        VkImageLayout*  uv_layout_in_out;
        std::uint32_t*  y_qf_in_out;
        std::uint32_t*  uv_qf_in_out;
        std::uint32_t   src_w;
        std::uint32_t   src_h;
        // 8 → R8 / R8G8 image views (NV12). 16 → R16 / R16G16 (P010 / P016).
        std::uint32_t   bit_depth;
    };
    auto convert_av_vk_frame(const VkFrameImports& imports,
                             VkImage             dst,
                             std::uint32_t       dst_w,
                             std::uint32_t       dst_h,
                             const ColorMatrix&  cm)
        -> rstd::Result<int, Error>;

    /* Zero-copy VAAPI path: imports the DrmFrameView's dma-buf fds as
     * a disjoint multi-plane VkImage (NV12 → R8 + R8G8 plane views),
     * runs the same nv12_to_rgba.comp into `dst`. The transient
     * VkImage / VkDeviceMemory / fd dups live until the *next*
     * convert_drm_prime call returns (cycled via last_drm_*). */
    auto convert_drm_prime(const DrmFrameView& drm,
                           VkImage             dst,
                           std::uint32_t       dst_w,
                           std::uint32_t       dst_h,
                           const ColorMatrix&  cm)
        -> rstd::Result<int, Error>;

private:
    YuvToRgba() = default;

    bool init(VkInstance instance, VkPhysicalDevice phys, VkDevice device,
              std::uint32_t queue_family, VkQueue queue,
              std::uint32_t max_w, std::uint32_t max_h, Error* err);
    int  convert_nv12_(VkImage dst, std::uint32_t dst_w, std::uint32_t dst_h,
                       const std::uint8_t* nv12, std::size_t nv12_size,
                       const ColorMatrix& cm, Error* err);
    int  convert_av_vk_frame_(const VkFrameImports& imports, VkImage dst,
                              std::uint32_t dst_w, std::uint32_t dst_h,
                              const ColorMatrix& cm, Error* err);
    int  convert_drm_prime_(const DrmFrameView& drm, VkImage dst,
                            std::uint32_t dst_w, std::uint32_t dst_h,
                            const ColorMatrix& cm, Error* err);

    VkInstance       instance_      { VK_NULL_HANDLE };
    VkPhysicalDevice phys_          { VK_NULL_HANDLE };
    VkDevice         device_        { VK_NULL_HANDLE };
    VkQueue          queue_         { VK_NULL_HANDLE };
    std::uint32_t    queue_family_  { 0 };

    std::uint32_t    max_w_         { 0 };
    std::uint32_t    max_h_         { 0 };

    VkShaderModule        shader_      { VK_NULL_HANDLE };
    VkDescriptorSetLayout dsl_         { VK_NULL_HANDLE };
    VkPipelineLayout      pipeline_layout_ { VK_NULL_HANDLE };
    VkPipeline            pipeline_    { VK_NULL_HANDLE };

    VkSampler        sampler_       { VK_NULL_HANDLE };

    VkImage          y_image_       { VK_NULL_HANDLE };
    VkDeviceMemory   y_memory_      { VK_NULL_HANDLE };
    VkImageView      y_view_        { VK_NULL_HANDLE };

    VkImage          uv_image_      { VK_NULL_HANDLE };
    VkDeviceMemory   uv_memory_     { VK_NULL_HANDLE };
    VkImageView      uv_view_       { VK_NULL_HANDLE };

    VkBuffer         staging_buf_   { VK_NULL_HANDLE };
    VkDeviceMemory   staging_mem_   { VK_NULL_HANDLE };
    void*            staging_map_   { nullptr };
    VkDeviceSize     staging_size_  { 0 };

    VkCommandPool    cmd_pool_      { VK_NULL_HANDLE };
    VkCommandBuffer  cmd_           { VK_NULL_HANDLE };

    VkSemaphore      signal_sem_    { VK_NULL_HANDLE };
    VkFence          done_fence_    { VK_NULL_HANDLE };
    bool             fence_pending_ { false };

    VkDescriptorPool dpool_         { VK_NULL_HANDLE };
    VkDescriptorSet  dset_          { VK_NULL_HANDLE };

    VkImageView      last_dst_view_ { VK_NULL_HANDLE };
    VkImageView      last_y_view_   { VK_NULL_HANDLE };
    VkImageView      last_uv_view_  { VK_NULL_HANDLE };

    /* Cycle of imported DRM-PRIME resources kept alive for one extra
     * frame so the prior submit's GPU work can drain before destroy. */
    VkImage          last_drm_image_       { VK_NULL_HANDLE };
    VkDeviceMemory   last_drm_memories_[4] { VK_NULL_HANDLE, VK_NULL_HANDLE,
                                             VK_NULL_HANDLE, VK_NULL_HANDLE };
    std::uint32_t    last_drm_memory_count_ { 0 };

    PFN_vkGetSemaphoreFdKHR        vkGetSemaphoreFdKHR_        { nullptr };
    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR_ { nullptr };
};

} // namespace wavsen::video
