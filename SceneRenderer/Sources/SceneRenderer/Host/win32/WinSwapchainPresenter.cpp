#include "WinSwapchainPresenter.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sr::win32 {

WinSwapchainPresenter::WinSwapchainPresenter()  = default;
WinSwapchainPresenter::~WinSwapchainPresenter() { Destroy(); }

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------

bool WinSwapchainPresenter::Init(HWND hwnd, HINSTANCE hinstance,
                                  std::uint32_t width, std::uint32_t height,
                                  bool enable_validation) {
    m_validation_enabled = enable_validation;

    if (!CreateInstance(enable_validation)) return false;
    if (!CreateSurface(hwnd, hinstance))    return false;
    if (!PickPhysicalDevice())              return false;
    if (!CreateDevice())                    return false;
    if (!CreateCommandPool())               return false;
    if (!CreateSwapchain(width, height))    return false;
    if (!CreateSyncObjects())               return false;
    if (!CreateStagingResources(width, height)) return false;

    return true;
}

void WinSwapchainPresenter::Destroy() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    DestroyStagingResources();
    DestroySwapchain();

    if (m_frame_fence != VK_NULL_HANDLE) { vkDestroyFence(m_device, m_frame_fence, nullptr); m_frame_fence = VK_NULL_HANDLE; }
    if (m_image_avail != VK_NULL_HANDLE) { vkDestroySemaphore(m_device, m_image_avail, nullptr); m_image_avail = VK_NULL_HANDLE; }
    if (m_render_done != VK_NULL_HANDLE) { vkDestroySemaphore(m_device, m_render_done, nullptr); m_render_done = VK_NULL_HANDLE; }
    if (m_command_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_command_pool, nullptr); m_command_pool = VK_NULL_HANDLE; }
    if (m_device != VK_NULL_HANDLE)       { vkDestroyDevice(m_device, nullptr); m_device = VK_NULL_HANDLE; }
    if (m_surface != VK_NULL_HANDLE)      { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
    if (m_instance != VK_NULL_HANDLE)     { vkDestroyInstance(m_instance, nullptr); m_instance = VK_NULL_HANDLE; }
}

// -----------------------------------------------------------------------------
// Present
// -----------------------------------------------------------------------------

void WinSwapchainPresenter::Present(const std::uint8_t* rgba,
                                     std::uint32_t src_width,
                                     std::uint32_t src_height) {
    if (m_device == VK_NULL_HANDLE || rgba == nullptr) return;
    if (src_width == 0 || src_height == 0) return;

    if (m_need_recreate) {
        Resize(m_swapchain_width, m_swapchain_height);
        m_need_recreate = false;
    }

    std::uint32_t image_index = 0;
    VkResult acquire = vkAcquireNextImageKHR(m_device, m_swapchain,
        UINT64_MAX, m_image_avail, VK_NULL_HANDLE, &image_index);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR || acquire == VK_SUBOPTIMAL_KHR) return;
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) return;

    std::uint32_t src_size = src_width * src_height * 4;
    if (src_size > m_staging_size) {
        DestroyStagingResources();
        std::uint32_t new_pixels = src_width * src_height + (src_width * src_height / 2);
        if (!CreateStagingResources(new_pixels, 1)) return;
    }
    std::memcpy(m_staging_mapped, rgba, src_size);

    VkCommandBufferAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
        m_command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Simplified: barrier + copy + present. Full blit-to-local-image pipeline
    // deferred to M4 when engine VkImage sharing is available.
    // For M1, the staging buffer upload alone validates the Vulkan path.
    // Engine RGBA pixels are placed into staging; the full present chain
    // will be completed when integrated with the engine's VkDevice.
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr,
        1, &m_image_avail, &wait_stage, 1, &cmd,
        1, &m_render_done
    };
    vkResetFences(m_device, 1, &m_frame_fence);
    vkQueueSubmit(m_queue, 1, &submit_info, m_frame_fence);

    VkPresentInfoKHR present_info = {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr,
        1, &m_render_done, 1, &m_swapchain, &image_index, nullptr
    };
    VkResult present = vkQueuePresentKHR(m_queue, &present_info);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        m_need_recreate = true;
    }

    vkWaitForFences(m_device, 1, &m_frame_fence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(m_device, m_command_pool, 1, &cmd);
}

bool WinSwapchainPresenter::Resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return false;
    m_swapchain_width  = width;
    m_swapchain_height = height;
    vkDeviceWaitIdle(m_device);
    DestroySwapchain();
    return CreateSwapchain(width, height);
}

// -----------------------------------------------------------------------------
// Vulkan init helpers
// -----------------------------------------------------------------------------

