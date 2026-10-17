// VideoViewer — standalone Linux video wallpaper preview. FFmpeg decodes the
// video (see VideoRendererEngine), GLFW + Vulkan present it (mirroring
// SceneViewer). No Qt windowing in the render path.

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VideoManifest.h"
#include "VideoRendererEngine.h"
#include "frag_spv.h"
#include "vert_spv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ViewerArgs {
    const char* workshop = nullptr;
    int width = 1280;
    int height = 720;
    float volume = 1.0f;
    bool muted = false;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
};

void printUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s <wallpaper-dir> [options]\n\n"
                 "Options:\n"
                 "  --width N              window width  (default 1280)\n"
                 "  --height N             window height (default 720)\n"
                 "  --volume 0..1          audio volume (default 1.0)\n"
                 "  --muted                start muted\n"
                 "  --fill MODE            cover | contain | stretch (default cover)\n"
                 "  --run-seconds N        exit after N seconds (test helper)\n"
                 "  -h, --help             show this help\n",
                 argv0);
}

const char* takeValue(int& index, int argc, char** argv, const char* option) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", option);
        return nullptr;
    }
    return argv[++index];
}

bool parseArgs(int argc, char** argv, ViewerArgs& out) {
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "-h") == 0 || std::strcmp(argument, "--help") == 0) {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (std::strcmp(argument, "--width") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.width = std::atoi(value);
        } else if (std::strcmp(argument, "--height") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.height = std::atoi(value);
        } else if (std::strcmp(argument, "--volume") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.volume = std::strtof(value, nullptr);
        } else if (std::strcmp(argument, "--muted") == 0) {
            out.muted = true;
        } else if (std::strcmp(argument, "--fill") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value || !VRParseVideoFillMode(value, out.fillMode)) return false;
        } else if (std::strcmp(argument, "--run-seconds") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.runSeconds = std::atoi(value);
        } else if (argument[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argument);
            return false;
        } else if (!out.workshop) {
            out.workshop = argument;
        } else {
            std::fprintf(stderr, "unexpected positional argument: %s\n", argument);
            return false;
        }
    }

    if (!out.workshop) {
        printUsage(argv[0]);
        return false;
    }
    if (out.width < 64) out.width = 64;
    if (out.height < 64) out.height = 64;
    out.volume = VRClampVideoVolume(out.volume);
    if (out.runSeconds < 0) out.runSeconds = 0;
    return true;
}

// NDC quad extents for the fill mode so the frame keeps its aspect.
std::array<double, 4> fillRect(double windowAspect, double frameAspect, VRVideoFillMode mode) {
    double sx = 1.0;
    double sy = 1.0;
    switch (mode) {
    case VRVideoFillModeStretch:
        break;
    case VRVideoFillModeContain:
        if (frameAspect >= windowAspect) {
            sy = windowAspect / frameAspect;
        } else {
            sx = frameAspect / windowAspect;
        }
        break;
    case VRVideoFillModeCover:
    default:
        if (frameAspect >= windowAspect) {
            sx = frameAspect / windowAspect;
        } else {
            sy = windowAspect / frameAspect;
        }
        break;
    }
    return { -sx, -sy, sx, sy };
}

struct QueueFamilies {
    std::uint32_t graphics = UINT32_MAX;
    std::uint32_t present = UINT32_MAX;
    bool complete() const { return graphics != UINT32_MAX && present != UINT32_MAX; }
};

QueueFamilies findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies families;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) families.graphics = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
        if (present == VK_TRUE) families.present = i;
        if (families.complete()) break;
    }
    return families;
}

} // namespace

struct Viewer {
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = UINT32_MAX;
    std::uint32_t presentFamily = UINT32_MAX;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent {};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory = VK_NULL_HANDLE;
    VkImageView textureView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    std::uint32_t imageCount = 0;
    std::uint32_t currentImage = 0;
    std::uint64_t uploadedSerial = 0;
    int textureWidth = 0;
    int textureHeight = 0;
    bool resizePending = false;
    int windowWidth = 1280;
    int windowHeight = 720;

