module;
#include <cstdio>
module wavsen.video;

import rstd.cppstd;
import rstd;
import vulkan;
import :vk_device;

namespace wavsen::video {

namespace {

bool fail(Error* err, std::string msg) {
    if (err) err->message = std::move(msg);
    return false;
}

const char* vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:                                return "VK_ERROR_?";
    }
}

bool device_has_ext(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, props.data());
    for (auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

} // namespace


Producer::~Producer() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (staging_map_)         vkUnmapMemory(device_, staging_mem_);
        if (staging_buf_)         vkDestroyBuffer(device_, staging_buf_, nullptr);
        if (staging_mem_)         vkFreeMemory(device_, staging_mem_, nullptr);
        if (signal_sem_)          vkDestroySemaphore(device_, signal_sem_, nullptr);
        if (done_fence_)          vkDestroyFence(device_, done_fence_, nullptr);
        if (cmd_pool_)            vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        if (owns_device_)         vkDestroyDevice(device_, nullptr);
    }
    if (owns_device_ && instance_) vkDestroyInstance(instance_, nullptr);
    if (drm_render_fd_ >= 0) rstd::sys::libc::close(drm_render_fd_);
}

auto Producer::create(uint32_t width, uint32_t height)
    -> rstd::Result<std::unique_ptr<Producer>, Error> {
    Error err;
    auto p = build_(width, height, /*render_node=*/nullptr, &err);
    if (!p) return rstd::Err(std::move(err));
    return rstd::Ok(std::move(p));
}

auto Producer::create_with_render_node(uint32_t width, uint32_t height,
                                       const std::string& render_node)
    -> rstd::Result<std::unique_ptr<Producer>, Error> {
    Error err;
    auto p = build_(width, height, &render_node, &err);
    if (!p) return rstd::Err(std::move(err));
    return rstd::Ok(std::move(p));
}

auto Producer::from_external(ExternalDeviceInfo info)
    -> rstd::Result<std::unique_ptr<Producer>, Error> {
    if (!info.instance || !info.physical_device || !info.device || !info.queue) {
        return rstd::Err(Error { "Producer::from_external: missing handle(s)" });
    }
    auto self = std::unique_ptr<Producer>(new Producer());
    self->owns_device_ = false;
    self->instance_    = info.instance;
    self->phys_        = info.physical_device;
    self->device_      = info.device;
    self->queue_       = info.queue;
    self->queue_family_ = info.queue_family_index;
    self->width_       = info.width;
    self->height_      = info.height;
    self->instance_api_version_ = info.api_version;
    self->enabled_inst_exts_ = std::move(info.enabled_instance_extensions);
    self->enabled_dev_exts_  = std::move(info.enabled_device_extensions);
    self->queue_families_    = std::move(info.queue_families);
    /* Caller-supplied DRM fd is adopted (Producer destructor closes it
     * when >= 0). Passing -1 simply means "we don't have one"; FFmpeg's
     * vaapi path doesn't need it when running on the shared Vulkan
     * device. */
    self->drm_render_fd_     = info.drm_render_fd;
    self->drm_render_major_  = info.drm_render_major;
    self->drm_render_minor_  = info.drm_render_minor;
    /* Probe vkGetSemaphoreFdKHR — required by the bridge upload path,
     * but adopted Producers don't expose upload_into. Best-effort. */
    self->vkGetSemaphoreFdKHR_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(self->device_, "vkGetSemaphoreFdKHR"));
    return rstd::Ok(std::move(self));
}

