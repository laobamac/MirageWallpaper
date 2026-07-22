#pragma once

// ---------------------------------------------------------------------------
// WinDesktopHost.h
//
// Windows C API for the SceneRenderer desktop wallpaper host.
// Mirrors MacDesktopHost.h with the same struct/function signatures,
// except Present which uses RGBA pixel data instead of a Metal texture.
//
// In the entry point, prefer these aliases for clarity; the underlying
// symbols are the same (linker resolves via the WinDesktopHost.cpp object).
// ---------------------------------------------------------------------------

#include <cstdint>

extern "C" {

struct SceneRendererMacDesktopConfig {
    const char*     title;
    std::uint32_t   input_hz;
    std::uint32_t   screen_index; // 0 = primary monitor
};

struct SceneRendererMacDesktopCallbacks {
    void (*mouse_move)(double x, double y, void* userdata);
    void (*mouse_button)(int button, int down, void* userdata);
    void (*mouse_enter)(int entered, void* userdata);
    void (*closed)(void* userdata);
    void* userdata;
};

// --- Lifecycle ---

void* SceneRendererMacDesktopCreate(
    const SceneRendererMacDesktopConfig* config,
    SceneRendererMacDesktopCallbacks      callbacks);

void  SceneRendererMacDesktopDestroy(void* handle);

// Run the message pump. Blocks until Stop() or window close.
// Returns 0 on clean exit.
int   SceneRendererMacDesktopRun(void* handle);

// Signal the run loop to exit (thread-safe).
void  SceneRendererMacDesktopStop(void* handle);

// Wake the message pump to process a pending redraw (thread-safe).
void  SceneRendererMacDesktopWake(void* handle);

// --- Present (Windows-specific signature) ---
//
// Present RGBA pixel data from the engine's CPU readback callback.
// rgba is 8-bit-per-channel, row-major, top-left origin.
// width/height describe the source image; the host scales to fill the window.
void  SceneRendererMacDesktopPresent(void* handle,
                                     const std::uint8_t* rgba,
                                     std::uint32_t width,
                                     std::uint32_t height);

// --- Queries ---

std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle);
std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle);

// --- Convenience aliases (use in Windows entry point for clarity) ---
#define SceneRendererWinDesktopConfig     SceneRendererMacDesktopConfig
#define SceneRendererWinDesktopCallbacks  SceneRendererMacDesktopCallbacks
#define SceneRendererWinDesktopCreate     SceneRendererMacDesktopCreate
#define SceneRendererWinDesktopDestroy    SceneRendererMacDesktopDestroy
#define SceneRendererWinDesktopRun        SceneRendererMacDesktopRun
#define SceneRendererWinDesktopStop       SceneRendererMacDesktopStop
#define SceneRendererWinDesktopWake       SceneRendererMacDesktopWake
#define SceneRendererWinDesktopPresent    SceneRendererMacDesktopPresent
#define SceneRendererWinDesktopPixelWidth  SceneRendererMacDesktopPixelWidth
#define SceneRendererWinDesktopPixelHeight SceneRendererMacDesktopPixelHeight

} // extern "C"
