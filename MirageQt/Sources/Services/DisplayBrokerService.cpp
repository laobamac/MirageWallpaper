#include "Services/DisplayBrokerService.h"

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
