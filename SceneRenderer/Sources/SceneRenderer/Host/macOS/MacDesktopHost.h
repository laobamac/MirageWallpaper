#pragma once

#include <cstdint>

extern "C" {

struct SceneRendererMacDesktopConfig {
    const char* title;
    std::uint32_t input_hz;
    std::uint32_t screen_index; // 0 = main screen; N selects NSScreen.screens[N]
    // Keep the window in the compositor, but fully transparent, until
    // SceneRendererMacDesktopActivate(). This allows CAMetalLayer to produce
    // drawables while the previous wallpaper remains visible.
    bool deferred_show;
};

struct SceneRendererMacDesktopCallbacks {
    void (*mouse_move)(double x, double y, void* userdata);
    void (*mouse_button)(int button, int down, void* userdata);
    void (*mouse_enter)(int entered, void* userdata);
    void (*closed)(void* userdata);
    // Fired once, from Metal's drawable-presented callback, after the first
    // real scene frame has reached the presentation path.
    void (*first_frame_presented)(void* userdata);
    // Fired once after activation and a subsequent drawable presentation.
    // The parent can safely retire the previous wallpaper at this point.
    void (*activated)(void* userdata);
    void* userdata;
};

void* SceneRendererMacDesktopCreate(const SceneRendererMacDesktopConfig* config,
                                    SceneRendererMacDesktopCallbacks callbacks);
void  SceneRendererMacDesktopDestroy(void* handle);
int   SceneRendererMacDesktopRun(void* handle);
void  SceneRendererMacDesktopStop(void* handle);
void  SceneRendererMacDesktopWake(void* handle);
void  SceneRendererMacDesktopActivate(void* handle);
// Borrowed CAMetalLayer pointer used to create VK_EXT_metal_surface.
void* SceneRendererMacDesktopMetalLayer(void* handle);
std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle);
std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle);

// NOTE: media-status routing is not exposed through this C host API. The
// runtime entry point is `sr::SceneWallpaper::setMediaStatus(sr::MediaStatus)`,
// which the app layer (owner of the SceneWallpaper) should call from a
// now-playing observer.

} // extern "C"
