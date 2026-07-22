#include "WinWallpaperWindow.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sr::win32 {

// -----------------------------------------------------------------------------
// Construction / Destruction
// -----------------------------------------------------------------------------

WinWallpaperWindow::WinWallpaperWindow()  = default;
WinWallpaperWindow::~WinWallpaperWindow() { Destroy(); }

// -----------------------------------------------------------------------------
// Create — register class, find WorkerW, create popup window, start input poll
// -----------------------------------------------------------------------------

bool WinWallpaperWindow::Create(const wchar_t* title,
                                std::uint32_t screen_index,
                                std::uint32_t input_hz,
                                WinWallpaperCallbacks callbacks) {
    m_hinstance     = GetModuleHandleW(nullptr);
    m_input_hz      = (std::max)(input_hz, 1u);
    m_screen_index  = screen_index;
    m_callbacks     = callbacks;

    // --- Select target monitor ---
    if (!SelectMonitor(screen_index)) {
        return false;
    }

    // --- Register window class ---
    m_class_name = L"SceneRendererWallpaper_";
    m_class_name += std::to_wstring(reinterpret_cast<std::uintptr_t>(this));

    WNDCLASSEXW wc  = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hinstance;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW = 32512
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_DESKTOP + 1);
    wc.lpszClassName = m_class_name.c_str();

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // --- Create popup window covering the monitor ---
    int x      = m_monitor_rect.left;
    int y      = m_monitor_rect.top;
    int width  = m_monitor_rect.right  - m_monitor_rect.left;
    int height = m_monitor_rect.bottom - m_monitor_rect.top;

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        m_class_name.c_str(), title,
        WS_POPUP,
        x, y, width, height,
        nullptr, nullptr, m_hinstance, this);

    if (m_hwnd == nullptr) {
        UnregisterClassW(m_class_name.c_str(), m_hinstance);
        return false;
    }

    UpdatePixelSize();

    // --- Embed into WorkerW chain (behind desktop icons) ---
    EmbedIntoWorkerW();

    // --- Show window ---
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // --- Start input polling thread ---
    StartInputPolling();

    return true;
}

void WinWallpaperWindow::Destroy() {
    StopInputPolling();

    if (m_hwnd != nullptr) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (!m_class_name.empty()) {
        UnregisterClassW(m_class_name.c_str(), m_hinstance);
        m_class_name.clear();
    }
}

// -----------------------------------------------------------------------------
// Run / Stop / Wake — message pump
// -----------------------------------------------------------------------------

