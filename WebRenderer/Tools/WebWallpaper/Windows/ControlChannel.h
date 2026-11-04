#pragma once

#include <QJsonObject>
#include <QObject>
#include <atomic>
#include <functional>
#include <thread>
#include <windows.h>

class ControlChannel final : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<void(const QJsonObject&)>;
    ControlChannel(Handler handler, std::function<void()> onEof, QObject* parent = nullptr);
    ~ControlChannel() override;
    void start();

private:
    void readCommands();
    void dispatch(const QByteArray& line);
    Handler m_handler;
    std::function<void()> m_onEof;
    std::atomic<bool> m_stopping{false};
    std::thread m_reader;
    HANDLE m_input = nullptr;
    HANDLE m_finished = nullptr;
};
