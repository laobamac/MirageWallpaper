#include "Services/DisplayBrokerService.h"

#ifdef Q_OS_WIN
#include "Services/RendererController.h"
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <windows.h>
#include <dwmapi.h>
#include <QDebug>

namespace Mirage {
namespace {
DisplayOutputSnapshot snapshot(QScreen* screen) {
    DisplayOutputSnapshot output;
    output.stableId = RendererController::stableOutputId(screen);
    output.name = screen->name();
    const QRect geometry = screen->geometry();
    output.logicalX = geometry.x();
    output.logicalY = geometry.y();
    output.logicalWidth = geometry.width();
    output.logicalHeight = geometry.height();
    output.scale120 = qRound(screen->devicePixelRatio() * 120.0);
    output.refreshMhz = qRound(screen->refreshRate() * 1000.0);
    return output;
}
bool isDesktopWindow(HWND window) {
    if (!window || window == GetShellWindow() || window == GetDesktopWindow()) return true;
    wchar_t name[256]{};
    GetClassNameW(window, name, 256);
    return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
           wcscmp(name, L"SHELLDLL_DefView") == 0 || wcscmp(name, L"Shell_TrayWnd") == 0 ||
           wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}
}

DisplayBrokerService::DisplayBrokerService(QObject* parent) : QObject(parent) {
    m_windowTimer = new QTimer(this);
    m_windowTimer->setInterval(250);
    connect(m_windowTimer, &QTimer::timeout, this, &DisplayBrokerService::pollWindowState);
}
DisplayBrokerService::~DisplayBrokerService() { stop(); }
QString DisplayBrokerService::defaultSocketPath() { return {}; }
bool DisplayBrokerService::start(QString* error) {
    Q_UNUSED(error)
    if (m_windowTimer->isActive()) return true;
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen*) { syncOutputs(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { syncOutputs(); });
    syncOutputs();
    pollWindowState();
    m_windowTimer->start();
    return true;
}
void DisplayBrokerService::stop() {
    m_windowTimer->stop();
    if (qGuiApp) disconnect(qGuiApp, nullptr, this, nullptr);
    for (QScreen* screen : m_screens.keys()) disconnect(screen, nullptr, this, nullptr);
    m_screens.clear();
    m_windowFlags.clear();
}
void DisplayBrokerService::syncOutputs() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    const auto known = m_screens;
    for (auto it = known.constBegin(); it != known.constEnd(); ++it) {
        if (screens.contains(it.key())) continue;
        m_screens.remove(it.key());
        m_windowFlags.remove(it.value());
        emit outputRemoved(it.value());
    }
    for (QScreen* screen : screens) {
        const QString stableId = RendererController::stableOutputId(screen);
        if (stableId.isEmpty()) continue;
        if (m_screens.contains(screen)) {
            emit outputUpdated(snapshot(screen));
            continue;
        }
        m_screens.insert(screen, RendererController::stableOutputId(screen));
        connect(screen, &QScreen::geometryChanged, this, [this, screen](const QRect&) { emit outputUpdated(snapshot(screen)); });
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, [this, screen](qreal) { emit outputUpdated(snapshot(screen)); });
        connect(screen, &QScreen::refreshRateChanged, this, [this, screen](qreal) { emit outputUpdated(snapshot(screen)); });
        emit outputAdded(snapshot(screen));
    }
}
void DisplayBrokerService::pollWindowState() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    DWORD shellProcessId = 0;
    if (progman) GetWindowThreadProcessId(progman, &shellProcessId);
    if (shellProcessId != 0 && shellProcessId != m_shellProcessId) {
        m_shellProcessId = shellProcessId;
        emit desktopShellAvailable();
    }

    const HWND foreground = GetForegroundWindow();
    const bool focusedApplication = !isDesktopWindow(foreground) && IsWindowVisible(foreground) && !IsIconic(foreground);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    bool fullscreen = false;
    if (focusedApplication) {
        DWORD cloaked = 0;
        DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        RECT bounds{};
        if (monitor && !cloaked && GetMonitorInfoW(monitor, &monitorInfo)) {
            if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
                GetWindowRect(foreground, &bounds);
            const RECT& target = monitorInfo.rcMonitor;
            fullscreen = bounds.left <= target.left && bounds.top <= target.top &&
                         bounds.right >= target.right && bounds.bottom >= target.bottom;
        }
    }
    const QString fullscreenDevice = fullscreen ? QString::fromWCharArray(monitorInfo.szDevice) : QString();
    for (auto it = m_screens.constBegin(); it != m_screens.constEnd(); ++it) {
        quint32 flags = focusedApplication ? 0x2u : 0u;
        if (fullscreen && fullscreenOutputMatchesStableId(it.value(), fullscreenDevice)) flags |= 0x8u;
        if (m_windowFlags.contains(it.value()) && m_windowFlags.value(it.value()) == flags) continue;
        m_windowFlags.insert(it.value(), flags);
        emit windowStateChanged(it.value(), flags);
    }
}
} // namespace Mirage
#else
#include <mirage_display_broker.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

