#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include "macOS/MacDesktopHost.h"
#endif

namespace sr::host {
struct DesktopConfig {
    const char* title { nullptr };
    std::uint32_t input_hz { 60 };
    std::uint32_t screen_index { 0 };
    std::uint32_t display_id { 0 };
    bool deferred_show { false };
    std::string output_id;
};
struct DesktopCallbacks {
    void (*mouse_move)(double, double, void*) { nullptr };
    void (*mouse_button)(int, int, void*) { nullptr };
    void (*mouse_enter)(int, void*) { nullptr };
    void (*closed)(void*) { nullptr };
    void (*redraw_requested)(void*) { nullptr };
    void (*first_frame_presented)(void*) { nullptr };
    void (*activated)(void*) { nullptr };
    void (*activation_failed)(void*) { nullptr };
    void (*deactivated)(void*) { nullptr };
    void* userdata { nullptr };
};
struct DesktopSurfaceInfo {
    std::vector<std::string> instance_extensions;
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> create_surface;
};
#if defined(__APPLE__)
inline void* DesktopCreate(const DesktopConfig* config, DesktopCallbacks callbacks) {
    SceneRendererMacDesktopConfig native {
        .title = config->title, .input_hz = config->input_hz,
        .screen_index = config->screen_index, .display_id = config->display_id,
        .deferred_show = config->deferred_show,
    };
    SceneRendererMacDesktopCallbacks cb {
        .mouse_move = callbacks.mouse_move, .mouse_button = callbacks.mouse_button,
        .mouse_enter = callbacks.mouse_enter, .closed = callbacks.closed,
        .redraw_requested = callbacks.redraw_requested,
        .first_frame_presented = callbacks.first_frame_presented,
        .activated = callbacks.activated, .activation_failed = callbacks.activation_failed,
        .deactivated = callbacks.deactivated, .userdata = callbacks.userdata,
    };
    return SceneRendererMacDesktopCreate(&native, cb);
}
inline void DesktopDestroy(void* p) { SceneRendererMacDesktopDestroy(p); }
inline int DesktopRun(void* p) { return SceneRendererMacDesktopRun(p); }
inline void DesktopStop(void* p) { SceneRendererMacDesktopStop(p); }
inline void DesktopWake(void* p) { SceneRendererMacDesktopWake(p); }
inline void DesktopActivate(void* p) { SceneRendererMacDesktopActivate(p); }
inline void DesktopDeactivate(void* p) { SceneRendererMacDesktopDeactivate(p); }
inline void DesktopSetPaused(void* p, bool paused) { SceneRendererMacDesktopSetPaused(p, paused); }
inline std::uint32_t DesktopPixelWidth(void* p) { return SceneRendererMacDesktopPixelWidth(p); }
inline std::uint32_t DesktopPixelHeight(void* p) { return SceneRendererMacDesktopPixelHeight(p); }
inline void* DesktopMetalLayer(void* p) { return SceneRendererMacDesktopMetalLayer(p); }
inline bool DesktopPrepareMetalFX(void* p) { return SceneRendererMacDesktopPrepareMetalFX(p); }
inline void DesktopPresentMetalFrame(void* p, void* texture, void* queue, std::uint32_t width, std::uint32_t height) {
    SceneRendererMacDesktopPresentMetalFrame(p, texture, queue, width, height);
}
#elif defined(_WIN32)
void* DesktopCreate(const DesktopConfig*, DesktopCallbacks);
void DesktopDestroy(void*);
int DesktopRun(void*);
void DesktopStop(void*);
void DesktopWake(void*);
void DesktopActivate(void*);
void DesktopDeactivate(void*);
std::uint32_t DesktopPixelWidth(void*);
std::uint32_t DesktopPixelHeight(void*);
bool DesktopGetSurfaceInfo(void*, DesktopSurfaceInfo&);
void DesktopFramePresented(void*);
void DesktopStartParentGuard(void*);
bool DesktopPumpPending(void*);
#endif
} // namespace sr::host