    ~Viewer() { cleanup(); }

    void cleanup() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (renderFinished != VK_NULL_HANDLE) vkDestroySemaphore(device, renderFinished, nullptr);
        if (imageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(device, imageAvailable, nullptr);
        if (inFlight != VK_NULL_HANDLE) vkDestroyFence(device, inFlight, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descPool, nullptr);
        if (stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, stagingBuffer, nullptr);
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device, stagingMemory, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(device, sampler, nullptr);
        if (textureView != VK_NULL_HANDLE) vkDestroyImageView(device, textureView, nullptr);
        if (textureImage != VK_NULL_HANDLE) vkDestroyImage(device, textureImage, nullptr);
        if (textureMemory != VK_NULL_HANDLE) vkFreeMemory(device, textureMemory, nullptr);
        if (indexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, indexBuffer, nullptr);
        if (indexMemory != VK_NULL_HANDLE) vkFreeMemory(device, indexMemory, nullptr);
        if (vertexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, vertexBuffer, nullptr);
        if (vertexMemory != VK_NULL_HANDLE) vkFreeMemory(device, vertexMemory, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
        if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, renderPass, nullptr);
        destroySwapchainResources();
        if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        if (window != nullptr) glfwDestroyWindow(window);
        device = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
        window = nullptr;
    }

    void destroySwapchainResources() {
        for (auto& fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        framebuffers.clear();
        for (auto& view : swapViews) vkDestroyImageView(device, view, nullptr);
        swapViews.clear();
        if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        swapImages.clear();
    }

    bool init(int width, int height) {
        windowWidth = width;
        windowHeight = height;
        if (glfwInit() != GLFW_TRUE) {
            std::fprintf(stderr, "VideoViewer: glfwInit failed\n");
            return false;
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window = glfwCreateWindow(width, height, "VideoViewer", nullptr, nullptr);
        if (window == nullptr) {
            std::fprintf(stderr, "VideoViewer: glfwCreateWindow failed\n");
            return false;
        }
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) {
            auto* viewer = static_cast<Viewer*>(glfwGetWindowUserPointer(w));
            if (viewer) viewer->resizePending = true;
        });

        VkApplicationInfo appInfo {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "VideoViewer",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "Mirage VideoRenderer",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_0,
        };
        std::uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        if (glfwExts == nullptr) {
            std::fprintf(stderr, "VideoViewer: GLFW has no Vulkan surface extensions\n");
            return false;
        }
        VkInstanceCreateInfo instInfo {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &appInfo,
            .enabledExtensionCount = glfwExtCount,
            .ppEnabledExtensionNames = glfwExts,
        };
        if (vkCreateInstance(&instInfo, nullptr, &instance) != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkCreateInstance failed\n");
            return false;
        }
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: glfwCreateWindowSurface failed\n");
            return false;
        }
        std::uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            std::fprintf(stderr, "VideoViewer: no Vulkan physical device\n");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            QueueFamilies families = findQueueFamilies(candidate, surface);
            if (families.complete()) {
                physical = candidate;
                graphicsFamily = families.graphics;
                presentFamily = families.present;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) {
            std::fprintf(stderr, "VideoViewer: no suitable Vulkan device\n");
            return false;
        }

