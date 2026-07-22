#pragma once

#include <cstdint>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>

#include <windows.h>

namespace sr::win32 {

// ---------------------------------------------------------------------------
// WinSwapchainPresenter
//
// Owns an independent Vulkan device/swapchain and presents RGBA pixel data
// received from the engine's CPU readback callback onto the wallpaper window.
//
// Per-frame flow:
//   1. Receive rgba[width*height*4] from engine callback
//   2. memcpy → staging VkBuffer (host-visible)
//   3. vkCmdCopyBufferToImage → local VkImage
//   4. vkCmdBlitImage → swapchain image (with aspect-ratio fill)
//   5. vkQueuePresentKHR
// ---------------------------------------------------------------------------
class WinSwapchainPresenter {
public:
    WinSwapchainPresenter();
    ~WinSwapchainPresenter();

    WinSwapchainPresenter(const WinSwapchainPresenter&)            = delete;
    WinSwapchainPresenter& operator=(const WinSwapchainPresenter&) = delete;
    WinSwapchainPresenter(WinSwapchainPresenter&&)                 = delete;
    WinSwapchainPresenter& operator=(WinSwapchainPresenter&&)      = delete;

    bool Init(HWND hwnd, HINSTANCE hinstance,
              std::uint32_t width, std::uint32_t height,
              bool enable_validation = false);

    void Destroy();

    void Present(const std::uint8_t* rgba,
                 std::uint32_t src_width, std::uint32_t src_height);

    bool Resize(std::uint32_t width, std::uint32_t height);

    bool IsValid() const { return m_device != VK_NULL_HANDLE; }

private:
    bool CreateInstance(bool enable_validation);
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSurface(HWND hwnd, HINSTANCE hinstance);
    bool CreateSwapchain(std::uint32_t width, std::uint32_t height);
    bool CreateCommandPool();
    bool CreateSyncObjects();
    bool CreateStagingResources(std::uint32_t max_width, std::uint32_t max_height);

    void DestroySwapchain();
    void DestroyStagingResources();
    std::uint32_t FindMemoryType(std::uint32_t type_filter,
                                  VkMemoryPropertyFlags properties) const;

    VkInstance        m_instance        = VK_NULL_HANDLE;
    VkDevice          m_device          = VK_NULL_HANDLE;
    VkQueue           m_queue           = VK_NULL_HANDLE;
    VkSurfaceKHR      m_surface         = VK_NULL_HANDLE;
    VkSwapchainKHR    m_swapchain       = VK_NULL_HANDLE;
    VkCommandPool     m_command_pool    = VK_NULL_HANDLE;
    VkFence           m_frame_fence     = VK_NULL_HANDLE;
    VkSemaphore       m_image_avail     = VK_NULL_HANDLE;
    VkSemaphore       m_render_done     = VK_NULL_HANDLE;

    VkBuffer          m_staging_buffer  = VK_NULL_HANDLE;
    VkDeviceMemory    m_staging_memory  = VK_NULL_HANDLE;
    void*             m_staging_mapped  = nullptr;
    std::uint32_t     m_staging_size    = 0;

    VkPhysicalDevice  m_physical_device = VK_NULL_HANDLE;
    std::uint32_t     m_queue_family    = 0;

    std::uint32_t     m_swapchain_width  = 0;
    std::uint32_t     m_swapchain_height = 0;
    bool              m_need_recreate    = false;
    bool              m_validation_enabled = false;
};

} // namespace sr::win32
