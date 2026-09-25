#include "ProtocolVideoRenderer.h"

#include <mirage_display.h>
#include <mirage_display_producer.h>

#include <QFileInfo>

#include <vulkan/vulkan.h>

#include <atomic>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
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

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>

namespace {

constexpr std::uint32_t DrmFourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr std::uint32_t kDrmXbgr8888 = DrmFourcc('X', 'B', '2', '4');
constexpr std::uint32_t kDrmAbgr8888 = DrmFourcc('A', 'B', '2', '4');
constexpr std::uint32_t kDrmXrgb8888 = DrmFourcc('X', 'R', '2', '4');
constexpr std::uint32_t kDrmArgb8888 = DrmFourcc('A', 'R', '2', '4');

constexpr std::uint32_t kExportBufferCount = 3;

// Vulkan reports the PCI vendor of the render node that mirage-display selected.
// Keep libmpv's decoder and GL interop on that same, already-validated GPU instead
// of letting its independent "auto" selection initialize unrelated drivers.
constexpr std::uint32_t kAmdVendorId = 0x1002U;
constexpr std::uint32_t kNvidiaVendorId = 0x10deU;
constexpr std::uint32_t kIntelVendorId = 0x8086U;

const char* FillModeName(VRVideoFillMode mode) {
    switch (mode) {
    case VRVideoFillModeContain: return "contain";
    case VRVideoFillModeStretch: return "stretch";
    case VRVideoFillModeCover:
    default: return "cover";
    }
}

int OpenRenderNode(std::uint32_t major, std::uint32_t minor) {
    /* Vulkan already proved this exact render node owns the selected physical
     * device. Open only its canonical DRM path; probing another node would
     * reintroduce the mixed-GPU failure this binding is intended to prevent. */
    char path[64];
    if (major == 0U || minor < 128U || minor > 255U) return -1;
    int written = std::snprintf(path, sizeof(path), "/dev/dri/renderD%u", minor);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) return -1;
    return ::open(path, O_RDWR | O_CLOEXEC);
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
        md_producer_config_t output_config {};
        {
            std::lock_guard state_lock(m_state_mutex);
            if (m_config_version == 0U) return MD_ERR_STATE;
            output_config = m_config;
        }
        const int result = md_producer_offer_buffers(m_producer, pool);
        if (result != MD_OK) return result;
        /* Source coordinates address the producer's buffer, while destination
         * coordinates are output physical pixels.  These dimensions were
         * previously identical, but Intel deliberately renders a smaller
         * logical-size pool to reduce vaapi-copy bandwidth.  Keeping the pool
         * size as the destination then covered only part of a fractionally
         * scaled Plasma output.  Map the complete source onto the complete
         * negotiated output so QSG performs exactly one final scale. */
        md_display_config_t display_config {
            .generation = pool->generation,
            .source = {0.0f, 0.0f, static_cast<float>(pool->width),
                       static_cast<float>(pool->height)},
            .destination = {0.0f, 0.0f,
                            static_cast<float>(output_config.physical_width),
                            static_cast<float>(output_config.physical_height)},
            .transform = MD_TRANSFORM_NORMAL,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        return md_producer_set_config(m_producer, &display_config);
    }

    bool bindGpu(const md_producer_gpu_info_t& gpu) {
        std::lock_guard lock(m_producer_mutex);
        return m_producer != nullptr &&
               md_producer_bind_gpu(m_producer, &gpu) == MD_OK;
    }

    bool reconnectWithFormats(const std::vector<md_format_cap_t>& formats) {
        if (formats.empty()) return false;
        std::uint64_t previous_epoch = 0;
        std::uint64_t previous_version = 0;
        {
            std::lock_guard lock(m_state_mutex);
            previous_epoch = m_connection_epoch;
            previous_version = m_config_version;
        }
        const bool was_running = m_running.exchange(false);
        m_state_cv.notify_all();
        if (was_running && m_io_thread.joinable()) m_io_thread.join();
        {
            std::lock_guard lock(m_producer_mutex);
            m_formats = formats;
            if (m_producer != nullptr) {
                md_producer_free(m_producer);
                m_producer = nullptr;
            }
            if (!connectProducerLocked()) return false;
        }
        if (was_running) {
            m_running.store(true);
            m_io_thread = std::thread([this] { ioLoop(); });
        }
        std::unique_lock lock(m_state_mutex);
        return m_state_cv.wait_for(lock, std::chrono::seconds(15), [this, previous_epoch,
                                                                      previous_version] {
            return m_connection_epoch > previous_epoch &&
                   m_config_version > previous_version;
        });
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
        const md_format_cap_t default_formats[] = {
            {.fourcc = kDrmXrgb8888, .plane_count = 1, .modifier = 0},
            {.fourcc = kDrmArgb8888, .plane_count = 1, .modifier = 0},
            {.fourcc = kDrmXbgr8888, .plane_count = 1, .modifier = 0},
            {.fourcc = kDrmAbgr8888, .plane_count = 1, .modifier = 0},
        };
        const md_format_cap_t* formats = m_formats.empty() ? default_formats : m_formats.data();
        const std::uint32_t format_count = m_formats.empty()
                                                ? static_cast<std::uint32_t>(std::size(default_formats))
                                                : static_cast<std::uint32_t>(m_formats.size());
        md_producer_info_t info {
            .stable_output_id = m_output_id.c_str(),
            .kind = "video",
            .drm_render_major = m_drm_major,
            .drm_render_minor = m_drm_minor,
            .device_uuid = {},
            .driver_uuid = {},
            .formats = formats,
            .format_count = format_count,
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
    std::vector<md_format_cap_t> m_formats;
    std::uint8_t m_device_uuid[16] { 0 };
    std::uint8_t m_driver_uuid[16] { 0 };
    std::atomic_bool m_running { false };
    std::thread m_io_thread;
};

class VRProtocolVideoRenderer::Impl : public QObject {
public:
    explicit Impl(Config config) : m_config(std::move(config)) {
        m_diagnostics = qEnvironmentVariableIsSet("MIRAGE_DISPLAY_DIAGNOSTICS");
    }

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

private:
    struct DirectSlot {
        gbm_bo* bo { nullptr };
        EGLImageKHR image { EGL_NO_IMAGE_KHR };
        GLuint texture { 0 };
        GLuint framebuffer { 0 };
        int dma_fd { -1 };
        std::uint32_t stride { 0 };
        std::uint32_t release_handle { 0 };
        bool busy { false };
    };

public:

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
        md_producer_gpu_info_t bound_gpu {
            .drm_render_major = m_drm_major,
            .drm_render_minor = m_drm_minor,
            .device_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
            .driver_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
        };
        std::memcpy(bound_gpu.device_uuid, m_device_uuid, sizeof(bound_gpu.device_uuid));
        std::memcpy(bound_gpu.driver_uuid, m_driver_uuid, sizeof(bound_gpu.driver_uuid));
        if (!m_host->bindGpu(bound_gpu)) {
            setError(error, "target GPU binding was rejected by mirage-display");
            return false;
        }
        m_running.store(true);
        m_render_thread = std::thread([this] { renderLoop(); });
        return true;
    }

    void stop() {
        if (m_stopped.exchange(true)) return;
        m_running.store(false);
        {
            std::lock_guard lock(m_control_mutex);
            if (m_mpv != nullptr) mpv_wakeup(m_mpv);
        }
        m_control_cv.notify_all();
        if (m_render_thread.joinable()) m_render_thread.join();
        if (m_host != nullptr) m_host->stop();
        destroyVulkan();
        m_host.reset();
    }

    void play() {
        // 解除暂停必须在 mpv 线程；经命令队列 + mpv_wakeup 投递（见 postMpvCommand）。
        // loop-file=inf 下循环由 mpv 内建处理，恢复播放即继续当前循环，无需 seek。
        postMpvCommand([this] {
            int paused = 0;
            if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0) {
                std::fprintf(stderr, "VideoWallpaper: mpv pause=0 failed\n");
            }
        });
    }

