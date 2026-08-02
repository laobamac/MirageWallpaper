#include "ProtocolVideoRenderer.h"

#include <mirage_display.h>
#include <mirage_display_producer.h>
#include <mirage_display_vulkan_export.h>

#include <QFileInfo>
#include <QUrl>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pulse/error.h>
#include <pulse/simple.h>
}

namespace {

constexpr std::uint32_t DrmFourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr std::uint32_t kDrmXbgr8888 = DrmFourcc('X', 'B', '2', '4');
constexpr std::uint32_t kDrmAbgr8888 = DrmFourcc('A', 'B', '2', '4');

constexpr std::uint32_t kExportBufferCount = 3;

const char* FillModeName(VRVideoFillMode mode) {
    switch (mode) {
    case VRVideoFillModeContain: return "contain";
    case VRVideoFillModeStretch: return "stretch";
    case VRVideoFillModeCover:
    default: return "cover";
    }
}

int OpenRenderNode(std::uint32_t major, std::uint32_t minor) {
    char path[64];
    if (minor >= 128u && minor <= 255u) {
        int written = std::snprintf(path, sizeof(path), "/dev/dri/renderD%u", minor);
        if (written > 0 && static_cast<std::size_t>(written) < sizeof(path)) {
            int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) return fd;
        }
    }
    if (major != 0 || minor != 0) {
        int written = std::snprintf(path, sizeof(path), "/dev/char/%u:%u", major, minor);
        if (written > 0 && static_cast<std::size_t>(written) < sizeof(path)) {
            int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) return fd;
        }
    }
    return -1;
}

std::uint32_t ChooseMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                               VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & preferred) == preferred) {
            return i;
        }
    }
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) != 0) return i;
    }
    return UINT32_MAX;
}

} // namespace

class ProtocolHost {
public:
    ProtocolHost(std::string socket_path, std::string output_id)
        : m_socket_path(std::move(socket_path)), m_output_id(std::move(output_id)) {}

    ~ProtocolHost() { stop(); }

    ProtocolHost(const ProtocolHost&) = delete;
    ProtocolHost& operator=(const ProtocolHost&) = delete;

    void setGpuInfo(std::uint32_t major, std::uint32_t minor,
                    const std::uint8_t* device_uuid, const std::uint8_t* driver_uuid) {
        std::lock_guard lock(m_producer_mutex);
        m_drm_major = major;
        m_drm_minor = minor;
        if (device_uuid != nullptr) std::memcpy(m_device_uuid, device_uuid, 16);
        if (driver_uuid != nullptr) std::memcpy(m_driver_uuid, driver_uuid, 16);
    }

    bool start() {
        if (m_socket_path.empty() || m_output_id.empty()) return false;
        {
            std::lock_guard lock(m_producer_mutex);
            if (!connectProducerLocked()) return false;
        }
        runIo();
        std::unique_lock lock(m_state_mutex);
        return m_state_cv.wait_for(lock, std::chrono::seconds(15), [this] {
            return m_config_version != 0;
        }) && m_config_version != 0;
    }

    void runIo() {
        if (m_running.exchange(true)) return;
        m_io_thread = std::thread([this] { ioLoop(); });
    }

    void stop() {
        const bool was_running = m_running.exchange(false);
        m_state_cv.notify_all();
        if (was_running) {
            std::lock_guard lock(m_producer_mutex);
            if (m_producer != nullptr) md_producer_close(m_producer);
        }
        if (m_io_thread.joinable()) m_io_thread.join();
        std::lock_guard lock(m_producer_mutex);
        if (m_producer != nullptr) {
            md_producer_free(m_producer);
            m_producer = nullptr;
        }
    }

    bool snapshotConfig(std::uint64_t last_version, std::uint64_t last_epoch,
                        md_producer_config_t& config, std::uint64_t& version,
                        std::uint64_t& epoch) const {
        std::lock_guard lock(m_state_mutex);
        if (m_config_version == 0 ||
            (m_config_version == last_version && m_connection_epoch == last_epoch)) {
            return false;
        }
        config = m_config;
        version = m_config_version;
        epoch = m_connection_epoch;
        return true;
    }

    bool currentConfig(md_producer_config_t& config, std::uint64_t& version,
                       std::uint64_t& epoch) const {
        return snapshotConfig(0, 0, config, version, epoch);
    }

    std::uint64_t takeRetireGeneration() {
        std::lock_guard lock(m_state_mutex);
        return std::exchange(m_retire_generation, UINT64_C(0));
    }

    std::uint64_t nextGeneration() { return m_next_generation.fetch_add(1); }

