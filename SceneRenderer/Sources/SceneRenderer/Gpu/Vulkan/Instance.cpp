module;

#include <rstd/macro.hpp>
#include <cstdio>
#include "vvk/macros.hpp"

module sr.vulkan;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;

using namespace sr::vulkan;

constexpr std::array<InstanceLayer, 0> base_inst_layers {};

// MoltenVK is reported as a "portability" driver. The loader will hide
// it from vkEnumeratePhysicalDevices unless we both:
//   1) enable VK_KHR_portability_enumeration at instance create time, AND
//   2) pass VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR (handled in
//      vvk::Instance::Create in vulkan_wrapper.cpp).
// The extension-name macros from vulkan_core.h aren't re-exported by the
// `sr.vulkan` module, so use the bare strings here.
constexpr std::array base_inst_exts {
    Extension { true,  VK_EXT_DEBUG_UTILS_EXTENSION_NAME },
    Extension { true,  "VK_KHR_portability_enumeration" },
    Extension { false, "VK_KHR_get_physical_device_properties2" },
};

namespace
{

VkBool32 DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                     VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                     void* /*pUserData*/) {
    VkBool32 result = VK_FALSE;
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        result |= VK_TRUE;
        // stderr (unbuffered) so messages survive an abort() right after.
        // printf to stdout was line-buffered and eaten on assertion failure.
        rstd_error("validation layer: {}", pCallbackData->pMessage);
    } else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rstd_warn("validation layer: {}", pCallbackData->pMessage);
    }
    return result;
}

vvk::DebugUtilsMessenger SetupDebugCallback(vvk::Instance& instance) {
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext           = nullptr,
        .flags           = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugUtilsMessengerCallback,
        .pUserData       = nullptr,
    });
}

VkResult CreatInstance(vvk::Instance* inst, std::span<const std::string_view> exts,
                       std::span<const std::string_view> layers, vvk::InstanceDispatch& dld) {
    VkApplicationInfo app_info {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = SCENERENDERER_APPLICATION_NAME,
        .applicationVersion = SCENERENDERER_VULKAN_VERSION,
        .pEngineName        = "vulkan",
        .apiVersion         = SCENERENDERER_VULKAN_VERSION,
    };

    std::vector<const char*> extension_names_c;
    std::transform(exts.begin(), exts.end(), std::back_inserter(extension_names_c), [](auto& ext) {
        return ext.data();
    });

    std::vector<const char*> layer_names_c;
    std::transform(
        layers.begin(), layers.end(), std::back_inserter(layer_names_c), [](auto& layer) {
            return layer.data();
        });

    return vvk::Instance::Create(*inst, app_info, layer_names_c, extension_names_c, dld);
}
void EnumateExts(sr::Set<std::string>& set, const vvk::InstanceDispatch& dld) {
    if (auto rv = vvk::EnumerateInstanceExtensionProperties(dld); rv.has_value()) {
        for (const auto& ext : *rv) set.insert(ext.extensionName);
    }
}
void EnumateLayers(sr::Set<std::string>& set, const vvk::InstanceDispatch& dld) {
    if (auto rv = vvk::EnumerateInstanceLayerProperties(dld); rv.has_value()) {
        for (const auto& ext : *rv) set.insert(ext.layerName);
    }
}
} // namespace

bool Instance::ChoosePhysicalDevice(const CheckGpuOp&             checkgpu,
                                    std::span<const std::uint8_t> uuid) {
    auto deviceList = m_vinst.EnumeratePhysicalDevices();

    auto logGpu = [](const VkPhysicalDeviceProperties& props) {
        rstd_info("vulkan device: {}", props.deviceName);
    };

    vvk::PhysicalDevice        final_gpu;
    VkPhysicalDeviceProperties final_props;

    for (const auto& d : deviceList) {
        VkPhysicalDeviceIDProperties device_id_props {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = NULL
        };
        VkPhysicalDeviceProperties2 props2 { .sType =
                                                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &device_id_props };
        d.GetProperties2KHR(props2);
        auto& props = props2.properties;
        if (uuid.size() > 0) {
            decltype(uuid) device_uuid { device_id_props.deviceUUID };
            if (std::equal(uuid.begin(), uuid.end(), device_uuid.begin(), device_uuid.end())) {
                final_props = props;
                final_gpu   = d;
                break;
            }
        } else {
            if (checkgpu(d)) {
                final_props = props;
                final_gpu   = d;
                break;
            }
        }
    }
    if (final_gpu) {
        logGpu(final_props);
        m_gpu = final_gpu;
        return true;
    } else {
        rstd_error("failed to find GPU with vulkan support");
        return false;
    }
}

const vvk::Instance&       Instance::inst() const { return m_vinst; }
const vvk::PhysicalDevice& Instance::gpu() const { return m_gpu; }
const vvk::SurfaceKHR&     Instance::surface() const { return m_surface; }

bool Instance::offscreen() const { return ! m_surface; }

void Instance::setSurface(VkSurfaceKHR sf) {
    m_surface = vvk::SurfaceKHR(sf, *m_vinst, m_vinst.Dispatch());
}

bool Instance::supportExt(std::string_view name) const { return exists(m_extensions, name); }
bool Instance::supportLayer(std::string_view name) const { return exists(m_layers, name); }

void Instance::Destroy() {}

bool Instance::Create(Instance& inst, std::span<const Extension> instExts,
                      std::span<const InstanceLayer> instLayers) {
    vvk::LoadLibrary(inst.m_vklib, inst.m_dld);
    vvk::Load(inst.m_dld);

    EnumateExts(inst.m_extensions, inst.m_dld);
    Set<std::string> exts, layers;
    std::array       test_exts_array { std::span<const Extension>(base_inst_exts), instExts };
    for (auto& test_exts : test_exts_array) {
        for (auto& ext : test_exts) {
            bool ok = inst.supportExt(ext.name);
            if (ok) exts.insert(std::string(ext.name));
            if (ext.required && ! ok) {
                rstd_error("required vulkan instance extension \"{}\" is not supported", ext.name);
                return false;
            }
        }
    }

    EnumateLayers(inst.m_layers, inst.m_dld);
    std::array test_layers_array { std::span<const InstanceLayer>(base_inst_layers), instLayers };
    for (auto& test_layers : test_layers_array) {
        for (auto& layer : test_layers) {
            bool ok = inst.supportLayer(layer.name);
            if (ok) layers.insert(std::string(layer.name));
            if (layer.required && ! ok) {
                rstd_error("required vulkan instance layer \"{}\" is not supported", layer.name);
                return false;
            }
        }
    }

    std::vector<std::string_view> exts_vec { exts.begin(), exts.end() },
        layers_vec { layers.begin(), layers.end() };

    VVK_CHECK_BOOL_RE(CreatInstance(&inst.m_vinst, exts_vec, layers_vec, inst.m_dld));
    vvk::Load(*inst.m_vinst, inst.m_dld);

    inst.m_debug_utils = SetupDebugCallback(inst.m_vinst);
    return true;
}