    void pause() {
        // 暂停必须在 mpv 线程；经命令队列 + mpv_wakeup 投递（见 postMpvCommand）。
        postMpvCommand([this] {
            int paused = 1;
            if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0) {
                std::fprintf(stderr, "VideoWallpaper: mpv pause=1 failed\n");
            }
        });
    }

    void setVolume(float volume) {
        m_config.volume = VRClampVideoVolume(volume);
        m_volume.store(m_config.volume);
        postMpvCommand([this, volume] {
            // mpv volume 属性范围 0..100；协议约定 0..1。
            double mpv_volume = static_cast<double>(VRClampVideoVolume(volume)) * 100.0;
            if (mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &mpv_volume) < 0) {
                std::fprintf(stderr, "VideoWallpaper: mpv volume set failed\n");
            }
        });
    }

    void setMuted(bool muted) {
        m_config.muted = muted;
        m_muted.store(muted);
        postMpvCommand([this, muted] {
            int mute = muted ? 1 : 0;
            if (mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &mute) < 0) {
                std::fprintf(stderr, "VideoWallpaper: mpv mute set failed\n");
            }
        });
    }

    void setFillMode(VRVideoFillMode fillMode) {
        // The render thread applies this to libmpv before the next frame.  The
        // GL viewport alone is insufficient because mpv owns the final
        // viewport/aspect calculation during mpv_render_context_render().
        m_fill_mode.store(fillMode);
    }