    int offerPool(const md_buffer_pool_t* pool) {
        if (pool == nullptr) return MD_ERR_INVALID;
        std::lock_guard lock(m_producer_mutex);
        if (m_producer == nullptr ||
            md_producer_connection_state(m_producer) != MD_CONNECTION_READY) {
            return MD_ERR_DISCONNECTED;
        }
        const int result = md_producer_offer_buffers(m_producer, pool);
        if (result != MD_OK) return result;
        md_display_config_t display_config {
            .generation = pool->generation,
            .source = {0.0f, 0.0f, static_cast<float>(pool->width),
                       static_cast<float>(pool->height)},
            .destination = {0.0f, 0.0f, static_cast<float>(pool->width),
                            static_cast<float>(pool->height)},
            .transform = MD_TRANSFORM_NORMAL,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        return md_producer_set_config(m_producer, &display_config);
    }

    int submitFrame(std::uint64_t generation, std::uint32_t index, std::uint64_t sequence,
                    int acquire_fd, int release_fd) {
        std::lock_guard lock(m_producer_mutex);
        if (m_producer == nullptr ||
            md_producer_connection_state(m_producer) != MD_CONNECTION_READY) {
            if (acquire_fd >= 0) close(acquire_fd);
            if (release_fd >= 0) close(release_fd);
            return MD_ERR_DISCONNECTED;
        }
        return md_producer_submit_frame(m_producer, generation, index, sequence,
                                        acquire_fd, release_fd);
    }

    void retireDone(std::uint64_t generation) {
        std::lock_guard lock(m_producer_mutex);
        if (m_producer != nullptr &&
            md_producer_connection_state(m_producer) == MD_CONNECTION_READY) {
            (void)md_producer_retire_done(m_producer, generation);
        }
    }

private:
    static void OnConnected(void* opaque, std::uint64_t, std::uint64_t) {
        auto* self = static_cast<ProtocolHost*>(opaque);
        {
            std::lock_guard lock(self->m_state_mutex);
            ++self->m_connection_epoch;
            self->m_retire_generation = 0;
        }
        self->m_state_cv.notify_all();
    }

    static void OnOutputConfig(void* opaque, const md_producer_config_t* config) {
        auto* self = static_cast<ProtocolHost*>(opaque);
        if (config == nullptr) return;
        {
            std::lock_guard lock(self->m_state_mutex);
            self->m_config = *config;
            ++self->m_config_version;
        }
        self->m_state_cv.notify_all();
    }

    static void OnRetire(void* opaque, std::uint64_t generation) {
        auto* self = static_cast<ProtocolHost*>(opaque);
        std::lock_guard lock(self->m_state_mutex);
        self->m_retire_generation = generation;
    }

    static void OnPointerEnter(void*, const md_pointer_enter_t*) {}
    static void OnPointerLeave(void*, std::uint64_t) {}
    static void OnPointerMotion(void*, const md_pointer_motion_t*) {}
    static void OnPointerButton(void*, const md_pointer_button_t*) {}
    static void OnPointerAxis(void*, const md_pointer_axis_t*) {}

    static void OnDisconnected(void* opaque, md_result_t, const char*) {
        auto* self = static_cast<ProtocolHost*>(opaque);
        self->m_state_cv.notify_all();
    }

    bool connectProducerLocked() {
        if (m_producer != nullptr) md_producer_free(m_producer);
        md_producer_callbacks_t callbacks {
            .on_connected = OnConnected,
            .on_output_config = OnOutputConfig,
            .on_retire_buffers = OnRetire,
            .on_pointer_enter = OnPointerEnter,
            .on_pointer_leave = OnPointerLeave,
            .on_pointer_motion = OnPointerMotion,
            .on_pointer_button = OnPointerButton,
            .on_pointer_axis = OnPointerAxis,
            .on_disconnected = OnDisconnected,
            .user_data = this,
        };
        m_producer = md_producer_new(&callbacks);
        if (m_producer == nullptr) return false;
        const md_format_cap_t formats[] = {
            {.fourcc = kDrmXbgr8888, .plane_count = 1, .modifier = 0},
            {.fourcc = kDrmAbgr8888, .plane_count = 1, .modifier = 0},
        };
        md_producer_info_t info {
            .stable_output_id = m_output_id.c_str(),
            .kind = "video",
            .drm_render_major = m_drm_major,
            .drm_render_minor = m_drm_minor,
            .device_uuid = {},
            .driver_uuid = {},
            .formats = formats,
            .format_count = static_cast<std::uint32_t>(std::size(formats)),
        };
        std::memcpy(info.device_uuid, m_device_uuid, sizeof(info.device_uuid));
        std::memcpy(info.driver_uuid, m_driver_uuid, sizeof(info.driver_uuid));
        const int result = md_producer_connect(m_producer, m_socket_path.c_str(),
                                               "VideoWallpaper", "0.1.0", &info, 3000);
        if (result == MD_OK) return true;
        md_producer_free(m_producer);
        m_producer = nullptr;
        return false;
    }

    void ioLoop() {
        while (m_running.load()) {
            int fd = -1;
            bool wants_write = false;
            {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer != nullptr) {
                    fd = md_producer_get_fd(m_producer);
                    wants_write = md_producer_wants_writable(m_producer);
                }
            }
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::lock_guard lock(m_producer_mutex);
                if (m_running.load()) (void)connectProducerLocked();
                continue;
            }
            pollfd descriptor {
                .fd = fd,
                .events = static_cast<short>(POLLIN | (wants_write ? POLLOUT : 0)),
                .revents = 0,
            };
            const int ready = poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            bool reconnect = ready < 0 ||
                             (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (!reconnect && ready > 0) {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer == nullptr) continue;
                if ((descriptor.revents & POLLIN) != 0 &&
                    md_producer_dispatch(m_producer) < 0) {
                    reconnect = true;
                }
                if (!reconnect && (descriptor.revents & POLLOUT) != 0 &&
                    md_producer_handle_writable(m_producer) < 0) {
                    reconnect = true;
                }
            }
            if (reconnect && m_running.load()) {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer != nullptr) {
                    md_producer_free(m_producer);
                    m_producer = nullptr;
                }
            }
        }
    }

    std::string m_socket_path;
    std::string m_output_id;

    mutable std::mutex m_state_mutex;
    std::condition_variable m_state_cv;
    md_producer_config_t m_config {};
    std::uint64_t m_config_version { 0 };
    std::uint64_t m_connection_epoch { 0 };
    std::uint64_t m_retire_generation { 0 };
    std::atomic_uint64_t m_next_generation { 1 };

    std::mutex m_producer_mutex;
    md_producer_t* m_producer { nullptr };
    std::uint32_t m_drm_major { 0 };
    std::uint32_t m_drm_minor { 0 };
    std::uint8_t m_device_uuid[16] { 0 };
    std::uint8_t m_driver_uuid[16] { 0 };
    std::atomic_bool m_running { false };
    std::thread m_io_thread;
};