std::unique_ptr<Producer>
Producer::build_(uint32_t width, uint32_t height,
                 const std::string* render_node_ptr, Error* err) {
    const std::string render_node = render_node_ptr ? *render_node_ptr : std::string{};
    if (width == 0 || height == 0) {
        fail(err, "Producer: width/height must be non-zero");
        return nullptr;
    }

    auto self = std::unique_ptr<Producer>(new Producer());
    self->width_ = width;
    self->height_ = height;

    /* API 1.3 is the minimum FFmpeg's AV_HWDEVICE_TYPE_VULKAN requires
     * (see hwcontext_vulkan.h: "Must be at least version 1.3"). */
    self->enabled_inst_exts_ = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app {};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wavsen-video";
    app.apiVersion       = VK_API_VERSION_1_3;
    self->instance_api_version_ = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = static_cast<uint32_t>(self->enabled_inst_exts_.size());
    ici.ppEnabledExtensionNames = self->enabled_inst_exts_.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &self->instance_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateInstance: ") + vk_result_str(r));
        return nullptr;
    }

    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(self->instance_, &pd_count, nullptr);
    if (pd_count == 0) {
        fail(err, "no Vulkan physical devices found");
        return nullptr;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(self->instance_, &pd_count, pds.data());

    const char* req_dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    static constexpr const char* DRM_EXT = "VK_EXT_physical_device_drm";

    /* Iter 4: when `render_node` is set, only the device whose DRM
     * render major:minor matches the requested path is acceptable. */
    bool                 pinning  = !render_node.empty();
    rstd::sys::libc::dev_t want_rdev = 0;
    if (pinning) {
        rstd::sys::libc::stat_t st {};
        if (rstd::sys::libc::stat(render_node.c_str(), &st) != 0) {
            fail(err, std::string("Producer: stat(") + render_node + ") failed: " +
                       std::strerror(rstd::sys::libc::errno()));
            return nullptr;
        }
        want_rdev = st.st_rdev;
    }

    auto vkGetPhysicalDeviceProperties2_ =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(self->instance_,
                                  "vkGetPhysicalDeviceProperties2"));

    bool have_drm_ext = false;
    for (auto pd : pds) {
        bool ok = true;
        for (const char* e : req_dev_exts) {
            if (!device_has_ext(pd, e)) { ok = false; break; }
        }
        if (!ok) continue;

        bool pd_has_drm = device_has_ext(pd, DRM_EXT);
        if (pinning) {
            if (!pd_has_drm || !vkGetPhysicalDeviceProperties2_) continue;
            VkPhysicalDeviceDrmPropertiesEXT drm {};
            drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 props {};
            props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props.pNext = &drm;
            vkGetPhysicalDeviceProperties2_(pd, &props);
            if (!drm.hasRender) continue;
            const rstd::sys::libc::dev_t pd_rdev =
                rstd::sys::libc::makedev(static_cast<unsigned>(drm.renderMajor),
                                         static_cast<unsigned>(drm.renderMinor));
            if (pd_rdev != want_rdev) continue;
        }

        self->phys_   = pd;
        have_drm_ext  = pd_has_drm;
        break;
    }
    if (self->phys_ == VK_NULL_HANDLE) {
        fail(err, pinning
             ? std::string("Producer: no Vulkan device matches render_node ") + render_node
             : std::string("no physical device supports the DMA-BUF export extension set"));
        return nullptr;
    }

    /* Enumerate ALL queue families on the device. We create one queue
     * from each family so FFmpeg's AV_HWDEVICE_TYPE_VULKAN can pick
     * whichever family suits its needs (graphics / compute / transfer /
     * video decode/encode). The single queue we use ourselves comes from
     * the first family with GRAPHICS|COMPUTE|TRANSFER capability. */
    {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(self->phys_, &n, nullptr);
        if (n == 0) { fail(err, "no queue families"); return nullptr; }
        std::vector<VkQueueFamilyProperties> qprops(n);
        vkGetPhysicalDeviceQueueFamilyProperties(self->phys_, &n, qprops.data());

        self->queue_families_.reserve(n);
        bool picked_self = false;
        for (uint32_t i = 0; i < n; ++i) {
            QueueFamily q {};
            q.index      = i;
            q.flags      = qprops[i].queueFlags;
            q.video_caps = 0;  /* video_caps probing requires VK_KHR_video_queue
                                * — left at 0; FFmpeg falls back to flag-based
                                * discovery when this is unset. */
            self->queue_families_.push_back(q);
            if (!picked_self
                && (q.flags & (VK_QUEUE_GRAPHICS_BIT
                               | VK_QUEUE_COMPUTE_BIT
                               | VK_QUEUE_TRANSFER_BIT))) {
                self->queue_family_ = i;
                picked_self = true;
            }
        }
        if (!picked_self) {
            fail(err, "no graphics/compute/transfer queue family");
            return nullptr;
        }
    }

    /* One queue per family. */
    std::vector<float>                   prios(self->queue_families_.size(), 1.0f);
    std::vector<VkDeviceQueueCreateInfo> qcis;
    qcis.reserve(self->queue_families_.size());
    for (const auto& q : self->queue_families_) {
        VkDeviceQueueCreateInfo qci {};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = q.index;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &prios[0];
        qcis.push_back(qci);
    }

    /* Required device extensions. */
    self->enabled_dev_exts_.assign(std::begin(req_dev_exts), std::end(req_dev_exts));
    if (have_drm_ext) self->enabled_dev_exts_.push_back(DRM_EXT);

    /* Best-effort optional extensions FFmpeg's vulkan path likes. We
     * only add the ones the device advertises so vkCreateDevice doesn't
     * fail on a missing-but-requested extension. */
    static constexpr const char* opt_dev_exts[] = {
        "VK_KHR_video_queue",
        "VK_KHR_video_decode_queue",
        "VK_KHR_video_decode_h264",
        "VK_KHR_video_decode_h265",
        "VK_KHR_video_decode_av1",
        "VK_EXT_external_memory_host",
        "VK_KHR_push_descriptor",
        "VK_KHR_synchronization2",
        "VK_KHR_timeline_semaphore",
        "VK_EXT_descriptor_buffer",
        "VK_EXT_shader_object",
    };
    for (const char* e : opt_dev_exts) {
        if (device_has_ext(self->phys_, e)) self->enabled_dev_exts_.push_back(e);
    }

    /* Enable the 1.2/1.3 features FFmpeg's vulkan decode path expects.
     * Querying first lets us OR in only the bits the device supports;
     * vkCreateDevice rejects features the device doesn't advertise. */
    VkPhysicalDeviceVulkan12Features f12 {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features f13 {};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 feats2 {};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext = &f12;
    if (vkGetPhysicalDeviceProperties2_) {
        auto vkGetPhysicalDeviceFeatures2_ =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(self->instance_,
                                      "vkGetPhysicalDeviceFeatures2"));
        if (vkGetPhysicalDeviceFeatures2_)
            vkGetPhysicalDeviceFeatures2_(self->phys_, &feats2);
    }
    /* Drop features the device doesn't have, then keep the ones we
     * actually want enabled. timelineSemaphore + synchronization2 are
     * the load-bearing ones for FFmpeg + AVVkFrame. */
    VkPhysicalDeviceVulkan12Features want12 {};
    want12.sType                    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    want12.timelineSemaphore        = f12.timelineSemaphore;
    want12.bufferDeviceAddress      = f12.bufferDeviceAddress;
    VkPhysicalDeviceVulkan13Features want13 {};
    want13.sType                    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    want13.synchronization2         = f13.synchronization2;
    want13.maintenance4             = f13.maintenance4;
    want12.pNext                    = &want13;
    VkPhysicalDeviceFeatures2 want_feats2 {};
    want_feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want_feats2.pNext = &want12;
    want_feats2.features.samplerAnisotropy = feats2.features.samplerAnisotropy;

    VkDeviceCreateInfo dci {};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &want_feats2;
    dci.queueCreateInfoCount    = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos       = qcis.data();
    dci.enabledExtensionCount   = static_cast<uint32_t>(self->enabled_dev_exts_.size());
    dci.ppEnabledExtensionNames = self->enabled_dev_exts_.data();
    if (VkResult r = vkCreateDevice(self->phys_, &dci, nullptr, &self->device_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateDevice: ") + vk_result_str(r));
        return nullptr;
    }
    vkGetDeviceQueue(self->device_, self->queue_family_, 0, &self->queue_);

    self->vkGetSemaphoreFdKHR_ =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(self->device_, "vkGetSemaphoreFdKHR"));
    if (!self->vkGetSemaphoreFdKHR_) {
        fail(err, "vkGetSemaphoreFdKHR missing");
        return nullptr;
    }

    /* The function pointer was loaded above for the device-pick pass;
     * reuse it here for UUID + DRM property capture. */
    if (vkGetPhysicalDeviceProperties2_) {
        VkPhysicalDeviceIDProperties id_props {};
        id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceDrmPropertiesEXT drm {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        if (have_drm_ext) id_props.pNext = &drm;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &id_props;
        vkGetPhysicalDeviceProperties2_(self->phys_, &props);
        std::memcpy(self->device_uuid_, id_props.deviceUUID, 16);
        std::memcpy(self->driver_uuid_, id_props.driverUUID, 16);
        self->have_uuid_ = true;
        if (have_drm_ext && drm.hasRender) {
            self->drm_render_major_ = static_cast<uint32_t>(drm.renderMajor);
            self->drm_render_minor_ = static_cast<uint32_t>(drm.renderMinor);
        }
    }

    if (self->drm_render_minor_ != 0) {
        for (int i = 128; i < 192; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
            int fd = rstd::sys::libc::open(path,
                                           rstd::sys::libc::O_RDWR | rstd::sys::libc::O_CLOEXEC);
            if (fd >= 0) {
                self->drm_render_fd_ = fd;
                break;
            }
        }
    }

    VkCommandPoolCreateInfo cpi {};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = self->queue_family_;
    if (VkResult r = vkCreateCommandPool(self->device_, &cpi, nullptr,
                                         &self->cmd_pool_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateCommandPool: ") + vk_result_str(r));
        return nullptr;
    }
    VkCommandBufferAllocateInfo cbi {};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = self->cmd_pool_;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (VkResult r = vkAllocateCommandBuffers(self->device_, &cbi, &self->cmd_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkAllocateCommandBuffers: ") + vk_result_str(r));
        return nullptr;
    }
    VkFenceCreateInfo fci {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (VkResult r = vkCreateFence(self->device_, &fci, nullptr,
                                   &self->done_fence_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateFence: ") + vk_result_str(r));
        return nullptr;
    }

    VkExportSemaphoreCreateInfo exp_sem {};
    exp_sem.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_sem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sem_ci {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_ci.pNext = &exp_sem;
    if (VkResult r = vkCreateSemaphore(self->device_, &sem_ci, nullptr,
                                       &self->signal_sem_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateSemaphore(acquire): ") + vk_result_str(r));
        return nullptr;
    }

    const VkDeviceSize tight = VkDeviceSize(width) * height * 4;
    self->staging_size_ = tight;
    VkBufferCreateInfo bci {};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = tight;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VkResult r = vkCreateBuffer(self->device_, &bci, nullptr,
                                    &self->staging_buf_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateBuffer(staging): ") + vk_result_str(r));
        return nullptr;
    }
    VkMemoryRequirements bmr {};
    vkGetBufferMemoryRequirements(self->device_, self->staging_buf_, &bmr);
    VkPhysicalDeviceMemoryProperties mprops {};
    vkGetPhysicalDeviceMemoryProperties(self->phys_, &mprops);
    uint32_t host_type = rstd::u32_::MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        const auto pf = mprops.memoryTypes[i].propertyFlags;
        if ((bmr.memoryTypeBits & (1u << i))
            && (pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            && (pf & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            host_type = i;
            break;
        }
    }
    if (host_type == rstd::u32_::MAX) {
        fail(err, "no HOST_VISIBLE|COHERENT memory type for staging");
        return nullptr;
    }
    VkMemoryAllocateInfo smai {};
    smai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    smai.allocationSize  = bmr.size;
    smai.memoryTypeIndex = host_type;
    if (VkResult r = vkAllocateMemory(self->device_, &smai, nullptr,
                                      &self->staging_mem_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkAllocateMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }
    if (VkResult r = vkBindBufferMemory(self->device_, self->staging_buf_,
                                        self->staging_mem_, 0);
        r != VK_SUCCESS) {
        fail(err, std::string("vkBindBufferMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }
    if (VkResult r = vkMapMemory(self->device_, self->staging_mem_, 0,
                                 VK_WHOLE_SIZE, 0, &self->staging_map_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkMapMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }

    return self;
}

auto Producer::upload_into(VkImage target, uint32_t target_w, uint32_t target_h,
                           const uint8_t* data, std::size_t size)
    -> rstd::Result<int, Error> {
    Error err;
    int fd = upload_into_(target, target_w, target_h, data, size, &err);
    if (fd < 0) return rstd::Err(std::move(err));
    return rstd::Ok(fd);
}

int Producer::upload_into_(VkImage target, uint32_t target_w, uint32_t target_h,
                           const uint8_t* data, std::size_t size, Error* err) {
    if (target == VK_NULL_HANDLE) { fail(err, "upload_into: target VkImage is null"); return -1; }
    if (staging_buf_ == VK_NULL_HANDLE) {
        fail(err, "upload_into: Producer has no staging buffer "
                  "(from_external Producers are decode-only)");
        return -1;
    }
    if (size != staging_size_)    { fail(err, "upload_into: size mismatch"); return -1; }

    if (fence_pending_) {
        if (VkResult r = vkWaitForFences(device_, 1, &done_fence_, VK_TRUE,
                                         /* 1s */ 1'000'000'000ull);
            r != VK_SUCCESS) {
            fail(err, std::string("vkWaitForFences(prev upload): ") + vk_result_str(r));
            return -1;
        }
        if (VkResult r = vkResetFences(device_, 1, &done_fence_); r != VK_SUCCESS) {
            fail(err, std::string("vkResetFences: ") + vk_result_str(r));
            return -1;
        }
        fence_pending_ = false;
    }

    std::memcpy(staging_map_, data, size);

    if (VkResult r = vkResetCommandBuffer(cmd_, 0); r != VK_SUCCESS) {
        fail(err, std::string("vkResetCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (VkResult r = vkBeginCommandBuffer(cmd_, &bi); r != VK_SUCCESS) {
        fail(err, std::string("vkBeginCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    VkImageMemoryBarrier to_dst {};
    to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask       = 0;
    to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image               = target;
    to_dst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd_,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region {};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = { 0, 0, 0 };
    region.imageExtent                     = { target_w, target_h, 1 };
    vkCmdCopyBufferToImage(cmd_, staging_buf_, target,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    VkImageMemoryBarrier to_foreign {};
    to_foreign.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_foreign.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_foreign.dstAccessMask       = 0;
    to_foreign.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_foreign.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    to_foreign.srcQueueFamilyIndex = queue_family_;
    to_foreign.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    to_foreign.image               = target;
    to_foreign.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_foreign);

    if (VkResult r = vkEndCommandBuffer(cmd_); r != VK_SUCCESS) {
        fail(err, std::string("vkEndCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    VkSubmitInfo si {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &signal_sem_;
    if (VkResult r = vkQueueSubmit(queue_, 1, &si, done_fence_); r != VK_SUCCESS) {
        fail(err, std::string("vkQueueSubmit: ") + vk_result_str(r));
        return -1;
    }
    fence_pending_ = true;

    VkSemaphoreGetFdInfoKHR sgfi {};
    sgfi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sgfi.semaphore  = signal_sem_;
    sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int sync_fd = -1;
    if (VkResult r = vkGetSemaphoreFdKHR_(device_, &sgfi, &sync_fd);
        r != VK_SUCCESS) {
        fail(err, std::string("vkGetSemaphoreFdKHR: ") + vk_result_str(r));
        return -1;
    }
    return sync_fd;
}

} // namespace wavsen::video