        const float queuePriority = 1.0f;
        std::array<VkDeviceQueueCreateInfo, 2> queueInfos {};
        std::uint32_t queueInfoCount = 0;
        auto addQueue = [&](std::uint32_t family) {
            for (std::uint32_t i = 0; i < queueInfoCount; ++i) {
                if (queueInfos[i].queueFamilyIndex == family) return;
            }
            queueInfos[queueInfoCount] = VkDeviceQueueCreateInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = family,
                .queueCount = 1,
                .pQueuePriorities = &queuePriority,
            };
            ++queueInfoCount;
        };
        addQueue(graphicsFamily);
        addQueue(presentFamily);
        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo deviceInfo {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = queueInfoCount,
            .pQueueCreateInfos = queueInfos.data(),
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = deviceExtensions,
        };
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkCreateDevice failed\n");
            return false;
        }
        vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);

        if (!createSwapchain()) return false;
        if (!createRenderPass()) return false;
        if (!createDescriptorSetLayout()) return false;
        if (!createPipeline()) return false;
        if (!createQuadBuffers()) return false;
        if (!createSyncObjects()) return false;
        return true;
    }

    bool createSwapchain() {
        destroySwapchainResources();
        VkSurfaceCapabilitiesKHR caps {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
        VkSurfaceFormatKHR format = formats[0];
        for (const auto& candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = candidate;
                break;
            }
        }
        swapFormat = format.format;

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &presentModeCount,
                                                  presentModes.data());
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = mode;
                break;
            }
        }

        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        VkExtent2D extent {
            .width = static_cast<std::uint32_t>(fbWidth),
            .height = static_cast<std::uint32_t>(fbHeight),
        };
        extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
        swapExtent = extent;
        windowWidth = static_cast<int>(extent.width);
        windowHeight = static_cast<int>(extent.height);

        imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
            imageCount = caps.maxImageCount;
        }
        VkSwapchainCreateInfoKHR swapInfo {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface,
            .minImageCount = imageCount,
            .imageFormat = swapFormat,
            .imageColorSpace = format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = presentMode,
            .clipped = VK_TRUE,
            .oldSwapchain = VK_NULL_HANDLE,
        };
        if (graphicsFamily != presentFamily) {
            const std::array<std::uint32_t, 2> families { graphicsFamily, presentFamily };
            swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            swapInfo.queueFamilyIndexCount = 2;
            swapInfo.pQueueFamilyIndices = families.data();
        } else {
            swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        if (vkCreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain) != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkCreateSwapchainKHR failed\n");
            return false;
        }
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        swapImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapImages.data());
        swapViews.resize(imageCount);
        framebuffers.resize(imageCount);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = swapImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = swapFormat,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            if (vkCreateImageView(device, &viewInfo, nullptr, &swapViews[i]) != VK_SUCCESS) {
                return false;
            }
            VkFramebufferCreateInfo fbInfo {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = renderPass,
                .attachmentCount = 1,
                .pAttachments = &swapViews[i],
                .width = extent.width,
                .height = extent.height,
                .layers = 1,
            };
            if (vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    bool createRenderPass() {
        VkAttachmentDescription attachment {
            .format = swapFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        VkAttachmentReference colorRef {
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkSubpassDescription subpass {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorRef,
        };
        VkSubpassDependency dependency {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        VkRenderPassCreateInfo info {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
        };
        return vkCreateRenderPass(device, &info, nullptr, &renderPass) == VK_SUCCESS;
    }

    bool createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding binding {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        VkDescriptorSetLayoutCreateInfo info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding,
        };
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &descLayout) != VK_SUCCESS) {
            return false;
        }
        VkPushConstantRange pushRange {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = 16,
        };
        VkPipelineLayoutCreateInfo layoutInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &descLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushRange,
        };
        return vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) == VK_SUCCESS;
    }

    VkShaderModule createShaderModule(const std::uint32_t* code, std::size_t size) {
        VkShaderModuleCreateInfo info {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = size,
            .pCode = code,
        };
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &module);
        return module;
    }

    bool createPipeline() {
        VkShaderModule vert = createShaderModule(reinterpret_cast<const std::uint32_t*>(vert_spv),
                                                 vert_spv_size);
        VkShaderModule frag = createShaderModule(reinterpret_cast<const std::uint32_t*>(frag_spv),
                                                 frag_spv_size);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            std::fprintf(stderr, "VideoViewer: shader module creation failed\n");
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vert,
              .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = frag,
              .pName = "main" },
        };
        VkVertexInputBindingDescription bindingDesc {
            .binding = 0,
            .stride = 4 * sizeof(float),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        std::array<VkVertexInputAttributeDescription, 2> attributes {
            VkVertexInputAttributeDescription { .location = 0, .binding = 0,
                                               .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
            VkVertexInputAttributeDescription { .location = 1, .binding = 0,
                                               .format = VK_FORMAT_R32G32_SFLOAT,
                                               .offset = 2 * sizeof(float) },
        };
        VkPipelineVertexInputStateCreateInfo vertexInput {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &bindingDesc,
            .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
        };
        VkPipelineInputAssemblyStateCreateInfo inputAssembly {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkViewport viewport {
            .x = 0, .y = 0,
            .width = static_cast<float>(swapExtent.width),
            .height = static_cast<float>(swapExtent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor { .offset = { 0, 0 }, .extent = swapExtent };
        VkPipelineViewportStateCreateInfo viewportState {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };
        VkPipelineRasterizationStateCreateInfo rasterizer {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo multisample {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState blendAttachment {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo colorBlend {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment,
        };
        VkGraphicsPipelineCreateInfo pipelineInfo {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pColorBlendState = &colorBlend,
            .layout = pipelineLayout,
            .renderPass = renderPass,
            .subpass = 0,
        };
        const VkResult result =
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkCreateGraphicsPipelines failed\n");
            return false;
        }
        return true;
    }

    bool createQuadBuffers() {
        const std::array<float, 16> vertices {
            // position (NDC unit quad) + uv. Vulkan's clip space Y points
            // down, so pos(0,0) lands at the top of the framebuffer; keep
            // v=0 (image row 0 = video top) there to avoid vertical flip.
            0.0f, 0.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 1.0f, 0.0f, 1.0f,
        };
        const std::array<std::uint16_t, 6> indices { 0, 1, 2, 2, 3, 0 };
        return createBuffer(vertices.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            vertexBuffer, vertexMemory, vertices.data()) &&
               createBuffer(indices.size() * sizeof(std::uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            indexBuffer, indexMemory, indices.data());
    }

    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer,
                      VkDeviceMemory& memory,
                      const void* data) {
        VkBufferCreateInfo bufferInfo {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        std::uint32_t memoryType = UINT32_MAX;
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                memoryType = i;
                break;
            }
        }
        if (memoryType == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocateInfo {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryType,
        };
        if (vkAllocateMemory(device, &allocateInfo, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(device, buffer, memory, 0);
        if (data != nullptr) {
            void* mapped = nullptr;
            vkMapMemory(device, memory, 0, size, 0, &mapped);
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            vkUnmapMemory(device, memory);
        }
        return true;
    }

    bool createTexture(int width, int height) {
        if (textureImage != VK_NULL_HANDLE) {
            vkDestroyImageView(device, textureView, nullptr);
            vkDestroyImage(device, textureImage, nullptr);
            vkFreeMemory(device, textureMemory, nullptr);
            textureImage = VK_NULL_HANDLE;
            textureView = VK_NULL_HANDLE;
            textureMemory = VK_NULL_HANDLE;
        }
        VkImageCreateInfo imageInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(device, &imageInfo, nullptr, &textureImage) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, textureImage, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        std::uint32_t memoryType = UINT32_MAX;
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                memoryType = i;
                break;
            }
        }
        if (memoryType == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocateInfo {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryType,
        };
        if (vkAllocateMemory(device, &allocateInfo, nullptr, &textureMemory) != VK_SUCCESS) {
            return false;
        }
        vkBindImageMemory(device, textureImage, textureMemory, 0);
        VkImageViewCreateInfo viewInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = textureImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (vkCreateImageView(device, &viewInfo, nullptr, &textureView) != VK_SUCCESS) {
            return false;
        }
        if (sampler == VK_NULL_HANDLE) {
            VkSamplerCreateInfo samplerInfo {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = 0.0f,
            };
            if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
                return false;
            }
        }
        if (descPool == VK_NULL_HANDLE) {
            VkDescriptorPoolSize poolSize {
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
            };
            VkDescriptorPoolCreateInfo poolInfo {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &poolSize,
            };
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool) != VK_SUCCESS) {
                return false;
            }
            VkDescriptorSetAllocateInfo allocateInfo {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &descLayout,
            };
            if (vkAllocateDescriptorSets(device, &allocateInfo, &descSet) != VK_SUCCESS) {
                return false;
            }
        }
        VkDescriptorImageInfo imageInfoDesc {
            .sampler = sampler,
            .imageView = textureView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoDesc,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        textureWidth = width;
        textureHeight = height;
        return true;
    }

    bool ensureStaging(std::size_t size) {
        if (stagingBuffer != VK_NULL_HANDLE &&
            stagingCapacity >= size) return true;
        if (stagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
        }
        stagingCapacity = size;
        return createBuffer(size,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            stagingBuffer, stagingMemory, nullptr);
    }

    bool createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkFenceCreateInfo fenceInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlight) != VK_SUCCESS) {
            return false;
        }
        VkCommandPoolCreateInfo poolInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = graphicsFamily,
        };
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            return false;
        }
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo allocInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = imageCount,
        };
        return vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) == VK_SUCCESS;
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer,
                             std::uint32_t imageIndex,
                             bool newTexture,
                             std::array<double, 4> rect,
                             std::size_t frameBytes) {
        VkCommandBufferBeginInfo beginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        if (newTexture) {
            VkImageMemoryBarrier toTransfer {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = textureImage,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
            VkBufferImageCopy copyRegion {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { static_cast<std::uint32_t>(textureWidth),
                                 static_cast<std::uint32_t>(textureHeight), 1 },
            };
            vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, textureImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
            VkImageMemoryBarrier toShader {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = textureImage,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toShader);
        }

        VkRenderPassBeginInfo renderPassInfo {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = framebuffers[imageIndex],
            .renderArea = { { 0, 0 }, swapExtent },
        };
        VkClearValue clear { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const float rectFloats[4] {
            static_cast<float>(rect[0]),
            static_cast<float>(rect[1]),
            static_cast<float>(rect[2]),
            static_cast<float>(rect[3]),
        };
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(rectFloats), rectFloats);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &descSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(commandBuffer, 6, 1, 0, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
        vkEndCommandBuffer(commandBuffer);
        (void)frameBytes;
    }

    bool renderFrame(VRVideoRendererEngine& engine, VRVideoFillMode fillMode) {
        if (resizePending) {
            resizePending = false;
            vkDeviceWaitIdle(device);
            if (!createSwapchain()) {
                std::fprintf(stderr, "VideoViewer: swapchain recreation failed after resize\n");
                return false;
            }
            commandBuffers.clear();
            VkCommandBufferAllocateInfo allocInfo {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = commandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = imageCount,
            };
            commandBuffers.resize(imageCount);
            if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
                std::fprintf(stderr, "VideoViewer: command buffer recreation failed after resize\n");
                return false;
            }
        }

        // 前置等待：确保上一帧 GPU 命令完成后才可能重建纹理。createTexture
        // 会 vkDestroyImage 销毁旧纹理，若 GPU 仍在引用则属未定义行为，忙转
        // 主循环（MAILBOX 非阻塞 present）下必然触发设备丢失；该等待同时把
        // 主循环节流到 GPU 帧率，避免无节制提交。inFlight 初始为 signaled。
        vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);

        VRVideoRendererEngine::Frame frame;
        const bool haveFrame = engine.currentFrame(frame);
        bool newTexture = false;
        if (haveFrame && frame.serial != uploadedSerial) {
            uploadedSerial = frame.serial;
            const std::size_t bytes =
                static_cast<std::size_t>(frame.width) * frame.height * 4u;
            if (!ensureStaging(bytes) || !createTexture(frame.width, frame.height)) {
                std::fprintf(stderr, "VideoViewer: texture/staging allocation failed\n");
                return false;
            }
            void* mapped = nullptr;
            vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped);
            std::memcpy(mapped, frame.rgb, bytes);
            vkUnmapMemory(device, stagingMemory);
            newTexture = true;
        }

        std::uint32_t imageIndex = 0;
        VkResult acquireResult = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                       imageAvailable, VK_NULL_HANDLE,
                                                       &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(device);
            if (!createSwapchain()) return false;
            return true;
        }
        if (acquireResult != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkAcquireNextImageKHR failed: %d\n",
                         static_cast<int>(acquireResult));
            return false;
        }

        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        const double windowAspect =
            fbHeight > 0 ? static_cast<double>(fbWidth) / fbHeight : 1.0;
        const double frameAspect =
            frame.height > 0 ? static_cast<double>(frame.width) / frame.height : 1.0;
        const auto rect = fillRect(windowAspect, frameAspect, fillMode);

        recordCommandBuffer(commandBuffers[imageIndex], imageIndex, newTexture, rect, 0);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &imageAvailable,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffers[imageIndex],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &renderFinished,
        };
        vkResetFences(device, 1, &inFlight);
        const VkResult submitResult = vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlight);
        if (submitResult != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkQueueSubmit failed: %d\n",
                         static_cast<int>(submitResult));
            return false;
        }
        VkPresentInfoKHR presentInfo {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderFinished,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIndex,
        };
        const VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(device);
            if (!createSwapchain()) return false;
        } else if (presentResult != VK_SUCCESS) {
            std::fprintf(stderr, "VideoViewer: vkQueuePresentKHR failed: %d\n",
                         static_cast<int>(presentResult));
            return false;
        }
        currentImage = imageIndex;
        return true;
    }

    VkDeviceSize stagingCapacity { 0 };
};

