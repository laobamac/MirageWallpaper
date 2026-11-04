#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace Mirage::Windows {

inline std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) return {};
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), count)) return {};
    return result;
}

inline std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count == 0) return {};
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), count, nullptr, nullptr)) return {};
    return result;
}

inline std::wstring screenDevice(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return {};
    return info.szDevice;
}

inline HWND desktopWorker() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(progman, 0x052c, 0, 0, SMTO_ABORTIFHUNG, 1000, &reply)) return nullptr;
    HWND worker = nullptr;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr)) {
            const HWND next = FindWindowExW(nullptr, window, L"WorkerW", nullptr);
            if (next) {
                *reinterpret_cast<HWND*>(data) = next;
                return FALSE;
            }
            const HWND nested = FindWindowExW(window, nullptr, L"WorkerW", nullptr);
            if (nested) {
                *reinterpret_cast<HWND*>(data) = nested;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&worker));
    return worker;
}

// bounds 使用显示器的物理虚拟桌面坐标，调用者须启用 per-monitor DPI awareness。
inline bool attachToDesktop(HWND window, const RECT& bounds, DWORD* error = nullptr, bool show = true) {
    auto failed = [error](DWORD value) {
        if (error) *error = value;
        return false;
    };
    if (!IsWindow(window)) return failed(ERROR_INVALID_WINDOW_HANDLE);
    const HWND worker = desktopWorker();
    if (!worker) return failed(ERROR_NOT_FOUND);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!SetWindowLongPtrW(window, GWL_STYLE,
                          (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD)
        && GetLastError() != ERROR_SUCCESS) return failed(GetLastError());
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!SetWindowLongPtrW(window, GWL_EXSTYLE,
                          (exStyle & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
        && GetLastError() != ERROR_SUCCESS) return failed(GetLastError());
    SetLastError(ERROR_SUCCESS);
    if (!SetParent(window, worker) && GetLastError() != ERROR_SUCCESS) return failed(GetLastError());
    POINT origin{bounds.left, bounds.top};
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(HWND_DESKTOP, worker, &origin, 1)
        && GetLastError() != ERROR_SUCCESS) return failed(GetLastError());
    if (!SetWindowPos(window, HWND_BOTTOM, origin.x, origin.y,
                      bounds.right - bounds.left, bounds.bottom - bounds.top,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED | (show ? SWP_SHOWWINDOW : 0))) return failed(GetLastError());
    if (error) *error = ERROR_SUCCESS;
    return true;
}

} // namespace Mirage::Windows
