#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#if defined(__APPLE__)
#include "macOS/MacDesktopHost.h"
#endif

namespace sr::host
{

struct DesktopConfig {
    const char* title { nullptr };
    std::uint32_t input_hz { 60 };
    std::uint32_t screen_index { 0 };
    bool deferred_show { false };
};

struct DesktopCallbacks {
    void (*mouse_move)(double x, double y, void* userdata) { nullptr };
    void (*mouse_button)(int button, int down, void* userdata) { nullptr };
    void (*mouse_enter)(int entered, void* userdata) { nullptr };
    void (*closed)(void* userdata) { nullptr };
    void (*first_frame_presented)(void* userdata) { nullptr };
    void (*activated)(void* userdata) { nullptr };
    void* userdata { nullptr };
};

struct DesktopSurfaceInfo {
    std::vector<std::string> instance_extensions;
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> create_surface;
};

#if defined(__APPLE__)

inline void* DesktopCreate(const DesktopConfig* config, DesktopCallbacks callbacks) {
    SceneRendererMacDesktopConfig mac_config {
        .title        = config != nullptr ? config->title : nullptr,
        .input_hz     = config != nullptr ? config->input_hz : 60,
        .screen_index = config != nullptr ? config->screen_index : 0,
        .deferred_show = config != nullptr ? config->deferred_show : false,
    };
    SceneRendererMacDesktopCallbacks mac_callbacks {
        .mouse_move   = callbacks.mouse_move,
        .mouse_button = callbacks.mouse_button,
        .mouse_enter  = callbacks.mouse_enter,
        .closed       = callbacks.closed,
        .first_frame_presented = callbacks.first_frame_presented,
        .activated    = callbacks.activated,
        .userdata     = callbacks.userdata,
    };
    return SceneRendererMacDesktopCreate(&mac_config, mac_callbacks);
}
inline void DesktopDestroy(void* handle) { SceneRendererMacDesktopDestroy(handle); }
inline int  DesktopRun(void* handle) { return SceneRendererMacDesktopRun(handle); }
inline void DesktopStop(void* handle) { SceneRendererMacDesktopStop(handle); }
inline void DesktopWake(void* handle) { SceneRendererMacDesktopWake(handle); }
inline void DesktopActivate(void* handle) { SceneRendererMacDesktopActivate(handle); }
inline void DesktopPresent(void* handle, void* texture, std::uint32_t width, std::uint32_t height) {
    SceneRendererMacDesktopPresent(handle, texture, width, height);
}
inline std::uint32_t DesktopPixelWidth(void* handle) { return SceneRendererMacDesktopPixelWidth(handle); }
inline std::uint32_t DesktopPixelHeight(void* handle) { return SceneRendererMacDesktopPixelHeight(handle); }
inline bool DesktopGetSurfaceInfo(void*, DesktopSurfaceInfo&) { return false; }

#else

void* DesktopCreate(const DesktopConfig* config, DesktopCallbacks callbacks);
void  DesktopDestroy(void* handle);
int   DesktopRun(void* handle);
void  DesktopStop(void* handle);
void  DesktopWake(void* handle);
void  DesktopActivate(void* handle);
void  DesktopPresent(void* handle, void* texture, std::uint32_t width, std::uint32_t height);
std::uint32_t DesktopPixelWidth(void* handle);
std::uint32_t DesktopPixelHeight(void* handle);
bool DesktopGetSurfaceInfo(void* handle, DesktopSurfaceInfo& out);

#endif

} // namespace sr::host