class VRProtocolVideoRenderer::Impl : public QObject {
public:
    explicit Impl(Config config) : m_config(std::move(config)) {}

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool start(QString* error) {
        if (m_config.socketPath.isEmpty() || m_config.outputId.isEmpty() ||
            m_config.videoPath.isEmpty()) {
            setError(error, "protocol renderer requires socket, output id and video path");
            return false;
        }
        const QFileInfo info(m_config.videoPath);
        if (!info.exists() || !info.isFile()) {
            setError(error, QStringLiteral("video file not found: %1").arg(m_config.videoPath));
            return false;
        }

        m_host = std::make_unique<ProtocolHost>(m_config.socketPath.toStdString(),
                                                m_config.outputId.toStdString());
        if (!m_host->start()) {
            setError(error,
                     "cannot connect to mirage-display broker or receive output configuration");
            return false;
        }

        if (!createVulkan(error)) return false;
        if (!rebuildPool()) {
            setError(error, "cannot create export pool for the negotiated output configuration");
            return false;
        }

        m_running.store(true);
        m_render_thread = std::thread([this] { renderLoop(); });
        return true;
    }

    void stop() {
        if (m_stopped.exchange(true)) return;
        m_running.store(false);
        m_control_cv.notify_all();
        if (m_render_thread.joinable()) m_render_thread.join();
        if (m_host != nullptr) m_host->stop();
        destroyUploadResources();
        if (m_exporter != nullptr) {
            md_vk_exporter_free(m_exporter);
            m_exporter = nullptr;
        }
        closeMedia();
        destroyVulkan();
        m_host.reset();
    }

    void play() {
        {
            std::lock_guard lock(m_control_mutex);
            m_user_paused = false;
            if (m_eof.load()) {
                m_restart_requested = true;
                m_eof.store(false);
            }
        }
        m_control_cv.notify_all();
    }

    void pause() {
        {
            std::lock_guard lock(m_control_mutex);
            m_user_paused = true;
        }
        m_control_cv.notify_all();
    }

    void setVolume(float volume) {
        m_config.volume = VRClampVideoVolume(volume);
        m_volume.store(m_config.volume);
    }

    void setMuted(bool muted) {
        m_config.muted = muted;
        m_muted.store(muted);
    }

    void setFillMode(VRVideoFillMode fillMode) {
        m_fill_mode.store(fillMode);
    }

private:
    bool createVulkan(QString* error) {
        VkApplicationInfo app_info {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "VideoWallpaper",
            .applicationVersion = 1,
            .pEngineName = nullptr,
            .engineVersion = 0,
            .apiVersion = VK_API_VERSION_1_1,
        };
        const char* drm_ext = VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
        std::uint32_t instance_ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &instance_ext_count, nullptr);
        std::vector<VkExtensionProperties> instance_exts(instance_ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &instance_ext_count,
                                               instance_exts.data());
        bool have_drm_ext = false;
        for (const auto& ext : instance_exts) {
            if (std::strcmp(ext.extensionName, drm_ext) == 0) { have_drm_ext = true; break; }
        }
        const char* enabled_instance_exts[1] = {drm_ext};
        VkInstanceCreateInfo instance_info {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pApplicationInfo = &app_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = have_drm_ext ? 1u : 0u,
            .ppEnabledExtensionNames = have_drm_ext ? enabled_instance_exts : nullptr,
        };
        if (vkCreateInstance(&instance_info, nullptr, &m_instance) != VK_SUCCESS) {
            setError(error, "cannot create Vulkan instance");
            return false;
        }
        m_have_drm_ext = have_drm_ext;

