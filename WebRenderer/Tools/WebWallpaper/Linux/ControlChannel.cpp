#include "ControlChannel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>

#include <thread>

ControlChannel::ControlChannel(Handler handler, std::function<void()> onEof, QObject* parent)
    : QObject(parent), m_handler(std::move(handler)), m_onEof(std::move(onEof)) {}

void ControlChannel::start() {
    std::thread([this] {
        QTextStream stream(stdin);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8());
            if (!document.isObject()) continue;
            const QJsonObject command = document.object();
            QMetaObject::invokeMethod(this, [this, command] {
                if (command.value(QStringLiteral("cmd")).toString() == QStringLiteral("quit")) {
                    if (m_onEof) m_onEof();
                    return;
                }
                if (m_handler) m_handler(command);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { if (m_onEof) m_onEof(); }, Qt::QueuedConnection);
    }).detach();
}
