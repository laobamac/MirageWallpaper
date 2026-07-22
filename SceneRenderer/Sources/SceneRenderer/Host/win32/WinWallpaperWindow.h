#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <windows.h>

namespace sr::win32 {

// --- Callbacks (mirrors MacDesktopHost.h callbacks) --------------------------

struct WinWallpaperCallbacks {
    void (*mouse_move)(double x, double y, void* userdata)   = nullptr;
    void (*mouse_button)(int button, int down, void* userdata) = nullptr;
    void (*mouse_enter)(int entered, void* userdata)         = nullptr;
    void (*closed)(void* userdata)                           = nullptr;
    void* userdata                                           = nullptr;
};

// --- WorkerW wallpaper window manager ----------------------------------------

// Places a borderless WS_POPUP window behind desktop icons by parenting it
// into the WorkerW chain. Handles display-change, input polling, and provides
// the HWND needed by VK_KHR_win32_surface.
class WinWallpaperWindow {
public:
    WinWallpaperWindow();
    ~WinWallpaperWindow();

    // Non-copyable, non-movable (owns OS window resources).
    WinWallpaperWindow(const WinWallpaperWindow&)            = delete;
    WinWallpaperWindow& operator=(const WinWallpaperWindow&) = delete;
    WinWallpaperWindow(WinWallpaperWindow&&)                 = delete;
    WinWallpaperWindow& operator=(WinWallpaperWindow&&)      = delete;

    // Create the wallpaper window. screen_index selects the target monitor
    // (0 = primary). Returns true on success.
    bool Create(const wchar_t* title, std::uint32_t screen_index,
                std::uint32_t input_hz, WinWallpaperCallbacks callbacks);

    // Destroy the window and release resources.
    void Destroy();

    // Enter the message pump. Blocks until Stop() is called or the window is
    // closed. Returns 0 on clean exit, non-zero on error.
    int Run();

    // Signal the message pump to exit (thread-safe).
    void Stop();

    // Wake the message pump to process a pending redraw (thread-safe).
    void Wake();

    // --- Getters ---
    HWND           Hwnd()          const { return m_hwnd; }
    HINSTANCE      HInstance()     const { return m_hinstance; }
    std::uint32_t  PixelWidth()    const { return m_pixel_width; }
    std::uint32_t  PixelHeight()   const { return m_pixel_height; }
    bool           IsValid()       const { return m_hwnd != nullptr; }

private:
    // --- Window procedure ---
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // --- WorkerW setup ---
    static BOOL CALLBACK FindWorkerWEnum(HWND hwnd, LPARAM lparam);
    HWND FindWorkerW();
    bool EmbedIntoWorkerW();

    // --- Input polling ---
    void StartInputPolling();
    void StopInputPolling();

public:
    void PollMousePosition();  // public for static thread proc access

    // --- Display ---
    bool SelectMonitor(std::uint32_t screen_index);
    void UpdatePixelSize();

    // --- State ---
    HWND               m_hwnd       = nullptr;
    HINSTANCE          m_hinstance  = nullptr;
    HWND               m_worker_w   = nullptr;
    std::wstring       m_class_name;
    std::uint32_t      m_input_hz   = 60;
    std::uint32_t      m_pixel_width  = 0;
    std::uint32_t      m_pixel_height = 0;
    volatile bool      m_running    = false;
    volatile bool      m_wake       = false;

    WinWallpaperCallbacks m_callbacks {};
    std::uint32_t      m_screen_index = 0;
    RECT               m_monitor_rect {};

    // Input polling
    HANDLE             m_input_thread = nullptr;
    volatile bool      m_input_running = false;
    POINT              m_last_cursor {};
    bool               m_cursor_inside = false;
};

} // namespace sr::win32
