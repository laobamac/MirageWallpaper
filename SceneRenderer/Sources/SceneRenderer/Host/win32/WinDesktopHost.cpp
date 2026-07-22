#include "WinDesktopHost.h"
#include "WinWallpaperWindow.h"
#include "WinSwapchainPresenter.h"

#include <memory>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Internal Host structure — bundles the wallpaper window + Vulkan presenter.
// ---------------------------------------------------------------------------

struct WinDesktopHostState {
    sr::win32::WinWallpaperWindow    window;
    sr::win32::WinSwapchainPresenter presenter;
    bool                             valid { false };
};

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------

extern "C" {

void* SceneRendererMacDesktopCreate(
    const SceneRendererMacDesktopConfig* config,
    SceneRendererMacDesktopCallbacks      callbacks) {

    if (config == nullptr) return nullptr;

    auto* host = new WinDesktopHostState();

    // Convert narrow title to wide for Win32 API.
    std::string narrow_title(config->title ? config->title : "SceneRenderer Wallpaper");
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, narrow_title.c_str(), -1, nullptr, 0);
    std::wstring wide_title(wide_len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow_title.c_str(), -1,
                        &wide_title[0], wide_len);

    // Map callbacks.
    sr::win32::WinWallpaperCallbacks wc {};
    wc.mouse_move   = callbacks.mouse_move;
    wc.mouse_button = callbacks.mouse_button;
    wc.mouse_enter  = callbacks.mouse_enter;
    wc.closed       = callbacks.closed;
    wc.userdata     = callbacks.userdata;

    // Create the WorkerW wallpaper window.
    if (!host->window.Create(wide_title.c_str(),
                              config->screen_index,
                              config->input_hz,
                              wc)) {
        delete host;
        return nullptr;
    }

    // Initialize Vulkan presenter on the window.
    // Validation layers off by default; enable via engine's --valid-layer.
    // (The entry point can call a separate setter if needed.)
    if (!host->presenter.Init(host->window.Hwnd(),
                               host->window.HInstance(),
                               host->window.PixelWidth(),
                               host->window.PixelHeight(),
                               false)) {
        host->window.Destroy();
        delete host;
        return nullptr;
    }

    host->valid = true;
    return host;
}

void SceneRendererMacDesktopDestroy(void* handle) {
    if (handle == nullptr) return;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    host->presenter.Destroy();
    host->window.Destroy();
    delete host;
}

int SceneRendererMacDesktopRun(void* handle) {
    if (handle == nullptr) return 1;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    if (!host->valid) return 1;
    return host->window.Run();
}

void SceneRendererMacDesktopStop(void* handle) {
    if (handle == nullptr) return;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    host->window.Stop();
}

void SceneRendererMacDesktopWake(void* handle) {
    if (handle == nullptr) return;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    host->window.Wake();
}

void SceneRendererMacDesktopPresent(void* handle,
                                     const std::uint8_t* rgba,
                                     std::uint32_t width,
                                     std::uint32_t height) {
    if (handle == nullptr || rgba == nullptr) return;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    host->presenter.Present(rgba, width, height);
}

std::uint32_t SceneRendererMacDesktopPixelWidth(void* handle) {
    if (handle == nullptr) return 0;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    return host->window.PixelWidth();
}

std::uint32_t SceneRendererMacDesktopPixelHeight(void* handle) {
    if (handle == nullptr) return 0;
    auto* host = static_cast<WinDesktopHostState*>(handle);
    return host->window.PixelHeight();
}

} // extern "C"
