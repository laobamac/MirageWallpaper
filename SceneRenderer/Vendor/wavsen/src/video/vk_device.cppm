export module wavsen.video:vk_device;

import rstd.cppstd;
import rstd;
export import vulkan;

export namespace wavsen::video {

// Unified error carrier. Free-form message — callers either log or
// surface as an opaque diagnostic.
struct Error {
    std::string message;
};

// Per-queue-family record exposed to FFmpeg's AVVulkanDeviceContext::qf[].
// Producer creates 1 queue from each enumerated family and remembers each
// family's caps so FFmpeg can pick the right one for video decode/encode.
struct QueueFamily {
    std::uint32_t       index;
    VkQueueFlags        flags;
    /* Video codec ops the family advertises (only meaningful when the
     * device exposes VK_KHR_video_queue). 0 if unknown. */
    std::uint32_t       video_caps;
};

// Bring up a VkInstance/VkPhysicalDevice/VkDevice with the extension set
// the bridge pool's Vulkan backend needs (DMA-BUF export, modifier
// import, semaphore SYNC_FD), plus a HOST_VISIBLE|COHERENT staging
// buffer pre-mapped at `width*height*4` bytes for repeated RGBA8
// uploads.
class Producer {
public:
    ~Producer();
    Producer(const Producer&)            = delete;
    Producer& operator=(const Producer&) = delete;

    static auto create(std::uint32_t width, std::uint32_t height)
        -> rstd::Result<std::unique_ptr<Producer>, Error>;

    // Pin the picked VkPhysicalDevice to the GPU that exposes
    // `render_node` (e.g. "/dev/dri/renderD128"). Empty string → behaves
    // identically to the no-arg overload (first device that advertises
    // the required extension set wins).
    static auto create_with_render_node(std::uint32_t width, std::uint32_t height,
                                        const std::string& render_node)
        -> rstd::Result<std::unique_ptr<Producer>, Error>;

    // Adopt a caller-owned VkInstance/VkDevice. The returned Producer
    // exposes them via instance() / device() etc. and feeds FFmpeg's
    // AVVulkanDeviceContext via make_shared_vulkan_hwdevice, but does
    // NOT destroy them in its destructor — the caller retains ownership.
    // `enabled_inst_exts` and `enabled_dev_exts` are the extension lists
    // the caller passed to vkCreateInstance / vkCreateDevice; they're
    // mirrored to FFmpeg verbatim (the pointed-to char* must outlive
    // the Producer).
    // No staging buffer / command pool is allocated — `upload_into`
    // returns an error on adopted Producers; this constructor is meant
    // for shared-device decode only.
    struct ExternalDeviceInfo {
        VkInstance       instance;
        VkPhysicalDevice physical_device;
        VkDevice         device;
        VkQueue          queue;
        std::uint32_t    queue_family_index;
        // Full per-family caps list — typically as wide as
        // vkGetPhysicalDeviceQueueFamilyProperties returns. Used for
        // AVVulkanDeviceContext::qf[]. May be empty (FFmpeg falls back
        // to its own queue discovery).
        std::vector<QueueFamily>     queue_families;
        std::vector<const char*>     enabled_instance_extensions;
        std::vector<const char*>     enabled_device_extensions;
        std::uint32_t                api_version { 0x00403000u }; // VK_API_VERSION_1_3
        std::uint32_t                width  { 0 };
        std::uint32_t                height { 0 };
        // Optional DRM render-node info (renderD12X). drm_render_fd is
        // adopted (closed on Producer destruction) if >= 0.
        int                          drm_render_fd { -1 };
        std::uint32_t                drm_render_major { 0 };
        std::uint32_t                drm_render_minor { 0 };
    };
    static auto from_external(ExternalDeviceInfo info)
        -> rstd::Result<std::unique_ptr<Producer>, Error>;

    VkInstance       instance() const         { return instance_; }
    VkPhysicalDevice physical_device() const  { return phys_; }
    VkDevice         device() const           { return device_; }
    VkQueue          queue() const            { return queue_; }
    std::uint32_t    queue_family_index() const { return queue_family_; }
    std::uint32_t    drm_render_major() const { return drm_render_major_; }
    std::uint32_t    drm_render_minor() const { return drm_render_minor_; }
    const std::uint8_t* device_uuid() const   { return have_uuid_ ? device_uuid_ : nullptr; }
    const std::uint8_t* driver_uuid() const   { return have_uuid_ ? driver_uuid_ : nullptr; }
    int              drm_render_fd() const    { return drm_render_fd_; }
    std::uint32_t    width() const  { return width_; }
    std::uint32_t    height() const { return height_; }

    std::uint32_t    instance_api_version() const { return instance_api_version_; }
    const std::vector<const char*>& enabled_instance_extensions() const { return enabled_inst_exts_; }
    const std::vector<const char*>& enabled_device_extensions()   const { return enabled_dev_exts_; }
    const std::vector<QueueFamily>& queue_families()              const { return queue_families_; }

    // Copy `data` (tightly packed RGBA8, `size` bytes == width*height*4)
    // into `target` VkImage. On success returns an exported sync_fd that
    // signals when the GPU is done writing — the bridge pool takes
    // ownership.
    auto upload_into(VkImage target, std::uint32_t target_width, std::uint32_t target_height,
                     const std::uint8_t* data, std::size_t size)
        -> rstd::Result<int, Error>;

private:
    Producer() = default;

    // Internal builder used by the two public factories. Returns a
    // unique_ptr or nullptr; on failure populates `*err` with a message.
    static std::unique_ptr<Producer>
    build_(std::uint32_t width, std::uint32_t height,
           const std::string* render_node, Error* err);

    int upload_into_(VkImage target, std::uint32_t target_width, std::uint32_t target_height,
                     const std::uint8_t* data, std::size_t size, Error* err);

    VkInstance       instance_ { VK_NULL_HANDLE };
    VkPhysicalDevice phys_ { VK_NULL_HANDLE };
    VkDevice         device_ { VK_NULL_HANDLE };
    std::uint32_t    queue_family_ { 0 };
    VkQueue          queue_ { VK_NULL_HANDLE };

    VkCommandPool    cmd_pool_ { VK_NULL_HANDLE };
    VkCommandBuffer  cmd_ { VK_NULL_HANDLE };
    VkSemaphore      signal_sem_ { VK_NULL_HANDLE };
    VkFence          done_fence_ { VK_NULL_HANDLE };
    bool             fence_pending_ { false };

    VkBuffer         staging_buf_ { VK_NULL_HANDLE };
    VkDeviceMemory   staging_mem_ { VK_NULL_HANDLE };
    void*            staging_map_ { nullptr };
    VkDeviceSize     staging_size_ { 0 };

    std::uint32_t    width_ { 0 };
    std::uint32_t    height_ { 0 };
    std::uint32_t    drm_render_major_ { 0 };
    std::uint32_t    drm_render_minor_ { 0 };
    int              drm_render_fd_ { -1 };

    bool             have_uuid_ { false };
    std::uint8_t     device_uuid_[16] { 0 };
    std::uint8_t     driver_uuid_[16] { 0 };

    std::uint32_t            instance_api_version_ { 0 };
    std::vector<const char*> enabled_inst_exts_;
    std::vector<const char*> enabled_dev_exts_;
    std::vector<QueueFamily> queue_families_;

    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR_ { nullptr };

    /* When true, ~Producer destroys the VkInstance/VkDevice and the
     * staging/command-pool/fence/semaphore resources it allocated. When
     * false (from_external path) only resources we created are torn
     * down; instance/device belong to the caller. */
    bool             owns_device_ { true };
};

} // namespace wavsen::video
