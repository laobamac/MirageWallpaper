module;

#include <rstd/macro.hpp>
#include "vk_mem_alloc.h"

#include "vvk/macros.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

module sr.vulkan;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;

using namespace sr::vulkan;

namespace
{

void EnumateDeviceExts(const vvk::PhysicalDevice& gpu, sr::Set<std::string>& set) {
    std::vector<VkExtensionProperties> properties;
    VVK_CHECK_VOID_RE(gpu.EnumerateDeviceExtensionProperties(properties));
    for (auto& ext : properties) set.insert(ext.extensionName);
}

} // namespace

bool Device::initPipelineCache() {
    const auto props = m_gpu.GetProperties();
    std::ostringstream identity;
    identity << std::hex << std::setfill('0') << std::setw(8) << props.vendorID << '_'
             << std::setw(8) << props.deviceID << '_' << std::setw(8) << props.driverVersion << '_';
    for (std::uint8_t byte : props.pipelineCacheUUID) identity << std::setw(2) << (unsigned)byte;

    std::error_code ec;
    const char* home = std::getenv("HOME");
    auto cache_dir = home != nullptr && home[0] != '\0'
                         ? std::filesystem::path(home) / "Library/Caches/SceneRenderer/vulkan"
                         : std::filesystem::temp_directory_path() / "SceneRenderer/vulkan";
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        rstd_warn("pipeline cache directory unavailable: {}", ec.message());
        return false;
    }
    m_pipeline_cache_path =
        (cache_dir / ("pipeline_v1_" + identity.str() + ".bin")).string();

    std::vector<std::uint8_t> initial_data;
    {
        std::ifstream input(m_pipeline_cache_path, std::ios::binary | std::ios::ate);
        if (input) {
            const auto size = input.tellg();
            constexpr std::streamoff max_cache_size = 128 * 1024 * 1024;
            if (size > 0 && size <= max_cache_size) {
                initial_data.resize((std::size_t)size);
                input.seekg(0);
                if (! input.read(reinterpret_cast<char*>(initial_data.data()), size)) {
                    initial_data.clear();
                }
            }
        }
    }

    VkPipelineCacheCreateInfo info {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = initial_data.size(),
        .pInitialData    = initial_data.empty() ? nullptr : initial_data.data(),
    };
    VkResult result = m_device.CreatePipelineCache(info, &m_pipeline_cache);
    if (result != VK_SUCCESS && ! initial_data.empty()) {
        // Corrupt or stale blobs are never fatal: discard and recreate empty.
        info.initialDataSize = 0;
        info.pInitialData    = nullptr;
        result = m_device.CreatePipelineCache(info, &m_pipeline_cache);
    }
    if (result != VK_SUCCESS) {
        m_pipeline_cache = VK_NULL_HANDLE;
        rstd_warn("vkCreatePipelineCache failed: {}", (int)result);
        return false;
    }
    rstd_info("Vulkan pipeline cache: {} ({} bytes)",
              m_pipeline_cache_path,
              initial_data.size());
    return true;
}

