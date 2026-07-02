#pragma once

#include <cstdint>

extern "C" {

struct SceneRendererMacDesktopConfig {
    const char* title;
    std::uint32_t input_hz;
};

struct SceneRendererMacDesktopCallbacks {
    void (*mouse_move)(double x, double y, void* userdata);
    void (*mouse_button)(int button, int down, void* userdata);
    void (*mouse_enter)(int entered, void* userdata);
    void (*closed)(void* userdata);
    void* userdata;
};

void* SceneRendererMacDesktopCreate(const SceneRendererMacDesktopConfig* config,
                                    SceneRendererMacDesktopCallbacks callbacks);
void  SceneRendererMacDesktopDestroy(void* handle);
int   SceneRendererMacDesktopRun(void* handle);
void  SceneRendererMacDesktopStop(void* handle);
void  SceneRendererMacDesktopWake(void* handle);
void  SceneRendererMacDesktopPresent(void* handle, void* texture, std::uint32_t width,
                                     std::uint32_t height);
std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle);
std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle);

} // extern "C"
