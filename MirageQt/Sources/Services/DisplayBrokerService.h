// Linux 托管 mirage-display broker，Windows 直接枚举 Qt 显示器并报告
// Win32 焦点/全屏事实；两个平台共用输出和窗口状态信号。

#pragma once

#include <QObject>
#include <QString>
#include "Services/DisplayOutputModel.h"
#ifdef Q_OS_WIN
#include <QHash>
class QScreen;
class QTimer;
#else
#include <mirage_display_broker.h>
#endif

#include <atomic>
#include <cstdint>
#include <thread>

namespace Mirage {

class DisplayBrokerService final : public QObject {
    Q_OBJECT

public:
    explicit DisplayBrokerService(QObject* parent = nullptr);
    ~DisplayBrokerService() override;

    bool start(QString* error = nullptr);
    void stop();

    [[nodiscard]] static QString defaultSocketPath();
    [[nodiscard]] static bool fullscreenOutputMatchesStableId(const QString& stableOutputId,
                                                              const QString& fullscreenDevice) {
        return !fullscreenDevice.isEmpty() && stableOutputId == fullscreenDevice;
    }

signals:
    // Desktop window facts reported by a display adapter (stable output id +
    // WINDOW_STATE flags). Emitted from the broker dispatch thread; receivers
    // on the main thread get a queued delivery.
    void windowStateChanged(const QString& stableId, quint32 flags);
    void outputAdded(const Mirage::DisplayOutputSnapshot& output);
    void outputUpdated(const Mirage::DisplayOutputSnapshot& output);
    void outputRemoved(const QString& stableId);
#ifdef Q_OS_WIN
    void desktopShellAvailable();
#endif

private:
#ifdef Q_OS_WIN
    void syncOutputs();
    void pollWindowState();
    QTimer* m_windowTimer = nullptr;
    QHash<QScreen*, QString> m_screens;
    QHash<QString, quint32> m_windowFlags;
    quint32 m_shellProcessId = 0;
#else
    static void onWindowState(void* userData, const char* stableId, uint32_t flags);
    static void onOutputAdded(void* userData, const md_output_info_t* output);
    static void onOutputUpdated(void* userData, const md_output_info_t* output);
    static void onOutputRemoved(void* userData, const char* stableId);

    md_broker_t* m_broker = nullptr;
    QString m_socketPath;
    std::atomic_bool m_running { false };
    std::thread m_thread;
#endif
};

} // namespace Mirage
