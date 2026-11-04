#pragma once

#include <QObject>

#ifdef Q_OS_WIN

#include <memory>

namespace Mirage {

class WindowsPlaybackStateMonitor final : public QObject {
    Q_OBJECT

public:
    explicit WindowsPlaybackStateMonitor(QObject* parent = nullptr);
    ~WindowsPlaybackStateMonitor() override;

    void start();
    void stop();

signals:
    void otherApplicationAudioChanged(bool known, bool playing);
    // GUID_MONITOR_POWER_ON is a system-level monitor power state, not a
    // per-output sleep status. Unknown remains explicit until Windows reports it.
    void displayPowerChanged(bool known, bool asleep);
    void batteryStateChanged(bool known, bool onBattery);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Mirage

#endif // Q_OS_WIN
