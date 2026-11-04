#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <thread>
#include <windows.h>

class VRControlChannel final : public QObject {
public:
    using CommandHandler = std::function<void(const QJsonObject&)>;
    using CloseHandler = std::function<void()>;
    VRControlChannel(CommandHandler command, CloseHandler close, QObject* parent = nullptr);
    ~VRControlChannel() override;
    bool start(QString* error = nullptr);
    void stop();

private:
    void readLoop();
    void consume(const QByteArray& bytes);
    void finish();
    CommandHandler m_command;
    CloseHandler m_close;
    QByteArray m_buffer;
    HANDLE m_input = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    DWORD m_type = FILE_TYPE_UNKNOWN;
    std::thread m_thread;
    bool m_finished = false;
};
