#include <windows.h>
#include <tlhelp32.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "DesktopHost.h"
#include "../../../../Platform/Windows/DesktopHost.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <thread>
#include <vector>

namespace sr::host {
namespace {
constexpr UINT kStop = WM_APP + 1, kWake = WM_APP + 2, kActivate = WM_APP + 3;
constexpr UINT kDeactivate = WM_APP + 4, kPresented = WM_APP + 5;
constexpr UINT_PTR kInputTimer = 1, kDesktopTimer = 2, kActivationTimer = 3;
constexpr wchar_t kWindowClass[] = L"MirageSceneWallpaper";

struct Monitor {
    HMONITOR handle;
    MONITORINFOEXW info;
};
std::vector<Monitor> Monitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info))
            reinterpret_cast<std::vector<Monitor>*>(data)->push_back({monitor, info});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    std::stable_sort(monitors.begin(), monitors.end(), [](const auto& a, const auto& b) {
        return (a.info.dwFlags & MONITORINFOF_PRIMARY) > (b.info.dwFlags & MONITORINFOF_PRIMARY);
    });
    return monitors;
}

struct Host {
    HWND window { nullptr };
    DesktopConfig config;
    DesktopCallbacks callbacks;
    RECT bounds {};
    std::atomic<std::uint32_t> width { 0 }, height { 0 };
    std::atomic<bool> stopped { false }, frame_message_pending { false };
    std::atomic<std::uint64_t> presented { 0 };
    std::uint64_t activation_frame { 0 };
    bool first_frame { false }, active { false }, activation_pending { false };
    bool inside { false }, last_position_valid { false }, failed { false }, closing { false };
    DWORD last_attach_error = ERROR_SUCCESS;
    POINT last_position {};
    std::array<bool, 3> buttons {};
    HANDLE guard_cancel { nullptr }, parent { nullptr };
    std::thread guard;

