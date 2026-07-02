module;

#include <unistd.h>

// Vk types/enumerators/PFN typedefs flow in via `import vulkan;` (purview
// below). vk_mem_alloc.h still needs the raw vulkan.h declarations, so we
// let it pull them in transitively.
#include "vk_mem_alloc.h"

// Macros only — VVK_CHECK family.
#include "vvk/macros.hpp"

export module sr.vulkan;
import sr.core;
export import :vvk;
import rstd.log;
import rstd.cppstd;
import sr.types;

// Vulkan FFI: wavsen::ffi::vulkan exposes the full Vk symbol surface as
// a comprehensive FFI module. Re-exported so downstream consumers
// (sr.vulkan_render etc.) that `import sr.vulkan;` still see
// every Vk type / enumerator / PFN_* without needing their own
// `import vulkan;`.
export import vulkan;

// Re-export the host-only shader compile API. This keeps the render
// pipeline on a single Vulkan import surface while shader compilation
// remains a separate target.
export import sr.shader_compile;

// =================================================================
// Layer 1: vvk:: low-level Vulkan C++ wrapper
// =================================================================

// =================================================================
// Layer 2: sr::vulkan:: high-level wrapper
// =================================================================

export namespace sr
{

// ---------- ExSwapchain (formerly Swapchain/ExSwapchain.hpp) ----------

namespace vulkan
{

struct ImageParameters {
    VkImage     handle {};
    VkImageView view {};
    VkSampler   sampler {};
    VkExtent3D  extent {};
    uint32_t    mipmap_level { 1 };

    ImageParameters()  = default;
    ~ImageParameters() = default;
};

} // namespace vulkan

// Snapshot of the negotiated buffer set the producer should render into.
// Fired by ExSwapchain implementations whenever readiness flips or the
// negotiated extent/format changes.
struct ExSwapchainReadyEvent {
    bool     ready;
    unsigned width;
    unsigned height;
    VkFormat format;
};

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

// Per-slot frame descriptor consumed by the present path. The local
// offscreen swapchain fills `id`/`width`/`height`; external-handle fields
// stay at their sentinels on macOS.
struct ExHandle {
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };

    uint32_t drm_fourcc { 0 };
    uint64_t drm_modifier { 0 };
    uint64_t plane0_offset { 0 };
    uint32_t plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }

private:
    int32_t m_id { 0 };
};

template<typename T>
class TripleSwapchain {
public:
    virtual ~TripleSwapchain() = default;

    TripleSwapchain(const TripleSwapchain&)            = delete;
    TripleSwapchain& operator=(const TripleSwapchain&) = delete;
    TripleSwapchain(TripleSwapchain&&)                 = delete;
    TripleSwapchain& operator=(TripleSwapchain&&)      = delete;

    T* eatFrame() {
        if (! dirty().exchange(false)) return nullptr;
        presented() = ready().exchange(presented());
        return presented();
    }
    void renderFrame() {
        inprogress() = ready().exchange(inprogress());
        dirty().exchange(true);
    }
    T* getInprogress() { return inprogress(); }

    std::array<T*, 3> snapshot_all_slots() {
        return { presented().load(), ready().load(), inprogress().load() };
    }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    std::atomic<bool>& dirty() { return m_dirty; };
    std::atomic<bool>  m_dirty { false };
};

// Producer-side abstraction over the offscreen swapchain.
//   - LocalVmaExSwapchain: self-allocates 3 Metal-exportable VkImages via
//     the Vulkan TextureCache; the standalone viewer drives
//     bind_buffers / frame_ready itself and consumes `eatFrame()` /
//     `snapshot_all_slots()`.
class ExSwapchain {
public:
    virtual ~ExSwapchain() = default;

    ExSwapchain(const ExSwapchain&)            = delete;
    ExSwapchain& operator=(const ExSwapchain&) = delete;
    ExSwapchain(ExSwapchain&&)                 = delete;
    ExSwapchain& operator=(ExSwapchain&&)      = delete;

    virtual void poll() {}

    virtual bool acquireRenderTarget(vulkan::ImageParameters& out) = 0;

    virtual void submitRendered(int acquire_sync_fd) = 0;