void Device::savePipelineCache() {
    if (m_pipeline_cache == VK_NULL_HANDLE || m_pipeline_cache_path.empty()) return;
    std::size_t size = 0;
    if (m_device.GetPipelineCacheData(m_pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0 ||
        size > 128u * 1024u * 1024u)
        return;
    std::vector<std::uint8_t> data(size);
    if (m_device.GetPipelineCacheData(m_pipeline_cache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);

    const auto target = std::filesystem::path(m_pipeline_cache_path);
    // Multiple screens use separate renderer processes and may shut down at
    // nearly the same time. Give each writer its own staging file, then rely
    // on the same-directory rename for an atomic last-writer-wins update.
    const auto temp = target.string() + "." + std::to_string(::getpid()) + "." +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (! output.write(reinterpret_cast<const char*>(data.data()),
                           (std::streamsize)data.size())) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

void Device::destroyPipelineCache() {
    if (m_pipeline_cache == VK_NULL_HANDLE) return;
    m_device.DestroyPipelineCache(m_pipeline_cache);
    m_pipeline_cache = VK_NULL_HANDLE;
}

bool Device::CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                      VkSurfaceKHR surface) {
    auto props = gpu.GetQueueFamilyProperties();

    bool     has_graphics_queue { false };
    bool     has_present_queue { false };
    unsigned index { 0 };
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) has_graphics_queue = true;
        if (surface) {
            bool ok { false };
            VVK_CHECK(gpu.GetSurfaceSupportKHR(index, surface, ok));
            if (ok) has_present_queue = true;
        }
        index++;
    }
    if (! has_graphics_queue) return false;
    if (surface && ! has_present_queue) return false;

    Set<std::string> extensions;
    EnumateDeviceExts(gpu, extensions);
    bool requires_timeline_semaphore { false };
    for (auto& ext : exts) {
        if (ext.required) {
            if (! exists(extensions, ext.name)) return false;
            if (std::string_view(ext.name).compare(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
                requires_timeline_semaphore = true;
            }
        }
    }
    if (requires_timeline_semaphore) {
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_features {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
            .pNext = nullptr,
        };
        VkPhysicalDeviceFeatures2KHR features2 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
            .pNext = &timeline_features,
        };
        gpu.GetFeatures2KHR(features2);
        if (! timeline_features.timelineSemaphore) return false;
    }
    return true;
}

std::vector<VkDeviceQueueCreateInfo> Device::ChooseDeviceQueue(VkSurfaceKHR surface) {
    std::vector<VkDeviceQueueCreateInfo> queues;

    auto props = m_gpu.GetQueueFamilyProperties();

    std::vector<uint32_t> graphic_indexs, present_indexs;
    uint32_t              index = 0;
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) graphic_indexs.push_back(index);
        index++;
    }
    m_graphics_queue.family_index           = graphic_indexs.front();
    const static float defaultQueuePriority = 0.0f;
    m_present_queue.family_index            = graphic_indexs.front();
    if (surface) {
        index = 0;
        for (auto& prop : props) {
            (void)prop;
            bool ok { false };
            VVK_CHECK(m_gpu.GetSurfaceSupportKHR(index, surface, ok))
            if (ok) present_indexs.push_back(index);
            index++;
        }
        if (present_indexs.empty()) {
            rstd_error("not find present queue");
        } else {
            m_present_queue.family_index = present_indexs.front();
        }
    }
    for (uint32_t i = 0; i < props.size(); ++i) {
        if (props[i].queueCount == 0) continue;
        VkDeviceQueueCreateInfo info {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = i,
            .queueCount       = 1,
            .pQueuePriorities = &defaultQueuePriority,
        };
        queues.push_back(info);
    }
    return queues;
}