bool WinSwapchainPresenter::CreateInstance(bool enable_validation) {
    VkApplicationInfo app_info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
        "SceneRendererWallpaper", 1, "SceneRendererEngine", 1,
        VK_API_VERSION_1_2
    };

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
    std::vector<const char*> layers;
    if (enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo create_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
        &app_info,
        static_cast<std::uint32_t>(layers.size()), layers.data(),
        static_cast<std::uint32_t>(extensions.size()), extensions.data()
    };
    return vkCreateInstance(&create_info, nullptr, &m_instance) == VK_SUCCESS;
}

bool WinSwapchainPresenter::CreateSurface(HWND hwnd, HINSTANCE hinstance) {
    VkWin32SurfaceCreateInfoKHR surface_info = {
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0,
        hinstance, hwnd
    };
    return vkCreateWin32SurfaceKHR(m_instance, &surface_info, nullptr, &m_surface) == VK_SUCCESS;
}

bool WinSwapchainPresenter::PickPhysicalDevice() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) return false;

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (auto& device : devices) {
        std::uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_props(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qf_count, qf_props.data());

        for (std::uint32_t i = 0; i < qf_count; ++i) {
            if (!(qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present_support);
            if (present_support) {
                m_physical_device = device;
                m_queue_family    = i;
                return true;
            }
        }
    }
    return false;
}

bool WinSwapchainPresenter::CreateDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
        m_queue_family, 1, &priority
    };
    const char* swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dev_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0,
        1, &q_info, 0, nullptr, 1, &swapchain_ext, nullptr
    };
    if (vkCreateDevice(m_physical_device, &dev_info, nullptr, &m_device) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);
    return true;
}

bool WinSwapchainPresenter::CreateSwapchain(std::uint32_t width, std::uint32_t height) {
    VkSurfaceCapabilitiesKHR caps {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &caps);

    VkSwapchainCreateInfoKHR sci = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0,
        m_surface,
        (std::max)(caps.minImageCount, 2u),
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        { std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width),
          std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height) },
        1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr,
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE
    };
    if (vkCreateSwapchainKHR(m_device, &sci, nullptr, &m_swapchain) != VK_SUCCESS) {
        return false;
    }
    m_swapchain_width  = width;
    m_swapchain_height = height;
    return true;
}

void WinSwapchainPresenter::DestroySwapchain() {
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool WinSwapchainPresenter::CreateCommandPool() {
    VkCommandPoolCreateInfo info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        m_queue_family
    };
    return vkCreateCommandPool(m_device, &info, nullptr, &m_command_pool) == VK_SUCCESS;
}

bool WinSwapchainPresenter::CreateSyncObjects() {
    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
        VK_FENCE_CREATE_SIGNALED_BIT
    };
    VkSemaphoreCreateInfo sem_info = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0
    };
    return vkCreateFence(m_device, &fence_info, nullptr, &m_frame_fence) == VK_SUCCESS
        && vkCreateSemaphore(m_device, &sem_info, nullptr, &m_image_avail) == VK_SUCCESS
        && vkCreateSemaphore(m_device, &sem_info, nullptr, &m_render_done) == VK_SUCCESS;
}

bool WinSwapchainPresenter::CreateStagingResources(std::uint32_t max_width,
                                                     std::uint32_t max_height) {
    m_staging_size = max_width * max_height * 4;

    VkBufferCreateInfo buf_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
        m_staging_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr
    };
    if (vkCreateBuffer(m_device, &buf_info, nullptr, &m_staging_buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_req {};
    vkGetBufferMemoryRequirements(m_device, m_staging_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        mem_req.size,
        FindMemoryType(mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_staging_memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, m_staging_buffer, nullptr);
        m_staging_buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(m_device, m_staging_buffer, m_staging_memory, 0);
    vkMapMemory(m_device, m_staging_memory, 0, m_staging_size, 0, &m_staging_mapped);
    return true;
}

void WinSwapchainPresenter::DestroyStagingResources() {
    if (m_staging_mapped) {
        vkUnmapMemory(m_device, m_staging_memory);
        m_staging_mapped = nullptr;
    }
    if (m_staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_staging_memory, nullptr);
        m_staging_memory = VK_NULL_HANDLE;
    }
    if (m_staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_staging_buffer, nullptr);
        m_staging_buffer = VK_NULL_HANDLE;
    }
    m_staging_size = 0;
}

std::uint32_t WinSwapchainPresenter::FindMemoryType(
    std::uint32_t type_filter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties mem_props {};
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

} // namespace sr::win32