int main(int argc, char** argv) {
    ViewerArgs args;
    if (!parseArgs(argc, argv, args)) return 1;
    std::shared_ptr<VRVideoManifest> manifest;
    try {
        manifest = VRVideoManifest::loadFromDirectory(args.workshop);
    } catch (const VideoRendererManifestError &e) {
        std::fprintf(stderr, "VideoViewer: %s\n", e.what());
        return e.code;
    }
    VRVideoEngineConfig config = VRVideoRendererEngine::defaultConfig();
    config.fillMode = args.fillMode;
    config.initialVolume = args.volume;
    config.muted = args.muted;
    config.autoplay = true;

    VRVideoRendererEngine::Callbacks callbacks;
    callbacks.playbackError = [](const std::string& message) {
        std::fprintf(stderr, "VideoViewer: %s\n", message.c_str());
    };
    callbacks.videoDidEnd = []() {
        /* Keep the window open; the wallpaper app decides whether to advance. */
    };

    VRVideoRendererEngine engine(config, callbacks);
    std::string openError;
    if (!engine.openWallpaper(*manifest, &openError)) {
        std::fprintf(stderr, "VideoViewer: %s\n", openError.c_str());
        return 3;
    }

    Viewer viewer;
    if (!viewer.init(args.width, args.height)) return 1;

    const auto started = std::chrono::steady_clock::now();
    bool quit = false;
    while (!quit) {
        glfwPollEvents();
        if (glfwWindowShouldClose(viewer.window)) quit = true;
        if (!viewer.renderFrame(engine, args.fillMode)) {
            std::fprintf(stderr, "VideoViewer: render frame failed\n");
            break;
        }
        if (args.runSeconds > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            if (elapsed >= args.runSeconds) quit = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return 0;
}