    virtual ExHandle*                eatFrame() { return nullptr; }
    virtual std::array<ExHandle*, 3> snapshot_all_slots() { return { nullptr, nullptr, nullptr }; }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;
    virtual VkFormat format() const = 0;

    virtual VkImageLayout producerOutputLayout() const = 0;

    virtual uint32_t releaseTargetQueueFamily() const = 0;

    virtual bool ready() const = 0;

    virtual void setOnReadyChanged(std::function<void(const ExSwapchainReadyEvent&)>) = 0;

protected:
    ExSwapchain() = default;
};

namespace vulkan
{

// ---------- Instance.hpp ----------

struct Extension {
    bool             required { false };
    std::string_view name;
};

using InstanceLayer = Extension;

using CheckGpuOp = std::function<bool(vvk::PhysicalDevice)>;

constexpr std::string_view VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";

constexpr uint32_t    SCENERENDERER_VULKAN_VERSION { VK_API_VERSION_1_1 };
constexpr const char* SCENERENDERER_APPLICATION_NAME { "SceneRenderer" };

class Device;
class Instance {
public:
    Instance()  = default;
    ~Instance() = default;

    void Destroy();

    static bool Create(Instance&, std::span<const Extension>, std::span<const InstanceLayer>);
    bool ChoosePhysicalDevice(const CheckGpuOp& checkgpu, std::span<const std::uint8_t> uuid = {});

    const vvk::Instance&       inst() const;
    const vvk::PhysicalDevice& gpu() const;
    const vvk::SurfaceKHR&     surface() const;

    bool offscreen() const;
    void setSurface(VkSurfaceKHR);
    bool supportExt(std::string_view) const;
    bool supportLayer(std::string_view) const;

private:
    utils::DynamicLibrary m_vklib;
    vvk::InstanceDispatch m_dld;
    vvk::Instance         m_vinst;

    vvk::DebugUtilsMessenger m_debug_utils;
    vvk::PhysicalDevice      m_gpu {};

    vvk::SurfaceKHR  m_surface {};
    Set<std::string> m_extensions;
    Set<std::string> m_layers;
};

// ShaderSpv / Uni_ShaderSpv now live in sr.shader_compile (re-exported above).

// ---------- Parameters.hpp ----------

struct QueueParameters {
    vvk::Queue handle;
    uint32_t   family_index;
};

struct VmaBufferParameters {
    vvk::VmaBuffer handle;
    std::size_t    req_size;

    VmaBufferParameters();
    ~VmaBufferParameters();
    VmaBufferParameters(VmaBufferParameters&& o) noexcept;
    VmaBufferParameters& operator=(VmaBufferParameters&& o) noexcept;
};

struct BufferParameters {
    VkBuffer    handle;
    std::size_t req_size;
    BufferParameters()  = default;
    ~BufferParameters() = default;
    BufferParameters(const VmaBufferParameters& o) noexcept
        : handle(*o.handle), req_size(o.req_size) {}
};

struct VmaImageParameters : NoCopy {
    vvk::VmaImage  handle;
    vvk::ImageView view;
    vvk::Sampler   sampler;
    VkExtent3D     extent;
    unsigned       mipmap_level { 1 };

    VmaImageParameters();
    ~VmaImageParameters();
    VmaImageParameters(VmaImageParameters&& o) noexcept;
    VmaImageParameters& operator=(VmaImageParameters&& o) noexcept;
};

// `ImageParameters` itself is global-attached (defined in classic
// Swapchain/ExSwapchain.hpp). This free helper replaces the conversion
// ctor that used to live on it — that ctor needed module-attached
// Vma types which can't be visible in classic purview.
inline ImageParameters ToImageParameters(const VmaImageParameters& o) noexcept {
    ImageParameters out;
    out.handle       = *o.handle;
    out.view         = *o.view;
    out.sampler      = *o.sampler;
    out.extent       = o.extent;
    out.mipmap_level = o.mipmap_level;
    return out;
}

struct ImageSlots : NoCopy {
    std::vector<VmaImageParameters> slots;