private:
    bool createVulkan(QString* error) {
        md_producer_config_t target{};
        std::uint64_t target_version = 0;
        std::uint64_t target_epoch = 0;
        if (m_host == nullptr || !m_host->currentConfig(target, target_version, target_epoch) ||
            (target.target_gpu_flags & MD_TARGET_GPU_RENDER_NODE_VALID) == 0U) {
            setError(error, "mirage-display did not provide a target GPU render node");
            return false;
        }
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
            VkPhysicalDeviceDrmPropertiesEXT drm {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDeviceIDProperties id_props {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            id_props.pNext = &drm;
            VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &id_props;
            vkGetPhysicalDeviceProperties2(device, &properties);
            if (drm.hasRender != VK_TRUE || drm.renderMajor < 0 || drm.renderMinor < 0 ||
                static_cast<std::uint32_t>(drm.renderMajor) != target.target_drm_render_major ||
                static_cast<std::uint32_t>(drm.renderMinor) != target.target_drm_render_minor ||
                ((target.target_gpu_flags & MD_TARGET_GPU_DEVICE_UUID_VALID) != 0U &&
                 std::memcmp(id_props.deviceUUID, target.target_device_uuid,
                             sizeof(target.target_device_uuid)) != 0) ||
                ((target.target_gpu_flags & MD_TARGET_GPU_DRIVER_UUID_VALID) != 0U &&
                 std::memcmp(id_props.driverUUID, target.target_driver_uuid,
                             sizeof(target.target_driver_uuid)) != 0)) {
                continue;
            }
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
            m_gpu_vendor_id = properties.properties.vendorID;
            m_drm_major = static_cast<std::uint32_t>(drm.renderMajor);
            m_drm_minor = static_cast<std::uint32_t>(drm.renderMinor);
            std::memcpy(m_device_uuid, id_props.deviceUUID, sizeof(m_device_uuid));
            std::memcpy(m_driver_uuid, id_props.driverUUID, sizeof(m_driver_uuid));
            break;
        }
        if (m_physical_device == VK_NULL_HANDLE) {
            setError(error, QStringLiteral("no Vulkan DMA-BUF exporter matches consumer GPU renderD%1")
                                .arg(target.target_drm_render_minor));
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

        m_drm_fd = OpenRenderNode(m_drm_major, m_drm_minor);
        if (m_drm_fd < 0) {
            setError(error, QStringLiteral("cannot open consumer GPU renderD%1")
                                .arg(m_drm_minor));
            destroyVulkan();
            return false;
        }
        return true;
    }

    void destroyVulkan() {
        if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
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
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
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
        if (config.logical_width == 0 || config.logical_height == 0) return false;

        if (!createDirectPool(config)) {
            if (m_diagnostics) {
                std::fprintf(stderr, "VideoWallpaper diagnostics: direct pool create failed: %s\n",
                             m_last_error.c_str());
            }
            return false;
        }
        const int offer_result = m_host->offerPool(&m_direct_pool);
        if (offer_result != MD_OK) {
            if (m_diagnostics) {
                std::fprintf(stderr,
                             "VideoWallpaper diagnostics: direct pool offer failed: %d\n",
                             offer_result);
            }
            destroyDirectPool();
            return false;
        }
        m_generation = m_direct_pool.generation;
        m_config_version = version;
        m_connection_epoch = epoch;
        m_pool_width = m_direct_pool.width;
        m_pool_height = m_direct_pool.height;
        if (m_diagnostics) {
            std::fprintf(stderr,
                         "VideoWallpaper diagnostics: direct pool active generation=%llu "
                         "size=%ux%u fourcc=%u modifier=%llu\n",
                         static_cast<unsigned long long>(m_generation), m_pool_width,
                         m_pool_height, config.fourcc,
                         static_cast<unsigned long long>(config.modifier));
        }
        return true;
    }

    void serviceHostAndPool() {
        const std::uint64_t retire_generation = m_host->takeRetireGeneration();
        if (retire_generation != 0 && retire_generation == m_generation) {
            destroyDirectPool();
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
                fail(QStringLiteral("cannot rebuild GBM/EGL pool for new output configuration"));
            }
        }
    }

    // —— libmpv 集成（解码/音频/同步/循环全部交由 libmpv）——
    // 向渲染线程（mpv 线程）投递命令。mpv_wakeup 是唯一允许跨线程调用的
    // mpv API；其余 mpv_* 调用必须发生在渲染线程（见 renderLoop）。wakeup
    // 保持在锁内，与 cleanupMpv() 中 m_mpv 置空互斥，避免指向已销毁 handle。
    void postMpvCommand(std::function<void()> command) {
        std::lock_guard lock(m_control_mutex);
        m_mpv_commands.push_back(std::move(command));
        if (m_mpv != nullptr) mpv_wakeup(m_mpv);
    }

    bool openWithMpv() {
        m_mpv = mpv_create();
        if (m_mpv == nullptr) {
            // mpv_create 极少失败（仅内部分配失败）；打印 errno 与 locale 以便
            // 定位环境相关问题（如非 C locale 或库加载异常）。
            std::fprintf(stderr,
                         "VideoWallpaper: mpv_create failed: errno=%d (%s) "
                         "LC_NUMERIC=%s\n",
                         errno, std::strerror(errno),
                         std::setlocale(LC_NUMERIC, nullptr) != nullptr
                             ? std::setlocale(LC_NUMERIC, nullptr)
                             : "?");
            m_last_error = "cannot create libmpv handle";
            return false;
        }
        if (m_diagnostics) {
            // libmpv does not forward its terminal diagnostics to a client by
            // default. Request debug messages so VA-API initialization,
            // hardware-frame negotiation, and EGL interop failures reach the
            // renderer stderr stream consumed by MirageLogService.
            const int log_result = mpv_request_log_messages(m_mpv, "debug");
            if (log_result < 0) {
                m_last_error = "cannot enable libmpv debug logging";
                return false;
            }
        }
        // 行为可预测：不读用户 mpv.conf、不加载脚本；必须显式 vo=libmpv，
        // 否则 mpv 会打开默认 VO 窗口。解码器与 GL interop 必须按 Vulkan 已
        // 验证的目标 GPU 明确选择；mpv 的 auto 会在 Intel 上加载 CUDA interop，
        // 随后可能静默回退软解，破坏同一 render node 上的零拷贝路径。
        const char* hardware_decoder;
        const char* hardware_interop;
        switch (m_gpu_vendor_id) {
        case kIntelVendorId:
            /* The tested iHD stack corrupts its VA surface pool when mpv
             * repeatedly exports decoded surfaces into EGL.  Keep VA hardware
             * decode but copy completed NV12 frames before GL upload; the
             * negotiated Intel tiled output below avoids the separate 4K60
             * linear-render-target bottleneck that made this mode saturate the
             * integrated GPU. */
            hardware_decoder = "vaapi-copy";
            hardware_interop = "no";
            break;
        case kAmdVendorId:
            /* AMD keeps decoded VA surfaces on the GPU and imports them into
             * EGL directly; its existing zero-copy path is unchanged. */
            hardware_decoder = "vaapi";
            hardware_interop = "vaapi";
            break;
        case kNvidiaVendorId:
            hardware_decoder = "nvdec";
            hardware_interop = "cuda";
            break;
        default:
            m_last_error = "target GPU vendor has no configured hardware decoder";
            return false;
        }
        m_expected_hwdec = hardware_decoder;
        const std::string vaapi_device = "/dev/dri/renderD" + std::to_string(m_drm_minor);
        const struct {
            const char* name;
            const char* value;
        } options[] = {
            {"config", "no"},
            {"load-scripts", "no"},
            {"vo", "libmpv"},
            {"hwdec", hardware_decoder},
            {"gpu-hwdec-interop", hardware_interop},
            // Wallpaper rendering requires GPU decode. A decoder failure must be
            // reported by mpv instead of silently shifting sustained work to the CPU.
            {"hwdec-software-fallback", "no"},
            {"vaapi-device", vaapi_device.c_str()},
            // The presentation FBO already has the negotiated pool size. Bilinear scaling
            // needs one GPU pass, while mpv's higher-quality defaults add passes without
            // creating desktop-visible detail. These renderer options preserve the direct
            // hardware-decoded texture path and deliberately avoid an additional readback.
            {"scale", "bilinear"},
            {"cscale", "bilinear"},
            {"dscale", "bilinear"},
            {"correct-downscaling", "no"},
            {"linear-downscaling", "no"},
            {"sigmoid-upscaling", "no"},
            {"ao", "pipewire,pulseaudio,alsa"},
            // 自动循环交由 mpv 内建 loop-file=inf（与 macOS AVPlayerLooper 的无缝
            // 循环对齐）：每圈循环产生 MPV_EVENT_PLAYBACK_RESTART，由 handleMpvEvent
            // 映射为 video-did-end 事件。keep-open 不可用：实测 keep-open=yes 会吞掉
            // 结束事件，且 loop-file=inf 下 EOF 永不出现，keep-open 无意义。
            {"loop-file", "inf"},
        };
        for (const auto& option : options) {
            if (mpv_set_option_string(m_mpv, option.name, option.value) < 0) {
                m_last_error = std::string("cannot set libmpv option ") + option.name;
                return false;
            }
        }
        if (m_gpu_vendor_id == kIntelVendorId) {
            /* Intel uses vaapi-copy because the tested iHD zero-copy export
             * corrupts after a few dozen frames. The wallpaper output is SDR
             * RGBA8, so an rgba16f intermediate plus 8-bit dithering only
             * doubles integrated-memory traffic. mpv documents PBO uploads as
             * driver-dependent, and they increase GPU load on the tested Intel
             * iHD/Mesa stack, so the default direct upload path is retained.
             * Other vendors keep their existing zero-copy options unchanged. */
            const struct {
                const char* name;
                const char* value;
            } intel_options[] = {
                {"fbo-format", "rgba8"},
                {"dither-depth", "no"},
            };
            for (const auto& option : intel_options) {
                if (mpv_set_option_string(m_mpv, option.name, option.value) < 0) {
                    m_last_error = std::string("cannot set Intel libmpv option ") + option.name;
                    return false;
                }
            }
        }
        if (mpv_initialize(m_mpv) < 0) {
            m_last_error = "libmpv initialization failed";
            return false;
        }
        // 先建 headless EGL/GLES3 上下文（mpv GL render API 的宿主，需 GL
        // context current），再创建 render context。
        if (!createGlContext()) {
            m_last_error = "cannot create headless EGL/GLES3 context: " + m_last_error;
            return false;
        }
        const char* api = MPV_RENDER_API_TYPE_OPENGL;
        mpv_opengl_init_params gl_params = {
            .get_proc_address = getGlProcAddress,
            .get_proc_address_ctx = nullptr,
        };
        // The libmpv OpenGL API cannot derive a VA display from a surfaceless
        // EGL device context. Pass the already-opened target render node via
        // the documented DRM display parameter so VA-API interop initializes
        // on the same GPU as Vulkan, GBM, and EGL. fd/crtc/connector use the
        // API-defined invalid sentinel because mpv does not own scanout here.
        mpv_opengl_drm_params_v2 drm_params = {
            .fd = -1,
            .crtc_id = -1,
            .connector_id = -1,
            .atomic_request_ptr = nullptr,
            .render_fd = m_drm_fd,
        };
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(api)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_params},
            {MPV_RENDER_PARAM_DRM_DISPLAY_V2, &drm_params},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&m_render, m_mpv, render_params) < 0) {
            m_last_error = "libmpv opengl render context creation failed";
            return false;
        }
        // 初始属性（壁纸总是自动播放；volume 0..100）。
        double initial_volume =
            static_cast<double>(VRClampVideoVolume(m_config.volume)) * 100.0;
        int initial_mute = m_config.muted ? 1 : 0;
        int initial_pause = 0;
        if (mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &initial_volume) < 0 ||
            mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &initial_mute) < 0 ||
            mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &initial_pause) < 0) {
            m_last_error = "cannot set libmpv initial properties";
            return false;
        }
        const QByteArray path = m_config.videoPath.toUtf8();
        const char* load_command[] = {"loadfile", path.constData(), nullptr};
        if (mpv_command(m_mpv, load_command) < 0) {
            m_last_error = "libmpv loadfile failed";
            return false;
        }
        // 等待 FILE_LOADED 或加载错误（最多 15s），与引擎 open() 语义一致。
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!m_running.load()) {
                m_last_error = "renderer stopped while opening video";
                return false;
            }
            mpv_event* event = mpv_wait_event(m_mpv, 0.05);
            // Hardware decoder and GL interop initialization happens before
            // FILE_LOADED. Forward those diagnostics here; otherwise this
            // wait loop consumes and discards the only useful VA-API error.
            if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
                handleMpvEvent(event);
                continue;
            }
            if (event->event_id == MPV_EVENT_FILE_LOADED) return true;
            if (event->event_id == MPV_EVENT_END_FILE) {
                const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                if (end->reason == MPV_END_FILE_REASON_ERROR) {
                    const char* text = mpv_error_string(end->error);
                    m_last_error = std::string("libmpv failed to load video: ") +
                                   (text != nullptr ? text : "unknown error");
                    return false;
                }
            }
        }
        m_last_error = "timeout loading video with libmpv";
        return false;
    }

    void handleMpvEvent(const mpv_event* event) {
        switch (event->event_id) {
        case MPV_EVENT_LOG_MESSAGE: {
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            if (message != nullptr) {
                std::fprintf(stderr, "VideoWallpaper mpv[%s][%s]: %s",
                             message->prefix != nullptr ? message->prefix : "?",
                             message->level != nullptr ? message->level : "?",
                             message->text != nullptr ? message->text : "\n");
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            // loop-file=inf 下 EOF 永不出现（无缝循环，不产生 END_FILE）；
            // 此处仅处理加载错误。
            const auto* end = static_cast<const mpv_event_end_file*>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                const char* text = mpv_error_string(end->error);
                fail(QStringLiteral("libmpv playback error: %1")
                         .arg(text != nullptr ? QString::fromUtf8(text)
                                              : QStringLiteral("unknown error")));
            }
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART: {
            // loop-file=inf 每圈循环都会触发 PLAYBACK_RESTART（实测：7 圈 → 7 次）。
            // 首次 loadfile 后的第一个 restart 是首圈开始，不发事件；此后每个 restart
            // 表示上一圈播完、新一圈开始，映射为 video-did-end 事件——与 macOS
            // AVPlayerLooper 每圈结束发 videoDidEndBlock 的语义对齐（供 playlist
            // videoSequence 切壁纸）。仅在 mpv 线程执行，m_looped_once 无需原子。
            if (!m_looped_once) {
                m_looped_once = true;
                break;
            }
            if (m_config.videoDidEndCallback) {
                m_config.videoDidEndCallback();
            }
            break;
        }
        default:
            break;
        }
    }

    void processMpvCommands() {
        std::deque<std::function<void()>> commands;
        {
            std::lock_guard lock(m_control_mutex);
            commands.swap(m_mpv_commands);
        }
        for (auto& command : commands) command();
    }

    void cleanupMpv() {
        // mpv 资源在渲染线程释放（与创建同线程）；置空加锁避免与
        // postMpvCommand()/stop() 跨线程读 m_mpv 竞争。
        mpv_render_context* render = nullptr;
        mpv_handle* handle = nullptr;
        {
            std::lock_guard lock(m_control_mutex);
            render = m_render;
            m_render = nullptr;
            handle = m_mpv;
            m_mpv = nullptr;
        }
        destroyDirectPool();
        if (render != nullptr) mpv_render_context_free(render);
        if (handle != nullptr) mpv_terminate_destroy(handle);
        // DirectSlot 的 GL 对象已在 destroyDirectPool 中释放，再销毁 EGL 上下文。
        if (m_egl_context != EGL_NO_CONTEXT) {
            eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
            eglDestroyContext(m_egl_display, m_egl_context);
            m_egl_context = EGL_NO_CONTEXT;
        }
        if (m_egl_display != EGL_NO_DISPLAY) {
            eglTerminate(m_egl_display);
            m_egl_display = EGL_NO_DISPLAY;
        }
        if (m_gbm_device != nullptr) {
            gbm_device_destroy(m_gbm_device);
            m_gbm_device = nullptr;
        }
    }

    // mpv GL render API 的 GL 函数解析回调（EGL 提供）。
    static void* getGlProcAddress(void* fn_ctx, const char* name) {
        (void)fn_ctx;
        return reinterpret_cast<void*>(eglGetProcAddress(name));
    }

    // 创建绑定到 consumer render node 的 headless EGL/GLES3 上下文。使用
    // EGL_EXT_device_drm_render_node 选择设备，禁止 EGL_DEFAULT_DISPLAY，
    // 避免 EGL 与 Vulkan/VA-API 落到不同 GPU。
    bool createGlContext() {
        if (m_drm_fd < 0) {
            m_last_error = "DRM render node is not open for GBM";
            return false;
        }
        m_gbm_device = gbm_create_device(m_drm_fd);
        if (m_gbm_device == nullptr) {
            m_last_error = "gbm_create_device failed";
            return false;
        }
        using QueryDevices = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
        using QueryDeviceString = const char* (*)(EGLDeviceEXT, EGLint);
        auto query_devices = reinterpret_cast<QueryDevices>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto query_device_string = reinterpret_cast<QueryDeviceString>(
            eglGetProcAddress("eglQueryDeviceStringEXT"));
        auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (query_devices == nullptr || query_device_string == nullptr ||
            get_platform_display == nullptr) {
            m_last_error = "EGL device/render-node extensions are unavailable";
            return false;
        }
        const std::string target_node = "/dev/dri/renderD" + std::to_string(m_drm_minor);
        EGLDeviceEXT devices[16]{};
        EGLint device_count = 0;
        if (query_devices(16, devices, &device_count) != EGL_TRUE) {
            m_last_error = "eglQueryDevicesEXT failed";
            return false;
        }
        EGLDeviceEXT target_device = EGL_NO_DEVICE_EXT;
        for (EGLint index = 0; index < device_count; ++index) {
            const char* node = query_device_string(devices[index], EGL_DRM_RENDER_NODE_FILE_EXT);
            if (node != nullptr && target_node == node) {
                target_device = devices[index];
                break;
            }
        }
        if (target_device == EGL_NO_DEVICE_EXT) {
            m_last_error = "EGL has no device for consumer render node " + target_node;
            return false;
        }
        m_egl_display = get_platform_display(EGL_PLATFORM_DEVICE_EXT, target_device, nullptr);
        if (m_egl_display == EGL_NO_DISPLAY) {
            m_last_error = "eglGetPlatformDisplay(surfaceless) failed";
            return false;
        }
        EGLint egl_major = 0;
        EGLint egl_minor = 0;
        if (!eglInitialize(m_egl_display, &egl_major, &egl_minor)) {
            m_last_error = "eglInitialize failed";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            m_last_error = "eglBindAPI(ES) failed";
            return false;
        }
        const EGLint config_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint num_config = 0;
        if (!eglChooseConfig(m_egl_display, config_attrs, &config, 1, &num_config) ||
            num_config == 0) {
            m_last_error = "eglChooseConfig failed";
            return false;
        }
        const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_egl_context =
            eglCreateContext(m_egl_display, config, EGL_NO_CONTEXT, context_attrs);
        if (m_egl_context == EGL_NO_CONTEXT) {
            m_last_error = "eglCreateContext(ES3) failed";
            return false;
        }
        if (!eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            m_egl_context)) {
            m_last_error = "eglMakeCurrent(surfaceless) failed";
            return false;
        }
        m_egl_create_image = std::bit_cast<PFNEGLCREATEIMAGEKHRPROC>(
            eglGetProcAddress("eglCreateImageKHR"));
        m_egl_destroy_image = std::bit_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        m_egl_create_sync = std::bit_cast<PFNEGLCREATESYNCKHRPROC>(
            eglGetProcAddress("eglCreateSyncKHR"));
        m_egl_destroy_sync = std::bit_cast<PFNEGLDESTROYSYNCKHRPROC>(
            eglGetProcAddress("eglDestroySyncKHR"));
        m_egl_dup_native_fence = std::bit_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
        m_gl_image_target = std::bit_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (m_egl_create_image == nullptr || m_egl_destroy_image == nullptr ||
            m_egl_create_sync == nullptr || m_egl_destroy_sync == nullptr ||
            m_egl_dup_native_fence == nullptr || m_gl_image_target == nullptr) {
            m_last_error = "EGL DMA-BUF image or native-fence extensions are unavailable";
            return false;
        }
        return true;
    }

    void destroyDirectPool() {
        if (m_egl_display != EGL_NO_DISPLAY && m_egl_destroy_image != nullptr) {
            for (DirectSlot& slot : m_direct_slots) {
                if (slot.release_handle != 0U && m_drm_fd >= 0) {
                    drmSyncobjDestroy(m_drm_fd, slot.release_handle);
                }
                if (slot.framebuffer != 0U) glDeleteFramebuffers(1, &slot.framebuffer);
                if (slot.texture != 0U) glDeleteTextures(1, &slot.texture);
                if (slot.image != EGL_NO_IMAGE_KHR) m_egl_destroy_image(m_egl_display, slot.image);
                if (slot.dma_fd >= 0) ::close(slot.dma_fd);
                if (slot.bo != nullptr) gbm_bo_destroy(slot.bo);
                slot = DirectSlot{};
            }
        }
        m_direct_pool = md_buffer_pool_t{};
        m_direct_pool_active = false;
    }

    bool createDirectPool(const md_producer_config_t& config) {
        if (m_gbm_device == nullptr || m_egl_display == EGL_NO_DISPLAY) return false;
        if (config.fourcc != kDrmXrgb8888 && config.fourcc != kDrmArgb8888 &&
            config.fourcc != kDrmXbgr8888 && config.fourcc != kDrmAbgr8888) {
            m_last_error = "negotiated video format is not an RGBA GBM format";
            return false;
        }
        destroyDirectPool();
        const std::uint32_t gbm_format =
            config.fourcc == kDrmArgb8888
                ? GBM_FORMAT_ARGB8888
                : config.fourcc == kDrmAbgr8888
                      ? GBM_FORMAT_ABGR8888
                      : config.fourcc == kDrmXrgb8888 ? GBM_FORMAT_XRGB8888
                                                      : GBM_FORMAT_XBGR8888;
        /* Qt Wayland uses the next integer buffer scale for fractional-scale
         * Plasma surfaces: a 125% desktop can therefore expose a much larger
         * scene-graph backing store than its logical wallpaper item. Intel's
         * vaapi-copy path must not render that oversized private target; the
         * protocol pool has independent dimensions and QSG already scales its
         * texture node. AMD and NVIDIA retain their physical-size zero-copy
         * path, where this bandwidth reduction is neither needed nor desired. */
        const bool use_logical_pool = m_gpu_vendor_id == kIntelVendorId;
        const std::uint32_t pool_width =
            use_logical_pool ? config.logical_width : config.physical_width;
        const std::uint32_t pool_height =
            use_logical_pool ? config.logical_height : config.physical_height;
        m_direct_pool.generation = m_host->nextGeneration();
        m_direct_pool.buffer_count = kExportBufferCount;
        m_direct_pool.width = pool_width;
        m_direct_pool.height = pool_height;
        m_direct_pool.fourcc = config.fourcc;
        m_direct_pool.plane_count = 1U;
        m_direct_pool.modifier = config.modifier;
        for (std::uint32_t index = 0U; index < kExportBufferCount; ++index) {
            DirectSlot& slot = m_direct_slots[index];
            /* The display advertises modifier zero as the linear DRM layout.
             * Rendering-only allocation lets GBM choose a tiled BO on several
             * drivers, which no longer matches the protocol tuple. Request
             * linear storage explicitly for zero; non-zero negotiated modifiers
             * stay explicit so their tiling is preserved. */
            if (config.modifier == 0U) {
                slot.bo = gbm_bo_create(m_gbm_device, pool_width,
                                        pool_height, gbm_format,
                                        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            } else {
                const std::uint64_t requested_modifier = config.modifier;
                slot.bo = gbm_bo_create_with_modifiers2(
                    m_gbm_device, pool_width, pool_height, gbm_format,
                    &requested_modifier, 1U, GBM_BO_USE_RENDERING);
            }
            if (slot.bo == nullptr) {
                const int allocation_errno = errno;
                m_last_error = "GBM BO allocation failed for fourcc=" +
                               std::to_string(config.fourcc) +
                               " modifier=" + std::to_string(config.modifier) +
                               " size=" + std::to_string(pool_width) + "x" +
                               std::to_string(pool_height) +
                               " errno=" + std::to_string(allocation_errno) + " (" +
                               std::strerror(allocation_errno) + ")";
                destroyDirectPool();
                return false;
            }
            const std::uint64_t actual_modifier = gbm_bo_get_modifier(slot.bo);
            if (actual_modifier != config.modifier) {
                m_last_error = "GBM returned modifier=" + std::to_string(actual_modifier) +
                               " for negotiated modifier=" + std::to_string(config.modifier);
                destroyDirectPool();
                return false;
            }
            slot.dma_fd = gbm_bo_get_fd(slot.bo);
            slot.stride = gbm_bo_get_stride(slot.bo);
            if (slot.dma_fd < 0 || slot.stride == 0U) {
                m_last_error = "GBM DMA-BUF descriptor export failed";
                destroyDirectPool();
                return false;
            }
            std::array<EGLint, 20U> image_attrs{};
            std::uint32_t attr_count = 0U;
            image_attrs[attr_count++] = EGL_WIDTH;
            image_attrs[attr_count++] = static_cast<EGLint>(pool_width);
            image_attrs[attr_count++] = EGL_HEIGHT;
            image_attrs[attr_count++] = static_cast<EGLint>(pool_height);
            image_attrs[attr_count++] = EGL_LINUX_DRM_FOURCC_EXT;
            image_attrs[attr_count++] = static_cast<EGLint>(config.fourcc);
            image_attrs[attr_count++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            image_attrs[attr_count++] = slot.dma_fd;
            image_attrs[attr_count++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            image_attrs[attr_count++] = 0;
            image_attrs[attr_count++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            image_attrs[attr_count++] = static_cast<EGLint>(slot.stride);
            /* v1.2 treats modifier zero as an explicit linear modifier.  Pass
             * both halves even for zero so EGL cannot reinterpret the BO as an
             * implicit-layout image on drivers that distinguish the two forms. */
            image_attrs[attr_count++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            image_attrs[attr_count++] = static_cast<EGLint>(config.modifier & UINT64_C(0xffffffff));
            image_attrs[attr_count++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            image_attrs[attr_count++] = static_cast<EGLint>(config.modifier >> 32U);
            image_attrs[attr_count] = EGL_NONE;
            slot.image = m_egl_create_image(m_egl_display, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, nullptr, image_attrs.data());
            if (slot.image == EGL_NO_IMAGE_KHR) {
                m_last_error = "eglCreateImageKHR(DMA-BUF) failed";
                destroyDirectPool();
                return false;
            }
            glGenTextures(1, &slot.texture);
            glBindTexture(GL_TEXTURE_2D, slot.texture);
            /* EGLImage-backed DMA-BUF textures have no mipmap chain.  GLES
             * defaults GL_TEXTURE_MIN_FILTER to a mipmap mode, which makes
             * the FBO attachment incomplete even though the EGL import
             * itself succeeded. */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            m_gl_image_target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(slot.image));
            glGenFramebuffers(1, &slot.framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   slot.texture, 0);
            const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            const GLenum gl_error = glGetError();
            if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
                m_last_error = "GBM/EGL framebuffer is incomplete status=" +
                               std::to_string(static_cast<unsigned int>(framebuffer_status)) +
                               " gl_error=" +
                               std::to_string(static_cast<unsigned int>(gl_error));
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                destroyDirectPool();
                return false;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            md_plane_t& plane = m_direct_pool.planes[index][0];
            plane.fd = slot.dma_fd;
            plane.stride = slot.stride;
            plane.offset = 0U;
            plane.size = static_cast<std::uint64_t>(slot.stride) * pool_height;
        }
        m_direct_pool_active = true;
        return true;
    }

    int acquireDirectSlot() {
        for (std::uint32_t index = 0U; index < kExportBufferCount; ++index) {
            DirectSlot& slot = m_direct_slots[index];
            if (slot.busy) {
                std::uint32_t handle = slot.release_handle;
                if (drmSyncobjWait(m_drm_fd, &handle, 1U, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
                                   nullptr) == 0) {
                    drmSyncobjDestroy(m_drm_fd, slot.release_handle);
                    slot.release_handle = 0U;
                    slot.busy = false;
                }
            }
            if (!slot.busy) return static_cast<int>(index);
        }
        return -1;
    }

    bool submitDirectFrame(const std::uint32_t index) {
        DirectSlot& slot = m_direct_slots[index];
        const EGLint sync_attrs[] = {EGL_NONE};
        const EGLSyncKHR sync = m_egl_create_sync(m_egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                                                  sync_attrs);
        if (sync == EGL_NO_SYNC_KHR) return false;
        /* Creating a native fence inserts it after the frame writes in the GL
         * command stream.  Flush only after that insertion: a pre-fence flush
         * cannot submit the fence and leaves Intel/Mesa consumers waiting on an
         * acquire sync_file while the producer exhausts its three buffer slots. */
        glFlush();
        const EGLint acquire_fd = m_egl_dup_native_fence(m_egl_display, sync);
        const EGLBoolean sync_destroyed = m_egl_destroy_sync(m_egl_display, sync);
        if (sync_destroyed != EGL_TRUE || acquire_fd < 0) {
            if (acquire_fd >= 0) ::close(acquire_fd);
            return false;
        }
        if (drmSyncobjCreate(m_drm_fd, 0U, &slot.release_handle) != 0) {
            ::close(acquire_fd);
            return false;
        }
        int release_fd = -1;
        if (drmSyncobjHandleToFD(m_drm_fd, slot.release_handle, &release_fd) != 0) {
            drmSyncobjDestroy(m_drm_fd, slot.release_handle);
            slot.release_handle = 0U;
            ::close(acquire_fd);
            return false;
        }
        slot.busy = true;
        const int result = m_host->submitFrame(m_direct_pool.generation, index, m_sequence++,
                                               acquire_fd, release_fd);
        if (result != MD_OK) {
            drmSyncobjDestroy(m_drm_fd, slot.release_handle);
            slot.release_handle = 0U;
            slot.busy = false;
            return false;
        }
        if (!m_first_frame.exchange(true) && m_config.firstFrameCallback) {
            m_config.firstFrameCallback();
        }
        ++m_direct_frames;
        if (m_diagnostics && (m_direct_frames % 120U) == 0U) {
            std::fprintf(stderr,
                         "VideoWallpaper diagnostics: direct_frames=%llu slot_drops=%llu bytes=%llu\n",
                         static_cast<unsigned long long>(m_direct_frames),
                         static_cast<unsigned long long>(m_direct_slot_drops),
                         static_cast<unsigned long long>(m_pool_width) * m_pool_height * 4U);
        }
        return true;
    }

    // mpv renders directly into a GBM-backed EGLImage. Fill mode is expressed
    // by viewport/scissor state, so no CPU canvas, readback, or Vulkan upload
    // image is needed between mpv and the protocol DMA-BUF.
    void presentMpvFrame() {
        serviceHostAndPool();
        ++m_present_calls;
        if (!m_direct_pool_active || m_pool_width == 0 || m_pool_height == 0) {
            ++m_present_without_pool;
            return;
        }

        // 惰性初始化：读解码输出尺寸（video-params/w,h 实测为源尺寸，不受
        // 渲染路径影响），用于计算 fit 目标。
        if (m_video_src_w <= 0 || m_video_src_h <= 0) {
            long long src_w = 0;
            long long src_h = 0;
            if (mpv_get_property(m_mpv, "video-params/w", MPV_FORMAT_INT64, &src_w) < 0 ||
                mpv_get_property(m_mpv, "video-params/h", MPV_FORMAT_INT64, &src_h) < 0 ||
                src_w <= 0 || src_h <= 0) {
                ++m_present_without_video_params;
                return; // 尚无解码参数（加载中），跳过本帧
            }
            m_video_src_w = static_cast<int>(src_w);
            m_video_src_h = static_cast<int>(src_h);
        }

        if (!m_hwdec_validated) {
            // MPV_RENDER_UPDATE_FRAME also covers an initial blank redraw, so
            // hwdec-current is only authoritative after video-params confirms
            // that mpv has loaded the video decoder. Rejecting "no" at that
            // point prevents a real initialization failure from silently
            // becoming a CPU decoding session.
            char* current_hwdec = mpv_get_property_string(m_mpv, "hwdec-current");
            if (current_hwdec == nullptr ||
                std::strcmp(current_hwdec, m_expected_hwdec) != 0) {
                const QString actual = current_hwdec != nullptr
                    ? QString::fromUtf8(current_hwdec)
                    : QStringLiteral("unavailable");
                mpv_free(current_hwdec);
                fail(QStringLiteral("hardware decoder %1 was required, but libmpv selected %2")
                         .arg(QString::fromUtf8(m_expected_hwdec), actual));
                return;
            }
            std::fprintf(stderr,
                         "VideoWallpaper: hardware decoder active: %s on renderD%u\n",
                         current_hwdec, m_drm_minor);
            mpv_free(current_hwdec);
            m_hwdec_validated = true;
        }

        const VRVideoFillMode fill_mode = m_fill_mode.load();
        if (!m_fill_mode_applied || m_applied_fill_mode != fill_mode) {
            int keep_aspect = fill_mode == VRVideoFillModeStretch ? 0 : 1;
            double panscan = fill_mode == VRVideoFillModeCover ? 1.0 : 0.0;
            if (mpv_set_property(m_mpv, "keepaspect", MPV_FORMAT_FLAG,
                                 &keep_aspect) < 0 ||
                mpv_set_property(m_mpv, "panscan", MPV_FORMAT_DOUBLE,
                                 &panscan) < 0) {
                ++m_present_render_errors;
                return;
            }
            m_applied_fill_mode = fill_mode;
            m_fill_mode_applied = true;
        }
        const int index = acquireDirectSlot();
        if (index < 0) {
            ++m_direct_slot_drops;
            /* The protocol slots remain consumer-owned until their release
             * syncobjs signal, but libmpv must still consume this queued frame.
             * Skipping through the render API releases its VA-API surface;
             * returning without rendering eventually exhausts Intel's decoder
             * surface pool and turns subsequent frames into corrupted output. */
            int skip_rendering = 1;
            mpv_render_param skip_params[] = {
                {MPV_RENDER_PARAM_SKIP_RENDERING, &skip_rendering},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            if (mpv_render_context_render(m_render, skip_params) < 0) {
                ++m_present_render_errors;
            }
            return;
        }
        DirectSlot& slot = m_direct_slots[static_cast<std::size_t>(index)];
        glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, static_cast<GLsizei>(m_pool_width),
                  static_cast<GLsizei>(m_pool_height));
        glViewport(0, 0, static_cast<GLsizei>(m_pool_width),
                   static_cast<GLsizei>(m_pool_height));
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        mpv_opengl_fbo fbo_params = {
            .fbo = static_cast<int>(slot.framebuffer),
            /* mpv requires the dimensions of the attached framebuffer, not
             * the destination rectangle used by the wallpaper fill mode.
             * Passing rect.w/rect.h makes mpv reject a valid FBO whenever the
             * source aspect ratio differs from the output. */
            .w = static_cast<int>(m_pool_width),
            .h = static_cast<int>(m_pool_height),
            .internal_format = 0,
        };
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_params},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        const int render_result = mpv_render_context_render(m_render, render_params);
        if (render_result < 0) {
            ++m_present_render_errors;
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!submitDirectFrame(static_cast<std::uint32_t>(index))) {
            ++m_present_submit_errors;
        }
    }

    // 渲染线程 = mpv 线程：创建 mpv、处理事件与命令，并直接提交 GBM/EGL 帧。
    void renderLoop() {
        if (!openWithMpv()) {
            fail(QString::fromStdString(m_last_error));
            cleanupMpv(); // openWithMpv 中途失败可能已创建 mpv/render context，必须在此释放
            return;
        }
        md_producer_config_t negotiated_config {};
        std::uint64_t negotiated_version = 0;
        std::uint64_t negotiated_epoch = 0;
        if (m_host == nullptr ||
            !m_host->currentConfig(negotiated_config, negotiated_version, negotiated_epoch)) {
            fail(QStringLiteral("cannot read initial video output configuration"));
            cleanupMpv();
            return;
        }
        /* The bootstrap modifier zero only gets the target GPU identity.  GBM
         * may choose a render-only compression modifier that the consumer
         * cannot sample, so advertise the exact modifiers that this GBM
         * device can create and let the broker intersect them with KDE's Vulkan
         * sampled-image list. */
        std::vector<md_format_cap_t> render_formats;
        const auto append_render_formats = [&](const std::uint32_t fourcc,
                                                const VkFormat vk_format,
                                                const std::uint32_t gbm_format) {
            /*
             * EGL/GLX consumers advertise modifier zero as the explicit
             * linear DRM layout.  Vulkan's modifier list is allowed to omit
             * that implicit layout even though GBM can create it, so probe it
             * through the same allocation API used by createDirectPool().
             * Adding zero only after GBM confirms both allocation and the
             * returned modifier keeps the protocol tuple exact; it is not a
             * fallback for an unsupported layout.
             */
            bool linear_supported = false;
            /* Capability probing must use the same vendor-specific dimensions
             * as the eventual pool; a modifier accepted at another size does
             * not prove that the actual protocol allocation will succeed. */
            const bool use_logical_pool = m_gpu_vendor_id == kIntelVendorId;
            const std::uint32_t probe_width = use_logical_pool
                ? negotiated_config.logical_width : negotiated_config.physical_width;
            const std::uint32_t probe_height = use_logical_pool
                ? negotiated_config.logical_height : negotiated_config.physical_height;
            gbm_bo* linear_probe = gbm_bo_create(
                m_gbm_device, probe_width, probe_height, gbm_format,
                GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (linear_probe != nullptr) {
                const std::uint64_t actual_modifier = gbm_bo_get_modifier(linear_probe);
                linear_supported = actual_modifier == 0U;
                if (linear_supported && m_gpu_vendor_id != kIntelVendorId) {
                    render_formats.push_back({fourcc, 1U, 0U});
                }
                gbm_bo_destroy(linear_probe);
            }

            VkDrmFormatModifierPropertiesListEXT modifier_list{};
            modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
            VkFormatProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            properties.pNext = &modifier_list;
            vkGetPhysicalDeviceFormatProperties2(m_physical_device, vk_format, &properties);
            std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(
                modifier_list.drmFormatModifierCount);
            modifier_list.pDrmFormatModifierProperties = modifiers.data();
            vkGetPhysicalDeviceFormatProperties2(m_physical_device, vk_format, &properties);
            for (const VkDrmFormatModifierPropertiesEXT& modifier : modifiers) {
                if (modifier.drmFormatModifier == 0U ||
                    modifier.drmFormatModifierPlaneCount != 1U ||
                    (modifier.drmFormatModifierTilingFeatures &
                     VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0U) {
                    continue;
                }
                const std::uint64_t requested_modifier = modifier.drmFormatModifier;
                gbm_bo* probe = gbm_bo_create_with_modifiers2(
                    m_gbm_device, probe_width, probe_height, gbm_format,
                    &requested_modifier, 1U,
                    GBM_BO_USE_RENDERING);
                if (probe == nullptr || gbm_bo_get_modifier(probe) != requested_modifier) {
                    if (probe != nullptr) gbm_bo_destroy(probe);
                    continue;
                }
                gbm_bo_destroy(probe);
                render_formats.push_back({fourcc, 1U, requested_modifier});
            }
            /* On Intel, a 4K60 render target backed by explicit linear memory
             * saturates the integrated GPU and can starve the concurrent VA
             * decoder. Prefer an exact tiled intersection when EGL advertises
             * one, while retaining linear as the final protocol candidate for
             * displays that expose no texture-compatible tiled modifier.
             * Other vendors retain their established linear-first ordering. */
            if (linear_supported && m_gpu_vendor_id == kIntelVendorId) {
                render_formats.push_back({fourcc, 1U, 0U});
            }
        };
        append_render_formats(kDrmXrgb8888, VK_FORMAT_B8G8R8A8_UNORM, GBM_FORMAT_XRGB8888);
        append_render_formats(kDrmArgb8888, VK_FORMAT_B8G8R8A8_UNORM, GBM_FORMAT_ARGB8888);
        append_render_formats(kDrmXbgr8888, VK_FORMAT_R8G8B8A8_UNORM, GBM_FORMAT_XBGR8888);
        append_render_formats(kDrmAbgr8888, VK_FORMAT_R8G8B8A8_UNORM, GBM_FORMAT_ABGR8888);
        if (m_diagnostics) {
            std::fprintf(stderr, "VideoWallpaper diagnostics: GBM/Vulkan render candidates=%zu\n",
                         render_formats.size());
            for (const md_format_cap_t& format : render_formats) {
                std::fprintf(stderr, "  fourcc=%u modifier=0x%llx\n", format.fourcc,
                             static_cast<unsigned long long>(format.modifier));
            }
        }
        if (render_formats.empty() || !m_host->reconnectWithFormats(render_formats)) {
            fail(QStringLiteral("GBM has no modifier supported by the display"));
            cleanupMpv();
            return;
        }
        md_producer_gpu_info_t rebound_gpu {
            .drm_render_major = m_drm_major,
            .drm_render_minor = m_drm_minor,
            .device_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
            .driver_uuid = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
        };
        std::memcpy(rebound_gpu.device_uuid, m_device_uuid, sizeof(rebound_gpu.device_uuid));
        std::memcpy(rebound_gpu.driver_uuid, m_driver_uuid, sizeof(rebound_gpu.driver_uuid));
        if (!m_host->bindGpu(rebound_gpu)) {
            fail(QStringLiteral("display rejected rebound video GPU binding"));
            cleanupMpv();
            return;
        }
        if (!rebuildPool()) {
            fail(QString::fromStdString(m_last_error));
            cleanupMpv();
            return;
        }
        auto last_report = std::chrono::steady_clock::now();
        while (m_running.load()) {
            processMpvCommands();
            // 阻塞短暂超时：有事件立即返回，无事件每 10ms 轮询一次渲染。
            mpv_event* event = mpv_wait_event(m_mpv, 0.01);
            if (event->event_id != MPV_EVENT_NONE) handleMpvEvent(event);
            if (!m_running.load()) break;

            const uint64_t flags = mpv_render_context_update(m_render);
            ++m_update_polls;
            if ((flags & MPV_RENDER_UPDATE_FRAME) != 0u) {
                ++m_update_frames;
                presentMpvFrame();
            }

            // hwdec-current 诊断日志：报告 mpv 实际选用的解码路径（软解为
            // "no"）。仅读取属性打日志，不参与任何决策分支。
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(2)) {
                last_report = now;
                char* hwdec = mpv_get_property_string(m_mpv, "hwdec-current");
                char* video_codec = mpv_get_property_string(m_mpv, "video-codec");
                char* video_format = mpv_get_property_string(m_mpv, "video-format");
                char* video_params = mpv_get_property_string(m_mpv, "video-dec-params");
                if (m_diagnostics) {
                    std::fprintf(stderr,
                                 "VideoWallpaper diagnostics: hwdec-current=%s "
                                 "video-codec=%s video-format=%s video-dec-params=%s pool=%s "
                                 "fill=%s "
                                 "updates=%llu frame_updates=%llu presents=%llu "
                                 "no_pool=%llu no_params=%llu slot_drops=%llu "
                                 "render_errors=%llu submit_errors=%llu direct_frames=%llu\n",
                                 hwdec != nullptr ? hwdec : "?",
                                 video_codec != nullptr ? video_codec : "?",
                                 video_format != nullptr ? video_format : "?",
                                 video_params != nullptr ? video_params : "?",
                                 m_direct_pool_active ? "active" : "inactive",
                                 m_fill_mode_applied ? FillModeName(m_applied_fill_mode) : "pending",
                                 static_cast<unsigned long long>(m_update_polls),
                                 static_cast<unsigned long long>(m_update_frames),
                                 static_cast<unsigned long long>(m_present_calls),
                                 static_cast<unsigned long long>(m_present_without_pool),
                                 static_cast<unsigned long long>(m_present_without_video_params),
                                 static_cast<unsigned long long>(m_direct_slot_drops),
                                 static_cast<unsigned long long>(m_present_render_errors),
                                 static_cast<unsigned long long>(m_present_submit_errors),
                                 static_cast<unsigned long long>(m_direct_frames));
                }
                mpv_free(hwdec);
                mpv_free(video_codec);
                mpv_free(video_format);
                mpv_free(video_params);
            }
        }
        cleanupMpv();
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
    // PCI vendor for the exact Vulkan/render-node device selected in createVulkan().
    // The render thread uses it to bind libmpv to that device's native decode API.
    std::uint32_t m_gpu_vendor_id;
    std::uint32_t m_drm_major { 0 };
    std::uint32_t m_drm_minor { 0 };
    std::uint8_t m_device_uuid[16] { 0 };
    std::uint8_t m_driver_uuid[16] { 0 };
    int m_drm_fd { -1 };

    gbm_device* m_gbm_device { nullptr };
    std::array<DirectSlot, kExportBufferCount> m_direct_slots {};
    md_buffer_pool_t m_direct_pool {};
    bool m_direct_pool_active { false };
    bool m_diagnostics { false };
    std::uint64_t m_direct_frames { 0 };
    std::uint64_t m_direct_slot_drops { 0 };
    std::uint64_t m_update_polls { 0 };
    std::uint64_t m_update_frames { 0 };
    std::uint64_t m_present_calls { 0 };
    std::uint64_t m_present_without_pool { 0 };
    std::uint64_t m_present_without_video_params { 0 };
    std::uint64_t m_present_render_errors { 0 };
    std::uint64_t m_present_submit_errors { 0 };
    PFNEGLCREATEIMAGEKHRPROC m_egl_create_image { nullptr };
    PFNEGLDESTROYIMAGEKHRPROC m_egl_destroy_image { nullptr };
    PFNEGLCREATESYNCKHRPROC m_egl_create_sync { nullptr };
    PFNEGLDESTROYSYNCKHRPROC m_egl_destroy_sync { nullptr };
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC m_egl_dup_native_fence { nullptr };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_gl_image_target { nullptr };
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
    std::atomic<VRVideoFillMode> m_fill_mode { VRVideoFillModeCover };
    VRVideoFillMode m_applied_fill_mode { VRVideoFillModeCover };
    bool m_fill_mode_applied { false };
    std::atomic_bool m_first_frame { false };
    std::atomic_bool m_stopped { false };
    std::atomic<float> m_volume { 1.0f };
    std::atomic_bool m_muted { false };

    std::uint64_t m_generation { 0 };
    std::uint64_t m_config_version { 0 };
    std::uint64_t m_connection_epoch { 0 };
    std::uint64_t m_sequence { 1 };

    std::thread m_render_thread;
    std::atomic_bool m_running { false };
    // 首圈标记：loop-file=inf 下首个 PLAYBACK_RESTART 是 loadfile 后的首圈开始，
    // 需跳过不发 video-did-end；此后每圈 restart 才发事件。仅在 mpv 线程读写，
    // 无需原子操作（与 m_mpv 同线程独占）。
    bool m_looped_once { false };
    std::mutex m_control_mutex;
    std::condition_variable m_control_cv;
    // libmpv（渲染线程独占；m_mpv 置空/读取在 m_control_mutex 保护下）
    mpv_handle* m_mpv { nullptr };
    mpv_render_context* m_render { nullptr };
    std::deque<std::function<void()>> m_mpv_commands;
    // Set from the selected Vulkan GPU before mpv initialization and checked
    // against hwdec-current before any decoded frame reaches the desktop.
    const char* m_expected_hwdec;
    bool m_hwdec_validated { false };

    // 解码器输出尺寸（video-params/w,h 实测为源尺寸，不受渲染路径影响），
    // 用于计算 fit 目标尺寸。
    int m_video_src_w { 0 };
    int m_video_src_h { 0 };

    // headless EGL/GLES3 上下文（mpv GL render API 宿主，渲染线程独占）
    EGLDisplay m_egl_display { EGL_NO_DISPLAY };
    EGLContext m_egl_context { EGL_NO_CONTEXT };
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