bool Device::Create(Instance& inst, std::span<const Extension> exts, VkExtent2D extent,
                    Device& device) {
    device.dld                    = vvk::DeviceDispatch { inst.inst().Dispatch() };
    device.m_instance             = *inst.inst();
    device.m_instance_api_version = inst.api_version();
    device.m_gpu                  = inst.gpu();
    device.m_limits               = inst.gpu().GetProperties().limits;
    device.set_out_extent(extent);
    device.m_enabled_instance_extensions.assign(inst.enabled_extensions().begin(),
                                                inst.enabled_extensions().end());

    Set<std::string> tested_exts;
    {
        EnumateDeviceExts(inst.gpu(), device.m_extensions);
        for (auto& ext : exts) {
            bool ok = device.supportExt(ext.name);
            if (ok) tested_exts.insert(std::string(ext.name));
            if (ext.required && ! ok) {
                rstd_error("required vulkan device extension \"{}\" is not supported", ext.name);
                return false;
            }
        }
    }
    std::vector<const char*> tested_exts_c { tested_exts.size() };
    std::transform(
        tested_exts.begin(), tested_exts.end(), tested_exts_c.begin(), [](const auto& s) {
            return s.c_str();
        });
    device.m_enabled_device_extensions.assign(tested_exts.begin(), tested_exts.end());
    bool rq_surface = ! inst.offscreen();

    // Opt in to optional features the renderer actually uses. Geometry
    // shaders drive the genericropeparticle spline subdivision; without
    // pEnabledFeatures.geometryShader=VK_TRUE, the driver rejects a
    // pipeline with a GS stage.
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR supported_timeline {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext = nullptr,
    };
    VkPhysicalDeviceSynchronization2FeaturesKHR supported_sync2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext = nullptr,
    };
    supported_timeline.pNext = &supported_sync2;
    VkPhysicalDeviceFeatures2KHR supported2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
        .pNext = &supported_timeline,
    };
    device.m_gpu.GetFeatures2KHR(supported2);
    if (! supported_timeline.timelineSemaphore) {
        rstd_error("required vulkan feature timelineSemaphore is not supported");
        return false;
    }
    VkPhysicalDeviceFeatures enabled {};
    enabled.geometryShader    = supported2.features.geometryShader;
    enabled.sampleRateShading = supported2.features.sampleRateShading;
    enabled.samplerAnisotropy = supported2.features.samplerAnisotropy;
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR enabled_timeline {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext             = nullptr,
        .timelineSemaphore = VK_TRUE,
    };
    const bool enable_sync2 = exists(tested_exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) &&
                              supported_sync2.synchronization2;
    VkPhysicalDeviceSynchronization2FeaturesKHR enabled_sync2 {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext            = nullptr,
        .synchronization2 = enable_sync2 ? VK_TRUE : VK_FALSE,
    };
    if (enable_sync2) enabled_timeline.pNext = &enabled_sync2;

    VVK_CHECK_BOOL_RE(vvk::Device::Create(device.m_device,
                                          *device.m_gpu,
                                          device.ChooseDeviceQueue(*inst.surface()),
                                          tested_exts_c,
                                          &enabled_timeline,
                                          device.dld,
                                          &enabled));

    device.m_graphics_queue.handle = device.m_device.GetQueue(device.m_graphics_queue.family_index);
    device.m_present_queue.handle  = device.m_device.GetQueue(device.m_present_queue.family_index);
    (void)device.initPipelineCache();

    if (rq_surface) {
        if (! Swapchain::Create(device, *inst.surface(), extent, device.m_swapchain)) {
            rstd_error("create swapchain failed");
            return false;
        }
        device.set_out_extent(device.m_swapchain.extent());
    }
    {
        VkCommandPoolCreateInfo info { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = device.m_graphics_queue.family_index };
        VVK_CHECK_BOOL_RE(device.m_device.CreateCommandPool(info, device.m_command_pool));
    }
    {
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion       = device.m_instance_api_version;
        allocatorInfo.physicalDevice         = *device.m_gpu;
        allocatorInfo.device                 = *device.m_device;
        allocatorInfo.instance               = *inst.inst();
        VVK_CHECK_BOOL_RE(vvk::CreateVmaAllocator(allocatorInfo, device.m_allocator));
    }
    device.m_tex_cache  = std::make_unique<TextureCache>(device);
    device.m_mesh_cache = std::make_unique<MeshCache>(device);
    if (! device.m_mesh_cache->init()) {
        rstd_error("MeshCache init failed");
        return false;
    }
    return true;
}

VkDeviceSize Device::GetUsage() const {
    VmaBudget budget;
    vmaGetHeapBudgets(*m_allocator, &budget);
    return budget.usage;
}

void Device::Destroy() {
    VVK_CHECK(m_device.WaitIdle());
    savePipelineCache();
    destroyPipelineCache();
}

bool Device::RecreateSwapchain(VkSurfaceKHR surface, VkExtent2D extent) {
    VVK_CHECK_BOOL_RE(m_device.WaitIdle());

    Swapchain recreated;
    if (! Swapchain::Create(*this, surface, extent, recreated)) {
        rstd_error("recreate swapchain failed");
        return false;
    }

    m_swapchain = std::move(recreated);
    set_out_extent(m_swapchain.extent());
    return true;
}

Device::Device(): m_tex_cache(std::make_unique<TextureCache>(*this)) {}
Device::~Device() { destroyPipelineCache(); }

bool Device::supportExt(std::string_view name) const { return exists(m_extensions, name); }