    ~Host() {
        if (guard_cancel) SetEvent(guard_cancel);
        if (guard.joinable()) guard.join();
        if (parent) CloseHandle(parent);
        if (guard_cancel) CloseHandle(guard_cancel);
        closing = true;
        if (IsWindow(window)) DestroyWindow(window);
    }
    void call(void (*callback)(void*)) { if (callback) callback(callbacks.userdata); }
    bool selectMonitor() {
        const auto monitors = Monitors();
        const Monitor* selected = nullptr;
        if (!config.output_id.empty()) {
            const auto name = Mirage::Windows::utf8ToWide(config.output_id);
            for (const auto& monitor : monitors) {
                if (name == monitor.info.szDevice) { selected = &monitor; break; }
            }
        } else if (config.screen_index < monitors.size()) selected = &monitors[config.screen_index];
        if (!selected) return false;
        bounds = selected->info.rcMonitor;
        width.store(static_cast<std::uint32_t>(bounds.right - bounds.left));
        height.store(static_cast<std::uint32_t>(bounds.bottom - bounds.top));
        return width.load() > 0 && height.load() > 0;
    }
    bool attach() {
        DWORD error = 0;
        const bool ok = Mirage::Windows::attachToDesktop(window, bounds, &error, active);
        if (!active) ShowWindow(window, SW_HIDE);
        if (!ok && error != last_attach_error) {
            std::fprintf(stderr, "SceneWallpaper: desktop attach failed (%lu)\n", error);
            last_attach_error = error;
        } else if (ok) {
            last_attach_error = ERROR_SUCCESS;
        }
        return ok;
    }
    void activate() {
        active = false;
        ShowWindow(window, SW_HIDE);
        if (!selectMonitor() || !attach()) {
            call(callbacks.activation_failed);
            return;
        }
        activation_pending = true;
        activation_frame = presented.load() + 1U;
        SetTimer(window, kActivationTimer, 4000, nullptr);
        call(callbacks.redraw_requested);
    }
    void framePresented() {
        frame_message_pending.store(false);
        if (!first_frame) {
            first_frame = true;
            call(callbacks.first_frame_presented);
        }
        if (activation_pending && presented.load() >= activation_frame) {
            KillTimer(window, kActivationTimer);
            activation_pending = false;
            if (!IsWindow(GetParent(window))) { call(callbacks.activation_failed); return; }
            ShowWindow(window, SW_SHOWNOACTIVATE);
            active = IsWindowVisible(window) != FALSE;
            call(active ? callbacks.activated : callbacks.activation_failed);
        }
    }
    void pollInput() {
        POINT position {};
        if (!GetCursorPos(&position)) return;
        const HWND pointed = WindowFromPoint(position);
        const HWND root = GetAncestor(pointed, GA_ROOT);
        wchar_t root_class[64] {};
        GetClassNameW(root, root_class, 64);
        const bool desktop_point = root == GetAncestor(window, GA_ROOT) ||
            std::wcscmp(root_class, L"WorkerW") == 0 || std::wcscmp(root_class, L"Progman") == 0;
        const bool entered = active && PtInRect(&bounds, position) && desktop_point;
        bool changed = entered != inside;
        if (changed && callbacks.mouse_enter) callbacks.mouse_enter(entered ? 1 : 0, callbacks.userdata);
        inside = entered;
        if (entered && (!last_position_valid || position.x != last_position.x || position.y != last_position.y)) {
            if (callbacks.mouse_move) callbacks.mouse_move(
                static_cast<double>(position.x - bounds.left) / width.load(),
                static_cast<double>(position.y - bounds.top) / height.load(), callbacks.userdata);
            changed = true;
        }
        last_position = position;
        last_position_valid = true;
        constexpr std::array keys { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const bool down = entered && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (buttons[i] != down) {
                buttons[i] = down;
                if (callbacks.mouse_button) callbacks.mouse_button(static_cast<int>(i), down ? 1 : 0, callbacks.userdata);
                changed = true;
            }
        }
        if (changed) call(callbacks.redraw_requested);
    }
    void reconfigure(bool force = false) {
        const RECT old_bounds = bounds;
        if (!selectMonitor()) { failed = true; call(callbacks.closed); DesktopStop(this); return; }
        const HWND worker = Mirage::Windows::desktopWorker();
        if (!worker) return;
        const HWND parent_window = GetParent(window);
        if (force || !IsWindow(parent_window) || parent_window != worker ||
            std::memcmp(&bounds, &old_bounds, sizeof(RECT)) != 0) {
            if (!attach()) {
                if (last_attach_error == ERROR_NOT_FOUND ||
                    (last_attach_error == ERROR_INVALID_WINDOW_HANDLE && IsWindow(window))) return;
                failed = true;
                call(callbacks.closed);
                DesktopStop(this);
                return;
            }
            call(callbacks.redraw_requested);
        }
    }
    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case kStop: stopped.store(true); PostQuitMessage(0); return 0;
        case kWake: return 0;
        case kActivate: activate(); return 0;
        case kDeactivate:
            activation_pending = false;
            KillTimer(window, kActivationTimer);
            active = false;
            ShowWindow(window, SW_HIDE);
            call(callbacks.deactivated);
            return 0;
        case kPresented: framePresented(); return 0;
        case WM_TIMER:
            if (wp == kInputTimer) pollInput();
            else if (wp == kDesktopTimer) reconfigure();
            else if (wp == kActivationTimer) {
                KillTimer(window, kActivationTimer);
                activation_pending = false;
                ShowWindow(window, SW_HIDE);
                call(callbacks.activation_failed);
            }
            return 0;
        case WM_DISPLAYCHANGE: case WM_DPICHANGED: reconfigure(true); return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                RECT client {};
                if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
                    width.store(static_cast<std::uint32_t>(client.right));
                    height.store(static_cast<std::uint32_t>(client.bottom));
                    call(callbacks.redraw_requested);
                }
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint {}; BeginPaint(window, &paint); EndPaint(window, &paint);
            call(callbacks.redraw_requested);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_CLOSE: DesktopStop(this); return 0;
        case WM_DESTROY:
            if (!closing && !stopped.load()) { failed = true; call(callbacks.closed); }
            stopped.store(true); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    return host ? host->message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}
} // namespace