        const char* required_exts[] = {
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        };
        std::uint32_t device_count = 0;
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr) != VK_SUCCESS ||
            device_count == 0) {
            setError(error, "no Vulkan physical devices");
            destroyVulkan();
            return false;
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()) != VK_SUCCESS) {
            setError(error, "cannot enumerate Vulkan physical devices");
            destroyVulkan();
            return false;
        }
        for (VkPhysicalDevice device : devices) {
            std::uint32_t ext_count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
            std::vector<VkExtensionProperties> exts(ext_count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, exts.data());
            bool ok = true;
            for (const char* required : required_exts) {
                bool found = false;
                for (const auto& ext : exts) {
                    if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
                }
                if (!found) { ok = false; break; }
            }
            if (!ok) continue;
            std::uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
            std::uint32_t graphics_family = UINT32_MAX;
            for (std::uint32_t i = 0; i < family_count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    graphics_family = i;
                    break;
                }
            }
            if (graphics_family == UINT32_MAX) continue;
            m_physical_device = device;
            m_queue_family = graphics_family;
            break;
        }
        if (m_physical_device == VK_NULL_HANDLE) {
            setError(error, "no Vulkan device supports the DMA-BUF export extension set");
            destroyVulkan();
            return false;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = m_queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        VkDeviceCreateInfo device_info {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = static_cast<std::uint32_t>(std::size(required_exts)),
            .ppEnabledExtensionNames = required_exts,
            .pEnabledFeatures = nullptr,
        };
        if (vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device) != VK_SUCCESS) {
            setError(error, "cannot create Vulkan device");
            destroyVulkan();
            return false;
        }
        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

        PFN_vkGetPhysicalDeviceProperties2 get_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties2"));
        if (get_properties2 != nullptr && m_have_drm_ext) {
            VkPhysicalDeviceDrmPropertiesEXT drm {};
            drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceIDProperties id_props {};
            id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            id_props.pNext = &drm;
            VkPhysicalDeviceProperties2 properties {};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &id_props;
            get_properties2(m_physical_device, &properties);
            m_drm_major = drm.hasRender == VK_TRUE ? drm.renderMajor : 0u;
            m_drm_minor = drm.hasRender == VK_TRUE ? drm.renderMinor : 0u;
            std::memcpy(m_device_uuid, id_props.deviceUUID, sizeof(m_device_uuid));
            std::memcpy(m_driver_uuid, id_props.driverUUID, sizeof(m_driver_uuid));
        }

        m_drm_fd = OpenRenderNode(m_drm_major, m_drm_minor);
        if (m_drm_fd < 0) {
            for (std::uint32_t minor = 128; minor <= 255 && m_drm_fd < 0; ++minor) {
                m_drm_fd = OpenRenderNode(0, minor);
                if (m_drm_fd >= 0) m_drm_minor = minor;
            }
        }
        if (m_host != nullptr) {
            m_host->setGpuInfo(m_drm_major, m_drm_minor, m_device_uuid, m_driver_uuid);
        }

        md_vk_export_context_t context {
            .instance = m_instance,
            .physical_device = m_physical_device,
            .device = m_device,
            .queue = m_queue,
            .queue_family_index = m_queue_family,
            .drm_render_fd = m_drm_fd,
            .drm_render_major = m_drm_major,
            .drm_render_minor = m_drm_minor,
        };
        m_exporter = md_vk_exporter_new(&context);
        if (m_exporter == nullptr) {
            setError(error, "cannot create Vulkan export helper");
            destroyVulkan();
            return false;
        }

        VkCommandPoolCreateInfo pool_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_queue_family,
        };
        if (vkCreateCommandPool(m_device, &pool_info, nullptr, &m_upload_pool) != VK_SUCCESS) {
            setError(error, "cannot create Vulkan upload command pool");
            destroyVulkan();
            return false;
        }
        VkCommandBufferAllocateInfo command_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = m_upload_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(m_device, &command_info, &m_upload_cmd) != VK_SUCCESS) {
            setError(error, "cannot create Vulkan upload command buffer");
            destroyVulkan();
            return false;
        }
        VkFenceCreateInfo fence_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        if (vkCreateFence(m_device, &fence_info, nullptr, &m_upload_fence) != VK_SUCCESS) {
            setError(error, "cannot create Vulkan upload fence");
            destroyVulkan();
            return false;
        }
        return true;
    }

    void destroyVulkan() {
        if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
        if (m_upload_fence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, m_upload_fence, nullptr);
            m_upload_fence = VK_NULL_HANDLE;
        }
        if (m_upload_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_upload_pool, nullptr);
            m_upload_pool = VK_NULL_HANDLE;
            m_upload_cmd = VK_NULL_HANDLE;
        }
        if (m_device != VK_NULL_HANDLE) vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        if (m_instance != VK_NULL_HANDLE) vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        m_physical_device = VK_NULL_HANDLE;
        m_queue = VK_NULL_HANDLE;
        if (m_drm_fd >= 0) {
            close(m_drm_fd);
            m_drm_fd = -1;
        }
    }

    bool createUploadResources(std::uint32_t width, std::uint32_t height) {
        destroyUploadResources();
        VkImageCreateInfo image_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(m_device, &image_info, nullptr, &m_upload_image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(m_device, m_upload_image, &requirements);
        const std::uint32_t memory_type = ChooseMemoryType(m_physical_device,
                                                           requirements.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type == UINT32_MAX) {
            destroyUploadResources();
            return false;
        }
        VkMemoryAllocateInfo allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };
        if (vkAllocateMemory(m_device, &allocate_info, nullptr, &m_upload_memory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, m_upload_image, m_upload_memory, 0) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        const VkDeviceSize staging_size = static_cast<VkDeviceSize>(width) * height * 4u;
        VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = staging_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        if (vkCreateBuffer(m_device, &buffer_info, nullptr, &m_staging_buffer) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        vkGetBufferMemoryRequirements(m_device, m_staging_buffer, &requirements);
        const std::uint32_t staging_type = ChooseMemoryType(
            m_physical_device, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (staging_type == UINT32_MAX) {
            destroyUploadResources();
            return false;
        }
        VkMemoryAllocateInfo staging_allocate {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = staging_type,
        };
        if (vkAllocateMemory(m_device, &staging_allocate, nullptr, &m_staging_memory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_staging_buffer, m_staging_memory, 0) != VK_SUCCESS ||
            vkMapMemory(m_device, m_staging_memory, 0, staging_size, 0, &m_staging_map) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        m_upload_width = width;
        m_upload_height = height;

        if (vkResetCommandPool(m_device, m_upload_pool, 0) != VK_SUCCESS ||
            vkResetFences(m_device, 1, &m_upload_fence) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        VkCommandBufferBeginInfo begin_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        if (vkBeginCommandBuffer(m_upload_cmd, &begin_info) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        VkImageMemoryBarrier barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_upload_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(m_upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        if (vkEndCommandBuffer(m_upload_cmd) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        VkSubmitInfo submit_info {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &m_upload_cmd,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        if (vkQueueSubmit(m_queue, 1, &submit_info, m_upload_fence) != VK_SUCCESS ||
            vkWaitForFences(m_device, 1, &m_upload_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            destroyUploadResources();
            return false;
        }
        return true;
    }

    void destroyUploadResources() {
        if (m_device == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(m_device);
        if (m_staging_map != nullptr) {
            vkUnmapMemory(m_device, m_staging_memory);
            m_staging_map = nullptr;
        }
        if (m_staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_staging_buffer, nullptr);
            m_staging_buffer = VK_NULL_HANDLE;
        }
        if (m_staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_staging_memory, nullptr);
            m_staging_memory = VK_NULL_HANDLE;
        }
        if (m_upload_image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_upload_image, nullptr);
            m_upload_image = VK_NULL_HANDLE;
        }
        if (m_upload_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_upload_memory, nullptr);
            m_upload_memory = VK_NULL_HANDLE;
        }
    }

    bool rebuildPool() {
        md_producer_config_t config {};
        std::uint64_t version = 0;
        std::uint64_t epoch = 0;
        if (m_host == nullptr || !m_host->currentConfig(config, version, epoch)) return false;
        if (config.physical_width == 0 || config.physical_height == 0) return false;

        const std::uint64_t generation = m_host->nextGeneration();
        md_vk_export_pool_info_t pool_info {
            .generation = generation,
            .buffer_count = kExportBufferCount,
            .width = config.physical_width,
            .height = config.physical_height,
            .fourcc = config.fourcc,
            .plane_count = config.plane_count,
            .modifier = config.modifier,
        };
        if (m_exporter == nullptr || md_vk_exporter_create_pool(m_exporter, &pool_info) != MD_OK) {
            return false;
        }
        const md_buffer_pool_t* pool = md_vk_exporter_pool(m_exporter);
        if (m_host->offerPool(pool) != MD_OK) {
            md_vk_exporter_release_pool(m_exporter);
            return false;
        }
        m_generation = generation;
        m_config_version = version;
        m_connection_epoch = epoch;
        m_pool_width = config.physical_width;
        m_pool_height = config.physical_height;
        m_canvas.assign(static_cast<std::size_t>(m_pool_width) * m_pool_height * 4u, 0);
        return createUploadResources(m_pool_width, m_pool_height);
    }

    void serviceHostAndPool() {
        const std::uint64_t retire_generation = m_host->takeRetireGeneration();
        if (retire_generation != 0 && retire_generation == m_generation) {
            md_vk_exporter_release_pool(m_exporter);
            m_generation = 0;
            m_host->retireDone(retire_generation);
        }
        md_producer_config_t config {};
        std::uint64_t version = 0;
        std::uint64_t epoch = 0;
        if (m_host->snapshotConfig(m_config_version, m_connection_epoch, config, version, epoch)) {
            m_config_version = version;
            m_connection_epoch = epoch;
            if (!rebuildPool()) {
                fail(QStringLiteral("cannot rebuild export pool for new output configuration"));
            }
        }
    }

    bool openMedia() {
        avformat_network_init();
        if (avformat_open_input(&m_format, m_config.videoPath.toUtf8().constData(),
                                nullptr, nullptr) != 0) {
            m_last_error = "cannot open video file";
            return false;
        }
        if (avformat_find_stream_info(m_format, nullptr) < 0) {
            m_last_error = "cannot read video stream info";
            return false;
        }
        for (unsigned i = 0; i < m_format->nb_streams; ++i) {
            AVStream* stream = m_format->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_video_index < 0) {
                m_video_index = static_cast<int>(i);
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audio_index < 0) {
                m_audio_index = static_cast<int>(i);
            }
        }
        if (m_video_index < 0) {
            m_last_error = "no video stream";
            return false;
        }
        const AVCodec* video_codec =
            avcodec_find_decoder(m_format->streams[m_video_index]->codecpar->codec_id);
        if (video_codec == nullptr) {
            m_last_error = "unsupported video codec";
            return false;
        }
        m_video_codec = avcodec_alloc_context3(video_codec);
        if (avcodec_parameters_to_context(m_video_codec,
                                          m_format->streams[m_video_index]->codecpar) < 0 ||
            avcodec_open2(m_video_codec, video_codec, nullptr) < 0) {
            m_last_error = "cannot open video decoder";
            return false;
        }
        m_video_stream = m_format->streams[m_video_index];

        if (m_audio_index >= 0) {
            const AVCodec* audio_codec =
                avcodec_find_decoder(m_format->streams[m_audio_index]->codecpar->codec_id);
            if (audio_codec != nullptr) {
                m_audio_codec = avcodec_alloc_context3(audio_codec);
                if (avcodec_parameters_to_context(m_audio_codec,
                                                  m_format->streams[m_audio_index]->codecpar) == 0 &&
                    avcodec_open2(m_audio_codec, audio_codec, nullptr) == 0) {
                    m_audio_stream = m_format->streams[m_audio_index];
                } else {
                    avcodec_free_context(&m_audio_codec);
                    m_audio_codec = nullptr;
                }
            }
        }
        return true;
    }

    void closeMedia() {
        if (m_sws != nullptr) {
            sws_freeContext(m_sws);
            m_sws = nullptr;
        }
        if (m_swr != nullptr) {
            swr_free(&m_swr);
            m_swr = nullptr;
        }
        if (m_pulse != nullptr) {
            pa_simple_free(m_pulse);
            m_pulse = nullptr;
        }
        if (m_audio_codec != nullptr) {
            avcodec_free_context(&m_audio_codec);
            m_audio_codec = nullptr;
        }
        if (m_video_codec != nullptr) {
            avcodec_free_context(&m_video_codec);
            m_video_codec = nullptr;
        }
        if (m_format != nullptr) {
            avformat_close_input(&m_format);
            m_format = nullptr;
        }
        m_video_stream = nullptr;
        m_audio_stream = nullptr;
        m_video_index = -1;
        m_audio_index = -1;
    }

    bool openAudioOutput() {
        if (m_audio_codec == nullptr) return false;
        int error = 0;
        const pa_sample_spec spec {
            .format = PA_SAMPLE_S16LE,
            .rate = 48000,
            .channels = 2,
        };
        m_pulse = pa_simple_new(nullptr, "VideoWallpaper", PA_STREAM_PLAYBACK, nullptr,
                                "video", &spec, nullptr, nullptr, &error);
        if (m_pulse == nullptr) {
            std::fprintf(stderr, "VideoWallpaper: audio output unavailable (%s)\n",
                         pa_strerror(error));
            return false;
        }
        m_swr = swr_alloc();
        av_opt_set_int(m_swr, "in_channel_layout", m_audio_codec->ch_layout.order == AV_CHANNEL_ORDER_NATIVE
                                                     ? static_cast<std::int64_t>(m_audio_codec->ch_layout.u.mask)
                                                     : AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(m_swr, "in_sample_rate", m_audio_codec->sample_rate, 0);
        av_opt_set_sample_fmt(m_swr, "in_sample_fmt", m_audio_codec->sample_fmt, 0);
        av_opt_set_int(m_swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(m_swr, "out_sample_rate", 48000, 0);
        av_opt_set_sample_fmt(m_swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        if (swr_init(m_swr) < 0) {
            swr_free(&m_swr);
            pa_simple_free(m_pulse);
            m_pulse = nullptr;
            return false;
        }
        return true;
    }

    void writeAudio(const AVFrame* frame) {
        if (m_pulse == nullptr || m_swr == nullptr) return;
        const int out_samples =
            av_rescale_rnd(swr_get_delay(m_swr, frame->sample_rate) + frame->nb_samples,
                           48000, frame->sample_rate, AV_ROUND_UP);
        m_pcm.resize(static_cast<std::size_t>(out_samples) * 2u * 2u);
        uint8_t* out = m_pcm.data();
        int converted = swr_convert(m_swr, &out, out_samples,
                                    const_cast<const std::uint8_t**>(frame->data),
                                    frame->nb_samples);
        if (converted > 0) {
            int error = 0;
            if (!m_muted.load() && pa_simple_write(m_pulse, m_pcm.data(),
                                                   static_cast<std::size_t>(converted) * 4u,
                                                   &error) < 0) {
                /* Pulse hiccup; keep the wallpaper going. */
            }
        }
    }

    struct FitRect {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t w;
        std::uint32_t h;
    };

    FitRect computeFitRect(int source_w, int source_h) const {
        const double source_aspect = static_cast<double>(source_w) / source_h;
        const double pool_aspect = static_cast<double>(m_pool_width) / m_pool_height;
        FitRect rect {0, 0, m_pool_width, m_pool_height};
        switch (m_fill_mode.load()) {
        case VRVideoFillModeContain:
            if (source_aspect > pool_aspect) {
                rect.w = m_pool_width;
                rect.h = static_cast<std::uint32_t>(m_pool_width / source_aspect);
            } else {
                rect.h = m_pool_height;
                rect.w = static_cast<std::uint32_t>(m_pool_height * source_aspect);
            }
            rect.x = (m_pool_width - rect.w) / 2;
            rect.y = (m_pool_height - rect.h) / 2;
            break;
        case VRVideoFillModeCover:
            if (source_aspect > pool_aspect) {
                rect.h = m_pool_height;
                rect.w = static_cast<std::uint32_t>(m_pool_height * source_aspect);
            } else {
                rect.w = m_pool_width;
                rect.h = static_cast<std::uint32_t>(m_pool_width / source_aspect);
            }
            rect.x = (m_pool_width - rect.w) / 2;
            rect.y = (m_pool_height - rect.h) / 2;
            break;
        case VRVideoFillModeStretch:
        default:
            break;
        }
        return rect;
    }

    bool presentFrame(AVFrame* frame) {
        if (m_pool_width == 0 || m_upload_image == VK_NULL_HANDLE) return false;
        serviceHostAndPool();
        if (m_exporter == nullptr || md_vk_exporter_pool(m_exporter) == nullptr ||
            m_upload_image == VK_NULL_HANDLE) {
            return false;
        }

        const double pts = frame->pts != AV_NOPTS_VALUE
                               ? frame->pts * av_q2d(m_video_stream->time_base)
                               : -1.0;
        if (!pace(pts)) return true;

        const FitRect rect = computeFitRect(frame->width, frame->height);
        if (rect.w == 0 || rect.h == 0) return false;
        if (m_sws == nullptr ||
            m_sws_source_w != frame->width || m_sws_source_h != frame->height ||
            m_sws_source_fmt != frame->format ||
            m_sws_target_w != rect.w || m_sws_target_h != rect.h) {
            if (m_sws != nullptr) sws_freeContext(m_sws);
            m_sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                   static_cast<int>(rect.w), static_cast<int>(rect.h),
                                   AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            m_sws_source_w = frame->width;
            m_sws_source_h = frame->height;
            m_sws_source_fmt = frame->format;
            m_sws_target_w = rect.w;
            m_sws_target_h = rect.h;
            m_scaled.assign(static_cast<std::size_t>(rect.w) * rect.h * 4u, 0);
        }
        if (m_sws == nullptr) return false;
        uint8_t* dst = m_scaled.data();
        int dst_stride = static_cast<int>(rect.w) * 4;
        if (sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height,
                      &dst, &dst_stride) <= 0) {
            return false;
        }

        /* Composite the fitted region into the pool-sized canvas. */
        std::memset(m_canvas.data(), 0, m_canvas.size());
        for (std::uint32_t y = 0; y < rect.h; ++y) {
            std::memcpy(m_canvas.data() + (static_cast<std::size_t>(rect.y + y) * m_pool_width +
                                           rect.x) * 4u,
                        m_scaled.data() + static_cast<std::size_t>(y) * rect.w * 4u,
                        static_cast<std::size_t>(rect.w) * 4u);
        }
        return uploadAndSubmit();
    }

    bool pace(double pts) {
        using Clock = std::chrono::steady_clock;
        if (pts < 0.0) return true;
        const auto now = Clock::now();
        if (m_first_pts < 0.0) {
            m_first_pts = pts;
            m_t0 = now;
            return true;
        }
        if (pts < m_first_pts) {
            m_first_pts = pts;
            m_t0 = now;
            return true;
        }
        const double elapsed = std::chrono::duration<double>(now - m_t0).count();
        const double target = pts - m_first_pts;
        if (target > elapsed) {
            std::this_thread::sleep_until(m_t0 + std::chrono::duration<double>(target));
            return true;
        }
        if (elapsed - target > 0.25) return false; /* behind schedule: drop */
        return true;
    }

    bool uploadAndSubmit() {
        if (m_staging_map == nullptr) return false;
        const std::size_t bytes = static_cast<std::size_t>(m_pool_width) * m_pool_height * 4u;
        std::memcpy(m_staging_map, m_canvas.data(), bytes);

        if (vkResetCommandPool(m_device, m_upload_pool, 0) != VK_SUCCESS ||
            vkResetFences(m_device, 1, &m_upload_fence) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        if (vkBeginCommandBuffer(m_upload_cmd, &begin_info) != VK_SUCCESS) return false;
        VkImageMemoryBarrier to_dst {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_upload_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(m_upload_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_dst);
        VkBufferImageCopy region {
            .bufferOffset = 0,
            .bufferRowLength = m_pool_width,
            .bufferImageHeight = m_pool_height,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {m_pool_width, m_pool_height, 1},
        };
        vkCmdCopyBufferToImage(m_upload_cmd, m_staging_buffer, m_upload_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier to_general {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_upload_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(m_upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_general);
        if (vkEndCommandBuffer(m_upload_cmd) != VK_SUCCESS) return false;
        VkSubmitInfo submit_info {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &m_upload_cmd,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        if (vkQueueSubmit(m_queue, 1, &submit_info, m_upload_fence) != VK_SUCCESS) {
            return false;
        }
        if (vkWaitForFences(m_device, 1, &m_upload_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            return false;
        }

        if (md_vk_exporter_pool(m_exporter) == nullptr) return true;
        std::uint32_t buffer_index = 0;
        if (md_vk_exporter_acquire(m_exporter, &buffer_index) != MD_OK) {
            return true; /* all slots owned by the consumer; drop the frame */
        }
        int acquire_fd = -1;
        int release_fd = -1;
        int rc = md_vk_exporter_copy_frame(m_exporter, buffer_index, m_upload_image,
                                           VK_IMAGE_LAYOUT_GENERAL, m_pool_width,
                                           m_pool_height, &acquire_fd, &release_fd);
        if (rc != MD_OK) {
            md_vk_exporter_cancel_frame(m_exporter, buffer_index);
            return true;
        }
        rc = m_host->submitFrame(m_generation, buffer_index, m_sequence++,
                                 acquire_fd, release_fd);
        if (rc != MD_OK) {
            md_vk_exporter_cancel_frame(m_exporter, buffer_index);
            return true;
        }
        if (!m_first_frame.exchange(true) && m_config.firstFrameCallback) {
            m_config.firstFrameCallback();
        }
        return true;
    }

    void renderLoop() {
        if (!openMedia()) {
            fail(QString::fromStdString(m_last_error));
            return;
        }
        openAudioOutput();

        while (m_running.load()) {
            if (m_eof.load()) {
                if (!waitForRestart()) break;
                continue;
            }
            if (m_user_paused.load()) {
                std::unique_lock lock(m_control_mutex);
                m_control_cv.wait(lock, [this] {
                    return !m_running.load() || !m_user_paused.load();
                });
                if (!m_running.load()) break;
                continue;
            }
            if (!readAndPresentOne()) {
                if (!m_running.load()) break;
                if (m_eof.load()) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        m_running.store(false);
        m_control_cv.notify_all();
    }

    bool readAndPresentOne() {
        if (m_format == nullptr || m_video_codec == nullptr) return false;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) return false;
        const int read_result = av_read_frame(m_format, packet);
        if (read_result < 0) {
            av_packet_free(&packet);
            if (read_result == AVERROR_EOF || m_format->pb == nullptr ||
                avio_feof(m_format->pb)) {
                handleEof();
            } else {
                fail(QStringLiteral("video demux error"));
            }
            return false;
        }
        if (packet->stream_index == m_video_index) {
            if (avcodec_send_packet(m_video_codec, packet) == 0) {
                AVFrame* frame = av_frame_alloc();
                while (avcodec_receive_frame(m_video_codec, frame) == 0) {
                    (void)presentFrame(frame);
                    av_frame_unref(frame);
                }
                av_frame_free(&frame);
            }
        } else if (m_audio_index >= 0 && packet->stream_index == m_audio_index) {
            if (avcodec_send_packet(m_audio_codec, packet) == 0) {
                AVFrame* frame = av_frame_alloc();
                while (avcodec_receive_frame(m_audio_codec, frame) == 0) {
                    writeAudio(frame);
                    av_frame_unref(frame);
                }
                av_frame_free(&frame);
            }
        }
        av_packet_free(&packet);
        return true;
    }

    void handleEof() {
        if (m_pulse != nullptr) {
            int error = 0;
            (void)pa_simple_flush(m_pulse, &error);
        }
        if (m_eof.exchange(true)) return;
        if (!m_eof_notified.exchange(true) && m_config.videoDidEndCallback) {
            m_config.videoDidEndCallback();
        }
    }

    bool waitForRestart() {
        std::unique_lock lock(m_control_mutex);
        m_control_cv.wait(lock, [this] {
            return !m_running.load() || m_restart_requested.load();
        });
        if (!m_running.load()) return false;
        m_restart_requested = false;
        m_eof.store(false);
        m_eof_notified.store(false);
        lock.unlock();

        if (m_format != nullptr) {
            if (av_seek_frame(m_format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                avformat_seek_file(m_format, -1, INT64_MIN, 0, 0, 0);
            }
            avcodec_flush_buffers(m_video_codec);
            if (m_audio_codec != nullptr) avcodec_flush_buffers(m_audio_codec);
            if (m_pulse != nullptr) {
                int error = 0;
                (void)pa_simple_flush(m_pulse, &error);
            }
            m_first_pts = -1.0;
        }
        return true;
    }

    void fail(const QString& message) {
        std::fprintf(stderr, "VideoWallpaper: %s\n", message.toLocal8Bit().constData());
        if (m_config.errorCallback) m_config.errorCallback(message);
        m_running.store(false);
        m_control_cv.notify_all();
    }

    static void setError(QString* error, const QString& message) {
        if (error != nullptr) *error = message;
    }

    Config m_config;

    std::unique_ptr<ProtocolHost> m_host;

    VkInstance m_instance { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice m_device { VK_NULL_HANDLE };
    VkQueue m_queue { VK_NULL_HANDLE };
    std::uint32_t m_queue_family { 0 };
    bool m_have_drm_ext { false };
    std::uint32_t m_drm_major { 0 };
    std::uint32_t m_drm_minor { 0 };
    std::uint8_t m_device_uuid[16] { 0 };
    std::uint8_t m_driver_uuid[16] { 0 };
    int m_drm_fd { -1 };

    md_vk_exporter_t* m_exporter { nullptr };
    VkImage m_upload_image { VK_NULL_HANDLE };
    VkDeviceMemory m_upload_memory { VK_NULL_HANDLE };
    VkBuffer m_staging_buffer { VK_NULL_HANDLE };
    VkDeviceMemory m_staging_memory { VK_NULL_HANDLE };
    void* m_staging_map { nullptr };
    VkCommandPool m_upload_pool { VK_NULL_HANDLE };
    VkCommandBuffer m_upload_cmd { VK_NULL_HANDLE };
    VkFence m_upload_fence { VK_NULL_HANDLE };
    std::uint32_t m_upload_width { 0 };
    std::uint32_t m_upload_height { 0 };

    std::uint32_t m_pool_width { 0 };
    std::uint32_t m_pool_height { 0 };
    std::vector<std::uint8_t> m_canvas;
    std::vector<std::uint8_t> m_scaled;
    std::atomic<VRVideoFillMode> m_fill_mode { VRVideoFillModeCover };
    std::atomic_bool m_first_frame { false };
    std::atomic_bool m_eof_notified { false };
    std::atomic_bool m_stopped { false };
    std::atomic<float> m_volume { 1.0f };
    std::atomic_bool m_muted { false };

    std::uint64_t m_generation { 0 };
    std::uint64_t m_config_version { 0 };
    std::uint64_t m_connection_epoch { 0 };
    std::uint64_t m_sequence { 1 };

    std::thread m_render_thread;
    std::atomic_bool m_running { false };
    std::atomic_bool m_user_paused { false };
    std::atomic_bool m_eof { false };
    std::atomic_bool m_restart_requested { false };
    std::mutex m_control_mutex;
    std::condition_variable m_control_cv;
    double m_first_pts { -1.0 };
    std::chrono::steady_clock::time_point m_t0 {};

    AVFormatContext* m_format { nullptr };
    AVCodecContext* m_video_codec { nullptr };
    AVCodecContext* m_audio_codec { nullptr };
    AVStream* m_video_stream { nullptr };
    AVStream* m_audio_stream { nullptr };
    int m_video_index { -1 };
    int m_audio_index { -1 };
    SwsContext* m_sws { nullptr };
    int m_sws_source_w { 0 };
    int m_sws_source_h { 0 };
    int m_sws_source_fmt { -1 };
    std::uint32_t m_sws_target_w { 0 };
    std::uint32_t m_sws_target_h { 0 };
    SwrContext* m_swr { nullptr };
    pa_simple* m_pulse { nullptr };
    std::vector<std::uint8_t> m_pcm;
    std::string m_last_error;
};

VRProtocolVideoRenderer::VRProtocolVideoRenderer(Config config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

VRProtocolVideoRenderer::~VRProtocolVideoRenderer() = default;

bool VRProtocolVideoRenderer::start(QString* error) {
    return m_impl->start(error);
}

void VRProtocolVideoRenderer::stop() {
    m_impl->stop();
}

void VRProtocolVideoRenderer::play() {
    m_impl->play();
}

void VRProtocolVideoRenderer::pause() {
    m_impl->pause();
}

void VRProtocolVideoRenderer::setVolume(float volume) {
    m_impl->setVolume(volume);
}

void VRProtocolVideoRenderer::setMuted(bool muted) {
    m_impl->setMuted(muted);
}

void VRProtocolVideoRenderer::setFillMode(VRVideoFillMode fillMode) {
    m_impl->setFillMode(fillMode);
}