    ImageSlots();
    ~ImageSlots();
    ImageSlots(ImageSlots&& o) noexcept;
    ImageSlots& operator=(ImageSlots&& o) noexcept;
};

struct ImageSlotsRef {
    std::vector<ImageParameters> slots;

    idx active { 0 };

    auto& getActive() const {
        if (active > 0 && active >= std::ssize(slots)) return slots[0];
        return slots[(usize)active];
    }
    ImageSlotsRef();
    ~ImageSlotsRef();
    ImageSlotsRef(const ImageSlots&);
};

// ---------- Swapchain.hpp ----------

class Swapchain {
public:
    static bool                      Create(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&);
    const vvk::SwapchainKHR&         handle() const;
    VkFormat                         format() const;
    VkExtent2D                       extent() const;
    VkImageUsageFlags                 usage() const;
    VkPresentModeKHR                 presentMode() const;
    std::span<const ImageParameters> images() const;

private:
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format;
    VkExtent2D                   m_extent;
    VkImageUsageFlags            m_usage {};
    VkPresentModeKHR             m_present_mode;
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;
};

// ---------- TextureCache.hpp ----------

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureKey {
    i32                   width;
    i32                   height;
    TexUsage              usage;
    TextureFormat         format;
    TextureSample         sample;
    unsigned              mipmap_level { 1 };
    VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    struct VideoDecodeOptions {
        std::string hwdec { "auto" };
    };

    TextureCache(const Device&);
    ~TextureCache();

    void Clear();

    void SetVideoDecodeOptions(VideoDecodeOptions);

    std::optional<VmaImageParameters> CreateRenderTargetTex(uint32_t width, uint32_t height,
                                                            VkFormat);
    ImageSlotsRef                    CreateTex(Image&);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

    /* Per-frame hook: advance every registered video-tex by `dt_seconds`,
     * pull as many decoded frames as needed to catch up to wall PTS,
     * convert NV12→RGBA on the CPU, and upload to the slot's stable
     * VkImage. No-op if no video textures are registered. */
    void PumpVideoTextures(double dt_seconds);

    /* vkCmdCopyBufferToImage a sub-rect of `atlas` into the VkImage stored
     * under `key`. Returns false if `key` has no entry yet (the VkImage
     * hasn't been allocated — CreateTex hasn't run); caller may retry
     * next frame. */
    bool UploadFontAtlasRegion(const std::string& key, const std::uint8_t* atlas,
                               std::uint32_t atlas_w, std::uint32_t x, std::uint32_t y,
                               std::uint32_t w, std::uint32_t h);

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    /* VIDEO-typed Image branch of CreateTex: registers a wavsen
     * VideoDecoder + stable RGBA8 VkImage and returns an ImageSlotsRef
     * pointing at that same VkImage so material binding is transparent. */
    ImageSlotsRef       CreateVideoTex(Image&);
    void                allocateCmd();
    vvk::CommandBuffers m_tex_cmds;
    vvk::CommandBuffer  m_tex_cmd;

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;
    VideoDecodeOptions           m_video_decode_options;

    /* Opaque pImpl for the active video-tex set. Defined inside
     * TextureCache.cpp to keep wavsen.video out of the public
     * sr.vulkan module interface. */
    struct VideoRegistry;
    std::unique_ptr<VideoRegistry> m_video_registry;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
};

// ---------- Device.hpp ----------

class PipelineParameters;

class MeshCache;

class Device : NoCopy, NoMove {
public:
    Device();
    ~Device();

    static bool Create(Instance&, std::span<const Extension> exts, VkExtent2D extent, Device&);
    static bool CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                         VkSurfaceKHR surface);

    void Destroy();

    const auto& graphics_queue() const { return m_graphics_queue; }
    const auto& present_queue() const { return m_present_queue; }
    const auto& device() const { return m_device; }
    const auto& handle() const { return m_device; }
    const auto& gpu() const { return m_gpu; }
    const auto& limits() const { return m_limits; }
    const auto& vma_allocator() const { return *m_allocator; }
    const auto& cmd_pool() const { return m_command_pool; }
    const auto& swapchain() const { return m_swapchain; }
    const auto& out_extent() const { return m_extent; }
    void        set_out_extent(VkExtent2D v) { m_extent = v; }

