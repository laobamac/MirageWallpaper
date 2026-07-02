module;

#include <vulkan/vulkan.h>
#include <cstdint>

// Capture macros before #undef. Comprehensive Vulkan FFI module — covers
// the symbol surface needed by both wavsen video plumbing and OWE's vvk
// dispatch-table renderer. Macros that resist tokenisation are captured
// in a hidden namespace, #undef'd, and re-exported as constexpr alternates.
namespace _wv_vk {
inline constexpr std::uint32_t k_VK_TRUE = VK_TRUE;
inline constexpr std::uint32_t k_VK_FALSE = VK_FALSE;
inline constexpr std::uint32_t k_VK_API_VERSION_1_3 = VK_API_VERSION_1_3;
inline constexpr std::uint32_t k_VK_QUEUE_FAMILY_IGNORED = VK_QUEUE_FAMILY_IGNORED;
inline constexpr std::uint32_t k_VK_QUEUE_FAMILY_FOREIGN_EXT = VK_QUEUE_FAMILY_FOREIGN_EXT;
inline constexpr std::uint64_t k_VK_WHOLE_SIZE = VK_WHOLE_SIZE;
inline constexpr std::uint32_t k_VK_API_VERSION_1_1 = VK_API_VERSION_1_1;
inline constexpr std::uint32_t k_VK_REMAINING_ARRAY_LAYERS = VK_REMAINING_ARRAY_LAYERS;
inline constexpr std::uint32_t k_VK_REMAINING_MIP_LEVELS = VK_REMAINING_MIP_LEVELS;
inline constexpr std::uint32_t k_VK_SUBPASS_EXTERNAL = VK_SUBPASS_EXTERNAL;
inline constexpr std::uint32_t k_VK_VERSION_1_1 = VK_VERSION_1_1;
}

#undef VK_TRUE
#undef VK_FALSE
#undef VK_API_VERSION_1_3
#undef VK_QUEUE_FAMILY_IGNORED
#undef VK_QUEUE_FAMILY_FOREIGN_EXT
#undef VK_WHOLE_SIZE
#undef VK_API_VERSION_1_1
#undef VK_REMAINING_ARRAY_LAYERS
#undef VK_REMAINING_MIP_LEVELS
#undef VK_SUBPASS_EXTERNAL
#undef VK_VERSION_1_1

namespace _wv_vk_ext {
inline constexpr const char* k_VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME = VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME = VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_DEBUG_UTILS_EXTENSION_NAME = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_MEMORY_BUDGET_EXTENSION_NAME = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
inline constexpr const char* k_VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME = VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
inline constexpr const char* k_VK_KHR_SWAPCHAIN_EXTENSION_NAME = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
}

#undef VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
#undef VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
#undef VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
#undef VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
#undef VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME
#undef VK_EXT_DEBUG_UTILS_EXTENSION_NAME
#undef VK_EXT_MEMORY_BUDGET_EXTENSION_NAME
#undef VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME
#undef VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME
#undef VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME
#undef VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
#undef VK_KHR_SWAPCHAIN_EXTENSION_NAME

// VK_NULL_HANDLE is `#define VK_NULL_HANDLE 0` — wrap with conversion ops
// so it compares cleanly against both dispatchable (pointer) and non-
// dispatchable (uint64_t) handles.
namespace _wv_vk {
struct NullHandle {
    constexpr operator std::uint64_t() const noexcept { return 0; }
    template <class T>
    constexpr operator T*() const noexcept { return nullptr; }
};
}
#undef VK_NULL_HANDLE
#undef VK_MAKE_VERSION

export module vulkan;

