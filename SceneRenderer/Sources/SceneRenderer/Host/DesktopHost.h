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
    std::uint32_t display_id { 0 };
    bool deferred_show { false };
};

struct DesktopCallbacks {
    void (*mouse_move)(double x, double y, void* userdata) { nullptr };
    void (*mouse_button)(int button, int down, void* userdata) { nullptr };
    void (*mouse_enter)(int entered, void* userdata) { nullptr };
    void (*closed)(void* userdata) { nullptr };
    // macOS asks the runtime for a frame when an on-demand window becomes
    // visible or changes geometry; protocol hosts leave this callback unset.
    void (*redraw_requested)(void* userdata) { nullptr };
    void (*first_frame_presented)(void* userdata) { nullptr };
    void (*activated)(void* userdata) { nullptr };
    // macOS-only confirmations: activation_failed fires after a failed
    // activation is hidden again; deactivated after the window is confirmed
    // hidden. Linux hosts never call them.
    void (*activation_failed)(void* userdata) { nullptr };
    void (*deactivated)(void* userdata) { nullptr };
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
        .display_id   = config != nullptr ? config->display_id : 0,
        .deferred_show = config != nullptr ? config->deferred_show : false,
    };
    SceneRendererMacDesktopCallbacks mac_callbacks {
        .mouse_move   = callbacks.mouse_move,
        .mouse_button = callbacks.mouse_button,
        .mouse_enter  = callbacks.mouse_enter,
        .closed       = callbacks.closed,
        .redraw_requested = callbacks.redraw_requested,
        .first_frame_presented = callbacks.first_frame_presented,
        .activated    = callbacks.activated,
        .activation_failed = callbacks.activation_failed,
        .deactivated  = callbacks.deactivated,
        .userdata     = callbacks.userdata,
    };
    return SceneRendererMacDesktopCreate(&mac_config, mac_callbacks);
}
inline void DesktopDestroy(void* handle) { SceneRendererMacDesktopDestroy(handle); }
inline int  DesktopRun(void* handle) { return SceneRendererMacDesktopRun(handle); }
inline void DesktopStop(void* handle) { SceneRendererMacDesktopStop(handle); }
inline void DesktopWake(void* handle) { SceneRendererMacDesktopWake(handle); }
inline void DesktopActivate(void* handle) { SceneRendererMacDesktopActivate(handle); }
inline void DesktopDeactivate(void* handle) { SceneRendererMacDesktopDeactivate(handle); }
inline void DesktopSetPaused(void* handle, bool paused) {
    SceneRendererMacDesktopSetPaused(handle, paused);
}
inline void* DesktopMetalLayer(void* handle) { return SceneRendererMacDesktopMetalLayer(handle); }
inline bool DesktopPrepareMetalFX(void* handle) { return SceneRendererMacDesktopPrepareMetalFX(handle); }
inline void DesktopPresentMetalFrame(void* handle, void* texture, void* command_queue,
                                     std::uint32_t source_width, std::uint32_t source_height) {
    SceneRendererMacDesktopPresentMetalFrame(handle, texture, command_queue,
                                             source_width, source_height);
}
inline std::uint32_t DesktopPixelWidth(void* handle) { return SceneRendererMacDesktopPixelWidth(handle); }
inline std::uint32_t DesktopPixelHeight(void* handle) { return SceneRendererMacDesktopPixelHeight(handle); }
inline bool DesktopGetSurfaceInfo(void*, DesktopSurfaceInfo&) { return false; }

// There is no non-Apple Desktop* implementation: Linux wallpapers display
// through the mirage-display protocol (MirageProtocolHost in WallpaperApp.cpp),
// not a native desktop window host.

#endif

} // namespace sr::host
