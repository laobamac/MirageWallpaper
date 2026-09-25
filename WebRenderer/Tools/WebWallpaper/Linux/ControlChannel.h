#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>

// Reads one JSON command per line without blocking the Qt GUI thread.
class ControlChannel final : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<void(const QJsonObject&)>;
    ControlChannel(Handler handler, std::function<void()> onEof, QObject* parent = nullptr);
    void start();

private:
    Handler m_handler;
    std::function<void()> m_onEof;
};
