module;

#include <rstd/macro.hpp>
#include "vk_mem_alloc.h"

#include "vvk/macros.hpp"

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
    for (auto& ext : exts) {
        if (ext.required) {
            if (! exists(extensions, ext.name)) return false;
        }
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
    {
        VkDeviceQueueCreateInfo info {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = m_graphics_queue.family_index,
            .queueCount       = 1,
            .pQueuePriorities = &defaultQueuePriority,
        };
        queues.push_back(info);
    }
    m_present_queue.family_index = graphic_indexs.front();
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
        } else if (graphic_indexs.front() != present_indexs.front()) {
            m_present_queue.family_index = present_indexs.front();
            VkDeviceQueueCreateInfo info {
                .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = m_present_queue.family_index,
                .queueCount       = 1,
                .pQueuePriorities = &defaultQueuePriority,
            };
            queues.push_back(info);
        }
    }
    return queues;
}

bool Device::Create(Instance& inst, std::span<const Extension> exts, VkExtent2D extent,
                    Device& device) {
    device.dld      = vvk::DeviceDispatch { inst.inst().Dispatch() };
    device.m_gpu    = inst.gpu();
    device.m_limits = inst.gpu().GetProperties().limits;
    device.set_out_extent(extent);

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
    bool rq_surface = ! inst.offscreen();

    // Opt in to optional features the renderer actually uses. Geometry
    // shaders drive the genericropeparticle spline subdivision; without
    // pEnabledFeatures.geometryShader=VK_TRUE, the driver rejects a
    // pipeline with a GS stage.
    VkPhysicalDeviceFeatures2KHR supported2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR };
    device.m_gpu.GetFeatures2KHR(supported2);
    VkPhysicalDeviceFeatures enabled {};
    enabled.geometryShader    = supported2.features.geometryShader;
    enabled.sampleRateShading = supported2.features.sampleRateShading;
    enabled.samplerAnisotropy = supported2.features.samplerAnisotropy;

    VVK_CHECK_BOOL_RE(vvk::Device::Create(device.m_device,
                                          *device.m_gpu,
                                          device.ChooseDeviceQueue(*inst.surface()),
                                          tested_exts_c,
                                          nullptr,
                                          device.dld,
                                          &enabled));

    device.m_graphics_queue.handle = device.m_device.GetQueue(device.m_graphics_queue.family_index);
    device.m_present_queue.handle  = device.m_device.GetQueue(device.m_present_queue.family_index);

    if (rq_surface) {
        if (! Swapchain::Create(device, *inst.surface(), extent, device.m_swapchain)) {
            rstd_error("create swapchain failed");
            return false;
        }
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
        allocatorInfo.vulkanApiVersion       = SCENERENDERER_VULKAN_VERSION;
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

void Device::Destroy() { VVK_CHECK(m_device.WaitIdle()); }

Device::Device(): m_tex_cache(std::make_unique<TextureCache>(*this)) {}
Device::~Device() {}

bool Device::supportExt(std::string_view name) const { return exists(m_extensions, name); }
