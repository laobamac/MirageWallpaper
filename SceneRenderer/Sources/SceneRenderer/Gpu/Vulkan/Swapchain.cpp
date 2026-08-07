module;

#include <rstd/macro.hpp>
#include <vulkan/vulkan.h>
#include "vvk/macros.hpp"

module sr.vulkan;
import sr.types;
import rstd.log;
import rstd.cppstd;

using namespace sr::vulkan;

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

namespace
{

bool querySwapChainSupport(const vvk::PhysicalDevice& gpu, VkSurfaceKHR surface,
                           SwapChainSupportDetails& details) {
    VVK_CHECK_BOOL_RE(gpu.GetSurfaceCapabilitiesKHR(surface, details.capabilities));
    VVK_CHECK_BOOL_RE(gpu.GetSurfaceFormatsKHR(surface, details.formats));
    VVK_CHECK_BOOL_RE(gpu.GetSurfacePresentModesKHR(surface, details.presentModes));
    // chooseSwapSurfaceFormat indexes [0] as its fallback; a surface that
    // reports no format at all (driver hiccup, surface already lost) must not
    // reach it.
    if (details.formats.empty()) {
        rstd_error("surface reports no supported format");
        return false;
    }
    return true;
}

// Precondition: `availableFormats` is non-empty (querySwapChainSupport rejects
// an empty format list before this is reached).
VkSurfaceFormatKHR chooseSwapSurfaceFormat(std::span<const VkSurfaceFormatKHR> availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
            availableFormat.format == VK_FORMAT_R8G8B8A8_UNORM) {
            if (availableFormat.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR)
                return availableFormat;
        }
    }
    auto& format = availableFormats[0];
    rstd_info("swapchain format: {}, color space: {}",
              vvk::ToString(format.format),
              vvk::ToString(format.colorSpace));
    return format;
}

VkExtent2D GetSwapChainExtent(VkSurfaceCapabilitiesKHR& surface_capabilities, VkExtent2D ext) {
    auto min     = surface_capabilities.minImageExtent;
    auto max     = surface_capabilities.maxImageExtent;
    auto currExt = surface_capabilities.currentExtent;

    if (currExt.width != std::numeric_limits<uint32_t>::max() &&
        currExt.height != std::numeric_limits<uint32_t>::max()) {
        return currExt;
    }

    if (ext.width < min.width) ext.width = min.width;
    if (ext.height < min.height) ext.height = min.height;
    if (ext.width > max.width) ext.width = max.width;
    if (ext.height > max.height) ext.height = max.height;
    return ext;
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkFlags supported) {
    constexpr std::array preferred {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (auto alpha : preferred) {
        if (supported & alpha) return alpha;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

std::optional<vvk::ImageView> CreateSwapImageView(const vvk::Device& device, VkFormat format,
                                                  VkImage image) {
    VkImageViewCreateInfo ci {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    vvk::ImageView view;
    if (auto res = device.CreateImageView(ci, view); res == VK_SUCCESS) return view;
    return std::nullopt;
}
} // namespace

const vvk::SwapchainKHR& Swapchain::handle() const { return m_handle; }
VkFormat                 Swapchain::format() const { return m_format.format; }
VkExtent2D               Swapchain::extent() const { return m_extent; }
VkImageUsageFlags        Swapchain::usage() const { return m_usage; }

std::span<const ImageParameters> Swapchain::images() const { return m_images; }

VkPresentModeKHR Swapchain::presentMode() const { return m_present_mode; }

bool Swapchain::build(Device& device, VkSurfaceKHR surface, VkExtent2D extent,
                      VkSwapchainKHR old_swapchain) {
    // Views alias the *previous* swapchain's images; they must be gone before
    // that swapchain is destroyed, which the caller does only after this
    // returns (it is still live here as `old_swapchain`).
    m_images.clear();
    m_imageviews.clear();

    SwapChainSupportDetails swap_details;
    if (! querySwapChainSupport(device.gpu(), surface, swap_details)) return false;

    m_format = chooseSwapSurfaceFormat(swap_details.formats);

    auto& surfaceCapabilities = swap_details.capabilities;

    uint32_t image_count = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0 && image_count > surfaceCapabilities.maxImageCount)
        image_count = surfaceCapabilities.maxImageCount;
    m_extent = GetSwapChainExtent(surfaceCapabilities, extent);
    if (m_extent.width == 0 || m_extent.height == 0) {
        // Display detached / window minimized. A 0x0 swapchain is invalid
        // usage; the caller skips frames and retries.
        rstd_info("surface extent is 0x0, swapchain creation deferred");
        return false;
    }

    m_present_mode                               = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceTransformFlagBitsKHR preTransform   = surfaceCapabilities.currentTransform;
    VkCompositeAlphaFlagBitsKHR   compositeAlpha =
        ChooseCompositeAlpha(surfaceCapabilities.supportedCompositeAlpha);
    m_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        m_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    const std::array queue_family_indices {
        device.graphics_queue().family_index,
        device.present_queue().family_index,
    };
    const bool separate_present_queue = queue_family_indices[0] != queue_family_indices[1];

    VkSwapchainCreateInfoKHR sci {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext            = nullptr,
        .surface          = surface,
        .minImageCount    = image_count,
        .imageFormat      = m_format.format,
        .imageColorSpace  = m_format.colorSpace,
        .imageExtent      = m_extent,
        .imageArrayLayers = 1,
        .imageUsage       = m_usage,
        .imageSharingMode = separate_present_queue ? VK_SHARING_MODE_CONCURRENT
                                                   : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = separate_present_queue ? 2u : 0u,
        .pQueueFamilyIndices   = separate_present_queue ? queue_family_indices.data() : nullptr,
        .preTransform     = preTransform,
        .compositeAlpha   = compositeAlpha,
        .presentMode      = m_present_mode,
        .clipped          = true,
        .oldSwapchain     = old_swapchain,
    };

    // Move-assign into m_handle: it is already null on the recreate path
    // (the caller moved the live handle out to pass it as `oldSwapchain`).
    VVK_CHECK_BOOL_RE(device.device().CreateSwapchainKHR(sci, m_handle));
    {
        std::vector<VkImage> images;
        VVK_CHECK_BOOL_RE(m_handle.GetImages(images));
        std::transform(images.begin(), images.end(), std::back_inserter(m_images), [&](auto image) {
            ImageParameters image_paras {};
            image_paras.handle = image;
            image_paras.extent = { m_extent.width, m_extent.height, 1 };
            if (auto opt = CreateSwapImageView(device.device(), m_format.format, image);
                opt.has_value()) {
                m_imageviews.emplace_back(std::move(opt.value()));
                image_paras.view = *m_imageviews.back();
            }
            return image_paras;
        });
    }
    return true;
}

bool Swapchain::Create(Device& device, VkSurfaceKHR surface, VkExtent2D extent, Swapchain& swap) {
    return swap.build(device, surface, extent, VK_NULL_HANDLE);
}

bool Swapchain::Recreate(Device& device, VkSurfaceKHR surface, VkExtent2D extent, Swapchain& swap) {
    // The old VkSwapchainKHR stays alive (owned by `old`) for the whole
    // vkCreateSwapchainKHR call so the driver can retire it through
    // `oldSwapchain`; it is destroyed exactly once when `old` leaves scope.
    // Callers must have waited for device idle first.
    vvk::SwapchainKHR old = std::move(swap.m_handle);
    return swap.build(device, surface, extent, *old);
}

void Swapchain::Destroy() {
    m_images.clear();
    m_imageviews.clear();
    m_handle.reset();
}