namespace Mirage {

namespace {
DisplayOutputSnapshot snapshot(const md_output_info_t* output) {
    DisplayOutputSnapshot result;
    result.stableId = QString::fromUtf8(output->stable_id);
    result.name = QString::fromUtf8(output->name);
    result.logicalX = output->logical_x;
    result.logicalY = output->logical_y;
    result.logicalWidth = static_cast<int>(output->logical_width);
    result.logicalHeight = static_cast<int>(output->logical_height);
    result.scale120 = static_cast<int>(output->scale_120);
    result.refreshMhz = static_cast<int>(output->refresh_mhz);
    return result;
}
}

DisplayBrokerService::DisplayBrokerService(QObject* parent)
    : QObject(parent) {}

DisplayBrokerService::~DisplayBrokerService() {
    stop();
}

QString DisplayBrokerService::defaultSocketPath() {
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtimeDirectory.isEmpty()) return {};
    return QDir(runtimeDirectory).filePath(QStringLiteral("mirage-wallpaper/display-v1.sock"));
}

bool DisplayBrokerService::start(QString* error) {
    if (m_running.load()) return true;
    m_socketPath = defaultSocketPath();
    if (m_socketPath.isEmpty()) {
        if (error) *error = QStringLiteral("XDG_RUNTIME_DIR is not set");
        return false;
    }
    const QString directory = QFileInfo(m_socketPath).absolutePath();
    if (!QDir().mkpath(directory) ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner)) {
        if (error) *error = QStringLiteral("Cannot create secure broker directory: %1").arg(directory);
        return false;
    }

    // Remove stale socket file from previous crash or unclean shutdown
    QFile::remove(m_socketPath);

    const QByteArray socketBytes = m_socketPath.toUtf8();
    md_broker_options_t options {
        .socket_path = socketBytes.constData(),
        .server_name = "MirageQt",
        .server_version = "0.1.0",
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS |
                    MD_FEATURE_WINDOW_STATE,
        .max_routes = 16,
        .on_output_added = &DisplayBrokerService::onOutputAdded,
        .on_output_updated = &DisplayBrokerService::onOutputUpdated,
        .on_output_removed = &DisplayBrokerService::onOutputRemoved,
        .on_window_state = &DisplayBrokerService::onWindowState,
        .user_data = this,
    };
    m_broker = md_broker_new(&options);
    if (m_broker == nullptr || md_broker_listen(m_broker) != MD_OK) {
        if (error) *error = QStringLiteral("Cannot listen on display broker socket: %1")
                                .arg(m_socketPath);
        md_broker_free(m_broker);
        m_broker = nullptr;
        return false;
    }

    m_running.store(true);
    qWarning("[DisplayBroker] Starting dispatch thread...");
    // Serve the socket on a worker thread so the UI never blocks: dispatch
    // polls with a 100 ms timeout and md_broker_stop() (from stop()) wakes it,
    // so the thread can be joined without waiting for a full timeout.
    m_thread = std::thread([this] {
        qWarning("[DisplayBroker] Dispatch thread started, entering loop");
        while (m_running.load()) {
            const int result = md_broker_dispatch(m_broker, 100);
            if (result == MD_ERR_DISCONNECTED) {
                qWarning("[DisplayBroker] Dispatch: disconnected");
                break;
            }
            if (result < 0) {
                qWarning("[DisplayBroker] Dispatch failed: %d", result);
                break;
            }
        }
        qWarning("[DisplayBroker] Dispatch thread exiting");
        m_running.store(false);
    });
    qWarning("[DisplayBroker] Broker started successfully on %s", qPrintable(m_socketPath));
    return true;
}

void DisplayBrokerService::stop() {
    if (m_broker == nullptr) return;
    m_running.store(false);
    md_broker_stop(m_broker);
    if (m_thread.joinable()) m_thread.join();
    md_broker_free(m_broker);
    m_broker = nullptr;
}

/* Broker dispatch-thread trampoline: the stable id is borrowed for the call,
 * so it is copied into a QString before emitting the Qt signal. The signal
 * crosses into the main thread via a queued connection. */
void DisplayBrokerService::onWindowState(void* userData, const char* stableId,
                                         uint32_t flags) {
    auto* self = static_cast<DisplayBrokerService*>(userData);
    emit self->windowStateChanged(QString::fromUtf8(stableId != nullptr ? stableId : ""),
                                  static_cast<quint32>(flags));
}

void DisplayBrokerService::onOutputAdded(void* userData, const md_output_info_t* output) {
    auto* self = static_cast<DisplayBrokerService*>(userData);
    const DisplayOutputSnapshot copied = snapshot(output);
    emit self->outputAdded(copied);
}

void DisplayBrokerService::onOutputUpdated(void* userData, const md_output_info_t* output) {
    auto* self = static_cast<DisplayBrokerService*>(userData);
    const DisplayOutputSnapshot copied = snapshot(output);
    emit self->outputUpdated(copied);
}

void DisplayBrokerService::onOutputRemoved(void* userData, const char* stableId) {
    auto* self = static_cast<DisplayBrokerService*>(userData);
    emit self->outputRemoved(QString::fromUtf8(stableId));
}

} // namespace Mirage
#endif
