#pragma once

#include <QJsonObject>
#include <QObject>
#include <QThread>

namespace Mirage {

class ControlReader : public QObject {
    Q_OBJECT

public:
    explicit ControlReader(QObject* parent = nullptr);
    ~ControlReader() override;

    void start();
    void stop();

signals:
    void commandReceived(const QJsonObject& command);
    void inputClosed();

private:
    QThread* m_thread = nullptr;
    bool m_running = false;
};

} // namespace Mirage