int WinWallpaperWindow::Run() {
    m_running = true;

    // Fire initial mouse-enter to tell the engine the cursor state.
    // (Desktop icons layer doesn't reliably send WM_MOUSELEAVE on start.)
    if (m_callbacks.mouse_enter) {
        m_callbacks.mouse_enter(0, m_callbacks.userdata);
    }

    MSG msg {};
    while (m_running) {
        // Use MsgWaitForMultipleObjects for a hybrid wait that can be woken
        // by Wake() (via PostMessage).
        DWORD result = MsgWaitForMultipleObjectsEx(
            0, nullptr,
            static_cast<DWORD>(std::chrono::milliseconds(1000 / 60).count()),
            QS_ALLINPUT,
            MWMO_ALERTABLE);

        if (result == WAIT_OBJECT_0) {
            // Process all pending messages.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    m_running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        // result == WAIT_TIMEOUT → normal idle, loop back.
    }
    return 0;
}

void WinWallpaperWindow::Stop() {
    m_running = false;
    if (m_hwnd != nullptr) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

void WinWallpaperWindow::Wake() {
    if (m_hwnd != nullptr) {
        PostMessageW(m_hwnd, WM_USER, 0, 0); // harmless nudge
    }
}

// -----------------------------------------------------------------------------
// Window Procedure
// -----------------------------------------------------------------------------

LRESULT CALLBACK WinWallpaperWindow::WndProc(HWND hwnd, UINT msg,
                                              WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WinWallpaperWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self == nullptr && msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<WinWallpaperWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT WinWallpaperWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        if (m_callbacks.closed) {
            m_callbacks.closed(m_callbacks.userdata);
        }
        Stop();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_DISPLAYCHANGE:
        // Monitor configuration changed — recalculate pixel size.
        UpdatePixelSize();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        int  button = 0;
        bool down   = false;
        switch (msg) {
        case WM_LBUTTONDOWN: button = 0; down = true;  break;
        case WM_LBUTTONUP:   button = 0; down = false; break;
        case WM_RBUTTONDOWN: button = 1; down = true;  break;
        case WM_RBUTTONUP:   button = 1; down = false; break;
        case WM_MBUTTONDOWN: button = 2; down = true;  break;
        case WM_MBUTTONUP:   button = 2; down = false; break;
        }
        if (m_callbacks.mouse_button) {
            m_callbacks.mouse_button(button, down ? 1 : 0, m_callbacks.userdata);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        // Mouse-move events are handled by the polling thread for
        // normalized-coordinate accuracy. Ignore raw WM_MOUSEMOVE.
        return 0;

    case WM_MOUSELEAVE:
        m_cursor_inside = false;
        if (m_callbacks.mouse_enter) {
            m_callbacks.mouse_enter(0, m_callbacks.userdata);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
// WorkerW embedding
// -----------------------------------------------------------------------------

// WorkerW is a layered window chain: Progman → SHELLDLL_DefView → WorkerW.
// Wallpaper engines insert their own window between SHELLDLL_DefView and
// WorkerW so it sits on the wallpaper layer, behind icons.

BOOL CALLBACK WinWallpaperWindow::FindWorkerWEnum(HWND hwnd, LPARAM lparam) {
    auto* self = reinterpret_cast<WinWallpaperWindow*>(lparam);

    // Check if this WorkerW has a SHELLDLL_DefView child — that's the one
    // we want (the "real" desktop layer).
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView != nullptr) {
        self->m_worker_w = hwnd;
        return FALSE; // stop enumeration
    }
    return TRUE; // continue
}

HWND WinWallpaperWindow::FindWorkerW() {
    // Classic technique: send a message to Progman to spawn WorkerW, then
    // enumerate to find it.
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman != nullptr) {
        // 0x052C = undocumented message that tells Progman to create WorkerW.
        SendMessageTimeoutW(progman, 0x052C, 0, 0,
                             SMTO_NORMAL, 1000, nullptr);
    }

    m_worker_w = nullptr;
    EnumWindows(FindWorkerWEnum, reinterpret_cast<LPARAM>(this));

    // Fallback: if no WorkerW found (e.g. Win11 24H2 changed the chain),
    // try parenting to Progman directly.
    if (m_worker_w == nullptr && progman != nullptr) {
        m_worker_w = progman;
    }

    return m_worker_w;
}

bool WinWallpaperWindow::EmbedIntoWorkerW() {
    HWND worker = FindWorkerW();
    if (worker == nullptr) {
        return false; // cannot embed — window will stay on top
    }

    SetParent(m_hwnd, worker);

    // Set WS_EX_LAYERED to ensure the window participates in desktop layering.
    SetWindowLongW(m_hwnd, GWL_EXSTYLE,
                   GetWindowLongW(m_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);

    // Position at the bottom of the WorkerW z-order.
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    return true;
}

// -----------------------------------------------------------------------------
// Input polling
// -----------------------------------------------------------------------------

static DWORD WINAPI InputPollThread(LPVOID param) {
    auto* self = static_cast<WinWallpaperWindow*>(param);
    self->PollMousePosition(); // will loop until m_input_running == false
    return 0;
}

void WinWallpaperWindow::StartInputPolling() {
    m_input_running = true;
    m_input_thread  = CreateThread(
        nullptr, 0, InputPollThread, this, 0, nullptr);
}

void WinWallpaperWindow::StopInputPolling() {
    m_input_running = false;
    if (m_input_thread != nullptr) {
        WaitForSingleObject(m_input_thread, 2000);
        CloseHandle(m_input_thread);
        m_input_thread = nullptr;
    }
}

void WinWallpaperWindow::PollMousePosition() {
    const auto interval = std::chrono::milliseconds(1000 / m_input_hz);

    while (m_input_running) {
        POINT pt {};
        GetCursorPos(&pt);

        // ScreenToClient needs the HWND; window may be destroyed.
        if (m_hwnd != nullptr && ScreenToClient(m_hwnd, &pt)) {
            RECT client {};
            GetClientRect(m_hwnd, &client);

            bool inside = (pt.x >= 0 && pt.y >= 0 &&
                           pt.x < client.right && pt.y < client.bottom);

            // Normalized coordinates [0, 1]
            if (inside) {
                double nx = static_cast<double>(pt.x) /
                            static_cast<double>((std::max)(client.right, 1L));
                double ny = static_cast<double>(pt.y) /
                            static_cast<double>((std::max)(client.bottom, 1L));

                if (m_callbacks.mouse_move) {
                    m_callbacks.mouse_move(nx, ny, m_callbacks.userdata);
                }
            }

            // Track enter/leave
            if (inside && !m_cursor_inside) {
                m_cursor_inside = true;
                if (m_callbacks.mouse_enter) {
                    m_callbacks.mouse_enter(1, m_callbacks.userdata);
                }
            } else if (!inside && m_cursor_inside) {
                m_cursor_inside = false;
                if (m_callbacks.mouse_enter) {
                    m_callbacks.mouse_enter(0, m_callbacks.userdata);
                }
            }
        }

        std::this_thread::sleep_for(interval);
    }
}

// -----------------------------------------------------------------------------
// Display helpers
// -----------------------------------------------------------------------------

bool WinWallpaperWindow::SelectMonitor(std::uint32_t screen_index) {
    struct Ctx {
        std::uint32_t index;
        std::uint32_t current;
        RECT          rect {};
        bool          found;
    };

    Ctx ctx { screen_index, 0, {}, false };

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rc, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (c->current == c->index) {
                MONITORINFOEXW mi = { sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST),
                                    &mi)) {
                    c->rect  = mi.rcMonitor;
                    c->found = true;
                }
                return FALSE; // stop
            }
            c->current++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found) {
        // Fallback: use primary monitor
        m_monitor_rect.left   = 0;
        m_monitor_rect.top    = 0;
        m_monitor_rect.right  = GetSystemMetrics(SM_CXSCREEN);
        m_monitor_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    } else {
        m_monitor_rect = ctx.rect;
    }

    return true;
}

void WinWallpaperWindow::UpdatePixelSize() {
    RECT client {};
    if (m_hwnd != nullptr && GetClientRect(m_hwnd, &client)) {
        m_pixel_width  = static_cast<std::uint32_t>(client.right  - client.left);
        m_pixel_height = static_cast<std::uint32_t>(client.bottom - client.top);
    } else {
        // Fallback: monitor resolution
        m_pixel_width  = static_cast<std::uint32_t>(
            m_monitor_rect.right  - m_monitor_rect.left);
        m_pixel_height = static_cast<std::uint32_t>(
            m_monitor_rect.bottom - m_monitor_rect.top);
    }
}

} // namespace sr::win32
