#include "Services/DisplayBrokerService.h"

#include <mirage_display_broker.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

namespace Mirage {

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

    const QByteArray socketBytes = m_socketPath.toUtf8();
    md_broker_options_t options {
        .socket_path = socketBytes.constData(),
        .server_name = "MirageQt",
        .server_version = "0.1.0",
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                    MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS |
                    MD_FEATURE_WINDOW_STATE,
        .max_routes = 16,
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
    // Serve the socket on a worker thread so the UI never blocks: dispatch
    // polls with a 100 ms timeout and md_broker_stop() (from stop()) wakes it,
    // so the thread can be joined without waiting for a full timeout.
    m_thread = std::thread([this] {
        while (m_running.load()) {
            const int result = md_broker_dispatch(m_broker, 100);
            if (result == MD_ERR_DISCONNECTED) break;
            if (result < 0) {
                qWarning("Mirage display broker dispatch failed: %d", result);
                break;
            }
        }
        m_running.store(false);
    });
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

} // namespace Mirage
