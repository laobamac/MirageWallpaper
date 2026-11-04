#include "WRDesktopHost.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <QWebEngineView>
#include <cmath>
#include <cwchar>

WRDesktopHost::WRDesktopHost(WebRendererEngine* engine) : m_engine(engine) {
    m_repair.setInterval(1000);
    connect(&m_repair, &QTimer::timeout, this, [this] {
        if (!m_engine || !m_engine->view()) return;
        if (const HWND current = refreshWindow()) m_window = current;
        const HWND worker = Mirage::Windows::desktopWorker();
        if (!worker) return;
        if (!IsWindow(m_window) || GetParent(m_window) != worker) {
            QString error;
            if (!positionWindow(&error)) qWarning("WebWallpaper: %s", qPrintable(error));
        }
    });
}

WRDesktopHost::~WRDesktopHost() {
    m_repair.stop();
    if (m_hook) UnhookWindowsHookEx(m_hook);
    if (s_instance == this) s_instance = nullptr;
}

bool WRDesktopHost::attach(int screenIndex, const QString& deviceName, QString* error) {
    const auto screens = QGuiApplication::screens();
    QScreen* screen = nullptr;
    if (!deviceName.isEmpty()) {
        for (auto* candidate : screens) {
            const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
            const QString device = native ? QString::fromStdWString(Mirage::Windows::screenDevice(native->handle())) : QString();
            if (candidate->name().compare(deviceName, Qt::CaseInsensitive) == 0 ||
                device.compare(deviceName, Qt::CaseInsensitive) == 0) { screen = candidate; break; }
        }
    } else if (screenIndex >= 0 && screenIndex < screens.size()) {
        screen = screens.at(screenIndex);
    }
    if (!screen) { if (error) *error = QStringLiteral("requested Windows output does not exist"); return false; }
    const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    if (!native) { if (error) *error = QStringLiteral("Qt Windows screen native interface is unavailable"); return false; }
    m_device = Mirage::Windows::screenDevice(native->handle());
    m_engine->view()->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    m_engine->view()->setGeometry(screen->geometry());
    m_engine->view()->show();
    m_window = reinterpret_cast<HWND>(m_engine->view()->winId());
    if (!positionWindow(error)) return false;
    connect(screen, &QScreen::geometryChanged, this, [this](const QRect&) {
        QString error;
        if (!positionWindow(&error)) qWarning("WebWallpaper: %s", qPrintable(error));
    });
    s_instance = this;
    m_hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    if (!m_hook) {
        s_instance = nullptr;
        if (error) *error = QStringLiteral("desktop pointer hook failed: %1").arg(GetLastError());
        return false;
    }
    m_repair.start();
    return true;
}

HWND WRDesktopHost::refreshWindow() const {
    if (!m_engine || !m_engine->view()) return nullptr;
    auto* handle = m_engine->view()->windowHandle();
    if (!handle) return nullptr;
    const HWND current = reinterpret_cast<HWND>(m_engine->view()->winId());
    return IsWindow(current) ? current : nullptr;
}

bool WRDesktopHost::positionWindow(QString* error) {
    if (!m_engine || !m_engine->view()) {
        if (error) *error = QStringLiteral("QtWebEngine view is unavailable");
        return false;
    }
    // Reparenting into WorkerW can make Qt recreate the native window, so the
    // cached handle must be refreshed before the host is declared dead.
    if (!IsWindow(m_window)) m_window = refreshWindow();
    if (!IsWindow(m_window)) {
        if (error) *error = QStringLiteral("QtWebEngine native window was destroyed; exiting for recovery");
        QCoreApplication::exit(3);
        return false;
    }
    if (!Mirage::Windows::desktopWorker()) {
        if (error) *error = QStringLiteral("Windows desktop shell is not ready; waiting for WorkerW");
        return false;
    }
    struct Search { const std::wstring* device; RECT bounds{}; bool found = false; QStringList devices; } search{&m_device};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        const BOOL known = GetMonitorInfoW(monitor, &info);
        if (known) search.devices.append(QString::fromWCharArray(info.szDevice));
        if (known && _wcsicmp(info.szDevice, search.device->c_str()) == 0) {
            search.bounds = info.rcMonitor;
            search.found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
        if (error) *error = QStringLiteral("Qt output '%1' has no native monitor (available: %2)")
            .arg(QString::fromStdWString(m_device), search.devices.join(QStringLiteral(", ")));
        return false;
    }
    m_bounds = search.bounds;
    DWORD code = ERROR_SUCCESS;
    if (!Mirage::Windows::attachToDesktop(m_window, m_bounds, &code)) {
        if (error) *error = QStringLiteral("cannot attach QtWebEngine window to WorkerW: %1").arg(code);
        return false;
    }
    return true;
}

bool WRDesktopHost::desktopAt(POINT point) const {
    if (!PtInRect(&m_bounds, point)) return false;
    HWND target = WindowFromPoint(point);
    // Qt 自己收到的窗口事件无需再次注入，避免双击和双倍滚轮。
    if (target == m_window || IsChild(m_window, target)) return false;
    wchar_t name[128]{};
    while (target) {
        GetClassNameW(target, name, 128);
        if (std::wcscmp(name, L"Progman") == 0 || std::wcscmp(name, L"WorkerW") == 0) return true;
        target = GetParent(target);
    }
    return false;
}

LRESULT CALLBACK WRDesktopHost::mouseHook(int code, WPARAM message, LPARAM data) {
    auto* host = s_instance;
    if (code >= 0 && host && host->m_engine) {
        const MSLLHOOKSTRUCT event = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
        QMetaObject::invokeMethod(host, [host, message, event] { host->forwardPointer(message, event); },
                                  Qt::QueuedConnection);
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void WRDesktopHost::forwardPointer(WPARAM message, const MSLLHOOKSTRUCT& event) {
    if (!m_engine) return;
    const bool inside = desktopAt(event.pt);
    const float x = static_cast<float>(event.pt.x - m_bounds.left) / static_cast<float>(m_bounds.right - m_bounds.left);
    const float y = static_cast<float>(event.pt.y - m_bounds.top) / static_cast<float>(m_bounds.bottom - m_bounds.top);
    if (inside && !m_inside) m_engine->sendPointerEnter(x, y);
    if (!inside && m_inside) m_engine->sendPointerLeave();
    m_inside = inside;
    if (!inside) return;
    switch (message) {
    case WM_MOUSEMOVE: m_engine->sendPointerMotion(x, y); break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        m_engine->sendPointerButton(x, y, Qt::LeftButton, message == WM_LBUTTONDOWN); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        m_engine->sendPointerButton(x, y, Qt::RightButton, message == WM_RBUTTONDOWN); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        m_engine->sendPointerButton(x, y, Qt::MiddleButton, message == WM_MBUTTONDOWN); break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        const float delta = static_cast<float>(static_cast<short>(HIWORD(event.mouseData))) / WHEEL_DELTA;
        m_engine->sendPointerAxis(x, y, message == WM_MOUSEHWHEEL ? delta : 0.0f,
            message == WM_MOUSEWHEEL ? delta : 0.0f, false);
        break;
    }
    default: break;
    }
}