void* DesktopCreate(const DesktopConfig* config, DesktopCallbacks callbacks) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW type {};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kWindowClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    auto host = std::make_unique<Host>();
    if (config) host->config = *config;
    host->callbacks = callbacks;
    if (!host->selectMonitor()) return nullptr;
    const auto title = Mirage::Windows::utf8ToWide(host->config.title ? host->config.title : "SceneWallpaper");
    host->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, title.c_str(), WS_POPUP,
        host->bounds.left, host->bounds.top, static_cast<int>(host->width.load()),
        static_cast<int>(host->height.load()), nullptr, nullptr, type.hInstance, host.get());
    if (!host->window) return nullptr;
    if (!host->attach() && Mirage::Windows::desktopWorker()) return nullptr;
    SetTimer(host->window, kInputTimer, std::max(1U, 1000U / std::clamp(host->config.input_hz, 1U, 1000U)), nullptr);
    SetTimer(host->window, kDesktopTimer, 1000, nullptr);
    if (!host->config.deferred_show) {
        host->activation_pending = true;
        host->activation_frame = 1;
    }
    return host.release();
}
void DesktopDestroy(void* handle) { delete static_cast<Host*>(handle); }
bool DesktopPumpPending(void* handle) {
    auto* host = static_cast<Host*>(handle);
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { host->stopped.store(true); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return !host->stopped.load();
}
int DesktopRun(void* handle) {
    auto* host = static_cast<Host*>(handle);
    MSG message {};
    while (!host->stopped.load()) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result < 0) { host->failed = true; break; }
        if (result == 0) break;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return host->failed ? 0 : 1;
}
void DesktopStop(void* handle) {
    if (auto* host = static_cast<Host*>(handle)) {
        host->stopped.store(true);
        PostMessageW(host->window, kStop, 0, 0);
    }
}
void DesktopWake(void* handle) {
    if (auto* host = static_cast<Host*>(handle)) PostMessageW(host->window, kWake, 0, 0);
}
void DesktopActivate(void* handle) {
    if (auto* host = static_cast<Host*>(handle)) PostMessageW(host->window, kActivate, 0, 0);
}
void DesktopDeactivate(void* handle) {
    if (auto* host = static_cast<Host*>(handle)) PostMessageW(host->window, kDeactivate, 0, 0);
}
std::uint32_t DesktopPixelWidth(void* handle) { return static_cast<Host*>(handle)->width.load(); }
std::uint32_t DesktopPixelHeight(void* handle) { return static_cast<Host*>(handle)->height.load(); }
void DesktopFramePresented(void* handle) {
    auto* host = static_cast<Host*>(handle);
    host->presented.fetch_add(1);
    if (!host->frame_message_pending.exchange(true)) PostMessageW(host->window, kPresented, 0, 0);
}
bool DesktopGetSurfaceInfo(void* handle, DesktopSurfaceInfo& result) {
    auto* host = static_cast<Host*>(handle);
    if (!host || !IsWindow(host->window)) return false;
    result.instance_extensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    result.create_surface = [window = host->window](VkInstance instance, VkSurfaceKHR* surface) {
        const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
        if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkWin32SurfaceCreateInfoKHR info {};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = window;
        return create(instance, &info, nullptr, surface);
    };
    return true;
}
void DesktopStartParentGuard(void* handle) {
    auto* host = static_cast<Host*>(handle);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W process {};
    process.dwSize = sizeof(process);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &process)) {
        do {
            if (process.th32ProcessID == GetCurrentProcessId()) { parent = process.th32ParentProcessID; break; }
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    if (!parent) return;
    host->parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (!host->parent) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) DesktopStop(host);
        return;
    }
    FILETIME parent_created {}, child_created {}, exited {}, kernel {}, user {};
    if (GetProcessTimes(host->parent, &parent_created, &exited, &kernel, &user) &&
        GetProcessTimes(GetCurrentProcess(), &child_created, &exited, &kernel, &user) &&
        CompareFileTime(&parent_created, &child_created) > 0) {
        DesktopStop(host);
        return;
    }
    host->guard_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!host->guard_cancel) return;
    host->guard = std::thread([host] {
        HANDLE events[] { host->guard_cancel, host->parent };
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
        DesktopStop(host);
        if (WaitForSingleObject(host->guard_cancel, 2000) == WAIT_TIMEOUT) std::_Exit(0);
    });
}
} // namespace sr::host
