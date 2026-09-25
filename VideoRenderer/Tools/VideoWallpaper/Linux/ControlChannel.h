// VRControlChannel — line-oriented JSON control protocol over stdin; see
// SceneRenderer/Tools/SceneWallpaper/ControlChannel.h for the wire format.
// Each line is one {"cmd": ...} object; EOF or "quit" stops the host.

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QSocketNotifier;

class VRControlChannel final : public QObject {
public:
    using CommandHandler = std::function<void(const QJsonObject&)>;
    using CloseHandler = std::function<void()>;

    VRControlChannel(CommandHandler commandHandler, CloseHandler closeHandler,
                     QObject* parent = nullptr);
    ~VRControlChannel() override;

    [[nodiscard]] bool start(QString* error = nullptr);

private:
    void readAvailable();
    void handleLine(const QByteArray& line);
    void finish();

    CommandHandler m_commandHandler;
    CloseHandler m_closeHandler;
    QSocketNotifier* m_notifier = nullptr;
    QByteArray m_buffer;
    int m_originalFlags = -1;
    bool m_finished = false;
};