    bool supportExt(std::string_view) const;

    TextureCache& tex_cache() const { return *m_tex_cache; }
    MeshCache&    mesh_cache() const { return *m_mesh_cache; }

    VkDeviceSize GetUsage() const;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits m_limits;
    Set<std::string>       m_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    VkExtent2D m_extent { 1, 1 };

    std::unique_ptr<TextureCache> m_tex_cache;
    std::unique_ptr<MeshCache>    m_mesh_cache;
};

// ---------- Util.hpp ----------

inline bool CreateStagingBuffer(VmaAllocator allocator, std::size_t size,
                                VmaBufferParameters& buffer) {
    VkBufferCreateInfo ci {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .size  = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    buffer.req_size = ci.size;

    VmaAllocationCreateInfo vma_info = {};
    vma_info.usage                   = VMA_MEMORY_USAGE_CPU_ONLY;
    VVK_CHECK_BOOL_RE(vvk::CreateBuffer(allocator, ci, vma_info, buffer.handle));
    return true;
}

// ---------- StagingBuffer.hpp ----------

class StagingBuffer;

class StagingBufferRef {
public:
    VkDeviceSize size { 0 };
    VkDeviceSize offset { 0 };

    operator bool() const { return m_allocation != VK_NULL_HANDLE; }

private:
    friend class StagingBuffer;
    VmaVirtualAllocation m_allocation {};
    size_t               m_virtual_index { 0 };
};

class StagingBuffer : NoCopy, NoMove {
public:
    StagingBuffer(const Device&, VkDeviceSize size, VkBufferUsageFlags);
    ~StagingBuffer();

    bool allocate();
    void destroy();

    bool allocateSubRef(VkDeviceSize size, StagingBufferRef&, VkDeviceSize alignment = 1);
    void unallocateSubRef(const StagingBufferRef&);
    bool writeToBuf(const StagingBufferRef&, std::span<uint8_t>, size_t offset = 0);
    bool fillBuf(const StagingBufferRef& ref, size_t offset, size_t size, uint8_t c);

    bool recordUpload(vvk::CommandBuffer&);

    VkBuffer gpuBuf() const;

private:
    struct VirtualBlock {
        VmaVirtualBlock handle {};
        bool            enabled { false };
        size_t          index { 0 };
        VkDeviceSize    offset { 0 };
        VkDeviceSize    size { 0 };
    };

    VkResult      mapStageBuf();
    VirtualBlock* newVirtualBlock(VkDeviceSize);
    bool          increaseBuf(VkDeviceSize);

    const Device& m_device;
    VkDeviceSize  m_size_step;

    VkBufferUsageFlags m_usage;

    void*                     m_stage_raw { nullptr };
    std::vector<VirtualBlock> m_virtual_blocks {};

    VmaBufferParameters m_stage_buf;
    VmaBufferParameters m_gpu_buf;
};

// ---------- MeshCache.hpp ----------

// Key identifying one mesh data block (vertex array or index array). We key
// by pointer because SceneVertexArray/SceneIndexArray addresses are stable
// across the owning SceneMesh's lifetime, and SceneMesh::ChangeMeshDataFrom
// shares the underlying arrays via shared_ptr — so identical content shows
// up as the same pointer automatically.
struct MeshCacheKey {
    const void* array_ptr { nullptr };
    uint64_t    generation { 0 };

    bool operator==(const MeshCacheKey& o) const noexcept {
        return array_ptr == o.array_ptr && generation == o.generation;
    }
};

struct MeshCacheKeyHash {
    size_t operator()(const MeshCacheKey& k) const noexcept {
        return std::hash<const void*>()(k.array_ptr) ^
               (std::hash<uint64_t>()(k.generation) * 0x9E3779B97F4A7C15ULL);
    }
};

class MeshCache;

// RAII handle into MeshCache. Does NOT cache the VkBuffer — StagingBuffer's
// increaseBuf may invalidate it. Callers resolve buffer() at execute time.
class MeshBufferRef {
public:
    MeshBufferRef() = default;
    MeshBufferRef(MeshCache* owner, MeshCacheKey key, VkDeviceSize offset, VkDeviceSize size);
    ~MeshBufferRef();

    MeshBufferRef(const MeshBufferRef&)            = delete;
    MeshBufferRef& operator=(const MeshBufferRef&) = delete;

    MeshBufferRef(MeshBufferRef&& o) noexcept;
    MeshBufferRef& operator=(MeshBufferRef&& o) noexcept;

    explicit operator bool() const noexcept { return m_owner != nullptr && m_size > 0; }

    VkBuffer     buffer() const noexcept;
    VkDeviceSize offset() const noexcept { return m_offset; }
    VkDeviceSize size() const noexcept { return m_size; }

private:
    MeshCache*   m_owner { nullptr };
    MeshCacheKey m_key {};
    VkDeviceSize m_offset { 0 };
    VkDeviceSize m_size { 0 };
};

class MeshCache : NoCopy, NoMove {
public:
    explicit MeshCache(const Device&);
    ~MeshCache();

    bool init();
    void destroy();

    // Hit: refcount++, returns ref to existing sub-allocation.
    // Miss: allocateSubRef + writeToBuf, marks dirty, returns fresh ref.
    std::optional<MeshBufferRef> QueryOrUpload(MeshCacheKey key, std::span<const uint8_t> data,
                                               VkDeviceSize alignment = 4);

    // Called by ~MeshBufferRef; refcount--, no immediate free.
    void release(MeshCacheKey key);

    // Current GPU buffer handle. May change across increaseBuf in QueryOrUpload.
    VkBuffer gpuBuf() const;

    // Flushes any pending writes to GPU. No-op if nothing dirty since last flush.
    bool recordPendingUploads(vvk::CommandBuffer& cmd);

    // No-op for now; reserved as the hook downstream wires to clearLastRenderGraph.
    void onRenderGraphCleared();

    // Releases refcount==0 entries from the underlying StagingBuffer.
    void evictUnused();

private:
    struct Entry {
        StagingBufferRef ref;
        uint32_t         refcount { 0 };
    };

    const Device&                                             m_device;
    std::unique_ptr<StagingBuffer>                            m_buf;
    std::unordered_map<MeshCacheKey, Entry, MeshCacheKeyHash> m_map;
    bool                                                      m_dirty { false };
};

// ---------- GraphicsPipeline.hpp ----------

struct PipelineParameters {
    vvk::Pipeline       handle;
    vvk::PipelineLayout layout;
    vvk::RenderPass     pass;

    std::vector<vvk::DescriptorSetLayout> descriptor_layouts;
};

struct DescriptorSetInfo {
    bool push_descriptor { false };

    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

class GraphicsPipeline : NoCopy, NoMove {
public:
    GraphicsPipeline();
    ~GraphicsPipeline();

    void toDefault();
    bool create(const Device&, vvk::RenderPass&, PipelineParameters&);

    VkPipelineMultisampleStateCreateInfo   multisample {};
    VkPipelineRasterizationStateCreateInfo raster {};
    VkPipelineDepthStencilStateCreateInfo  depth {};

    ShaderSpv*  getShaderSpv(VkShaderStageFlagBits) const;
    const auto& pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& addDescriptorSetInfo(std::span<const DescriptorSetInfo>);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline&
        addInputAttributeDescription(std::span<const VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(std::span<const VkVertexInputBindingDescription>);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);
    GraphicsPipeline& setSampleCount(VkSampleCountFlagBits);

private:
    vvk::RenderPass m_pass;

    VkPipelineInputAssemblyStateCreateInfo         m_input_assembly {};
    std::vector<VkVertexInputBindingDescription>   m_input_bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> m_input_attr_descriptions;

    VkPipelineViewportStateCreateInfo                m_view;
    VkPipelineColorBlendStateCreateInfo              m_color;
    std::vector<VkDynamicState>                      m_dynamic_states;
    std::vector<VkPipelineColorBlendAttachmentState> m_color_attachments;
    std::vector<DescriptorSetInfo>                   m_descriptor_set_infos;
    Map<VkShaderStageFlagBits, Uni_ShaderSpv>        m_stage_spv_map;
};

// ShaderReflected / GenReflect / VulkanTarget / ShaderCompUnit / ShaderCompOpt /
// CompileAndLinkShaderUnits / Preprocess all live in sr.shader_compile
// (re-exported above).

// ---------- VertexInputState.hpp ----------

struct VertexInputState {
    VkPipelineInputAssemblyStateCreateInfo         input_assembly;
    VkPipelineVertexInputStateCreateInfo           input;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
};

// ---------- LocalVmaExSwapchain (offscreen, Metal-exportable) ----------

struct LocalVmaExHandle : NoCopy {
    ExHandle           handle;
    VmaImageParameters image;

    LocalVmaExHandle() = default;
    ~LocalVmaExHandle() = default;
    LocalVmaExHandle(LocalVmaExHandle&& o) noexcept: handle(o.handle), image(std::move(o.image)) {}
    LocalVmaExHandle& operator=(LocalVmaExHandle&& o) noexcept {
        handle = o.handle;
        image  = std::move(o.image);
        return *this;
    }
};

class LocalVmaExSwapchain final : public ::sr::ExSwapchain,
                                  public ::sr::TripleSwapchain<::sr::ExHandle> {
public:
    LocalVmaExSwapchain(std::array<LocalVmaExHandle, 3> handles, VkExtent2D ext)
        : m_handles(std::move(handles)), m_extent(ext) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle = h.handle;
            handle       = ::sr::ExHandle(index++);
            handle.width = (i32)h.image.extent.width;
            handle.height = (i32)h.image.extent.height;
            handle.fd     = -1;
            handle.size   = 0;
        }
        m_presented  = &m_handles[0].handle;
        m_ready      = &m_handles[1].handle;
        m_inprogress = &m_handles[2].handle;
    }

    bool acquireRenderTarget(ImageParameters& out) override {
        out = ToImageParameters(m_handles.at((std::size_t)(*this->inprogress()).id()).image);
        return true;
    }

    void submitRendered(int) override { this->renderFrame(); }

    ::sr::ExHandle* eatFrame() override {
        return this->TripleSwapchain<::sr::ExHandle>::eatFrame();
    }
    std::array<::sr::ExHandle*, 3> snapshot_all_slots() override {
        return this->TripleSwapchain<::sr::ExHandle>::snapshot_all_slots();
    }

    unsigned width() const override { return m_extent.width; }
    unsigned height() const override { return m_extent.height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }

    VkImageLayout producerOutputLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }
    uint32_t      releaseTargetQueueFamily() const override { return VK_QUEUE_FAMILY_IGNORED; }
    bool          ready() const override { return true; }

    void setOnReadyChanged(std::function<void(const ::sr::ExSwapchainReadyEvent&)> cb) override {
        if (cb) {
            ::sr::ExSwapchainReadyEvent e {
                .ready  = true,
                .width  = m_extent.width,
                .height = m_extent.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            cb(e);
        }
    }

protected:
    std::atomic<::sr::ExHandle*>& presented() override { return m_presented; }
    std::atomic<::sr::ExHandle*>& ready() override { return m_ready; }
    std::atomic<::sr::ExHandle*>& inprogress() override { return m_inprogress; }

private:
    std::array<LocalVmaExHandle, 3> m_handles;
    std::atomic<::sr::ExHandle*>   m_presented { nullptr };
    std::atomic<::sr::ExHandle*>   m_ready { nullptr };
    std::atomic<::sr::ExHandle*>   m_inprogress { nullptr };
    VkExtent2D                      m_extent;
};

inline std::unique_ptr<ExSwapchain> CreateLocalExSwapchain(const Device& device, unsigned w,
                                                           unsigned h, VkImageTiling tiling) {
    (void)tiling;
    std::array<LocalVmaExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = device.tex_cache().CreateRenderTargetTex(
                w, h, VK_FORMAT_R8G8B8A8_UNORM);
            rv.has_value())
            handle.image = std::move(rv.value());
        else
            return nullptr;
    }
    return std::make_unique<LocalVmaExSwapchain>(std::move(handles), VkExtent2D { w, h });
}

} // namespace vulkan
} // namespace sr
