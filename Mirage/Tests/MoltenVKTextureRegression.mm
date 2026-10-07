static constexpr const char* copyrightNotice = "Copyright © 2026 王孝慈. All rights reserved.";

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdlib>
#include <iostream>
#include <vector>

static void require(bool condition, const char* operation) {
    if (!condition) {
        std::cerr << operation << " failed\n";
        std::exit(1);
    }
}

#define LOAD_INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name)); require(name != nullptr, #name)
#define LOAD_DEVICE(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); require(name != nullptr, #name)

int main(int argc, char** argv) {
    require(argc == 2, "library argument");
    @autoreleasepool {
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(library != nullptr, "dlopen");
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        require(get != nullptr, "vkGetInstanceProcAddr");
        auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        require(create != nullptr, "vkCreateInstance entry");
        VkApplicationInfo application { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
        VkInstanceCreateInfo instance_info { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application };
        VkInstance instance {};
        require(create(&instance_info, nullptr, &instance) == VK_SUCCESS, "vkCreateInstance");
        LOAD_INSTANCE(vkEnumeratePhysicalDevices);
        LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
        LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
        LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
        LOAD_INSTANCE(vkCreateDevice);
        LOAD_INSTANCE(vkGetDeviceProcAddr);
        LOAD_INSTANCE(vkDestroyInstance);
        uint32_t count = 0;
        require(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0, "physical devices");
        std::vector<VkPhysicalDevice> gpus(count);
        require(vkEnumeratePhysicalDevices(instance, &count, gpus.data()) == VK_SUCCESS, "physical device enumeration");
        const auto gpu = gpus.front();
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        std::cout << "GPU: " << properties.deviceName << '\n';
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());
        uint32_t family = 0;
        while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        require(family < count, "graphics queue");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                        .queueCount = 1, .pQueuePriorities = &priority };
        const char* extensions[] = { "VK_KHR_portability_subset", "VK_EXT_metal_objects" };
        VkDeviceCreateInfo device_info { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                         .pQueueCreateInfos = &queue, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions };
        VkDevice device {};
        require(vkCreateDevice(gpu, &device_info, nullptr, &device) == VK_SUCCESS, "vkCreateDevice");
        LOAD_DEVICE(vkCreateImage);
        LOAD_DEVICE(vkGetImageMemoryRequirements);
        LOAD_DEVICE(vkAllocateMemory);
        LOAD_DEVICE(vkBindImageMemory);
        LOAD_DEVICE(vkExportMetalObjectsEXT);
        LOAD_DEVICE(vkDestroyImage);
        LOAD_DEVICE(vkFreeMemory);
        LOAD_DEVICE(vkDestroyDevice);
        VkPhysicalDeviceMemoryProperties memory {};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        const VkFormat formats[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                     VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
        const VkImageUsageFlags usages[] = {
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        };
        unsigned checked = 0;
        for (auto format : formats) {
            for (auto usage : usages) {
                VkExportMetalObjectCreateInfoEXT export_create { .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
                    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT };
                VkImageCreateInfo image_info { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &export_create,
                    .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
                VkImage image {};
                require(vkCreateImage(device, &image_info, nullptr, &image) == VK_SUCCESS, "vkCreateImage");
                VkMemoryRequirements requirements {};
                vkGetImageMemoryRequirements(device, image, &requirements);
                uint32_t type = 0;
                while (type < memory.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
                require(type < memory.memoryTypeCount, "memory type");
                VkMemoryAllocateInfo allocation_info { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                       .allocationSize = requirements.size, .memoryTypeIndex = type };
                VkDeviceMemory allocation {};
                require(vkAllocateMemory(device, &allocation_info, nullptr, &allocation) == VK_SUCCESS, "vkAllocateMemory");
                require(vkBindImageMemory(device, image, allocation, 0) == VK_SUCCESS, "vkBindImageMemory");
                VkExportMetalTextureInfoEXT texture_info { .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
                                                           .image = image, .plane = VK_IMAGE_ASPECT_COLOR_BIT };
                VkExportMetalObjectsInfoEXT export_info { .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &texture_info };
                vkExportMetalObjectsEXT(device, &export_info);
                id<MTLTexture> texture = texture_info.mtlTexture;
                require(texture != nil, "exported Metal texture");
                const bool protected_target = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                std::cout << "format=" << format << " usage=" << usage
                          << " allowGPUOptimizedContents=" << int(texture.allowGPUOptimizedContents) << '\n';
                require(bool(texture.allowGPUOptimizedContents) == !protected_target, "targeted compression safeguard");
                ++checked;
                vkDestroyImage(device, image, nullptr);
                vkFreeMemory(device, allocation, nullptr);
            }
        }
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        dlclose(library);
        std::cout << "Metal texture descriptor checks passed: " << checked << '\n';
    }
}