export {

// ---- captured macros ----

inline constexpr std::uint32_t VK_TRUE = _wv_vk::k_VK_TRUE;
inline constexpr std::uint32_t VK_FALSE = _wv_vk::k_VK_FALSE;
inline constexpr std::uint32_t VK_API_VERSION_1_3 = _wv_vk::k_VK_API_VERSION_1_3;
inline constexpr std::uint32_t VK_QUEUE_FAMILY_IGNORED = _wv_vk::k_VK_QUEUE_FAMILY_IGNORED;
inline constexpr std::uint32_t VK_QUEUE_FAMILY_FOREIGN_EXT = _wv_vk::k_VK_QUEUE_FAMILY_FOREIGN_EXT;
inline constexpr std::uint64_t VK_WHOLE_SIZE = _wv_vk::k_VK_WHOLE_SIZE;
inline constexpr std::uint32_t VK_API_VERSION_1_1 = _wv_vk::k_VK_API_VERSION_1_1;
inline constexpr std::uint32_t VK_REMAINING_ARRAY_LAYERS = _wv_vk::k_VK_REMAINING_ARRAY_LAYERS;
inline constexpr std::uint32_t VK_REMAINING_MIP_LEVELS = _wv_vk::k_VK_REMAINING_MIP_LEVELS;
inline constexpr std::uint32_t VK_SUBPASS_EXTERNAL = _wv_vk::k_VK_SUBPASS_EXTERNAL;
inline constexpr std::uint32_t VK_VERSION_1_1 = _wv_vk::k_VK_VERSION_1_1;

inline constexpr const char* VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
inline constexpr const char* VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
inline constexpr const char* VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
inline constexpr const char* VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
inline constexpr const char* VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
inline constexpr const char* VK_EXT_DEBUG_UTILS_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
inline constexpr const char* VK_EXT_MEMORY_BUDGET_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
inline constexpr const char* VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME = _wv_vk_ext::k_VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
inline constexpr const char* VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME;
inline constexpr const char* VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
inline constexpr const char* VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
inline constexpr const char* VK_KHR_SWAPCHAIN_EXTENSION_NAME = _wv_vk_ext::k_VK_KHR_SWAPCHAIN_EXTENSION_NAME;

inline constexpr _wv_vk::NullHandle VK_NULL_HANDLE {};

inline constexpr std::uint32_t VK_MAKE_VERSION(std::uint32_t major, std::uint32_t minor,
                                               std::uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

// ---- handle types, struct types, enums, flags ----

using ::VkAccessFlags;
using ::VkApplicationInfo;
using ::VkAttachmentDescription;
using ::VkAttachmentLoadOp;
using ::VkAttachmentReference;
using ::VkBool32;
using ::VkBuffer;
using ::VkBufferCopy;
using ::VkBufferCreateInfo;
using ::VkBufferImageCopy;
using ::VkBufferMemoryBarrier;
using ::VkBufferUsageFlags;
using ::VkClearAttachment;
using ::VkClearColorValue;
using ::VkClearRect;
using ::VkClearValue;
using ::VkColorComponentFlags;
using ::VkColorSpaceKHR;
using ::VkCommandBuffer;
using ::VkCommandBufferAllocateInfo;
using ::VkCommandBufferBeginInfo;
using ::VkCommandBufferLevel;
using ::VkCommandPool;
using ::VkCommandPoolCreateInfo;
using ::VkComponentSwizzle;
using ::VkCompositeAlphaFlagBitsKHR;
using ::VkComputePipelineCreateInfo;
using ::VkDebugUtilsLabelEXT;
using ::VkDebugUtilsMessageSeverityFlagBitsEXT;
using ::VkDebugUtilsMessageTypeFlagsEXT;
using ::VkDebugUtilsMessengerCallbackDataEXT;
using ::VkDebugUtilsMessengerCreateInfoEXT;
using ::VkDebugUtilsMessengerEXT;
using ::VkDependencyFlags;
using ::VkDescriptorBufferInfo;
using ::VkDescriptorImageInfo;
using ::VkDescriptorPool;
using ::VkDescriptorPoolCreateInfo;
using ::VkDescriptorPoolSize;
using ::VkDescriptorSet;
using ::VkDescriptorSetAllocateInfo;
using ::VkDescriptorSetLayout;
using ::VkDescriptorSetLayoutBinding;
using ::VkDescriptorSetLayoutCreateFlags;
using ::VkDescriptorSetLayoutCreateInfo;
using ::VkDescriptorUpdateTemplateKHR;
using ::VkDevice;
using ::VkDeviceCreateInfo;
using ::VkDeviceMemory;
using ::VkDeviceQueueCreateInfo;
using ::VkDeviceSize;
using ::VkDynamicState;
using ::VkEvent;
using ::VkExportMemoryAllocateInfo;
using ::VkExportSemaphoreCreateInfo;
using ::VkExtensionProperties;
using ::VkExtent2D;
using ::VkExtent3D;
using ::VkExternalMemoryImageCreateInfo;
using ::VkExternalSemaphoreHandleTypeFlagBits;
using ::VkFence;
using ::VkFenceCreateInfo;
using ::VkFilter;
using ::VkFormat;
using ::VkFormatProperties;
using ::VkFramebuffer;
using ::VkFramebufferCreateInfo;
using ::VkGraphicsPipelineCreateInfo;
using ::VkImage;
using ::VkImageAspectFlags;
using ::VkImageBlit;
using ::VkImageCopy;
using ::VkImageCreateInfo;
using ::VkImageDrmFormatModifierExplicitCreateInfoEXT;
using ::VkImageDrmFormatModifierPropertiesEXT;
using ::VkImageLayout;
using ::VkImageMemoryBarrier;
using ::VkImageResolve;
using ::VkImageSubresource;
using ::VkImageSubresourceLayers;
using ::VkImageSubresourceRange;
using ::VkImageTiling;
using ::VkImageType;
using ::VkImageUsageFlags;
using ::VkImageView;
using ::VkImageViewCreateInfo;
using ::VkImageViewType;
using ::VkIndexType;
using ::VkImportMemoryFdInfoKHR;
using ::VkInstance;
using ::VkInstanceCreateInfo;
using ::VkLayerProperties;
using ::VkLogicOp;
using ::VkMemoryAllocateInfo;
using ::VkMemoryBarrier;
using ::VkMemoryDedicatedAllocateInfo;
using ::VkMemoryFdPropertiesKHR;
using ::VkMemoryGetFdInfoKHR;
using ::VkMemoryPropertyFlags;
using ::VkMemoryRequirements;
using ::VkOffset3D;
using ::VkPhysicalDevice;
using ::VkPhysicalDeviceDrmPropertiesEXT;
using ::VkPhysicalDeviceFeatures;
using ::VkPhysicalDeviceFeatures2;
using ::VkPhysicalDeviceFeatures2KHR;
using ::VkPhysicalDeviceIDProperties;
using ::VkPhysicalDeviceIDPropertiesKHR;
using ::VkPhysicalDeviceLimits;
using ::VkPhysicalDeviceMemoryProperties;
using ::VkPhysicalDeviceMemoryProperties2;
using ::VkPhysicalDeviceProperties;
using ::VkPhysicalDeviceProperties2;
using ::VkPhysicalDeviceProperties2KHR;
using ::VkPhysicalDeviceVulkan12Features;
using ::VkPhysicalDeviceVulkan13Features;
using ::VkPipeline;
using ::VkPipelineBindPoint;
using ::VkPipelineColorBlendAttachmentState;
using ::VkPipelineColorBlendStateCreateInfo;
using ::VkPipelineDepthStencilStateCreateInfo;
using ::VkPipelineDynamicStateCreateInfo;
using ::VkPipelineInputAssemblyStateCreateInfo;
using ::VkPipelineLayout;
using ::VkPipelineLayoutCreateInfo;
using ::VkPipelineMultisampleStateCreateInfo;
using ::VkPipelineRasterizationStateCreateInfo;
using ::VkPipelineShaderStageCreateInfo;
using ::VkPipelineStageFlags;
using ::VkPipelineVertexInputStateCreateInfo;
using ::VkPipelineViewportStateCreateInfo;
using ::VkPresentInfoKHR;
using ::VkPresentModeKHR;
using ::VkPrimitiveTopology;
using ::VkPushConstantRange;
using ::VkQueryControlFlags;
using ::VkQueryPool;
using ::VkQueue;
using ::VkQueueFamilyProperties;
using ::VkQueueFlagBits;
using ::VkQueueFlags;
using ::VkRect2D;
using ::VkRenderPass;
using ::VkRenderPassBeginInfo;
using ::VkRenderPassCreateInfo;
using ::VkResult;
using ::VkSampleCountFlagBits;
using ::VkSampler;
using ::VkSamplerAddressMode;
using ::VkSamplerCreateInfo;
using ::VkSamplerMipmapMode;
using ::VkSemaphore;
using ::VkSemaphoreCreateInfo;
using ::VkSemaphoreGetFdInfoKHR;
using ::VkSemaphoreWaitInfoKHR;
using ::VkShaderModule;
using ::VkShaderModuleCreateInfo;
using ::VkShaderStageFlagBits;
using ::VkShaderStageFlags;
using ::VkSharingMode;
using ::VkStencilFaceFlags;
using ::VkSubmitInfo;
using ::VkSubpassContents;
using ::VkSubpassDependency;
using ::VkSubpassDescription;
using ::VkSubresourceLayout;
using ::VkSurfaceCapabilitiesKHR;
using ::VkSurfaceFormatKHR;
using ::VkSurfaceKHR;
using ::VkSurfaceTransformFlagBitsKHR;
using ::VkSwapchainCreateInfoKHR;
using ::VkSwapchainKHR;
using ::VkTimelineSemaphoreSubmitInfo;
using ::VkVertexInputAttributeDescription;
using ::VkVertexInputBindingDescription;
using ::VkVideoCodecOperationFlagBitsKHR;
using ::VkViewport;
using ::VkWriteDescriptorSet;

// ---- enumerator constants ----

using ::VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
using ::VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
using ::VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
using ::VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
using ::VK_ACCESS_MEMORY_READ_BIT;
using ::VK_ACCESS_MEMORY_WRITE_BIT;
using ::VK_ACCESS_SHADER_READ_BIT;
using ::VK_ACCESS_SHADER_WRITE_BIT;
using ::VK_ACCESS_TRANSFER_READ_BIT;
using ::VK_ACCESS_TRANSFER_WRITE_BIT;
using ::VK_ACCESS_UNIFORM_READ_BIT;
using ::VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
using ::VK_ATTACHMENT_LOAD_OP_CLEAR;
using ::VK_ATTACHMENT_LOAD_OP_DONT_CARE;
using ::VK_ATTACHMENT_LOAD_OP_LOAD;
using ::VK_ATTACHMENT_STORE_OP_DONT_CARE;
using ::VK_ATTACHMENT_STORE_OP_STORE;
using ::VK_BLEND_FACTOR_ONE;
using ::VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
using ::VK_BLEND_FACTOR_SRC_ALPHA;
using ::VK_BLEND_FACTOR_ZERO;
using ::VK_BLEND_OP_ADD;
using ::VK_BORDER_COLOR_INT_OPAQUE_BLACK;
using ::VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
using ::VK_BUFFER_USAGE_TRANSFER_DST_BIT;
using ::VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
using ::VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
using ::VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
using ::VK_COLORSPACE_SRGB_NONLINEAR_KHR;
using ::VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
using ::VK_COLOR_COMPONENT_A_BIT;
using ::VK_COLOR_COMPONENT_B_BIT;
using ::VK_COLOR_COMPONENT_G_BIT;
using ::VK_COLOR_COMPONENT_R_BIT;
using ::VK_COMMAND_BUFFER_LEVEL_PRIMARY;
using ::VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
using ::VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
using ::VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
using ::VK_COMPARE_OP_LESS_OR_EQUAL;
using ::VK_COMPARE_OP_NEVER;
using ::VK_COMPONENT_SWIZZLE_IDENTITY;
using ::VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
using ::VK_CULL_MODE_BACK_BIT;
using ::VK_CULL_MODE_FRONT_BIT;
using ::VK_CULL_MODE_NONE;
using ::VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
using ::VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
using ::VK_DEPENDENCY_BY_REGION_BIT;
using ::VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
using ::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
using ::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
using ::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
using ::VK_DYNAMIC_STATE_SCISSOR;
using ::VK_DYNAMIC_STATE_VIEWPORT;
using ::VK_ERROR_DEVICE_LOST;
using ::VK_ERROR_EXTENSION_NOT_PRESENT;
using ::VK_ERROR_FEATURE_NOT_PRESENT;
using ::VK_ERROR_FORMAT_NOT_SUPPORTED;
using ::VK_ERROR_INCOMPATIBLE_DRIVER;
using ::VK_ERROR_INITIALIZATION_FAILED;
using ::VK_ERROR_LAYER_NOT_PRESENT;
using ::VK_ERROR_OUT_OF_DATE_KHR;
using ::VK_ERROR_OUT_OF_DEVICE_MEMORY;
using ::VK_ERROR_OUT_OF_HOST_MEMORY;
using ::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
using ::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
using ::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
using ::VK_FENCE_CREATE_SIGNALED_BIT;
using ::VK_FILTER_LINEAR;
using ::VK_FILTER_NEAREST;
using ::VK_FORMAT_B8G8R8A8_UNORM;
using ::VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
using ::VK_FORMAT_BC2_UNORM_BLOCK;
using ::VK_FORMAT_BC3_UNORM_BLOCK;
using ::VK_FORMAT_D32_SFLOAT;
using ::VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
using ::VK_FORMAT_R16G16_UNORM;
using ::VK_FORMAT_R16_UNORM;
using ::VK_FORMAT_R8G8B8A8_UNORM;
using ::VK_FORMAT_R8G8B8_UNORM;
using ::VK_FORMAT_R8G8_UNORM;
using ::VK_FORMAT_R8_UNORM;
using ::VK_FORMAT_UNDEFINED;
using ::VK_FRONT_FACE_COUNTER_CLOCKWISE;
using ::VK_IMAGE_ASPECT_COLOR_BIT;
using ::VK_IMAGE_ASPECT_DEPTH_BIT;
using ::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
using ::VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
using ::VK_IMAGE_LAYOUT_GENERAL;
using ::VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
using ::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
using ::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
using ::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
using ::VK_IMAGE_LAYOUT_UNDEFINED;
using ::VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
using ::VK_IMAGE_TILING_LINEAR;
using ::VK_IMAGE_TILING_OPTIMAL;
using ::VK_IMAGE_TYPE_2D;
using ::VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
using ::VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
using ::VK_IMAGE_USAGE_SAMPLED_BIT;
using ::VK_IMAGE_USAGE_TRANSFER_DST_BIT;
using ::VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
using ::VK_IMAGE_VIEW_TYPE_2D;
using ::VK_INDEX_TYPE_UINT32;
using ::VK_LOGIC_OP_COPY;
using ::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
using ::VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
using ::VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
using ::VK_PIPELINE_BIND_POINT_COMPUTE;
using ::VK_PIPELINE_BIND_POINT_GRAPHICS;
using ::VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
using ::VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
using ::VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
using ::VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
using ::VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
using ::VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
using ::VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
using ::VK_PIPELINE_STAGE_TRANSFER_BIT;
using ::VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
using ::VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
using ::VK_POLYGON_MODE_FILL;
using ::VK_PRESENT_MODE_FIFO_KHR;
using ::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
using ::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
using ::VK_QUEUE_COMPUTE_BIT;
using ::VK_QUEUE_GRAPHICS_BIT;
using ::VK_QUEUE_TRANSFER_BIT;
using ::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
using ::VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
using ::VK_SAMPLER_ADDRESS_MODE_REPEAT;
using ::VK_SAMPLER_MIPMAP_MODE_LINEAR;
using ::VK_SAMPLER_MIPMAP_MODE_NEAREST;
using ::VK_SAMPLE_COUNT_1_BIT;
using ::VK_SHADER_STAGE_COMPUTE_BIT;
using ::VK_SHADER_STAGE_FRAGMENT_BIT;
using ::VK_SHADER_STAGE_GEOMETRY_BIT;
using ::VK_SHADER_STAGE_VERTEX_BIT;
using ::VK_SHARING_MODE_EXCLUSIVE;
using ::VK_STRUCTURE_TYPE_APPLICATION_INFO;
using ::VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
using ::VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
using ::VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
using ::VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
using ::VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
using ::VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
using ::VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
using ::VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
using ::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
using ::VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
using ::VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
using ::VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
using ::VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
using ::VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
using ::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
using ::VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
using ::VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
using ::VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
using ::VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR;
using ::VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
using ::VK_STRUCTURE_TYPE_SUBMIT_INFO;
using ::VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
using ::VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
using ::VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
using ::VK_SUBOPTIMAL_KHR;
using ::VK_SUBPASS_CONTENTS_INLINE;
using ::VK_SUCCESS;
using ::VK_VERTEX_INPUT_RATE_VERTEX;

// ---- functions (loadable directly when not using VK_NO_PROTOTYPES) ----

using ::vkAcquireNextImageKHR;
using ::vkAllocateCommandBuffers;
using ::vkAllocateDescriptorSets;
using ::vkAllocateMemory;
using ::vkBeginCommandBuffer;
using ::vkBindBufferMemory;
using ::vkBindImageMemory;
using ::vkCmdBindDescriptorSets;
using ::vkCmdBindPipeline;
using ::vkCmdBlitImage;
using ::vkCmdClearColorImage;
using ::vkCmdCopyBufferToImage;
using ::vkCmdCopyImage;
using ::vkCmdDispatch;
using ::vkCmdPipelineBarrier;
using ::vkCmdPushConstants;
using ::vkCreateBuffer;
using ::vkCreateCommandPool;
using ::vkCreateComputePipelines;
using ::vkCreateDescriptorPool;
using ::vkCreateDescriptorSetLayout;
using ::vkCreateDevice;
using ::vkCreateFence;
using ::vkCreateImage;
using ::vkCreateImageView;
using ::vkCreateInstance;
using ::vkCreatePipelineLayout;
using ::vkCreateSampler;
using ::vkCreateSemaphore;
using ::vkCreateShaderModule;
using ::vkCreateSwapchainKHR;
using ::vkDestroyBuffer;
using ::vkDestroyCommandPool;
using ::vkDestroyDescriptorPool;
using ::vkDestroyDescriptorSetLayout;
using ::vkDestroyDevice;
using ::vkDestroyFence;
using ::vkDestroyImage;
using ::vkDestroyImageView;
using ::vkDestroyInstance;
using ::vkDestroyPipeline;
using ::vkDestroyPipelineLayout;
using ::vkDestroySampler;
using ::vkDestroySemaphore;
using ::vkDestroyShaderModule;
using ::vkDestroySurfaceKHR;
using ::vkDestroySwapchainKHR;
using ::vkDeviceWaitIdle;
using ::vkEndCommandBuffer;
using ::vkEnumerateDeviceExtensionProperties;
using ::vkEnumeratePhysicalDevices;
using ::vkFreeMemory;
using ::vkGetBufferMemoryRequirements;
using ::vkGetDeviceProcAddr;
using ::vkGetDeviceQueue;
using ::vkGetImageMemoryRequirements;
using ::vkGetInstanceProcAddr;
using ::vkGetMemoryFdPropertiesKHR;
using ::vkGetPhysicalDeviceFeatures2;
using ::vkGetPhysicalDeviceMemoryProperties;
using ::vkGetPhysicalDeviceProperties2;
using ::vkGetPhysicalDeviceQueueFamilyProperties;
using ::vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
using ::vkGetPhysicalDeviceSurfaceFormatsKHR;
using ::vkGetPhysicalDeviceSurfaceSupportKHR;
using ::vkGetSwapchainImagesKHR;
using ::vkMapMemory;
using ::vkQueuePresentKHR;
using ::vkQueueSubmit;
using ::vkResetCommandBuffer;
using ::vkResetFences;
using ::vkUnmapMemory;
using ::vkUpdateDescriptorSets;
using ::vkWaitForFences;

// ---- function pointer typedefs (PFN_*) for runtime dispatch tables ----

using ::PFN_vkAcquireNextImageKHR;
using ::PFN_vkAllocateCommandBuffers;
using ::PFN_vkAllocateDescriptorSets;
using ::PFN_vkAllocateMemory;
using ::PFN_vkBeginCommandBuffer;
using ::PFN_vkBindBufferMemory;
using ::PFN_vkBindImageMemory;
using ::PFN_vkCmdBeginDebugUtilsLabelEXT;
using ::PFN_vkCmdBeginQuery;
using ::PFN_vkCmdBeginRenderPass;
using ::PFN_vkCmdBindDescriptorSets;
using ::PFN_vkCmdBindIndexBuffer;
using ::PFN_vkCmdBindPipeline;
using ::PFN_vkCmdBindVertexBuffers;
using ::PFN_vkCmdBlitImage;
using ::PFN_vkCmdClearAttachments;
using ::PFN_vkCmdClearColorImage;
using ::PFN_vkCmdCopyBuffer;
using ::PFN_vkCmdCopyBufferToImage;
using ::PFN_vkCmdCopyImage;
using ::PFN_vkCmdCopyImageToBuffer;
using ::PFN_vkCmdDispatch;
using ::PFN_vkCmdDraw;
using ::PFN_vkCmdDrawIndexed;
using ::PFN_vkCmdEndDebugUtilsLabelEXT;
using ::PFN_vkCmdEndQuery;
using ::PFN_vkCmdEndRenderPass;
using ::PFN_vkCmdFillBuffer;
using ::PFN_vkCmdPipelineBarrier;
using ::PFN_vkCmdPushConstants;
using ::PFN_vkCmdPushDescriptorSetKHR;
using ::PFN_vkCmdPushDescriptorSetWithTemplateKHR;
using ::PFN_vkCmdResolveImage;
using ::PFN_vkCmdSetBlendConstants;
using ::PFN_vkCmdSetDepthBias;
using ::PFN_vkCmdSetDepthBounds;
using ::PFN_vkCmdSetEvent;
using ::PFN_vkCmdSetLineWidth;
using ::PFN_vkCmdSetScissor;
using ::PFN_vkCmdSetStencilCompareMask;
using ::PFN_vkCmdSetStencilReference;
using ::PFN_vkCmdSetStencilWriteMask;
using ::PFN_vkCmdSetViewport;
using ::PFN_vkCmdWaitEvents;
using ::PFN_vkCreateBuffer;
using ::PFN_vkCreateBufferView;
using ::PFN_vkCreateCommandPool;
using ::PFN_vkCreateComputePipelines;
using ::PFN_vkCreateDebugUtilsMessengerEXT;
using ::PFN_vkCreateDescriptorPool;
using ::PFN_vkCreateDescriptorSetLayout;
using ::PFN_vkCreateDescriptorUpdateTemplateKHR;
using ::PFN_vkCreateDevice;
using ::PFN_vkCreateEvent;
using ::PFN_vkCreateFence;
using ::PFN_vkCreateFramebuffer;
using ::PFN_vkCreateGraphicsPipelines;
using ::PFN_vkCreateImage;
using ::PFN_vkCreateImageView;
using ::PFN_vkCreateInstance;
using ::PFN_vkCreatePipelineLayout;
using ::PFN_vkCreateQueryPool;
using ::PFN_vkCreateRenderPass;
using ::PFN_vkCreateSampler;
using ::PFN_vkCreateSemaphore;
using ::PFN_vkCreateShaderModule;
using ::PFN_vkCreateSwapchainKHR;
using ::PFN_vkDestroyBuffer;
using ::PFN_vkDestroyBufferView;
using ::PFN_vkDestroyCommandPool;
using ::PFN_vkDestroyDebugUtilsMessengerEXT;
using ::PFN_vkDestroyDescriptorPool;
using ::PFN_vkDestroyDescriptorSetLayout;
using ::PFN_vkDestroyDescriptorUpdateTemplateKHR;
using ::PFN_vkDestroyDevice;
using ::PFN_vkDestroyEvent;
using ::PFN_vkDestroyFence;
using ::PFN_vkDestroyFramebuffer;
using ::PFN_vkDestroyImage;
using ::PFN_vkDestroyImageView;
using ::PFN_vkDestroyInstance;
using ::PFN_vkDestroyPipeline;
using ::PFN_vkDestroyPipelineLayout;
using ::PFN_vkDestroyQueryPool;
using ::PFN_vkDestroyRenderPass;
using ::PFN_vkDestroySampler;
using ::PFN_vkDestroySemaphore;
using ::PFN_vkDestroyShaderModule;
using ::PFN_vkDestroySurfaceKHR;
using ::PFN_vkDestroySwapchainKHR;
using ::PFN_vkDeviceWaitIdle;
using ::PFN_vkEndCommandBuffer;
using ::PFN_vkEnumerateDeviceExtensionProperties;
using ::PFN_vkEnumerateInstanceExtensionProperties;
using ::PFN_vkEnumerateInstanceLayerProperties;
using ::PFN_vkEnumeratePhysicalDevices;
using ::PFN_vkFreeCommandBuffers;
using ::PFN_vkFreeDescriptorSets;
using ::PFN_vkFreeMemory;
using ::PFN_vkGetBufferMemoryRequirements2;
using ::PFN_vkGetDeviceProcAddr;
using ::PFN_vkGetDeviceQueue;
using ::PFN_vkGetEventStatus;
using ::PFN_vkGetFenceStatus;
using ::PFN_vkGetImageDrmFormatModifierPropertiesEXT;
using ::PFN_vkGetImageMemoryRequirements;
using ::PFN_vkGetImageSubresourceLayout;
using ::PFN_vkGetInstanceProcAddr;
using ::PFN_vkGetMemoryFdKHR;
using ::PFN_vkGetMemoryFdPropertiesKHR;
using ::PFN_vkGetPhysicalDeviceFeatures2;
using ::PFN_vkGetPhysicalDeviceFeatures2KHR;
using ::PFN_vkGetPhysicalDeviceFormatProperties;
using ::PFN_vkGetPhysicalDeviceMemoryProperties;
using ::PFN_vkGetPhysicalDeviceMemoryProperties2;
using ::PFN_vkGetPhysicalDeviceProperties;
using ::PFN_vkGetPhysicalDeviceProperties2;
using ::PFN_vkGetPhysicalDeviceProperties2KHR;
using ::PFN_vkGetPhysicalDeviceQueueFamilyProperties;
using ::PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
using ::PFN_vkGetPhysicalDeviceSurfaceFormatsKHR;
using ::PFN_vkGetPhysicalDeviceSurfacePresentModesKHR;
using ::PFN_vkGetPhysicalDeviceSurfaceSupportKHR;
using ::PFN_vkGetPipelineExecutablePropertiesKHR;
using ::PFN_vkGetPipelineExecutableStatisticsKHR;
using ::PFN_vkGetQueryPoolResults;
using ::PFN_vkGetSemaphoreCounterValueKHR;
using ::PFN_vkGetSemaphoreFdKHR;
using ::PFN_vkGetSwapchainImagesKHR;
using ::PFN_vkMapMemory;
using ::PFN_vkQueuePresentKHR;
using ::PFN_vkQueueSubmit;
using ::PFN_vkResetFences;
using ::PFN_vkSetDebugUtilsObjectNameEXT;
using ::PFN_vkSetDebugUtilsObjectTagEXT;
using ::PFN_vkUnmapMemory;
using ::PFN_vkUpdateDescriptorSetWithTemplateKHR;
using ::PFN_vkUpdateDescriptorSets;
using ::PFN_vkVoidFunction;
using ::PFN_vkWaitForFences;
using ::PFN_vkWaitSemaphoresKHR;

}
